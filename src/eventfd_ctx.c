#include "eventfd_ctx.h"

#include <sys/event.h>
#include <sys/param.h>

#include <fcntl.h>
#include <unistd.h>

#include <assert.h>

static_assert(sizeof(unsigned int) < sizeof(uint64_t), "");

errno_t
eventfd_ctx_init(EventFDCtx *eventfd, unsigned int counter, bool is_semaphore)
{
	*eventfd = (EventFDCtx){
	    .kq_ = kqueue(),
	    .counter_ = counter,
	    .is_semaphore_ = is_semaphore,
	};
	if (eventfd->kq_ < 0) {
		return (errno);
	}

	struct kevent kevs[1];

	EV_SET(&kevs[0], 0, EVFILT_USER, EV_ADD | EV_CLEAR, 0, 0, 0);
	if (kevent(eventfd->kq_, kevs, nitems(kevs), NULL, 0, NULL) < 0) {
		errno_t err = errno;
		close(eventfd->kq_);
		return (err);
	}

	if (counter > 0) {
		struct kevent kevs[1];
		EV_SET(&kevs[0], 0, EVFILT_USER, 0, NOTE_TRIGGER, 0, 0);

		if (kevent(eventfd->kq_, kevs, nitems(kevs), /**/
			NULL, 0, NULL) < 0) {
			close(eventfd->kq_);
			return (errno);
		}
	}

	return (0);
}

errno_t
eventfd_ctx_terminate(EventFDCtx *eventfd)
{
	if (close(eventfd->kq_) < 0) {
		return (errno);
	}

	return (0);
}

int
eventfd_ctx_fd(EventFDCtx *eventfd)
{
	return (eventfd->kq_);
}

errno_t
eventfd_ctx_write(EventFDCtx *eventfd, uint64_t value)
{
	if (value == UINT64_MAX) {
		return (EINVAL);
	}

	for (;;) {
		uint_least64_t current_value = atomic_load(&eventfd->counter_);

		uint_least64_t new_value;
		if (__builtin_add_overflow(current_value, value, &new_value) ||
		    new_value > UINT64_MAX - 1) {
			return (EAGAIN);
		}

		if (atomic_compare_exchange_weak(&eventfd->counter_,
			&current_value, new_value)) {
			break;
		}
	}

	struct kevent kevs[1];
	EV_SET(&kevs[0], 0, EVFILT_USER, 0, NOTE_TRIGGER, 0, 0);

	if (kevent(eventfd->kq_, kevs, nitems(kevs), NULL, 0, NULL) < 0) {
		return (errno);
	}

	return (0);
}

errno_t
eventfd_ctx_read(EventFDCtx *eventfd, uint64_t *value)
{
	uint_least64_t current_value;

	for (;;) {
		current_value = atomic_load(&eventfd->counter_);
		if (current_value == 0) {
			return (EAGAIN);
		}

		uint_least64_t new_value =
		    eventfd->is_semaphore_ ? current_value - 1 : 0;

		if (new_value == 0) {
			struct timespec zero_timeout = {0, 0};
			struct kevent kevs[32];
			int n;

			while ((n = kevent(eventfd->kq_, NULL, 0, kevs,
				    nitems(kevs), &zero_timeout)) > 0) {
			}
			if (n < 0) {
				return (errno);
			}
		}

		if (atomic_compare_exchange_weak(&eventfd->counter_,
			&current_value, new_value)) {
			break;
		}

		if (new_value == 0 && current_value > 0) {
			struct kevent kevs[1];
			EV_SET(&kevs[0], 0, EVFILT_USER, /**/
			    0, NOTE_TRIGGER, 0, 0);

			if (kevent(eventfd->kq_, kevs, nitems(kevs), /**/
				NULL, 0, NULL) < 0) {
				return (errno);
			}
		}
	}

	*value = eventfd->is_semaphore_ ? 1 : current_value;
	return (0);
}
/*
   Unix SMB/CIFS implementation.

   main select loop and event handling - kqueue implementation

   Copyright (C) Andrew Tridgell	2003-2005
   Copyright (C) Stefan Metzmacher	2005-2013
   Copyright (C) Jeremy Allison		2013
   Copyright (C) iXsystems		2020

     ** NOTE! The following LGPL license applies to the tevent
     ** library. This does NOT imply that all of Samba is released
     ** under the LGPL

   This library is free software; you can redistribute it and/or
   modify it under the terms of the GNU Lesser General Public
   License as published by the Free Software Foundation; either
   version 3 of the License, or (at your option) any later version.

   This library is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
   Lesser General Public License for more details.

   You should have received a copy of the GNU Lesser General Public
   License along with this library; if not, see <http://www.gnu.org/licenses/>.
*/
#define TEVENT_DEPRECATED 1
#include <sys/cdefs.h>
#include "replace.h"
#include <sys/event.h>
#include <search.h>
#include <aio.h>
#include "system/filesys.h"
#include "system/select.h"
#include "tevent.h"
#include "tevent_internal.h"
#include "tevent_util.h"
#include "tevent_kqueue.h"

/* struct tevent_fd additional flags */
#define TEVENT_FD_RW (TEVENT_FD_READ|TEVENT_FD_WRITE)
#define HAS_WRITE(flags) (flags & TEVENT_FD_WRITE)
#define HAS_READ(flags) (flags & TEVENT_FD_READ)
#define HAS_READ_OR_WRITE(flags) (flags & TEVENT_FD_RW)
#define HAS_READ_AND_WRITE(flags) ((flags & TEVENT_FD_RW) == TEVENT_FD_RW)
#define KEVENT_TO_TEVENT_FLAGS(f) (abs(f) & TEVENT_FD_RW)

struct tkq {
	int kq_fd;
	struct kevent *kq_array;
	size_t kq_sz;
};

struct kq_context {
	/* a pointer back to the generic event_context */
	struct tevent_context *ev;

	/*
	 * queue for  EVFILT_READ | EVFILT_WRITE | EVFILT_AIO
	 * may add more queues if needed for signals and timers
	 */
	struct tkq *rdwrq;

	pid_t pid;
	bool busy;
	bool panic_force_replay;
	bool *panic_state;
	bool (*panic_fallback)(struct tevent_context *ev, bool replay);
	TALLOC_CTX *aio_pool;
};
#define	EVTOKQ(x) (talloc_get_type_abort(x->additional_data, struct kq_context))
#define	DATATOFDE(x) (talloc_get_type_abort(x, struct tevent_fd))

static void kqueue_panic(struct kq_context *kqueue_ev,
			 const char *reason, bool replay);

static void kqueue_add_event(struct kq_context *kqueue_ev,
			     struct tevent_fd *fde);

/*
  called to set the panic fallback function.
*/
_PRIVATE_ void tevent_kqueue_set_panic_fallback(struct tevent_context *ev,
	bool (*panic_fallback)(struct tevent_context *ev,
	bool replay))
{
	struct kq_context *kqueue_ev = EVTOKQ(ev);
	kqueue_ev->panic_fallback = panic_fallback;
}

/*
  called when a kqueue call fails
*/
static void kqueue_panic(struct kq_context *kqueue_ev,
			 const char *reason, bool replay)
{
	struct tevent_context *ev = kqueue_ev->ev;
	bool (*panic_fallback)(struct tevent_context *ev, bool replay);

	panic_fallback = kqueue_ev->panic_fallback;

	if (kqueue_ev->panic_state != NULL) {
		*kqueue_ev->panic_state = true;
	}

	if (kqueue_ev->panic_force_replay) {
		replay = true;
	}

	if (panic_fallback == NULL) {
		tevent_debug(ev, TEVENT_DEBUG_FATAL,
			"%s (%s) replay[%u] - calling abort()\n",
			reason, strerror(errno), (unsigned)replay);
		abort();
	}
	if (!panic_fallback(ev, replay)) {
		/* Fallback failed. */
		tevent_debug(ev, TEVENT_DEBUG_FATAL,
			"%s (%s) replay[%u] - calling abort()\n",
			reason, strerror(errno), (unsigned)replay);
		abort();
	}
}

static int kq_destructor(struct tkq *kq)
{
	close(kq->kq_fd);
	kq->kq_fd = -1;
	return 0;
}

static int do_kqueue(struct kq_context *kqueue_ev, struct tkq *kq)
{
	#define INIT_KQ_SZ	16
	kq->kq_fd = kqueue();
	if (kq->kq_fd == -1) {
		tevent_debug(kqueue_ev->ev, TEVENT_DEBUG_FATAL,
			     "do_kqueue: Failed to create kqueue: %s \n",
			     strerror(errno));
		return -1;
	}
	if (!ev_set_close_on_exec(kq->kq_fd)) {
		tevent_debug(kqueue_ev->ev, TEVENT_DEBUG_WARNING,
			     "do_kqueue: "
			     "Failed to set close-on-exec on queue, "
			     "file descriptor may be leaked to children.\n");
	}
	kq->kq_array = talloc_zero_array(kqueue_ev, struct kevent, INIT_KQ_SZ);
	kq->kq_sz = INIT_KQ_SZ;
	talloc_set_destructor(kq, kq_destructor);
	return 0;
}

static int kqueue_init_ctx(struct kq_context *kqueue_ev)
{
	int ret;

	kqueue_ev->rdwrq = talloc_zero(kqueue_ev, struct tkq);
	if (kqueue_ev->rdwrq == 0) {
		abort();
	}

	ret = do_kqueue(kqueue_ev, kqueue_ev->rdwrq);
	if (ret != 0) {
		return ret;
	}

	kqueue_ev->pid = getpid();

	return 0;
}

/*
  The kqueue queue is not inherited by a child created with fork(2).
  So we need to re-initialize if this happens.
 */
static void kqueue_check_reopen(struct kq_context *kqueue_ev)
{
	struct tevent_fd *fde = NULL;
	int ret, signum;
	bool *caller_panic_state = kqueue_ev->panic_state;
	bool panic_triggered = false;

	if (kqueue_ev->pid == getpid()) {
		return;
	}
	/*
	 * We've forked. Re-initialize.
	 */

	close(kqueue_ev->rdwrq->kq_fd);
	ret = do_kqueue(kqueue_ev, kqueue_ev->rdwrq);
	if (ret != 0) {
		kqueue_panic(kqueue_ev, "kqueue() failed", false);
		return;
	}
	kqueue_ev->pid = getpid();
	kqueue_ev->panic_state = &panic_triggered;
	kqueue_ev->busy = true;
	for (fde=kqueue_ev->ev->fd_events;fde;fde=fde->next) {
		kqueue_add_event(kqueue_ev, fde);
		if (panic_triggered) {
			if (caller_panic_state != NULL) {
				*caller_panic_state = true;
			}
			kqueue_ev->busy = false;
			return;
		}
	}

	kqueue_ev->busy = false;
	kqueue_ev->panic_state = NULL;
}

/*
 * add the kqueue event to the given fd event.
 * filter is removed in the destructor for the fd event.
 */
static void kqueue_add_event(struct kq_context *kqueue_ev,
			     struct tevent_fd *fde)
{
	struct kevent event[2];
	int ret, count;
	struct tevent_fd *mpx_fde = NULL;
	short kevent_filter = 0;
	count = 0;
	/*
	 * kevents are uniquely identified by the tuple (queue, ident, and
	 * filter). This means that if a tevent fd event has both
	 * TEVENT_FD_READ and TEVENT_FD_WRITE flags set, then two separate
	 * kevents will need to be generated. Consequently, an array of kevents
	 * must be retrieved in loop_once() so that the correct tevent flags
	 * can be determined when calling the relevant handler function.
	 */
	if (!HAS_READ_OR_WRITE(fde->flags)) {
		return;
	}

	/*
	 * In tevent, multiple fd events with overlapping flags may be
	 * registered for the fd. On collision with existing kevent (same
	 * kqueue, fd, and filter), the existing kevent will be overwritten
	 * with new data. Future room for improvement here is to check if the
	 * existing kevent(s) already cover all the requisite filters. In this
	 * case, then we can simply place a pointer to new tevent_fd as
	 * additional_data in the existing tevent_fd.
	 */
	for (mpx_fde = kqueue_ev->ev->fd_events;
	     mpx_fde; mpx_fde = mpx_fde->next) {
		if ((mpx_fde == fde) || (mpx_fde->fd != fde->fd)) {
			continue;
		}
		fde->additional_data = mpx_fde;
		mpx_fde->additional_data = fde;
		break;
	}

	if (HAS_READ(fde->flags)) {
		EV_SET(&event[0],		//kev
		       fde->fd,			//ident
		       EVFILT_READ,		//filter
		       EV_ADD,			//flags
		       0,			//fflags
		       0,			//data
		       fde);			//udata
		count++;
	}
	if (HAS_WRITE(fde->flags)) {
		EV_SET(&event[count],		//kev
		       fde->fd,			//ident
		       EVFILT_WRITE,		//filter
		       EV_ADD,			//flags
		       0,			//fflags
		       0,			//data
		       fde);			//udata
		count++;
	}
	if (count) {
		ret = kevent(kqueue_ev->rdwrq->kq_fd,
			event, count, NULL, 0, NULL);
		if (ret != 0) {
			goto failure;
		}
	}

	return;

failure:
	/*
	 * kevent() will set errno to EBADF if the kqueue fd is invalid.
	 * In the context of samba, this likely means that we've forked and
	 * are in a child process.
	 * kevent() will set errno EINVAL if the filter is invalid. In
	 * the case of an fd-based filter (EVFILT_READ, EVFILT_WRITE), it
	 * most likely indicates that the fd we're monitoring is closed.
	 * In the former failure case, we should re-init the kqueue event
	 * context (re-adding tevent_fd events). In the latter case, we should
	 * remove the problematic fd from the list of things we are monitoring.
	 */
	if (errno == EBADF) {
		if (kqueue_ev->busy) {
			return;
		}
		return kqueue_check_reopen(kqueue_ev);
	}
	else if (errno == EINVAL) {
		tevent_debug(kqueue_ev->ev, TEVENT_DEBUG_FATAL,
			     "kevent EINVAL for "
			     "fde[%p] fd[%d] - disabling\n",
			     fde, fde->fd);
		DLIST_REMOVE(kqueue_ev->ev->fd_events, fde);
		fde->wrapper = NULL;
		fde->event_ctx = NULL;
		return;
	} else {
		kqueue_panic(kqueue_ev, "KEVENT add failed", false);
		return;
	}
}

static void kqueue_delete_event(struct kq_context *kqueue_ev,
			        struct tevent_fd *fde,
				uint16_t to_delete)
{
	struct kevent event[2];
	int ret, count = 0;
	struct tevent_fd empty = { 0 }, *mpx_fde = &empty;

	/*
	 * One or more flags is being removed from the fde. If the existing
	 * kevent is associated with multiple tevent fd events, then the
	 * existing kevent will need to be updated so that it is associated
	 * with one of the un-discharged fd events.
	 */

	if (fde->additional_data != NULL) {
		mpx_fde = DATATOFDE(fde->additional_data);
		/* all flags removed from fde. No longer mpx */
		if ((fde->flags & ~to_delete) == 0) {
			mpx_fde->additional_data = NULL;
		}
	}

	if (HAS_READ(to_delete)) {
		if (HAS_READ(mpx_fde->flags)) {
			mpx_fde->additional_data = NULL;
			EV_SET(&event[count],		//kev
			       fde->fd,			//ident
			       EVFILT_READ,		//filter
			       EV_ADD,			//flags
			       0,			//fflags
			       0,			//data
			       mpx_fde);		//udata
		}
		else {
			EV_SET(&event[count],		//kev
			       fde->fd,			//ident
			       EVFILT_READ,		//filter
			       EV_DELETE,		//flags
			       0,			//fflags
			       0,			//data
			       0);			//udata
		}
		count++;
	}
	if (HAS_WRITE(to_delete)) {
		if (HAS_WRITE(mpx_fde->flags)) {
			mpx_fde->additional_data = NULL;
			EV_SET(&event[count],		//kev
			       fde->fd,			//ident
			       EVFILT_WRITE,		//filter
			       EV_ADD,			//flags
			       0,			//fflags
			       0,			//data
			       mpx_fde);		//udata
		}
		else {
			EV_SET(&event[count],		//kev
			       fde->fd,			//ident
			       EVFILT_WRITE,		//filter
			       EV_DELETE,		//flags
			       0,			//fflags
			       0,			//data
			       0);			//udata
		}
		count++;
	}
	if (count) {
		ret = kevent(kqueue_ev->rdwrq->kq_fd, event, count, NULL, 0, NULL);
		if (ret != 0) {
			if ((errno == ENOENT) || (errno == EBADF)) {
				return;
			}
			tevent_debug(kqueue_ev->ev, TEVENT_DEBUG_FATAL,
				"kqueue_delete_event() failed for fde[%p] "
				"err: [%s]\n", fde, strerror(errno));
		}
	}
}

static int kqueue_event_fd_destructor(struct tevent_fd *fde)
{
	struct tevent_context *ev = fde->event_ctx;
	struct kq_context *kqueue_ev = NULL;
	int flags = fde->flags;

	if (ev == NULL) {
		return tevent_common_fd_destructor(fde);
        }

	kqueue_ev = EVTOKQ(ev);
	/*
	 * we must remove the event from the list
	 * otherwise a panic fallback handler may
	 * reuse invalid memory
	 */
	DLIST_REMOVE(ev->fd_events, fde);
	kqueue_delete_event(kqueue_ev, fde, fde->flags);
	return tevent_common_fd_destructor(fde);
}

static int kqueue_process_fd(struct kq_context *kqueue_ev,
			     struct tevent_fd *fde,
			     uint16_t flags)
{
	int ret;
	struct tevent_fd *mpx_fde = NULL;

	if (fde->additional_data != NULL) {
		mpx_fde = DATATOFDE(fde->additional_data);
	}

	if (HAS_WRITE(flags)) {
		if (HAS_WRITE(fde->flags)) {
			mpx_fde = NULL;
		}
		if (mpx_fde && HAS_WRITE(mpx_fde->flags)) {
			fde = mpx_fde;
			mpx_fde = NULL;
		}
	}

	if (mpx_fde) {
		if ((flags & fde->flags) == 0) {
			fde = mpx_fde;
			mpx_fde = NULL;
		}
	}

	return tevent_common_invoke_fd_handler(fde, fde->flags & flags, NULL);
}

static void kqueue_process_aio(struct kq_context *kqueue_ev,
			       void *udata)
{
	struct tevent_aiocb *tiocbp = NULL;

	tiocbp = talloc_get_type_abort(udata, struct tevent_aiocb);
	if (tiocbp == NULL) {
		tevent_debug(kqueue_ev->ev, TEVENT_DEBUG_FATAL,
			     "aio request was freed after being put on kevent queue. "
			     "memory may leak.\n");
		return;
	}
	if (tiocbp->iocbp == NULL) {
		tevent_debug(kqueue_ev->ev, TEVENT_DEBUG_FATAL,
			     "aiocb request is already completed.\n");
		abort();
	}

	if (!tevent_req_is_in_progress(tiocbp->req)) {
		tevent_debug(kqueue_ev->ev, TEVENT_DEBUG_FATAL,
			     "tevent request for aio event is not in progress.\n");
		abort();
	}

	tiocbp->rv = aio_return(tiocbp->iocbp);
	if (tiocbp->rv == -1) {
		tevent_debug(
			kqueue_ev->ev, TEVENT_DEBUG_WARNING,
			"%s: processing AIO [%p] - failed: %s\n",
			 tiocbp->location, tiocbp->iocbp, strerror(errno)
		);
		tiocbp->saved_errno = errno;
		TALLOC_FREE(tiocbp->iocbp);
		tevent_req_error(tiocbp->req, errno);
		return;
	}

	TALLOC_FREE(tiocbp->iocbp);
	tevent_req_done(tiocbp->req);
}

static void kqueue_process_kev(struct kq_context *kqueue_ev,
			       struct kevent kev, struct tevent_fd **pfde,
			       bool *_is_fd_event)
{
	struct tevent_fd *fde = NULL;
	bool is_fd_event = false;

	switch (kev.filter) {
	case EVFILT_WRITE:
		fde = DATATOFDE(kev.udata);
		if ((kev.flags & EV_EOF) && !(fde->flags & TEVENT_FD_READ)) {
			/* If we only wait for TEVENT_FD_WRITE, we
			   should not tell the event handler about it,
			   and remove the writable flag, as we only
			   report errors when waiting for read events
			   to match the select behavior. */
			/*
			 * Socket error is returned in fflags. For now,
			 * log, but don't add error handling.
			 */
			if (kev.fflags != 0) {
				tevent_debug(kqueue_ev->ev, TEVENT_DEBUG_TRACE,
					"EVFILT_WRITE EV_EOF on [%s], "
					"fd [%d]: %s\n", fde->location, fde->fd,
					strerror(kev.fflags));
				TEVENT_FD_NOT_WRITEABLE(fde);
				*_is_fd_event = is_fd_event;
				return;
			}
		}
		*pfde = fde;
		/*
		 * Skip event handler if the event is busy.
		 * kevent will be automatically removed with
		 * last close of the fd.
		 */
		is_fd_event = fde->busy == 0 ? true : false;
		break;
	case EVFILT_READ:
		fde = DATATOFDE(kev.udata);
		if ((kev.flags & EV_EOF)) {
			if (kev.fflags != 0) {
				tevent_debug(kqueue_ev->ev, TEVENT_DEBUG_TRACE,
					"evfilt_read ev_eof on [%s]: %s\n",
					fde->location, strerror(kev.fflags));
			}
			tevent_debug(kqueue_ev->ev, TEVENT_DEBUG_TRACE,
				"EVFILT_READ EV_EOF on fd: %d flags: 0x%08x: "
				"additional flags: 0x%08lx\n",
				fde->fd, fde->flags,
				fde->additional_flags);
		}
		*pfde = fde;
		/*
		 * Skip event handler if the event is busy.
		 * kevent will be automatically removed with
		 * last close of the fd.
		 */
		if (fde->busy == 0 && HAS_READ(fde->flags)) {
			is_fd_event = true;
		}
		break;
	case EVFILT_AIO:
		kqueue_process_aio(kqueue_ev, kev.udata);
		break;
	default:
		kqueue_panic(kqueue_ev, "Unexpected kevent filter: %d",
			     kev.filter);
	}
	*_is_fd_event = is_fd_event;
	return;
}

static int search_kevent(struct tevent_fd *fde,
	struct kevent *events,
	int nkevents)
{
	int i, flags = 0;
	struct tevent_fd *mpx_fde = NULL;
	struct kq_context *kqueue_ev = EVTOKQ(fde->event_ctx);

	if (fde->additional_data != NULL) {
		mpx_fde = DATATOFDE(fde->additional_data);
	}

	for (i = 0; i < nkevents; i++) {
		bool is_fde;
		struct tevent_fd *next_fde = NULL;
		kqueue_process_kev(kqueue_ev, events[i], &next_fde, &is_fde);
		if (!is_fde) {
			continue;
		}

		if ((fde == next_fde) ||
		    (mpx_fde && (mpx_fde->fd == next_fde->fd))){
			flags = KEVENT_TO_TEVENT_FLAGS(events[i].filter);
		}
	}

	return flags;
}

static int process_kevents(struct kq_context *kqueue_ev, int nkevents)
{
	int flags, i;
	bool ok;
	struct kevent *received = kqueue_ev->rdwrq->kq_array;
	struct tevent_fd *head = NULL;

	for (i = 0; i < nkevents; i++) {
		kqueue_process_kev(kqueue_ev, received[i], &head, &ok);
		if (!ok) {
			continue;
		}
		flags = KEVENT_TO_TEVENT_FLAGS(received[i].filter);

		if (nkevents - (i + 1) == 0) {
			break;
		}

		/*
		 * libtevent allows setting
		 * TEVENT_FD_READ | TEVENT_FD_WRITE on a single
		 * fd event. This isn't possible in kqueue and so
		 * we register separate kevents. Search through the
		 * kevent list for the paired kevent and fill out flags
		 * for tevent_handler as needed.
		 */
		flags |= search_kevent(head,
				       &received[i + 1],
				       nkevents - (i + 1));

		break;
	}

	if (head == NULL) {
		/* Handler isn't called if event has busy state. */
		tevent_debug(kqueue_ev->ev, TEVENT_DEBUG_WARNING,
			"no actionable events in event list\n");
		return 0;
	}

	return kqueue_process_fd(kqueue_ev, head, flags);
}

/*
 * Process errors for kevent() call when reading new kevents on our tevent
 * queue. This function returns the number of kevents after processing.
 * Expected returns are as follows:
 *
 * -1 we encountered an error. That is somehow recoverable. This will
 *    eventually be passed back to caller of tevent_loop_once().
 *
 *  0 we encountered a signal or successfully re-initialized our queue after
 *    forking. This will be passed back to caller of tevent_loop_once().
 *
 * >0 we re-initialized our queue after forking and already had some new
 *    events by the time we ran kevent() to verify re-initialization is
 *    successful. This will be passed to calling function so that it can
 *     process array of kevents we received.
 *
 * panic - something totally unxpected happened and we want to abort so that
 *    we can sift through the ashes.
 */
static int check_kevent_errors(struct kq_context *kqueue_ev,
			       struct timespec *tsp,
			       int error)
{
	int nkevents = 0;

	if (error == EINTR) {
		if (kqueue_ev->ev->signal_events) {
			tevent_common_check_signal(kqueue_ev->ev);
		}
		return 0;
	}

	/*
	 * One likely cause for kevent() to fail with EBADF is that
	 * we forked. In this case call kqueue_check_reopen() to reinitialize
	 * the kqueue.
	 */
	if (error == EBADF) {
		if (kqueue_ev->busy) {
			return -1;
		}

		kqueue_check_reopen(kqueue_ev);

		/*
		 * If can't open new kqueue, try falling back to another event
		 * handler. In this case poll_mt.
		 */
		if (kqueue_ev->panic_state) {
			tevent_debug(kqueue_ev->ev, TEVENT_DEBUG_FATAL,
				"panic_tiggered in kqueue event loop once\n");
			errno = EINVAL;
			return -1;
		}

		/*
		 * We've forked and rebuilt our queue of monitored fds, check
		 * them for kevents.
		 */
		nkevents = kevent(kqueue_ev->rdwrq->kq_fd, NULL, 0,
				  kqueue_ev->rdwrq->kq_array,
				  kqueue_ev->rdwrq->kq_sz, tsp);

		/*
		 * Return the new number of kevents if we don't encounter
		 * an error.
		 */
		if (nkevents != -1) {
			return nkevents;
		}
	}

	/*
	 * We encountered some other unexpected error. Fall on our sword.
	 */
	tevent_debug(kqueue_ev->ev, TEVENT_DEBUG_FATAL,
		"kevent() failed: %s\n",strerror(error));
	kqueue_panic(kqueue_ev, "kevent() failed", false);

	return -1;
}

static void expand_queue(struct kq_context *kqueue_ev, struct tkq *kq)
{
	struct kevent *new_array = NULL;
	int new_size = kq->kq_sz * 2;

	new_array = talloc_realloc(kqueue_ev, kq->kq_array,
		 struct kevent, new_size);
	if (new_array == NULL) {
		tevent_debug(kqueue_ev->ev, TEVENT_DEBUG_FATAL,
			"realloc of kq array failed: %s\n", strerror(errno));
			abort();
	}
	kq->kq_array = new_array;
	kq->kq_sz = new_size;
}

static int kqueue_event_loop(struct kq_context *kqueue_ev,
			     struct timeval *tvalp)
{
	/*
	 * Loop through an array of kevents. We need to retrieve multiple
	 * events at once in order to properly reconstruct the correct flags to
	 * send to the tevent fd handler.
	 */
	int nkevents, ret, saved_errno;
	struct timespec ts;
	struct timespec *tsp = NULL;
	ret = 0;
	nkevents = 0;

	if (tvalp) {
		/*
		 * Adjust timeout for kevent() call to be slighly longer than
		 * the next timed event. This value is taken from other tevent
		 * backends where the reasoning is that it's better to have a
		 * timer fire a little late than too early.
		 */
		ts = (struct timespec) {
			.tv_sec = tvalp->tv_sec,
			.tv_nsec = (tvalp->tv_usec + 999) * 1000
		};
		if (ts.tv_nsec >= 1000000000) {
			ts.tv_sec += 1;
			ts.tv_nsec -= 1000000000;
		}
		tsp = &ts;
	}

	/*
	 * If timeout is a non-NULL pointer, it specifies a maximum interval
	 * to wait for an event. If timeout is a NULL pointer, kevent() waits
	 * indefinetly. To effect a poll, the timeout argument should be
	 * non-NULL, pointing to a zero-valued timespec structure.
	 *
	 * If tvalp is NULL, then we pass a NULL for kevent() timeout. This is
	 * equivalent to epoll_wait() on Linux with a timeout of -1, which is
	 * what is performed in epoll tevent backend.
	 */
	tevent_trace_point_callback(kqueue_ev->ev, TEVENT_TRACE_BEFORE_WAIT);
	nkevents = kevent(kqueue_ev->rdwrq->kq_fd,
			  NULL,				/* changelist */
			  0,				/* nchanges */
			  kqueue_ev->rdwrq->kq_array,	/* eventlist */
			  kqueue_ev->rdwrq->kq_sz,	/* nevents */
			  tsp);				/* timeout */
	saved_errno = errno;
	tevent_trace_point_callback(kqueue_ev->ev, TEVENT_TRACE_AFTER_WAIT);

	if (nkevents == -1) {
		nkevents = check_kevent_errors(kqueue_ev, tsp, saved_errno);
		if (nkevents == -1) {
			return nkevents;
		}
	}

	if ((nkevents == 0)  && tvalp) {
		/* we don't care about a possible delay here */
		tevent_common_loop_timer_delay(kqueue_ev->ev);
		return 0;
	}

	ret = process_kevents(kqueue_ev, nkevents);
	if (nkevents == kqueue_ev->rdwrq->kq_sz) {
		expand_queue(kqueue_ev, kqueue_ev->rdwrq);
	}

	return ret;
}

static int kqueue_event_context_init(struct tevent_context *ev)
{
	int ret;
	struct kq_context *kqueue_ev = NULL;

	/*
	 * We might be called during tevent_re_initialise()
	 * which means we need to free our old additional_data.
	 */
        TALLOC_FREE(ev->additional_data);

	kqueue_ev = talloc_zero(ev, struct kq_context);

	if (!kqueue_ev) return -1;
	kqueue_ev->ev = ev;

	ret = kqueue_init_ctx(kqueue_ev);
	if (ret != 0) {
		talloc_free(kqueue_ev);
		return ret;
	}

	ev->additional_data = kqueue_ev;
	return 0;
}

static void tevent_aio_waitcomplete(struct tevent_context *ev, struct aiocb *iocbp)
{
	int ret;
	struct timespec timeout = {30,0};

	tevent_debug(
		ev, TEVENT_DEBUG_WARNING,
		"tevent_aio_waitcomplete(): aio op currently in progress for "
		"fd [%d], waiting for completion\n", iocbp->aio_fildes
	);

	ret = aio_waitcomplete(&iocbp, &timeout);
	if (ret == -1) {
		tevent_debug(
			ev, TEVENT_DEBUG_FATAL,
			"tevent_aio_waitcomplete(): aio_waitcomplete() failed: %s\n",
			strerror(errno)
		);
		abort();
	} else if (ret == EINPROGRESS) {
		tevent_debug(
			ev, TEVENT_DEBUG_FATAL,
			"tevent_aio_waitcomplete(): aio_waitcomplete() "
			"failed to complete after 30 seconds\n"
		);
		abort();
	}
}

void tevent_aio_cancel(struct tevent_aiocb *taiocb)
{
	int ret;
	struct aiocb *iocbp = taiocb->iocbp;

	tevent_debug(
		taiocb->ev, TEVENT_DEBUG_WARNING,
		"tevent_aio_cancel(): "
		"taio: %p, iocbp: %p\n",
		taiocb, iocbp
	);

	if (iocbp == NULL) {
		abort();
	}

	ret = aio_cancel(iocbp->aio_fildes, iocbp);
	if (ret == -1) {
		tevent_debug(
			taiocb->ev, TEVENT_DEBUG_WARNING,
			"tevent_aio_cancel(): "
			"aio_cancel() returned -1: %s\n",
			 strerror(errno)
		);
		abort();

	/* return 0x2 = AIO_NOTCANCELED */
	} else if (ret == 2) {
		ret = aio_error(iocbp);
		if (ret == -1) {
			tevent_debug(
				taiocb->ev, TEVENT_DEBUG_WARNING,
				"tevent_aio_cancel(): "
				"aio_error() failed: %s\n",
				 strerror(errno)
			);
			abort();
		}
		else if (ret == EINPROGRESS) {
			tevent_aio_waitcomplete(taiocb->ev, iocbp);
		}
	}
	TALLOC_FREE(taiocb->iocbp);
}

int _tevent_add_aio_read(struct tevent_aiocb *taiocb, const char *location)
{
	int err;

	taiocb->location = location;
	err = aio_read(taiocb->iocbp);
	if (err) {
		TALLOC_FREE(taiocb->iocbp);
	}

	return err;
}

int _tevent_add_aio_write(struct tevent_aiocb *taiocb, const char *location)
{
	int err;

	taiocb->location = location;
	err = aio_write(taiocb->iocbp);
	if (err) {
		TALLOC_FREE(taiocb->iocbp);
	}

	return err;
}

int _tevent_add_aio_fsync(struct tevent_aiocb *taiocb, const char *location)
{
	int err;

	taiocb->location = location;
	err = aio_fsync(O_SYNC, taiocb->iocbp);
	if (err) {
		TALLOC_FREE(taiocb->iocbp);
	}

	return err;
}

static bool aio_req_cancel(struct tevent_req *req)
{
	struct tevent_aiocb *taiocb = tevent_req_data(req, struct tevent_aiocb);
	tevent_aio_cancel(taiocb);
	return true;
}

static int aio_destructor(struct tevent_aiocb *taio)
{
	if (taio->iocbp != NULL) {
		tevent_aio_cancel(taio);
	}
	return 0;
}

struct aiocb *tevent_ctx_get_iocb(struct tevent_aiocb *taiocb)
{
	struct kq_context *kqueue_ev = EVTOKQ(taiocb->ev);
	struct aiocb *iocbp = NULL;
	if (kqueue_ev->aio_pool == NULL) {
		kqueue_ev->aio_pool = talloc_pool(taiocb->ev, 128 * sizeof(struct aiocb));
		if (kqueue_ev->aio_pool == NULL) {
			abort();
		}
	}

	tevent_req_set_cancel_fn(taiocb->req, aio_req_cancel);
	iocbp = talloc_zero(kqueue_ev->aio_pool, struct aiocb);
	if (iocbp == NULL) {
		abort();
	}
	iocbp->aio_sigevent.sigev_notify_kqueue = kqueue_ev->rdwrq->kq_fd;
	iocbp->aio_sigevent.sigev_value.sival_ptr = taiocb;
	iocbp->aio_sigevent.sigev_notify = SIGEV_KEVENT;
	iocbp->aio_sigevent.sigev_notify_kevent_flags = EV_ONESHOT;
	taiocb->iocbp = iocbp;
	talloc_set_destructor(taiocb, aio_destructor);
	return iocbp;
}

static struct tevent_fd *kqueue_event_add_fd(struct tevent_context *ev,
					     TALLOC_CTX *mem_ctx,
					     int fd, uint16_t flags,
					     tevent_fd_handler_t handler,
					     void *private_data,
					     const char *handler_name,
					     const char *location)
{
	struct kq_context *kqueue_ev = EVTOKQ(ev);
	struct tevent_fd *fde = NULL;

	fde = tevent_common_add_fd(ev, mem_ctx, fd, flags,
				   handler, private_data,
				   handler_name, location);
	if (!fde) {
		tevent_debug(ev, TEVENT_DEBUG_FATAL,
			"tevent_common_add_fd() failed\n");
		return NULL;
	}
	talloc_set_destructor(fde, kqueue_event_fd_destructor);
	kqueue_add_event(kqueue_ev, fde);
	return fde;
}

static void kqueue_event_set_fd_flags(struct tevent_fd *fde, uint16_t flags)
{
	struct kq_context *kqueue_ev = NULL;
	uint16_t to_delete = 0;

	if (fde->flags == flags) return;

	to_delete = fde->flags & ~flags;
	kqueue_ev = EVTOKQ(fde->event_ctx);
	fde->flags = flags;
	kqueue_delete_event(kqueue_ev, fde, to_delete);
	kqueue_add_event(kqueue_ev, fde);
}

/*
  do a single event loop using the events defined in ev
*/
static int kqueue_event_loop_once(struct tevent_context *ev,
				  const char *location)
{
	struct kq_context *kqueue_ev = EVTOKQ(ev);
	int ret;
	bool panic_triggered = false;
	struct timeval tval;

	if (ev->signal_events &&
	    tevent_common_check_signal(ev)) {
		return 0;
	}

	if (ev->threaded_contexts != NULL) {
		tevent_common_threaded_activate_immediate(ev);
	}

	if (ev->immediate_events &&
	    tevent_common_loop_immediate(ev)) {
		return 0;
	}

	kqueue_ev->panic_state = &panic_triggered;
	kqueue_ev->panic_force_replay = true;

	tval = tevent_common_loop_timer_delay(ev);
	if (tevent_timeval_is_zero(&tval)) {
		return 0;
	}

	ret = kqueue_event_loop(kqueue_ev, &tval);

	kqueue_ev->panic_force_replay = false;
	kqueue_ev->panic_state = NULL;
	return ret;
}

static const struct tevent_ops kqueue_event_ops = {
	.context_init		= kqueue_event_context_init,
	.add_fd			= kqueue_event_add_fd,
	.set_fd_close_fn	= tevent_common_fd_set_close_fn,
	.get_fd_flags		= tevent_common_fd_get_flags,
	.set_fd_flags		= kqueue_event_set_fd_flags,
	.add_timer		= tevent_common_add_timer_v2,
	.schedule_immediate	= tevent_common_schedule_immediate,
	.add_signal		= tevent_common_add_signal,
	.loop_once		= kqueue_event_loop_once,
	.loop_wait		= tevent_common_loop_wait,
};

_PRIVATE_ bool tevent_kqueue_init(void)
{
	return tevent_register_backend("kqueue", &kqueue_event_ops);
}

/*
 * Copyright (c) 2000-2013 Apple Inc. All rights reserved.
 *
 * @APPLE_LICENSE_HEADER_START@
 *
 * This file contains Original Code and/or Modifications of Original Code
 * as defined in and that are subject to the Apple Public Source License
 * Version 2.0 (the 'License'). You may not use this file except in
 * compliance with the License. Please obtain a copy of the License at
 * http://www.opensource.apple.com/apsl/ and read it before using this
 * file.
 *
 * The Original Code and all software distributed under the License are
 * distributed on an 'AS IS' basis, WITHOUT WARRANTY OF ANY KIND, EITHER
 * EXPRESS OR IMPLIED, AND APPLE HEREBY DISCLAIMS ALL SUCH WARRANTIES,
 * INCLUDING WITHOUT LIMITATION, ANY WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE, QUIET ENJOYMENT OR NON-INFRINGEMENT.
 * Please see the License for the specific language governing rights and
 * limitations under the License.
 *
 * @APPLE_LICENSE_HEADER_END@
 */
/*
 * Copyright 1996 1995 by Open Software Foundation, Inc. 1997 1996 1995 1994 1993 1992 1991
 *              All Rights Reserved
 *
 * Permission to use, copy, modify, and distribute this software and
 * its documentation for any purpose and without fee is hereby granted,
 * provided that the above copyright notice appears in all copies and
 * that both the copyright notice and this permission notice appear in
 * supporting documentation.
 *
 * OSF DISCLAIMS ALL WARRANTIES WITH REGARD TO THIS SOFTWARE
 * INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 * FOR A PARTICULAR PURPOSE.
 *
 * IN NO EVENT SHALL OSF BE LIABLE FOR ANY SPECIAL, INDIRECT, OR
 * CONSEQUENTIAL DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM
 * LOSS OF USE, DATA OR PROFITS, WHETHER IN ACTION OF CONTRACT,
 * NEGLIGENCE, OR OTHER TORTIOUS ACTION, ARISING OUT OF OR IN CONNECTION
 * WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 *
 */
/*
 * MkLinux
 */

/*
 * POSIX Pthread Library
 */

#include "internal.h"

#include <stdio.h>	/* For printf(). */
#include <stdlib.h>
#include <errno.h>	/* For __mach_errno_addr() prototype. */
#include <signal.h>
#include <sys/time.h>
#include <sys/resource.h>
#include <sys/sysctl.h>
#include <sys/queue.h>
#include <sys/ulock.h>
#include <machine/vmparam.h>
#include <mach/vm_statistics.h>

#ifndef BUILDING_VARIANT /* [ */

OS_ALWAYS_INLINE
static inline int
_pthread_update_cancel_state(pthread_t thread, int mask, int state)
{
	uint16_t oldstate, newstate;
	os_atomic_rmw_loop(&thread->cancel_state, oldstate, newstate, relaxed, {
		newstate = oldstate;
		newstate &= ~mask;
		newstate |= state;
	});
	return oldstate;
}

/* When a thread exits set the cancellation state to DISABLE and DEFERRED */
void
_pthread_setcancelstate_exit(pthread_t thread, void *value_ptr)
{
	_pthread_update_cancel_state(thread,
			_PTHREAD_CANCEL_STATE_MASK | _PTHREAD_CANCEL_TYPE_MASK,
			PTHREAD_CANCEL_DISABLE | PTHREAD_CANCEL_DEFERRED |
			_PTHREAD_CANCEL_EXITING);
}

/*
 * Cancel a thread
 */
PTHREAD_NOEXPORT_VARIANT
int
pthread_cancel(pthread_t thread)
{
	if (!_pthread_is_valid(thread, NULL)) {
		return(ESRCH);
	}

	/* if the thread is a workqueue thread, then return error */
	if (thread->wqthread != 0) {
		return(ENOTSUP);
	}
	int state = os_atomic_or(&thread->cancel_state, _PTHREAD_CANCEL_PENDING, relaxed);
	if (state & PTHREAD_CANCEL_ENABLE) {
		mach_port_t kport = _pthread_tsd_slot(thread, MACH_THREAD_SELF);
		if (kport) __pthread_markcancel(kport);
	}
	return (0);
}

/*
 * Query/update the cancelability 'state' of a thread
 */
PTHREAD_NOEXPORT_VARIANT
int
pthread_setcancelstate(int state, int *oldstateptr)
{
	pthread_t self = pthread_self();

	_pthread_validate_signature(self);

	switch (state) {
	case PTHREAD_CANCEL_ENABLE:
		__pthread_canceled(1);
		break;
	case PTHREAD_CANCEL_DISABLE:
		__pthread_canceled(2);
		break;
	default:
		return EINVAL;
	}

	int oldstate = _pthread_update_cancel_state(self, _PTHREAD_CANCEL_STATE_MASK, state);
	if (oldstateptr) {
		*oldstateptr = oldstate & _PTHREAD_CANCEL_STATE_MASK;
	}
	return 0;
}

/*
 * Query/update the cancelability 'type' of a thread
 */
PTHREAD_NOEXPORT_VARIANT
int
pthread_setcanceltype(int type, int *oldtype)
{
	pthread_t self = pthread_self();

	_pthread_validate_signature(self);

	if ((type != PTHREAD_CANCEL_DEFERRED) &&
	    (type != PTHREAD_CANCEL_ASYNCHRONOUS))
		return EINVAL;
	int oldstate = _pthread_update_cancel_state(self, _PTHREAD_CANCEL_TYPE_MASK, type);
	if (oldtype) {
		*oldtype = oldstate & _PTHREAD_CANCEL_TYPE_MASK;
	}
	return (0);
}


OS_ALWAYS_INLINE
static inline bool
_pthread_is_canceled(pthread_t thread)
{
	const int flags = (PTHREAD_CANCEL_ENABLE|_PTHREAD_CANCEL_PENDING);
	int state = os_atomic_load(&thread->cancel_state, seq_cst);
	return (state & flags) == flags;
}

OS_ALWAYS_INLINE
static inline void *
_pthread_get_exit_value(pthread_t thread)
{
	if (os_unlikely(_pthread_is_canceled(thread))) {
		return PTHREAD_CANCELED;
	}
	return thread->tl_exit_value;
}

void
pthread_testcancel(void)
{
	pthread_t self = pthread_self();
	if (os_unlikely(_pthread_is_canceled(self))) {
		_pthread_validate_signature(self);
		// 4597450: begin
		self->canceled = true;
		// 4597450: end
		pthread_exit(PTHREAD_CANCELED);
	}
}

void
_pthread_markcancel_if_canceled(pthread_t thread, mach_port_t kport)
{
	if (os_unlikely(_pthread_is_canceled(thread))) {
		__pthread_markcancel(kport);
	}
}

void
_pthread_exit_if_canceled(int error)
{
	if ((error & 0xff) == EINTR && __pthread_canceled(0) == 0) {
		pthread_t self = pthread_self();

		_pthread_validate_signature(self);
		self->cancel_error = error;
		self->canceled = true;
		pthread_exit(PTHREAD_CANCELED);
	}
}

int
pthread_sigmask(int how, const sigset_t * set, sigset_t * oset)
{
	int err = 0;

	if (__pthread_sigmask(how, set, oset) == -1) {
		err = errno;
	}
	return(err);
}

// called with _pthread_list_lock held
semaphore_t
_pthread_joiner_prepost_wake(pthread_t thread)
{
	pthread_join_context_t ctx = thread->tl_join_ctx;
	semaphore_t sema = MACH_PORT_NULL;

	if (thread->tl_joinable) {
		sema = ctx->custom_stack_sema;
		thread->tl_joinable = false;
	} else {
		ctx->detached = true;
		thread->tl_join_ctx = NULL;
	}
	if (ctx->value_ptr) *ctx->value_ptr = _pthread_get_exit_value(thread);
	return sema;
}

static inline bool
_pthread_joiner_abort_wait(pthread_t thread, pthread_join_context_t ctx)
{
	bool aborted = false;

	_pthread_lock_lock(&_pthread_list_lock);
	if (!ctx->detached && thread->tl_exit_gate != MACH_PORT_DEAD) {
		/*
		 * _pthread_joiner_prepost_wake() didn't happen
		 * allow another thread to join
		 */
		PTHREAD_DEBUG_ASSERT(thread->tl_join_ctx == ctx);
		thread->tl_join_ctx = NULL;
		thread->tl_exit_gate = MACH_PORT_NULL;
		aborted = true;
	}
	_pthread_lock_unlock(&_pthread_list_lock);
	return aborted;
}

static int
_pthread_joiner_wait(pthread_t thread, pthread_join_context_t ctx,
		pthread_conformance_t conforming)
{
	uint32_t *exit_gate = &thread->tl_exit_gate;
	int ulock_op = UL_UNFAIR_LOCK | ULF_NO_ERRNO;

	if (conforming == PTHREAD_CONFORM_UNIX03_CANCELABLE) {
		ulock_op |= ULF_WAIT_CANCEL_POINT;
	}

	for (;;) {
		uint32_t cur = os_atomic_load(exit_gate, acquire);
		if (cur == MACH_PORT_DEAD) {
			break;
		}
		if (os_unlikely(cur != ctx->kport)) {
			PTHREAD_CLIENT_CRASH(cur, "pthread_join() state corruption");
		}
		int ret = __ulock_wait(ulock_op, exit_gate, ctx->kport, 0);
		switch (-ret) {
		case 0:
		case EFAULT:
			break;
		case EINTR:
			/*
			 * POSIX says:
			 *
			 *   As specified, either the pthread_join() call is canceled, or it
			 *   succeeds, but not both. The difference is obvious to the
			 *   application, since either a cancellation handler is run or
			 *   pthread_join() returns.
			 *
			 * When __ulock_wait() returns EINTR, we check if we have been
			 * canceled, and if we have, we try to abort the wait.
			 *
			 * If we can't, it means the other thread finished the join while we
			 * were being canceled and commited the waiter to return from
			 * pthread_join(). Returning from the join then takes precedence
			 * over the cancelation which will be acted upon at the next
			 * cancelation point.
			 */
			if (os_unlikely(conforming == PTHREAD_CONFORM_UNIX03_CANCELABLE &&
					_pthread_is_canceled(ctx->waiter))) {
				if (_pthread_joiner_abort_wait(thread, ctx)) {
					ctx->waiter->canceled = true;
					pthread_exit(PTHREAD_CANCELED);
				}
			}
			break;
		}
	}

	bool cleanup = false;

	_pthread_lock_lock(&_pthread_list_lock);
	// If pthread_detach() was called, we can't safely dereference the thread,
	// else, decide who gets to deallocate the thread (see _pthread_terminate).
	if (!ctx->detached) {
		PTHREAD_DEBUG_ASSERT(thread->tl_join_ctx == ctx);
		thread->tl_join_ctx = NULL;
		cleanup = thread->tl_joiner_cleans_up;
	}
	_pthread_lock_unlock(&_pthread_list_lock);

	if (cleanup) {
		_pthread_deallocate(thread, false);
	}
	return 0;
}

OS_NOINLINE
int
_pthread_join(pthread_t thread, void **value_ptr, pthread_conformance_t conforming)
{
	pthread_t self = pthread_self();
	pthread_join_context_s ctx = {
		.waiter = self,
		.value_ptr = value_ptr,
		.kport = MACH_PORT_NULL,
		.custom_stack_sema = MACH_PORT_NULL,
	};
	int res = 0;
	kern_return_t kr;

	if (!_pthread_validate_thread_and_list_lock(thread)) {
		return ESRCH;
	}

	_pthread_validate_signature(self);

	if (!thread->tl_joinable || (thread->tl_join_ctx != NULL)) {
		res = EINVAL;
	} else if (thread == self ||
			(self->tl_join_ctx && self->tl_join_ctx->waiter == thread)) {
		res = EDEADLK;
	} else if (thread->tl_exit_gate == MACH_PORT_DEAD) {
		TAILQ_REMOVE(&__pthread_head, thread, tl_plist);
		PTHREAD_DEBUG_ASSERT(thread->tl_joiner_cleans_up);
		thread->tl_joinable = false;
		if (value_ptr) *value_ptr = _pthread_get_exit_value(thread);
	} else {
		ctx.kport = _pthread_tsd_slot(thread, MACH_THREAD_SELF);
		thread->tl_exit_gate = ctx.kport;
		thread->tl_join_ctx = &ctx;
		if (thread->tl_has_custom_stack) {
			ctx.custom_stack_sema = (semaphore_t)os_get_cached_semaphore();
		}
	}
	_pthread_lock_unlock(&_pthread_list_lock);

	if (res == 0) {
		if (ctx.kport == MACH_PORT_NULL) {
			_pthread_deallocate(thread, false);
		} else {
			res = _pthread_joiner_wait(thread, &ctx, conforming);
		}
	}
	if (res == 0 && ctx.custom_stack_sema && !ctx.detached) {
		// threads with a custom stack need to make sure _pthread_terminate
		// returned before the joiner is unblocked, the joiner may quickly
		// deallocate the stack with rather dire consequences.
		//
		// When we reach this point we know the pthread_join has to succeed
		// so this can't be a cancelation point.
		do {
			kr = __semwait_signal_nocancel(ctx.custom_stack_sema, 0, 0, 0, 0, 0);
		} while (kr != KERN_SUCCESS);
	}
	if (ctx.custom_stack_sema) {
		os_put_cached_semaphore(ctx.custom_stack_sema);
	}
	return res;
}

#endif /* !BUILDING_VARIANT ] */

static inline pthread_conformance_t
_pthread_conformance(void)
{
#ifdef VARIANT_CANCELABLE
	return PTHREAD_CONFORM_UNIX03_CANCELABLE;
#else /* !VARIANT_CANCELABLE */
	return PTHREAD_CONFORM_UNIX03_NOCANCEL;
#endif
}

static inline void
_pthread_testcancel_if_cancelable_variant(void)
{
#ifdef VARIANT_CANCELABLE
	pthread_testcancel();
#endif
}

int
pthread_join(pthread_t thread, void **value_ptr)
{
	_pthread_testcancel_if_cancelable_variant();
	return _pthread_join(thread, value_ptr, _pthread_conformance());
}

int
pthread_cond_wait(pthread_cond_t *cond, pthread_mutex_t *mutex)
{
	return _pthread_cond_wait(cond, mutex, NULL, 0, _pthread_conformance());
}

int
pthread_cond_timedwait(pthread_cond_t *cond, pthread_mutex_t *mutex,
		const struct timespec *abstime)
{
	return _pthread_cond_wait(cond, mutex, abstime, 0, _pthread_conformance());
}

int
sigwait(const sigset_t * set, int * sig)
{
	int err = 0;

	_pthread_testcancel_if_cancelable_variant();

	if (__sigwait(set, sig) == -1) {
		err = errno;

		_pthread_testcancel_if_cancelable_variant();

		/*
		 * EINTR that isn't a result of pthread_cancel()
		 * is translated to 0.
		 */
		if (err == EINTR) {
			err = 0;
		}
	}
	return(err);
}


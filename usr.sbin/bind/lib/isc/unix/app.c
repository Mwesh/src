/*
 * Copyright (C) Internet Systems Consortium, Inc. ("ISC")
 *
 * Permission to use, copy, modify, and/or distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND ISC DISCLAIMS ALL WARRANTIES WITH
 * REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY
 * AND FITNESS.  IN NO EVENT SHALL ISC BE LIABLE FOR ANY SPECIAL, DIRECT,
 * INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM
 * LOSS OF USE, DATA OR PROFITS, WHETHER IN AN ACTION OF CONTRACT, NEGLIGENCE
 * OR OTHER TORTIOUS ACTION, ARISING OUT OF OR IN CONNECTION WITH THE USE OR
 * PERFORMANCE OF THIS SOFTWARE.
 */

/*! \file */



#include <sys/param.h>	/* Openserver 5.0.6A and FD_SETSIZE */
#include <sys/types.h>

#include <stddef.h>
#include <stdlib.h>
#include <errno.h>
#include <unistd.h>
#include <signal.h>
#include <sys/time.h>

#include <isc/app.h>
#include <isc/boolean.h>


#include <isc/msgs.h>
#include <isc/event.h>

#include <isc/strerror.h>
#include <string.h>
#include <isc/task.h>
#include <isc/time.h>
#include <isc/util.h>

/*%
 * For BIND9 internal applications built with threads, we use a single app
 * context and let multiple worker, I/O, timer threads do actual jobs.
 * For other cases (including BIND9 built without threads) an app context acts
 * as an event loop dispatching various events.
 */
#include "../timer_p.h"
#include "../task_p.h"
#include "socket_p.h"

/*%
 * The following are intended for internal use (indicated by "isc__"
 * prefix) but are not declared as static, allowing direct access from
 * unit tests etc.
 */
isc_result_t isc__app_start(void);
isc_result_t isc__app_ctxstart(isc_appctx_t *ctx);
isc_result_t isc__app_onrun(isc_task_t *task,
			    isc_taskaction_t action, void *arg);
isc_result_t isc__app_ctxrun(isc_appctx_t *ctx);
isc_result_t isc__app_run(void);
isc_result_t isc__app_ctxshutdown(isc_appctx_t *ctx);
isc_result_t isc__app_shutdown(void);
isc_result_t isc__app_reload(void);
isc_result_t isc__app_ctxsuspend(isc_appctx_t *ctx);
void isc__app_ctxfinish(isc_appctx_t *ctx);
void isc__app_finish(void);
void isc__app_block(void);
void isc__app_unblock(void);
isc_result_t isc__appctx_create(isc_appctx_t **ctxp);
void isc__appctx_destroy(isc_appctx_t **ctxp);
void isc__appctx_settaskmgr(isc_appctx_t *ctx, isc_taskmgr_t *taskmgr);
void isc__appctx_setsocketmgr(isc_appctx_t *ctx, isc_socketmgr_t *socketmgr);
void isc__appctx_settimermgr(isc_appctx_t *ctx, isc_timermgr_t *timermgr);
isc_result_t isc__app_ctxonrun(isc_appctx_t *ctx,
			       isc_task_t *task, isc_taskaction_t action,
			       void *arg);

/*
 * The application context of this module.  This implementation actually
 * doesn't use it. (This may change in the future).
 */
#define APPCTX_MAGIC		ISC_MAGIC('A', 'p', 'c', 'x')
#define VALID_APPCTX(c)		ISC_MAGIC_VALID(c, APPCTX_MAGIC)

typedef struct isc__appctx {
	isc_appctx_t		common;
	isc_eventlist_t		on_run;
	isc_boolean_t		shutdown_requested;
	isc_boolean_t		running;

	/*!
	 * We assume that 'want_shutdown' can be read and written atomically.
	 */
	isc_boolean_t		want_shutdown;
	/*
	 * We assume that 'want_reload' can be read and written atomically.
	 */
	isc_boolean_t		want_reload;

	isc_boolean_t		blocked;

	isc_taskmgr_t		*taskmgr;
	isc_socketmgr_t		*socketmgr;
	isc_timermgr_t		*timermgr;
} isc__appctx_t;

static isc__appctx_t isc_g_appctx;

static struct {
	isc_appmethods_t methods;

	/*%
	 * The following are defined just for avoiding unused static functions.
	 */
	void *run, *shutdown, *start, *reload, *finish, *block, *unblock;
} appmethods = {
	{
		isc__appctx_destroy,
		isc__app_ctxstart,
		isc__app_ctxrun,
		isc__app_ctxsuspend,
		isc__app_ctxshutdown,
		isc__app_ctxfinish,
		isc__appctx_settaskmgr,
		isc__appctx_setsocketmgr,
		isc__appctx_settimermgr,
		isc__app_ctxonrun
	},
	(void *)isc__app_run,
	(void *)isc__app_shutdown,
	(void *)isc__app_start,
	(void *)isc__app_reload,
	(void *)isc__app_finish,
	(void *)isc__app_block,
	(void *)isc__app_unblock
};

static void
reload_action(int arg) {
	UNUSED(arg);
	isc_g_appctx.want_reload = ISC_TRUE;
}

static isc_result_t
handle_signal(int sig, void (*handler)(int)) {
	struct sigaction sa;
	char strbuf[ISC_STRERRORSIZE];

	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = handler;

	if (sigfillset(&sa.sa_mask) != 0 ||
	    sigaction(sig, &sa, NULL) < 0) {
		isc__strerror(errno, strbuf, sizeof(strbuf));
		UNEXPECTED_ERROR(__FILE__, __LINE__,
				 "handle_signal() %d setup: %s", sig, strbuf);
		return (ISC_R_UNEXPECTED);
	}

	return (ISC_R_SUCCESS);
}

isc_result_t
isc__app_ctxstart(isc_appctx_t *ctx0) {
	isc__appctx_t *ctx = (isc__appctx_t *)ctx0;
	isc_result_t result;
	int presult;
	sigset_t sset;
	char strbuf[ISC_STRERRORSIZE];

	REQUIRE(VALID_APPCTX(ctx));

	/*
	 * Start an ISC library application.
	 */

	ISC_LIST_INIT(ctx->on_run);

	ctx->shutdown_requested = ISC_FALSE;
	ctx->running = ISC_FALSE;
	ctx->want_shutdown = ISC_FALSE;
	ctx->want_reload = ISC_FALSE;
	ctx->blocked = ISC_FALSE;

	/*
	 * Always ignore SIGPIPE.
	 */
	result = handle_signal(SIGPIPE, SIG_IGN);
	if (result != ISC_R_SUCCESS)
		goto cleanup;

	/*
	 * On Solaris 2, delivery of a signal whose action is SIG_IGN
	 * will not cause sigwait() to return. We may have inherited
	 * unexpected actions for SIGHUP, SIGINT, and SIGTERM from our parent
	 * process (e.g, Solaris cron).  Set an action of SIG_DFL to make
	 * sure sigwait() works as expected.  Only do this for SIGTERM and
	 * SIGINT if we don't have sigwait(), since a different handler is
	 * installed above.
	 */
	result = handle_signal(SIGHUP, SIG_DFL);
	if (result != ISC_R_SUCCESS)
		goto cleanup;

	result = handle_signal(SIGTERM, SIG_DFL);
	if (result != ISC_R_SUCCESS)
		goto cleanup;
	result = handle_signal(SIGINT, SIG_DFL);
	if (result != ISC_R_SUCCESS)
		goto cleanup;

	/*
	 * Unblock SIGHUP, SIGINT, SIGTERM.
	 *
	 * If we're not using threads, we need to make sure that SIGHUP,
	 * SIGINT and SIGTERM are not inherited as blocked from the parent
	 * process.
	 */
	if (sigemptyset(&sset) != 0 ||
	    sigaddset(&sset, SIGHUP) != 0 ||
	    sigaddset(&sset, SIGINT) != 0 ||
	    sigaddset(&sset, SIGTERM) != 0) {
		isc__strerror(errno, strbuf, sizeof(strbuf));
		UNEXPECTED_ERROR(__FILE__, __LINE__,
				 "isc_app_start() sigsetops: %s", strbuf);
		result = ISC_R_UNEXPECTED;
		goto cleanup;
	}
	presult = sigprocmask(SIG_UNBLOCK, &sset, NULL);
	if (presult != 0) {
		isc__strerror(errno, strbuf, sizeof(strbuf));
		UNEXPECTED_ERROR(__FILE__, __LINE__,
				 "isc_app_start() sigprocmask: %s", strbuf);
		result = ISC_R_UNEXPECTED;
		goto cleanup;
	}

	return (ISC_R_SUCCESS);

 cleanup:
	return (result);
}

isc_result_t
isc__app_start(void) {
	isc_g_appctx.common.impmagic = APPCTX_MAGIC;
	isc_g_appctx.common.magic = ISCAPI_APPCTX_MAGIC;
	isc_g_appctx.common.methods = &appmethods.methods;
	/* The remaining members will be initialized in ctxstart() */

	return (isc__app_ctxstart((isc_appctx_t *)&isc_g_appctx));
}

isc_result_t
isc__app_onrun(isc_task_t *task, isc_taskaction_t action,
	      void *arg)
{
	return (isc__app_ctxonrun((isc_appctx_t *)&isc_g_appctx,
				  task, action, arg));
}

isc_result_t
isc__app_ctxonrun(isc_appctx_t *ctx0, isc_task_t *task,
		  isc_taskaction_t action, void *arg)
{
	isc__appctx_t *ctx = (isc__appctx_t *)ctx0;
	isc_event_t *event;
	isc_task_t *cloned_task = NULL;
	isc_result_t result;

	if (ctx->running) {
		result = ISC_R_ALREADYRUNNING;
		goto unlock;
	}

	/*
	 * Note that we store the task to which we're going to send the event
	 * in the event's "sender" field.
	 */
	isc_task_attach(task, &cloned_task);
	event = isc_event_allocate(cloned_task, ISC_APPEVENT_SHUTDOWN,
				   action, arg, sizeof(*event));
	if (event == NULL) {
		isc_task_detach(&cloned_task);
		result = ISC_R_NOMEMORY;
		goto unlock;
	}

	ISC_LIST_APPEND(ctx->on_run, event, ev_link);

	result = ISC_R_SUCCESS;

 unlock:
	return (result);
}

/*!
 * Event loop for nonthreaded programs.
 */
static isc_result_t
evloop(isc__appctx_t *ctx) {
	isc_result_t result;

	while (!ctx->want_shutdown) {
		int n;
		isc_time_t when, now;
		struct timeval tv, *tvp;
		isc_socketwait_t *swait;
		isc_boolean_t readytasks;
		isc_boolean_t call_timer_dispatch = ISC_FALSE;

		/*
		 * Check the reload (or suspend) case first for exiting the
		 * loop as fast as possible in case:
		 *   - the direct call to isc__taskmgr_dispatch() in
		 *     isc__app_ctxrun() completes all the tasks so far,
		 *   - there is thus currently no active task, and
		 *   - there is a timer event
		 */
		if (ctx->want_reload) {
			ctx->want_reload = ISC_FALSE;
			return (ISC_R_RELOAD);
		}

		readytasks = isc__taskmgr_ready(ctx->taskmgr);
		if (readytasks) {
			tv.tv_sec = 0;
			tv.tv_usec = 0;
			tvp = &tv;
			call_timer_dispatch = ISC_TRUE;
		} else {
			result = isc__timermgr_nextevent(ctx->timermgr, &when);
			if (result != ISC_R_SUCCESS)
				tvp = NULL;
			else {
				uint64_t us;

				TIME_NOW(&now);
				us = isc_time_microdiff(&when, &now);
				if (us == 0)
					call_timer_dispatch = ISC_TRUE;
				tv.tv_sec = us / 1000000;
				tv.tv_usec = us % 1000000;
				tvp = &tv;
			}
		}

		swait = NULL;
		n = isc__socketmgr_waitevents(ctx->socketmgr, tvp, &swait);

		if (n == 0 || call_timer_dispatch) {
			/*
			 * We call isc__timermgr_dispatch() only when
			 * necessary, in order to reduce overhead.  If the
			 * select() call indicates a timeout, we need the
			 * dispatch.  Even if not, if we set the 0-timeout
			 * for the select() call, we need to check the timer
			 * events.  In the 'readytasks' case, there may be no
			 * timeout event actually, but there is no other way
			 * to reduce the overhead.
			 * Note that we do not have to worry about the case
			 * where a new timer is inserted during the select()
			 * call, since this loop only runs in the non-thread
			 * mode.
			 */
			isc__timermgr_dispatch(ctx->timermgr);
		}
		if (n > 0)
			(void)isc__socketmgr_dispatch(ctx->socketmgr, swait);
		(void)isc__taskmgr_dispatch(ctx->taskmgr);
	}
	return (ISC_R_SUCCESS);
}

isc_result_t
isc__app_ctxrun(isc_appctx_t *ctx0) {
	isc__appctx_t *ctx = (isc__appctx_t *)ctx0;
	int result;
	isc_event_t *event, *next_event;
	isc_task_t *task;

	REQUIRE(VALID_APPCTX(ctx));

	if (!ctx->running) {
		ctx->running = ISC_TRUE;

		/*
		 * Post any on-run events (in FIFO order).
		 */
		for (event = ISC_LIST_HEAD(ctx->on_run);
		     event != NULL;
		     event = next_event) {
			next_event = ISC_LIST_NEXT(event, ev_link);
			ISC_LIST_UNLINK(ctx->on_run, event, ev_link);
			task = event->ev_sender;
			event->ev_sender = NULL;
			isc_task_sendanddetach(&task, &event);
		}

	}

	if (ctx == &isc_g_appctx) {
		result = handle_signal(SIGHUP, reload_action);
		if (result != ISC_R_SUCCESS)
			return (ISC_R_SUCCESS);
	}

	(void) isc__taskmgr_dispatch(ctx->taskmgr);
	result = evloop(ctx);
	return (result);
}

isc_result_t
isc__app_run(void) {
	return (isc__app_ctxrun((isc_appctx_t *)&isc_g_appctx));
}

isc_result_t
isc__app_ctxshutdown(isc_appctx_t *ctx0) {
	isc__appctx_t *ctx = (isc__appctx_t *)ctx0;
	isc_boolean_t want_kill = ISC_TRUE;

	REQUIRE(VALID_APPCTX(ctx));

	REQUIRE(ctx->running);

	if (ctx->shutdown_requested)
		want_kill = ISC_FALSE;
	else
		ctx->shutdown_requested = ISC_TRUE;

	if (want_kill) {
		if (ctx != &isc_g_appctx)
			/* BIND9 internal, but using multiple contexts */
			ctx->want_shutdown = ISC_TRUE;
		else {
			ctx->want_shutdown = ISC_TRUE;
		}
	}

	return (ISC_R_SUCCESS);
}

isc_result_t
isc__app_shutdown(void) {
	return (isc__app_ctxshutdown((isc_appctx_t *)&isc_g_appctx));
}

isc_result_t
isc__app_ctxsuspend(isc_appctx_t *ctx0) {
	isc__appctx_t *ctx = (isc__appctx_t *)ctx0;
	isc_boolean_t want_kill = ISC_TRUE;

	REQUIRE(VALID_APPCTX(ctx));

	REQUIRE(ctx->running);

	/*
	 * Don't send the reload signal if we're shutting down.
	 */
	if (ctx->shutdown_requested)
		want_kill = ISC_FALSE;

	if (want_kill) {
		if (ctx != &isc_g_appctx)
			/* BIND9 internal, but using multiple contexts */
			ctx->want_reload = ISC_TRUE;
		else {
			ctx->want_reload = ISC_TRUE;
		}
	}

	return (ISC_R_SUCCESS);
}

isc_result_t
isc__app_reload(void) {
	return (isc__app_ctxsuspend((isc_appctx_t *)&isc_g_appctx));
}

void
isc__app_ctxfinish(isc_appctx_t *ctx0) {
	isc__appctx_t *ctx = (isc__appctx_t *)ctx0;

	REQUIRE(VALID_APPCTX(ctx));
}

void
isc__app_finish(void) {
	isc__app_ctxfinish((isc_appctx_t *)&isc_g_appctx);
}

void
isc__app_block(void) {
	REQUIRE(isc_g_appctx.running);
	REQUIRE(!isc_g_appctx.blocked);

	isc_g_appctx.blocked = ISC_TRUE;
}

void
isc__app_unblock(void) {

	REQUIRE(isc_g_appctx.running);
	REQUIRE(isc_g_appctx.blocked);

	isc_g_appctx.blocked = ISC_FALSE;

}

isc_result_t
isc__appctx_create(isc_appctx_t **ctxp) {
	isc__appctx_t *ctx;

	REQUIRE(ctxp != NULL && *ctxp == NULL);

	ctx = malloc(sizeof(*ctx));
	if (ctx == NULL)
		return (ISC_R_NOMEMORY);

	ctx->common.impmagic = APPCTX_MAGIC;
	ctx->common.magic = ISCAPI_APPCTX_MAGIC;
	ctx->common.methods = &appmethods.methods;

	ctx->taskmgr = NULL;
	ctx->socketmgr = NULL;
	ctx->timermgr = NULL;

	*ctxp = (isc_appctx_t *)ctx;

	return (ISC_R_SUCCESS);
}

void
isc__appctx_destroy(isc_appctx_t **ctxp) {
	isc__appctx_t *ctx;

	REQUIRE(ctxp != NULL);
	ctx = (isc__appctx_t *)*ctxp;
	REQUIRE(VALID_APPCTX(ctx));

	free(ctx);

	*ctxp = NULL;
}

void
isc__appctx_settaskmgr(isc_appctx_t *ctx0, isc_taskmgr_t *taskmgr) {
	isc__appctx_t *ctx = (isc__appctx_t *)ctx0;

	REQUIRE(VALID_APPCTX(ctx));

	ctx->taskmgr = taskmgr;
}

void
isc__appctx_setsocketmgr(isc_appctx_t *ctx0, isc_socketmgr_t *socketmgr) {
	isc__appctx_t *ctx = (isc__appctx_t *)ctx0;

	REQUIRE(VALID_APPCTX(ctx));

	ctx->socketmgr = socketmgr;
}

void
isc__appctx_settimermgr(isc_appctx_t *ctx0, isc_timermgr_t *timermgr) {
	isc__appctx_t *ctx = (isc__appctx_t *)ctx0;

	REQUIRE(VALID_APPCTX(ctx));

	ctx->timermgr = timermgr;
}

isc_result_t
isc__app_register(void) {
	return (isc_app_register(isc__appctx_create));
}

#include "../app_api.c"

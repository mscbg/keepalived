/*
 * Soft:        Keepalived is a failover program for the LVS project
 *              <www.linuxvirtualserver.org>. It monitor & manipulate
 *              a loadbalanced server pool using multi-layer checks.
 *
 * Part:        Signals framework.
 *
 * Author:      Kevin Lindsay, <kevinl@netnation.com>
 *              Alexandre Cassen, <acassen@linux-vs.org>
 *
 *              This program is distributed in the hope that it will be useful,
 *              but WITHOUT ANY WARRANTY; without even the implied warranty of
 *              MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 *              See the GNU General Public License for more details.
 *
 *              This program is free software; you can redistribute it and/or
 *              modify it under the terms of the GNU General Public License
 *              as published by the Free Software Foundation; either version
 *              2 of the License, or (at your option) any later version.
 *
 * Copyright (C) 2001-2017 Alexandre Cassen, <acassen@gmail.com>
 */

#include "config.h"

#if defined HAVE_PIPE2 && !defined _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <signal.h>
#include <string.h>

#include <fcntl.h>
#include <unistd.h>

#include <sys/types.h>
#include <sys/wait.h>
#include <errno.h>
#ifndef _DEBUG_
#define NDEBUG
#endif
#include <assert.h>
#include <syslog.h>

#include "signals.h"
#include "utils.h"
#include "logger.h"

#ifdef _WITH_JSON_
#include "../keepalived/include/vrrp_json.h"
#endif

#ifdef _WITH_JSON_
  /* We need to include the realtime signals, but
   * unfortunately SIGRTMIN/SIGRTMAX are not constants.
   * I'm not clear if _NSIG is always defined, so play safe.
   * Although we are not meant to use __SIGRTMAX, we are
   * using it here as an upper bound, which is eather different. */
  #ifdef _NSIG
    #define SIG_MAX	_NSIG
  #elif defined __SIGRTMAX
    #define SIG_MAX __SIGRTMAX
  #else
    #define SIG_MAX 64
  #endif
#else
  /* The signals currently used are HUP, INT, TERM, USR1,
   * USR2 and CHLD. */
  #if SIGCHLD > SIGUSR2
    /* Architectures except alpha and sparc - see signal(7) */
    #define SIG_MAX SIGCHLD
  #else
    /* alpha and sparc */
    #define SIG_MAX SIGUSR2
  #endif
#endif

/* Local Vars */
static void (*signal_handler_func[SIG_MAX]) (void *, int sig);
static void *signal_v[SIG_MAX];

static int signal_pipe[2] = { -1, -1 };

/* Remember our initial signal disposition */
static sigset_t ign_sig;
static sigset_t dfl_sig;

/* Signal handlers set in parent */
static sigset_t parent_sig;

int
get_signum(const char *sigfunc)
{
	if (!strcmp(sigfunc, "STOP"))
		return SIGTERM;
	else if (!strcmp(sigfunc, "RELOAD"))
		return SIGHUP;
	else if (!strcmp(sigfunc, "DATA"))
		return SIGUSR1;
	else if (!strcmp(sigfunc, "STATS"))
		return SIGUSR2;
#ifdef _WITH_JSON_
	else if (!strcmp(sigfunc, "JSON"))
		return SIGJSON;
#endif

	/* Not found */
	return -1;
}

#ifdef _INCLUDE_UNUSED_CODE_
/* Local signal test */
int
signal_pending(void)
{
	fd_set readset;
	int rc;
	struct timeval timeout = {
		.tv_sec = 0,
		.tv_usec = 0
	};

	FD_ZERO(&readset);
	FD_SET(signal_pipe[0], &readset);

	rc = select(signal_pipe[0] + 1, &readset, NULL, NULL, &timeout);

	return rc > 0 ? 1 : 0;
}
#endif

/* Signal flag */
static void
signal_handler(int sig)
{
	if (write(signal_pipe[1], &sig, sizeof(int)) != sizeof(int)) {
		DBG("signal_pipe write error %s", strerror(errno));
		assert(0);

		log_message(LOG_INFO, "BUG - write to signal_pipe[1] error %s - please report", strerror(errno));
	}
}

/* Signal wrapper */
void *
signal_set(int signo, void (*func) (void *, int), void *v)
{
	int ret;
	sigset_t sset;
	struct sigaction sig;
	struct sigaction osig;

	if (signo < 1 || signo > SIG_MAX) {
		log_message(LOG_INFO, "Invalid signal number %d passed to signal_set(). Max signal is %d", signo, SIG_MAX);
		return NULL;
	}

	if (func == (void*)SIG_IGN || func == (void*)SIG_DFL) {
		sig.sa_handler = (void*)func;

		/* We are no longer handling the signal, so
		 * clear our handlers
		 */
		func = NULL;
		v = NULL;
	}
	else
		sig.sa_handler = signal_handler;
	sigemptyset(&sig.sa_mask);
	sig.sa_flags = 0;
	sig.sa_flags |= SA_RESTART;

	/* Block the signal we are about to configure, to avoid
	 * any race conditions while setting the handler and
	 * parameter */
	if (func != NULL) {
		sigemptyset(&sset);
		sigaddset(&sset, signo);
		sigprocmask(SIG_BLOCK, &sset, NULL);

		/* If we are the parent, remember what signals
		 * we set, so vrrp and checker children can clear them */
		sigaddset(&parent_sig, signo);
	}

	ret = sigaction(signo, &sig, &osig);

	signal_handler_func[signo-1] = func;
	signal_v[signo-1] = v;

	if (ret < 0)
		return (SIG_ERR);

	/* Release the signal */
	if (func != NULL)
		sigprocmask(SIG_UNBLOCK, &sset, NULL);

	return ((osig.sa_flags & SA_SIGINFO) ? (void*)osig.sa_sigaction : (void*)osig.sa_handler);
}

/* Signal Ignore */
void *
signal_ignore(int signo)
{
	return signal_set(signo, (void*)SIG_IGN, NULL);
}

static void
clear_signal_handler_addresses(void)
{
	int i;

	for (i = 0; i < SIG_MAX; i++)
		signal_handler_func[i] = NULL;
}

/* Handlers intialization */
static void
open_signal_pipe(void)
{
	int n;

#ifdef HAVE_PIPE2
	n = pipe2(signal_pipe, O_CLOEXEC | O_NONBLOCK);
#else
	n = pipe(signal_pipe);
#endif

	assert(!n);
	if (n)
		log_message(LOG_INFO, "BUG - pipe in signal_handler_init failed (%s), please report", strerror(errno));

#ifndef HAVE_PIPE2
	fcntl(signal_pipe[0], F_SETFL, O_NONBLOCK | fcntl(signal_pipe[0], F_GETFL));
	fcntl(signal_pipe[1], F_SETFL, O_NONBLOCK | fcntl(signal_pipe[1], F_GETFL));

	fcntl(signal_pipe[0], F_SETFD, FD_CLOEXEC | fcntl(signal_pipe[0], F_GETFD));
	fcntl(signal_pipe[1], F_SETFD, FD_CLOEXEC | fcntl(signal_pipe[1], F_GETFD));
#endif
}

void
signal_handler_init(void)
{
	sigset_t sset;
	int sig;
	struct sigaction act, oact;

	open_signal_pipe();

	clear_signal_handler_addresses();

	/* Ignore all signals set to default (except essential ones) */
	sigfillset(&sset);
	sigdelset(&sset, SIGILL);
	sigdelset(&sset, SIGFPE);
	sigdelset(&sset, SIGSEGV);
	sigdelset(&sset, SIGBUS);
	sigdelset(&sset, SIGKILL);
	sigdelset(&sset, SIGSTOP);

	act.sa_handler = SIG_IGN;
	sigemptyset(&act.sa_mask);
	act.sa_flags = 0;

	sigemptyset(&ign_sig);
	sigemptyset(&dfl_sig);
	sigemptyset(&parent_sig);

	for (sig = 1; sig <= SIGRTMAX; sig++) {
		if (sigismember(&sset, sig)) {
			sigaction(sig, NULL, &oact);

			/* Remember the original disposition, and ignore
			 * any default action signals
			 */
			if (oact.sa_handler == SIG_IGN)
				sigaddset(&ign_sig, sig);
			else {
				sigaction(sig, &act, NULL);
				sigaddset(&dfl_sig, sig);
			}
		}
	}
}

void
signal_handler_child_clear(void)
{
	struct sigaction act;
	int sig;

	act.sa_handler = SIG_IGN;
	sigemptyset(&act.sa_mask);
	act.sa_flags = 0;

	for (sig = 1; sig <= SIGRTMAX; sig++) {
		if (sigismember(&parent_sig, sig))
			sigaction(sig, &act, NULL);
	}

	open_signal_pipe();

	clear_signal_handler_addresses();
}

static void
signal_handlers_clear(void *state)
{
	/* Ensure no more pending signals */
	signal_set(SIGHUP, state, NULL);
	signal_set(SIGINT, state, NULL);
	signal_set(SIGTERM, state, NULL);
	signal_set(SIGCHLD, state, NULL);
	signal_set(SIGUSR1, state, NULL);
	signal_set(SIGUSR2, state, NULL);
#ifdef _WITH_JSON_
	signal_set(SIGJSON, state, NULL);
#endif
}

void
signal_handler_destroy(void)
{
	signal_handlers_clear(SIG_IGN);
	close(signal_pipe[1]);
	close(signal_pipe[0]);
	signal_pipe[1] = -1;
	signal_pipe[0] = -1;
}

/* Called prior to exec'ing a script. The script can reasonably
 * expect to have the standard signal disposition */
void
signal_handler_script(void)
{
	struct sigaction ign, dfl;
	int sig;

	ign.sa_handler = SIG_IGN;
	ign.sa_flags = 0;
	sigemptyset(&ign.sa_mask);
	dfl.sa_handler = SIG_DFL;
	dfl.sa_flags = 0;
	sigemptyset(&dfl.sa_mask);

	for (sig = 1; sig <= SIGRTMAX; sig++) {
		if (sigismember(&ign_sig, sig))
			sigaction(sig, &ign, NULL);
		else if (sigismember(&dfl_sig, sig))
			sigaction(sig, &dfl, NULL);
	}
}

int
signal_rfd(void)
{
	return(signal_pipe[0]);
}

/* Handlers callback  */
void
signal_run_callback(void)
{
	int sig;

	while(read(signal_pipe[0], &sig, sizeof(int)) == sizeof(int)) {
		if (sig >= 1 && sig <= SIG_MAX && signal_handler_func[sig-1])
			signal_handler_func[sig-1](signal_v[sig-1], sig);
	}
}

void signal_pipe_close(int min_fd)
{
	if (signal_pipe[0] && signal_pipe[0] >= min_fd)
		close(signal_pipe[0]);
	if (signal_pipe[1] && signal_pipe[1] >= min_fd)
		close(signal_pipe[1]);
}

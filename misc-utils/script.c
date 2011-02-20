/*
 * Copyright (c) 1980 Regents of the University of California.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. All advertising materials mentioning features or use of this software
 *    must display the following acknowledgement:
 *	This product includes software developed by the University of
 *	California, Berkeley and its contributors.
 * 4. Neither the name of the University nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE REGENTS AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE REGENTS OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

/*
 * 1999-02-22 Arkadiusz Mi�kiewicz <misiek@pld.ORG.PL>
 * - added Native Language Support
 *
 * 2000-07-30 Per Andreas Buer <per@linpro.no> - added "q"-option
 */

/*
 * script
 */
#include <stdio.h>
#include <stdlib.h>
#include <paths.h>
#include <time.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <termios.h>
#include <sys/ioctl.h>
#include <sys/time.h>
#include <sys/file.h>
#include <signal.h>
#include <errno.h>
#include <err.h>
#include <string.h>
#include <getopt.h>
#include <unistd.h>

#include "nls.h"

#if HAVE_LIBUTIL && HAVE_PTY_H
#include <pty.h>
#endif

#ifdef HAVE_LIBUTEMPTER
#include <utempter.h>
#endif

void finish(int);
void done(void);
void fail(void);
void resize(int);
void fixtty(void);
void getmaster(void);
void getslave(void);
void doinput(void);
void dooutput(void);
void doshell(void);

char	*shell;
FILE	*fscript;
int	master = -1;
int	slave;
int	child;
int	subchild;
int	childstatus;
char	*fname;

struct	termios tt;
struct	winsize win;
int	lb;
int	l;
#if !HAVE_LIBUTIL || !HAVE_PTY_H
char	line[] = "/dev/ptyXX";
#endif
int	aflg = 0;
char	*cflg = NULL;
int	eflg = 0;
int	fflg = 0;
int	qflg = 0;
int	tflg = 0;

int die;
int resized;

static void
die_if_link(char *fn) {
	struct stat s;

	if (lstat(fn, &s) == 0 && (S_ISLNK(s.st_mode) || s.st_nlink > 1))
	        /* FIXME: there is no [options] to allow/force this to happen.  */
		errx(EXIT_FAILURE,
			_("Warning: `%s' is a link.\n"
			  "Use `%s [options] %s' if you really "
			  "want to use it.\n"
			  "Program not started.\n"),
			fn, program_invocation_short_name, fn);
}

/*
 * script -t prints time delays as floating point numbers
 * The example program (scriptreplay) that we provide to handle this
 * timing output is a perl script, and does not handle numbers in
 * locale format (not even when "use locale;" is added).
 * So, since these numbers are not for human consumption, it seems
 * easiest to set LC_NUMERIC here.
 */

int
main(int argc, char **argv) {
	sigset_t block_mask, unblock_mask;
	struct sigaction sa;
	extern int optind;
	int ch;

	setlocale(LC_ALL, "");
	setlocale(LC_NUMERIC, "C");	/* see comment above */
	bindtextdomain(PACKAGE, LOCALEDIR);
	textdomain(PACKAGE);

	if (argc == 2) {
		if (!strcmp(argv[1], "-V") || !strcmp(argv[1], "--version")) {
			printf(_("%s (%s)\n"),
			       program_invocation_short_name, PACKAGE_STRING);
			return 0;
		}
	}

	while ((ch = getopt(argc, argv, "ac:efqt")) != -1)
		switch((char)ch) {
		case 'a':
			aflg++;
			break;
		case 'c':
			cflg = optarg;
			break;
		case 'e':
			eflg++;
			break;
		case 'f':
			fflg++;
			break;
		case 'q':
			qflg++;
			break;
		case 't':
			tflg++;
			break;
		case '?':
		default:
			fprintf(stderr,
				_("usage: script [-a] [-e] [-f] [-q] [-t] [file]\n"));
			exit(1);
		}
	argc -= optind;
	argv += optind;

	if (argc > 0)
		fname = argv[0];
	else {
		fname = "typescript";
		die_if_link(fname);
	}
	if ((fscript = fopen(fname, aflg ? "a" : "w")) == NULL) {
		warn(_("open failed: %s"), fname);
		fail();
	}

	shell = getenv("SHELL");
	if (shell == NULL)
		shell = _PATH_BSHELL;

	getmaster();
	if (!qflg)
		printf(_("Script started, file is %s\n"), fname);
	fixtty();

#ifdef HAVE_LIBUTEMPTER
	utempter_add_record(master, NULL);
#endif
	/* setup SIGCHLD handler */
	sigemptyset(&sa.sa_mask);
	sa.sa_flags = 0;
	sa.sa_handler = finish;
	sigaction(SIGCHLD, &sa, NULL);

	/* init mask for SIGCHLD */
	sigprocmask(SIG_SETMASK, NULL, &block_mask);
	sigaddset(&block_mask, SIGCHLD);

	sigprocmask(SIG_SETMASK, &block_mask, &unblock_mask);
	child = fork();
	sigprocmask(SIG_SETMASK, &unblock_mask, NULL);

	if (child < 0) {
		warn(_("fork failed"));
		fail();
	}
	if (child == 0) {

		sigprocmask(SIG_SETMASK, &block_mask, NULL);
		subchild = child = fork();
		sigprocmask(SIG_SETMASK, &unblock_mask, NULL);

		if (child < 0) {
			warn(_("fork failed"));
			fail();
		}
		if (child)
			dooutput();
		else
			doshell();
	} else {
		sa.sa_handler = resize;
		sigaction(SIGWINCH, &sa, NULL);
	}
	doinput();

	return EXIT_SUCCESS;
}

void
doinput() {
	register int cc;
	char ibuf[BUFSIZ];

	(void) fclose(fscript);

	while (die == 0) {
		if ((cc = read(0, ibuf, BUFSIZ)) > 0) {
			ssize_t wrt = write(master, ibuf, cc);
			if (wrt == -1) {
				warn (_("write failed"));
				fail();
			}
		}
		else if (cc == -1 && errno == EINTR && resized)
			resized = 0;
		else
			break;
	}

	done();
}

#include <sys/wait.h>

void
finish(int dummy __attribute__ ((__unused__))) {
	int status;
	register int pid;

	while ((pid = wait3(&status, WNOHANG, 0)) > 0)
		if (pid == child) {
			childstatus = status;
			die = 1;
		}
}

void
resize(int dummy __attribute__ ((__unused__))) {
	resized = 1;
	/* transmit window change information to the child */
	(void) ioctl(0, TIOCGWINSZ, (char *)&win);
	(void) ioctl(slave, TIOCSWINSZ, (char *)&win);
}

/*
 * Stop extremely silly gcc complaint on %c:
 *  warning: `%c' yields only last 2 digits of year in some locales
 */
static void
my_strftime(char *buf, size_t len, const char *fmt, const struct tm *tm) {
	strftime(buf, len, fmt, tm);
}

void
dooutput() {
	register ssize_t cc;
	time_t tvec;
	char obuf[BUFSIZ];
	struct timeval tv;
	double oldtime=time(NULL), newtime;
	int flgs = 0;
	ssize_t wrt;
	ssize_t fwrt;

	(void) close(0);
#ifdef HAVE_LIBUTIL
	(void) close(slave);
#endif
	tvec = time((time_t *)NULL);
	my_strftime(obuf, sizeof obuf, "%c\n", localtime(&tvec));
	fprintf(fscript, _("Script started on %s"), obuf);

	do {
		if (die && flgs == 0) {
			/* ..child is dead, but it doesn't mean that there is
			 * nothing in buffers.
			 */
			flgs = fcntl(master, F_GETFL, 0);
			if (fcntl(master, F_SETFL, (flgs | O_NONBLOCK)) == -1)
				break;
		}
		if (tflg)
			gettimeofday(&tv, NULL);

		errno = 0;
		cc = read(master, obuf, sizeof (obuf));

		if (die && errno == EINTR && cc <= 0)
			/* read() has been interrupted by SIGCHLD, try it again
			 * with O_NONBLOCK
			 */
			continue;
		if (cc <= 0)
			break;
		if (tflg) {
			newtime = tv.tv_sec + (double) tv.tv_usec / 1000000;
			fprintf(stderr, "%f %zd\n", newtime - oldtime, cc);
			oldtime = newtime;
		}
		wrt = write(1, obuf, cc);
		if (wrt < 0) {
			warn (_("write failed"));
			fail();
		}
		fwrt = fwrite(obuf, 1, cc, fscript);
		if (fwrt < cc) {
			warn (_("cannot write script file"));
			fail();
		}
		if (fflg)
			(void) fflush(fscript);
	} while(1);

	if (flgs)
		fcntl(master, F_SETFL, flgs);
	done();
}

void
doshell() {
	char *shname;

#if 0
	int t;

	t = open(_PATH_DEV_TTY, O_RDWR);
	if (t >= 0) {
		(void) ioctl(t, TIOCNOTTY, (char *)0);
		(void) close(t);
	}
#endif

	getslave();
	(void) close(master);
	(void) fclose(fscript);
	(void) dup2(slave, 0);
	(void) dup2(slave, 1);
	(void) dup2(slave, 2);
	(void) close(slave);

	master = -1;

	shname = strrchr(shell, '/');
	if (shname)
		shname++;
	else
		shname = shell;

	if (cflg)
		execl(shell, shname, "-c", cflg, NULL);
	else
		execl(shell, shname, "-i", NULL);

	warn(_("failed to execute %s"), shell);
	fail();
}

void
fixtty() {
	struct termios rtt;

	rtt = tt;
	cfmakeraw(&rtt);
	rtt.c_lflag &= ~ECHO;
	(void) tcsetattr(0, TCSANOW, &rtt);
}

void
fail() {

	(void) kill(0, SIGTERM);
	done();
}

void
done() {
	time_t tvec;

	if (subchild) {
		if (!qflg) {
			char buf[BUFSIZ];
			tvec = time((time_t *)NULL);
			my_strftime(buf, sizeof buf, "%c\n", localtime(&tvec));
			fprintf(fscript, _("\nScript done on %s"), buf);
		}
		(void) fclose(fscript);
		(void) close(master);

		master = -1;
	} else {
		(void) tcsetattr(0, TCSADRAIN, &tt);
		if (!qflg)
			printf(_("Script done, file is %s\n"), fname);
#ifdef HAVE_LIBUTEMPTER
		if (master >= 0)
			utempter_remove_record(master);
#endif
	}

	if(eflg) {
		if (WIFSIGNALED(childstatus))
			exit(WTERMSIG(childstatus) + 0x80);
		else
			exit(WEXITSTATUS(childstatus));
	}
	exit(EXIT_SUCCESS);
}

void
getmaster() {
#if HAVE_LIBUTIL && HAVE_PTY_H
	(void) tcgetattr(0, &tt);
	(void) ioctl(0, TIOCGWINSZ, (char *)&win);
	if (openpty(&master, &slave, NULL, &tt, &win) < 0) {
		warn(_("openpty failed"));
		fail();
	}
#else
	char *pty, *bank, *cp;
	struct stat stb;

	pty = &line[strlen("/dev/ptyp")];
	for (bank = "pqrs"; *bank; bank++) {
		line[strlen("/dev/pty")] = *bank;
		*pty = '0';
		if (stat(line, &stb) < 0)
			break;
		for (cp = "0123456789abcdef"; *cp; cp++) {
			*pty = *cp;
			master = open(line, O_RDWR);
			if (master >= 0) {
				char *tp = &line[strlen("/dev/")];
				int ok;

				/* verify slave side is usable */
				*tp = 't';
				ok = access(line, R_OK|W_OK) == 0;
				*tp = 'p';
				if (ok) {
					(void) tcgetattr(0, &tt);
				    	(void) ioctl(0, TIOCGWINSZ, 
						(char *)&win);
					return;
				}
				(void) close(master);
				master = -1;
			}
		}
	}
	master = -1;
	warn(_("out of pty's"));
	fail();
#endif /* not HAVE_LIBUTIL */
}

void
getslave() {
#ifndef HAVE_LIBUTIL
	line[strlen("/dev/")] = 't';
	slave = open(line, O_RDWR);
	if (slave < 0) {
		warn(_("open failed: %s"), line);
		fail();
	}
	(void) tcsetattr(slave, TCSANOW, &tt);
	(void) ioctl(slave, TIOCSWINSZ, (char *)&win);
#endif
	(void) setsid();
	(void) ioctl(slave, TIOCSCTTY, 0);
}

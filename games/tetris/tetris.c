/*	$OpenBSD: tetris.c,v 1.16 2003/04/06 18:50:38 deraadt Exp $	*/
/*	$NetBSD: tetris.c,v 1.2 1995/04/22 07:42:47 cgd Exp $	*/

/*-
 * Copyright (c) 1992, 1993
 *	The Regents of the University of California.  All rights reserved.
 *
 * This code is derived from software contributed to Berkeley by
 * Chris Torek and Darren F. Provine.
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
 *
 *	@(#)tetris.c	8.1 (Berkeley) 5/31/93
 */

#ifndef lint
static const char copyright[] =
"@(#) Copyright (c) 1992, 1993\n\
	The Regents of the University of California.  All rights reserved.\n";
#endif /* not lint */

/*
 * Tetris (or however it is spelled).
 */

#include <sys/param.h>
#include <sys/time.h>
#include <sys/types.h>

#include <err.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "input.h"
#include "scores.h"
#include "screen.h"
#include "tetris.h"

cell	board[B_SIZE];
int	Rows, Cols;
const struct shape *curshape;
const struct shape *nextshape;
long	fallrate;
int	score;
gid_t	gid, egid;
char	key_msg[100];
int	showpreview, classic;

static void	elide(void);
static void	setup_board(void);
const struct shape *randshape(void);
void	onintr(int);
void	usage(void);

/*
 * Set up the initial board.  The bottom display row is completely set,
 * along with another (hidden) row underneath that.  Also, the left and
 * right edges are set.
 */
static void
setup_board()
{
	int i;
	cell *p;

	p = board;
	for (i = B_SIZE; i; i--)
		*p++ = i <= (2 * B_COLS) || (i % B_COLS) < 2;
}

/*
 * Elide any full active rows.
 */
static void
elide()
{
	int i, j, base;
	cell *p;

	for (i = A_FIRST; i < A_LAST; i++) {
		base = i * B_COLS + 1;
		p = &board[base];
		for (j = B_COLS - 2; *p++ != 0;) {
			if (--j <= 0) {
				/* this row is to be elided */
				memset(&board[base], 0, B_COLS - 2);
				scr_update();
				tsleep();
				while (--base != 0)
					board[base + B_COLS] = board[base];
				scr_update();
				tsleep();
				break;
			}
		}
	}
}

const struct shape *
randshape()
{
	const struct shape *tmp;
	int i, j;

	tmp = &shapes[random() % 7];
	j = random() % 4;
	for (i = 0; i < j; i++)
		tmp = &shapes[classic? tmp->rotc : tmp->rot];
	return (tmp);
}
	

int
main(argc, argv)
	int argc;
	char *argv[];
{
	int pos, c;
	char *keys;
	int level = 2;
	char key_write[6][10];
	int ch, i, j;

	keys = "jkl pq";

	gid = getgid();
	egid = getegid();
	setegid(gid);

	classic = showpreview = 0;
	while ((ch = getopt(argc, argv, "chk:l:ps")) != -1)
		switch(ch) {
		case 'c':
			/*
			 * this means:
			 *	- rotate the other way;
			 *	- no reverse video.
			 */
			classic = 1;
			break;
		case 'k':
			if (strlen(keys = optarg) != 6)
				usage();
			break;
		case 'l':
			level = atoi(optarg);
			if (level < MINLEVEL || level > MAXLEVEL)
				errx(1, "level must be from %d to %d",
				    MINLEVEL, MAXLEVEL);
			break;
		case 'p':
			showpreview = 1;
			break;
		case 's':
			showscores(0);
			exit(0);
		case '?':
		case 'h':
		default:
			usage();
		}

	argc -= optind;
	argv += optind;

	if (argc)
		usage();

	fallrate = 1000000 / level;

	for (i = 0; i <= 5; i++) {
		for (j = i+1; j <= 5; j++) {
			if (keys[i] == keys[j])
				errx(1, "duplicate command keys specified.");
		}
		if (keys[i] == ' ')
			strcpy(key_write[i], "<space>");
		else {
			key_write[i][0] = keys[i];
			key_write[i][1] = '\0';
		}
	}

	snprintf(key_msg, sizeof key_msg,
"%s - left   %s - rotate   %s - right   %s - drop   %s - pause   %s - quit",
		key_write[0], key_write[1], key_write[2], key_write[3],
		key_write[4], key_write[5]);

	(void)signal(SIGINT, onintr);
	scr_init();
	setup_board();

	srandomdev();
	scr_set();

	pos = A_FIRST*B_COLS + (B_COLS/2)-1;
	nextshape = randshape();
	curshape = randshape();

	scr_msg(key_msg, 1);

	for (;;) {
		place(curshape, pos, 1);
		scr_update();
		place(curshape, pos, 0);
		c = tgetchar();
		if (c < 0) {
			/*
			 * Timeout.  Move down if possible.
			 */
			if (fits_in(curshape, pos + B_COLS)) {
				pos += B_COLS;
				continue;
			}

			/*
			 * Put up the current shape `permanently',
			 * bump score, and elide any full rows.
			 */
			place(curshape, pos, 1);
			score++;
			elide();

			/*
			 * Choose a new shape.  If it does not fit,
			 * the game is over.
			 */
			curshape = nextshape;
			nextshape = randshape();
			pos = A_FIRST*B_COLS + (B_COLS/2)-1;
			if (!fits_in(curshape, pos))
				break;
			continue;
		}

		/*
		 * Handle command keys.
		 */
		if (c == keys[5]) {
			/* quit */
			break;
		}
		if (c == keys[4]) {
			static char msg[] =
			    "paused - press RETURN to continue";

			place(curshape, pos, 1);
			do {
				scr_update();
				scr_msg(key_msg, 0);
				scr_msg(msg, 1);
				(void) fflush(stdout);
			} while (rwait((struct timeval *)NULL) == -1);
			scr_msg(msg, 0);
			scr_msg(key_msg, 1);
			place(curshape, pos, 0);
			continue;
		}
		if (c == keys[0]) {
			/* move left */
			if (fits_in(curshape, pos - 1))
				pos--;
			continue;
		}
		if (c == keys[1]) {
			/* turn */
			const struct shape *new = &shapes[
			    classic? curshape->rotc : curshape->rot];

			if (fits_in(new, pos))
				curshape = new;
			continue;
		}
		if (c == keys[2]) {
			/* move right */
			if (fits_in(curshape, pos + 1))
				pos++;
			continue;
		}
		if (c == keys[3]) {
			/* move to bottom */
			while (fits_in(curshape, pos + B_COLS)) {
				pos += B_COLS;
				score++;
			}
			continue;
		}
		if (c == '\f') {
			scr_clear();
			scr_msg(key_msg, 1);
		}
	}

	scr_clear();
	scr_end();

	if (showpreview == 0)
		(void)printf("Your score:  %d point%s  x  level %d  =  %d\n",
		    score, score == 1 ? "" : "s", level, score * level);
	else {
		(void)printf("Your score:  %d point%s x level %d x preview penalty %0.3f = %d\n",
		    score, score == 1 ? "" : "s", level, (double)PRE_PENALTY,
		    (int)(score * level * PRE_PENALTY));
		score = score * PRE_PENALTY;
	}
	savescore(level);

	printf("\nHit RETURN to see high scores, ^C to skip.\n");

	while ((i = getchar()) != '\n')
		if (i == EOF)
			break;

	showscores(level);

	exit(0);
}

void
onintr(signo)
	int signo;
{
	scr_clear();		/* XXX signal race */
	scr_end();		/* XXX signal race */
	_exit(0);
}

void
usage()
{
	(void)fprintf(stderr, "usage: tetris [-ps] [-k keys] [-l level]\n");
	exit(1);
}

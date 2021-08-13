/*	$OpenBSD: buf.c,v 1.24 2019/06/28 13:34:59 deraadt Exp $	*/
/*	$NetBSD: buf.c,v 1.15 1995/04/23 10:07:28 cgd Exp $	*/

/* buf.c: This file contains the scratch-file buffer routines for the
   ed line editor. */
/*-
 * Copyright (c) 2011, 2012, 2020
 *	mirabilos <m@mirbsd.org>
 * Copyright (c) 1993 Andrew Moore, Talke Studio.
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
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#include <sys/types.h>
#include <sys/stat.h>

#include <errno.h>
#include <limits.h>
#include <signal.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "ed.h"

__RCSID("$MirOS: src/bin/ed/buf.c,v 1.11 2021/08/13 17:48:27 tg Exp $");

static FILE *sfp;			/* scratch file pointer */
static tp_ftell sfpos;			/* scratch file position */
static edbool seek_write;		/* seek before writing */
static line_t buffer_head;		/* incore buffer */

/* get_sbuf_line: get a line of text from the scratch file; return pointer
   to the text */
char *
get_sbuf_line(line_t *lp)
{
	static char *sfbuf = NULL;	/* buffer */
	static size_t sfbufsz = 0;	/* buffer size */
	size_t len;

	if (lp == &buffer_head)
		return NULL;
	seek_write = 1;				/* force seek on write */
	/* out of position */
	if (sfpos != lp->adr) {
		sfpos = lp->adr;
		if (do_fseek(sfp, sfpos, SEEK_SET) == -1) {
			perror(NULL);
			seterrmsg("cannot seek temp file");
			return NULL;
		}
	}
	len = lp->llen;
	REALLOC(sfbuf, sfbufsz, len + 1, NULL);
	if (fread(sfbuf, sizeof(char), len, sfp) != len) {
		perror(NULL);
		seterrmsg("cannot read temp file");
		return NULL;
	}
	sfpos += len;				/* update file position */
	sfbuf[len] = '\0';
	return sfbuf;
}


/* put_sbuf_line: write a line of text to the scratch file and add a line node
   to the editor buffer;  return a pointer to the end of the text */
char *
put_sbuf_line(char *cs)
{
	line_t *lp;
	size_t len;
	char *s;

	if ((lp = malloc(sizeof(line_t))) == NULL) {
		perror(NULL);
		seterrmsg("out of memory");
		return NULL;
	}
	/* assert: cs is '\n' terminated */
	for (s = cs; *s != '\n'; s++)
		;
	if (s - cs >= LINECHARS) {
		seterrmsg("line too long");
		free(lp);
		return NULL;
	}
	len = s - cs;
	/* out of position */
	if (seek_write) {
		if (do_fseek(sfp, (tp_ftell)0, SEEK_END) == -1) {
			perror(NULL);
			seterrmsg("cannot seek temp file");
			free(lp);
			return NULL;
		}
		sfpos = do_ftell(sfp);
		seek_write = 0;
	}
	/* assert: SPL1() */
	if (fwrite(cs, sizeof(char), len, sfp) != len) {
		sfpos = -1;
		perror(NULL);
		seterrmsg("cannot write temp file");
		free(lp);
		return NULL;
	}
	lp->llen = len;
	lp->adr = sfpos;
	add_line_node(lp);
	sfpos += len;			/* update file position */
	return ++s;
}


/* add_line_node: add a line node in the editor buffer after the current line */
void
add_line_node(line_t *lp)
{
	line_t *cp;

	/* this get_addressed_line_node last! */
	cp = get_addressed_line_node(current_addr);
	INSQUE(lp, cp);
	addr_last++;
	current_addr++;
}


/* get_line_node_addr: return line number of pointer */
int
get_line_node_addr(line_t *lp)
{
	line_t *cp = &buffer_head;
	int n = 0;

	while (cp != lp && (cp = cp->q_forw) != &buffer_head)
		n++;
	if (n && cp == &buffer_head) {
		seterrmsg("invalid address");
		return ERR;
	 }
	 return n;
}


/* get_addressed_line_node: return pointer to a line node in the editor buffer */
line_t *
get_addressed_line_node(int n)
{
	static line_t *lp = &buffer_head;
	static int on = 0;

	SPL1();
	if (n > on) {
		if (n <= (on + addr_last) >> 1)
			for (; on < n; on++)
				lp = lp->q_forw;
		else {
			lp = buffer_head.q_back;
			for (on = addr_last; on > n; on--)
				lp = lp->q_back;
		}
	} else {
		if (n >= on >> 1)
			for (; on > n; on--)
				lp = lp->q_back;
		else {
			lp = &buffer_head;
			for (on = 0; on < n; on++)
				lp = lp->q_forw;
		}
	}
	SPL0();
	return lp;
}


extern edbool newline_added;

static char *sfn;
static char *sfn_template;
static size_t sfnlen;

/* open_sbuf: open scratch file */
int
open_sbuf(void)
{
	int fd = -1;

	isbinary = newline_added = 0;
	memcpy(sfn, sfn_template, sfnlen);
	if ((fd = mkstemp(sfn)) == -1 ||
	    (sfp = fdopen(fd, "w+")) == NULL) {
		perror(sfn);
		if (fd != -1) {
			close(fd);
			unlink(sfn);
		}
		seterrmsg("cannot open temp file");
		return ERR;
	}
	return 0;
}


/* close_sbuf: close scratch file */
int
close_sbuf(void)
{
	if (sfp) {
		if (fclose(sfp) == EOF) {
			perror(sfn);
			seterrmsg("cannot close temp file");
			return ERR;
		}
		unlink(sfn);
		sfp = NULL;
	}
	sfpos = seek_write = 0;
	return 0;
}


/* quit: remove_lines scratch file and exit */
void
quit(int n)
{
	if (sfp) {
		fclose(sfp);
		unlink(sfn);
	}
	exit(n);
}


static unsigned char ctab[256];		/* character translation table */

/* init_buffers: open scratch buffer; initialise line queue */
void
init_buffers(void)
{
	int i = 0;

	if ((sfn = getenv("TMPDIR"))) {
		sfnlen = strlen(sfn);
		while (sfnlen && sfn[sfnlen - 1] == '/')
			sfn[--sfnlen] = '\0';
	}
	errno = ENOMEM;
	if (asprintf(&sfn_template, "%s/ed.XXXXXXXXXX",
	    sfn && *sfn ? sfn : "/tmp") == -1) {
 err:
		perror(NULL);
		quit(2);
	}
	sfnlen = strlen(sfn_template) + 1;
	if (!(sfn = malloc(sfnlen)))
		goto err;

	/* Read stdin one character at a time to avoid i/o contention
	   with shell escapes invoked by nonterminal input, e.g.,
	   ed - <<EOF
	   !cat
	   hello, world
	   EOF */
	setvbuf(stdin, NULL, _IONBF, 0);
	if (open_sbuf() < 0)
		quit(2);
	REQUE(&buffer_head, &buffer_head);
	for (i = 0; i < 256; i++)
		ctab[i] = i;
}


/* translit_text: translate characters in a string */
char *
translit_text(char *s, size_t len, int from, int to)
{
	static int i = 0;

	unsigned char *us;

	ctab[i] = i;			/* restore table to initial state */
	ctab[i = from] = to;
	for (us = (unsigned char *) s; len-- > 0; us++)
		*us = ctab[*us];
	return s;
}

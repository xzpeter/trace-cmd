/*
 * Copyright (C) 2015 Red Hat Inc, Steven Rostedt <srostedt@redhat.com>
 *
 * ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation;
 * version 2.1 of the License (not later!)
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this program; if not,  see <http://www.gnu.org/licenses>
 *
 * ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
 */

#include <stdio.h>
#include <ctype.h>

#include "trace-cmd.h"

struct hook_list *tracecmd_create_event_hook(const char *arg)
{
	struct hook_list *hook;
	char *system = NULL;
	char *event;
	char *match;
	char *flags = NULL;
	char *pid = NULL;
	char *str;
	char *tok;
	int index;
	int ch;
	int i;

	hook = malloc_or_die(sizeof(*hook));
	memset(hook, 0, sizeof(*hook));

	str = strdup(arg);
	if (!str)
		die("malloc");

	hook->str = str;
	hook->hook = arg;

	/*
	 * Hooks are in the form of:
	 *  [<start_system>:]<start_event>,<start_match>[,<start_pid>]/
	 *  [<end_system>:]<end_event>,<end_match>[,<flags>]
	 *
	 * Where start_system, start_pid, end_system, and flags are all
	 * optional.
	 *
	 * Flags are (case insensitive):
	 *  P - pinned to cpu (wont migrate)
	 *  G - global, not hooked to task - currently ignored.
	 *  S - save stacks for this event.
	 */
	tok = strtok(str, ":,");
	if (!tok)
		goto invalid_tok;

	/* See what the token was from the original arg */
	index = strlen(tok);
	if (arg[index] == ':') {
		/* this is a system, the next token must be ',' */
		system = tok;
		tok = strtok(NULL, ",");
		if (!tok)
			goto invalid_tok;
	}
	event = tok;

	tok = strtok(NULL, ",/");
	if (!tok)
		goto invalid_tok;
	match = tok;
	index = strlen(tok) + tok - str;
	if (arg[index] == ',') {
		tok = strtok(NULL, "/");
		if (!tok)
			goto invalid_tok;
		pid = tok;
	}

	hook->start_system = system;
	hook->start_event = event;
	hook->start_match = match;
	hook->pid = pid;

	/* Now process the end event */
	system = NULL;

	tok = strtok(NULL, ":,");
	if (!tok)
		goto invalid_tok;

	/* See what the token was from the original arg */
	index = tok - str + strlen(tok);
	if (arg[index] == ':') {
		/* this is a system, the next token must be ',' */
		system = tok;
		tok = strtok(NULL, ",");
		if (!tok)
			goto invalid_tok;
	}
	event = tok;

	tok = strtok(NULL, ",");
	if (!tok)
		goto invalid_tok;
	match = tok;
	index = strlen(tok) + tok - str;
	if (arg[index] == ',') {
		tok = strtok(NULL, "");
		if (!tok)
			goto invalid_tok;
		flags = tok;
	}

	hook->end_system = system;
	hook->end_event = event;
	hook->end_match = match;
	hook->migrate = 1;
	if (flags) {
		for (i = 0; flags[i]; i++) {
			ch = tolower(flags[i]);
			switch (ch) {
			case 'p':
				hook->migrate = 0;
				break;
			case 'g':
				hook->global = 1;
				break;
			case 's':
				hook->stack = 1;
				break;
			default:
				warning("unknown flag %c\n", flags[i]);
			}
		}
	}

	printf("start %s:%s:%s (%s) end %s:%s:%s (%s)\n",
	       hook->start_system,
	       hook->start_event,
	       hook->start_match,
	       hook->pid,
	       hook->end_system,
	       hook->end_event,
	       hook->end_match,
	       flags);
	return hook;

invalid_tok:
	die("Invalid hook format '%s'", arg);
	return NULL;
}
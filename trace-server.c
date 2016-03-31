/*
 * Copyright (C) 2016 Red Hat Inc, Peter Xu <peterx@redhat.com>
 *
 * ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; version 2 of the License (not later!)
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not,  see <http://www.gnu.org/licenses>
 *
 * ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <getopt.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <netdb.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <errno.h>

#include "trace-listen.h"
#include "trace-msg.h"
#include "trace-local.h"
#include "trace-cmd.h"

static int tracecmd_server_handle_clients(const char *node,
					  const char *port, int fd)
{
	return 0;
}

void trace_server(int argc, char **argv)
{
	char *logfile = NULL;
	char *port = NULL;
	int daemon = 0;
	int c;

	if (argc < 2)
		usage(argv);

	if (strcmp(argv[1], "server") != 0)
		usage(argv);

	for (;;) {
		int option_index = 0;
		static struct option long_options[] = {
			{"port", required_argument, NULL, 'p'},
			{"help", no_argument, NULL, '?'},
			{NULL, 0, NULL, 0}
		};

		c = getopt_long (argc-1, argv+1, "+hp:l:D",
			long_options, &option_index);
		if (c == -1)
			break;
		switch (c) {
		case 'h':
			usage(argv);
			break;
		case 'p':
			port = optarg;
			break;
		case 'l':
			logfile = optarg;
			break;
		case 'D':
			daemon = 1;
			break;
		default:
			usage(argv);
		}
	}

	if (!port)
		usage(argv);

	if ((argc - optind) >= 2)
		usage(argv);

	if (logfile) {
		/* set the writes to a logfile instead */
		logfp = fopen(logfile, "w");
		if (!logfp)
			die("creating log file %s", logfile);
	}

	if (daemon)
		start_daemon();

	plog("Starting trace-cmd server on port %s\n", port);

	tracecmd_listen_start(port, tracecmd_server_handle_clients);

	return;
}

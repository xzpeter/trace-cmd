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

#include "list.h"
#include "trace-listen.h"
#include "trace-msg.h"
#include "trace-local.h"
#include "trace-cmd.h"

struct tracecmd_record_req {
	struct list_head list;
	char *host;
	char *param;
};

static int tracecmd_server_handle_clients(const char *node,
					  const char *port, int fd)
{
	plog("Handling tracing request from %s:%s\n", node, port);

	return tracecmd_msg_svr_handle_record_req(fd);
}

static void tracecmd_server_conn_list_free(struct list_head *conn_list)
{
	struct tracecmd_record_req *p, *q;

	if (!conn_list)
		return;

	list_for_each_entry_safe(p, q, conn_list, list) {
		free(p->host);
		free(p->param);
		free(p);
	}
}

static struct tracecmd_record_req *
tracecmd_server_req_alloc(const char *host, const char *param)
{
	struct tracecmd_record_req *p = calloc(1, sizeof(*p));
	p->host = strdup(host);
	p->param = strdup(param);
	list_head_init(&p->list);
	return p;
}

static int tracecmd_server_parse_record_requests(int argc, char **argv,
						 struct list_head *conn_list)
{
	char param_buf[BUFSIZ];

	/*
	 * format: "--connect <host1> [params...]
	 *          --connect <host2> [params...]
	 *          ..."
	 */
	while (argc >= 3 && !strcmp(argv[0], "--connect")) {
		int len = 0;
		char *host;
		struct tracecmd_record_req *new;

		argc--; argv++;

		host = argv[0];
		argc--; argv++;

		do {
			len += snprintf(param_buf + len,
					sizeof(param_buf) - len,
					"%s ", argv[0]);
			if (len >= sizeof(param_buf)) {
				/* single line parameter too long */
				goto error;
			}
			argc--; argv++;
		} while (argc && strcmp(argv[0], "--connect"));

		/* remove ending space */
		param_buf[len - 1] = '\0';

		new = tracecmd_server_req_alloc(host, param_buf);
		list_add_tail(&new->list, conn_list);
	}

	if (argc) {
		puts("Incorrect parameter for --connect. Please use:");
		puts("  --connect <host:port> [params...]");
		goto error;
	}
	return 0;

error:
	tracecmd_server_conn_list_free(conn_list);
	return -1;
}

static int
tracecmd_server_handle_requests(struct list_head *conn_list)
{
	struct tracecmd_record_req *p;

	list_for_each_entry(p, conn_list, list) {
		printf("HANDLE REQ: host='%s', param='%s'\n",
		       p->host, p->param);
		/* TODO: fork one task for each connection */
	}

	return 0;
}

int tracecmd_server_connect (int argc, char **argv)
{
	int ret;
	struct list_head conn_list;

	list_head_init(&conn_list);

	tracecmd_server_parse_record_requests(argc, argv, &conn_list);
	if (list_empty(&conn_list))
		return -1;

	ret = tracecmd_server_handle_requests(&conn_list);
	tracecmd_server_conn_list_free(&conn_list);

	return ret;
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

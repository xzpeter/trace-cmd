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
#include "trace-server.h"

struct tracecmd_record_req {
	struct list_head list;
	char *hoststr;		/* possibly HOST:PORT */
	char *host;
	char *port;
	char *param;
};

static int param_count_spaces(char *p)
{
	int count = 0;
	while (*p)
		if (*p++ == ' ')
			count++;
	return count;
}

/* This is tricky: we simulate the main() interface, generating
 * argc/argv for trace_report(). So that we can have full
 * functionality for remote tracing with only little chunk of
 * work. Here, we are using memory space provided by params. */
static void tracecmd_server_generate_argv(char *params, int *argc_out,
					  char ***argv_out)
{
	char **argv;
	char *p, *word;
	int argc = param_count_spaces(params) + 3;
	int i;

	if (argc_out)
		*argc_out = argc;

	argv = malloc(sizeof(char *) * (argc + 1));
	argv[0] = NULL;		/* the program name, skip it */
	argv[1] = "record";
	argv[argc] = NULL;	/* ends the string list */

	/* then, start from arg 2 */
	i = 2;

	/* fill in all the rest of parameters, seperated by space */
	word = strtok_r(params, " ", &p);
	do {
		argv[i++] = word;
		word = strtok_r(NULL, " ", &p);
	} while (word);

	for (i = 1; i < argc; i++) {
		plog("DUMP ARGV[%d] = '%s'\n", i, argv[i]);
	}

	if (argv_out)
		*argv_out = argv;
}

static int tracecmd_server_handle_clients(const char *node,
					  const char *port, int fd)
{
	int argc;
	char **argv;
	char *params;

	plog("Handling tracing request from %s:%s\n", node, port);

	params = tracecmd_msg_svr_handle_record_req(fd);

	tracecmd_server_generate_argv(params, &argc, &argv);

	/*
	 * This is just like we ran the command:
	 *
	 *   trace-cmd record -N xxx [params...]
	 *
	 * The only difference is that, we do not provide "-N",
	 * instead, we set tracing_fd to valid, to let
	 * trace_record() know that this is a remote tracing.
	 */
	host = strdup(node);
	trace_record(argc, argv, fd);

	/* may not reach here... anyway, we should free them. */
	free(params);
	free(argv);

	return 0;
}

static void tracecmd_server_conn_list_free(struct list_head *conn_list)
{
	struct tracecmd_record_req *p, *q;

	if (!conn_list)
		return;

	list_for_each_entry_safe(p, q, conn_list, list) {
		free(p->hoststr);
		free(p->param);
		free(p->host);
		free(p->port);
		free(p);
	}
}

static struct tracecmd_record_req *
tracecmd_server_req_alloc(const char *host, const char *param)
{
	struct tracecmd_record_req *p = calloc(1, sizeof(*p));
	p->hoststr = strdup(host);
	p->param = strdup(param);
	p->host = p->port = NULL;
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

/* This keeps all the childs that is spawned by "--connect"
 * parameter. */
static struct process_list client_list;

static void clients_clean_up(int sig)
{
	int status;
	int ret;

	/* Clean up any children that has started before */
	do {
		ret = waitpid(0, &status, WNOHANG);
		if (ret > 0)
			remove_process(&client_list, ret);
	} while (ret > 0);
}

/**
 * tracecmd_server_client_spawn - spawn child to handle one --connect req
 * @fd: socket fd to server
 * @p: pointer to the recording request
 */
static void
tracecmd_server_client_spawn(int fd, struct tracecmd_record_req *p)
{
	int ret;
	int pid = do_fork(fd);

	if (pid) {
		/* parent */
		add_process(&client_list, pid);
		return;
	}

	/* child */
	printf("child for %s enter\n", p->hoststr);
	tracecmd_msg_svr_send_record_req(fd, p->param);

	/* Now, this client start to work as "trace-cmd-listen"
	 * server, collecting traces from the other side. */
	ret = tracecmd_listen_process_client(p->host, p->port, fd);
	if (ret) {
		printf("ERROR: child for %s failed collect trace\n",
		       p->hoststr);
	} else {
		printf("child for %s collected traces successfully\n",
		       p->hoststr);
	}

	printf("child for %s quit\n", p->hoststr);
	exit(0);
}

static int
tracecmd_server_handle_requests(struct list_head *conn_list)
{
	int sfd;
	struct tracecmd_record_req *p;

	/* Before doing anything, we need to overwrite the original
	 * SIGCHLD handler, to handle our own child list. */
	signal_setup(SIGCHLD, clients_clean_up);

	/* Firstly, we try to connect to all the targets. For each
	 * connect, we will spawn one child to handle further
	 * requests. */
	list_for_each_entry(p, conn_list, list) {
		network_parse_hoststr(p->hoststr, &p->host, &p->port);

		printf("Connecting to host '%s' port '%s': ",
		       p->host, p->port);

		sfd = network_connect_host(p->host, p->port);

		printf("connected\n");

		tracecmd_server_client_spawn(sfd, p);
	}

	printf("parent waiting for all childs to finish...\n");
	while (!done && !process_list_empty(&client_list)) {
		sleep (0.1);
	}

	if (!process_list_empty(&client_list)) {
		printf("parent killing all clients...\n");
		kill_clients(&client_list);
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

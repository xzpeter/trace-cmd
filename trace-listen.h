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
#ifndef _TRACE_LISTEN_H
#define _TRACE_LISTEN_H

typedef int (*tracecmd_listen_handler)(const char *node,
				       const char *port, int fd);

/**
 * tracecmd_listen_process_client - process one tracecmd-listen client
 * @node: hostname for tracecmd-listen client
 * @port: port for tracecmd-listen client
 * @fd: file handle of the client socket
 *
 * This handler will negociate with client to record N channel trace
 * information (corresponds to N client CPUs) and store it using
 * node and port info. For protocol itself, please refer to git
 * commit cc042aba4 for more information.
 */
int tracecmd_listen_process_client(const char *node,
				   const char *port, int fd);

/**
 * tracecmd_listen_start - start server daemon
 * @port: port on which to start the server, e.g., "12345"
 * @handler: handler to run for connected clients
 *
 * This will start tracecmd-listen server. For each connected
 * client, we will handle the requests using @handler that is
 * provided.
 */
void tracecmd_listen_start(char *port, tracecmd_listen_handler handler);

#endif /* _TRACE_LISTEN_H */

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
#ifndef _TRACE_SERVER_H
#define _TRACE_SERVER_H

/**
 * tracecmd_server_connect - handle "record --connect" request
 * @argc: argc start from the first "--connect"
 * @argv: argv start from the first "--connect"
 */
int tracecmd_server_connect (int argc, char **argv);

#define  TCMD_SERVER_FAKE_HOST  ("TCMD_SVR_FAKE_HOST")

#endif /* _TRACE_SERVER_H */

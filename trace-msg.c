/*
 * trace-msg.c : define message protocol for communication between clients and
 *               a server
 *
 * Copyright (C) 2013 Hitachi, Ltd.
 * Created by Yoshihiro YUNOMAE <yoshihiro.yunomae.ez@hitachi.com>
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
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this program; if not,  see <http://www.gnu.org/licenses>
 *
 * ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
 */

#include <errno.h>
#include <poll.h>
#include <fcntl.h>
#include <limits.h>
#include <stddef.h>
#include <stdio.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/types.h>
#include <linux/types.h>

#include "trace-cmd-local.h"
#include "trace-msg.h"

typedef __u32 u32;
typedef __be32 be32;

#define TRACECMD_MSG_MAX_LEN		BUFSIZ

					/* size + cmd */
#define TRACECMD_MSG_HDR_LEN		((sizeof(be32)) + (sizeof(be32)))

					/* + size of the metadata */
#define TRACECMD_MSG_META_MIN_LEN	\
				((TRACECMD_MSG_HDR_LEN) + (sizeof(be32)))

					/* - header size for error msg */
#define TRACECMD_MSG_META_MAX_LEN	\
((TRACECMD_MSG_MAX_LEN) - (TRACECMD_MSG_META_MIN_LEN) - TRACECMD_MSG_HDR_LEN)

					/* size + opt_cmd + size of str */
#define TRACECMD_OPT_MIN_LEN		\
			((sizeof(be32)) + (sizeof(be32)) + (sizeof(be32)))

#define TRACECMD_MSG_SVR_RECORD_HDR_LEN  (TRACECMD_MSG_HDR_LEN + sizeof(be32))
#define TRACECMD_MSG_SVR_RECORD_MIN_LEN  (TRACECMD_MSG_SVR_RECORD_HDR_LEN)


#define CPU_MAX				256

/* for both client and server */
bool use_tcp;
int cpu_count;

/* for client */
static int psfd;
unsigned int page_size;
int *client_ports;
bool send_metadata;

/* for server */
static int *port_array;
bool done;

struct tracecmd_msg_str {
	be32 size;
	char *buf;
} __attribute__((packed));

struct tracecmd_msg_opt {
	be32 size;
	be32 opt_cmd;
	struct tracecmd_msg_str str;
};

struct tracecmd_msg_tinit {
	be32 cpus;
	be32 page_size;
	be32 opt_num;
	struct tracecmd_msg_opt *opt;
} __attribute__((packed));

struct tracecmd_msg_rinit {
	be32 cpus;
	be32 port_array[CPU_MAX];
} __attribute__((packed));

struct tracecmd_msg_meta {
	struct tracecmd_msg_str str;
};

struct tracecmd_msg_error {
	be32 size;
	be32 cmd;
	union {
		struct tracecmd_msg_tinit tinit;
		struct tracecmd_msg_rinit rinit;
		struct tracecmd_msg_meta meta;
	} data;
} __attribute__((packed));

struct tracecmd_msg_svr_record_req {
	/* length of param[] */
	be32 size;
	char param[0];
} __attribute__((packed));

enum tracecmd_msg_cmd {
	MSG_CLOSE	= 1,
	MSG_TINIT	= 4,
	MSG_RINIT	= 5,
	MSG_SENDMETA	= 6,
	MSG_FINMETA	= 7,

	/* trace-cmd server msgs */
	MSG_SVR_RECORD_REQ = 8,
	MSG_SVR_RECORD_ACK = 9,

	MSG_MAX,
};

struct tracecmd_msg {
	be32 size;
	be32 cmd;
	union {
		struct tracecmd_msg_tinit tinit;
		struct tracecmd_msg_rinit rinit;
		struct tracecmd_msg_meta meta;
		struct tracecmd_msg_error err;
		struct tracecmd_msg_svr_record_req rec_req;
	} data;
} __attribute__((packed));

struct tracecmd_msg *errmsg;

static ssize_t msg_do_write_check(int fd, struct tracecmd_msg *msg)
{
	return __do_write_check(fd, msg, ntohl(msg->size));
}

static void tracecmd_msg_init(u32 cmd, u32 len, struct tracecmd_msg *msg)
{
	memset(msg, 0, len);
	msg->size = htonl(len);
	msg->cmd = htonl(cmd);
}

static int tracecmd_msg_alloc(u32 cmd, u32 len, struct tracecmd_msg **msg)
{
	len += TRACECMD_MSG_HDR_LEN;
	*msg = malloc(len);
	if (!*msg)
		return -ENOMEM;

	tracecmd_msg_init(cmd, len, *msg);
	return 0;
}

static int msgcpy(struct tracecmd_msg *msg, u32 offset,
		   const void *buf, u32 buflen)
{
	if (offset + buflen > ntohl(msg->size))
		return -EINVAL;

	memcpy(((void *)msg)+offset, buf, buflen);
	return 0;
}

static int optcpy(struct tracecmd_msg_opt *opt, u32 offset,
		   const void *buf, u32 buflen)
{
	if (offset + buflen > ntohl(opt->size))
		return -EINVAL;

	memcpy(((void *)opt)+offset, buf, buflen);
	return 0;
}

enum msg_opt_command {
	MSGOPT_USETCP = 1,
};

static int add_option_to_tinit(u32 cmd, const char *buf,
			       struct tracecmd_msg *msg, int offset)
{
	struct tracecmd_msg_opt *opt;
	u32 len = TRACECMD_OPT_MIN_LEN;
	u32 buflen = 0;

	if (buf) {
		buflen = strlen(buf);
		len += buflen;
	}

	opt = malloc(len);
	if (!opt)
		return -ENOMEM;

	opt->size = htonl(len);
	opt->opt_cmd = htonl(cmd);
	opt->str.size = htonl(buflen);

	if (buf)
		optcpy(opt, TRACECMD_OPT_MIN_LEN, buf, buflen);

	/* add option to msg */
	msgcpy(msg, offset, opt, ntohl(opt->size));

	free(opt);
	return len;
}

static int add_options_to_tinit(struct tracecmd_msg *msg)
{
	int offset = offsetof(struct tracecmd_msg, data.tinit.opt);
	int ret;

	if (use_tcp) {
		ret = add_option_to_tinit(MSGOPT_USETCP, NULL, msg, offset);
		if (ret < 0)
			return ret;
	}

	return 0;
}

static int make_tinit(struct tracecmd_msg *msg)
{
	int opt_num = 0;
	int ret = 0;

	if (use_tcp)
		opt_num++;

	if (opt_num) {
		ret = add_options_to_tinit(msg);
		if (ret < 0)
			return ret;
	}

	msg->data.tinit.cpus = htonl(cpu_count);
	msg->data.tinit.page_size = htonl(page_size);
	msg->data.tinit.opt_num = htonl(opt_num);

	return 0;
}

static int make_rinit(struct tracecmd_msg *msg)
{
	int i;
	u32 offset = TRACECMD_MSG_HDR_LEN;
	be32 port;

	msg->data.rinit.cpus = htonl(cpu_count);

	for (i = 0; i < cpu_count; i++) {
		/* + rrqports->cpus or rrqports->port_array[i] */
		offset += sizeof(be32);
		port = htonl(port_array[i]);
		msgcpy(msg, offset, &port, sizeof(be32) * cpu_count);
	}

	return 0;
}

static u32 tracecmd_msg_get_body_length(u32 cmd)
{
	struct tracecmd_msg *msg;
	u32 len = 0;

	switch (cmd) {
	case MSG_TINIT:
		len = sizeof(msg->data.tinit.cpus)
		      + sizeof(msg->data.tinit.page_size)
		      + sizeof(msg->data.tinit.opt_num);

		/*
		 * If we are using IPV4 and our page size is greater than
		 * or equal to 64K, we need to punt and use TCP. :-(
		 */

		/* TODO, test for ipv4 */
		if (page_size >= UDP_MAX_PACKET) {
			warning("page size too big for UDP using TCP in live read");
			use_tcp = true;
		}

		if (use_tcp)
			len += TRACECMD_OPT_MIN_LEN;

		return len;
	case MSG_RINIT:
		return sizeof(msg->data.rinit.cpus)
		       + sizeof(msg->data.rinit.port_array);
	case MSG_SENDMETA:
	case MSG_SVR_RECORD_REQ:
		return TRACECMD_MSG_MAX_LEN - TRACECMD_MSG_HDR_LEN;
	case MSG_CLOSE:
	case MSG_FINMETA:
	case MSG_SVR_RECORD_ACK:
		break;
	}

	return 0;
}

static int tracecmd_msg_make_body(u32 cmd, struct tracecmd_msg *msg)
{
	switch (cmd) {
	case MSG_TINIT:
		return make_tinit(msg);
	case MSG_RINIT:
		return make_rinit(msg);
	case MSG_CLOSE:
	case MSG_SENDMETA: /* meta data is not stored here. */
	case MSG_FINMETA:
	case MSG_SVR_RECORD_REQ:
	case MSG_SVR_RECORD_ACK:
		break;
	}

	return 0;
}

static int tracecmd_msg_create(u32 cmd, struct tracecmd_msg **msg)
{
	u32 len = 0;
	int ret = 0;

	len = tracecmd_msg_get_body_length(cmd);
	if (len > (TRACECMD_MSG_MAX_LEN - TRACECMD_MSG_HDR_LEN)) {
		plog("Exceed maximum message size cmd=%d\n", cmd);
		return -EINVAL;
	}

	ret = tracecmd_msg_alloc(cmd, len, msg);
	if (ret < 0)
		return ret;

	ret = tracecmd_msg_make_body(cmd, *msg);
	if (ret < 0)
		free(*msg);

	return ret;
}

static int tracecmd_msg_send(int fd, u32 cmd)
{
	struct tracecmd_msg *msg = NULL;
	int ret = 0;

	if (cmd >= MSG_MAX) {
		plog("Unsupported command: %d\n", cmd);
		return -EINVAL;
	}

	ret = tracecmd_msg_create(cmd, &msg);
	if (ret < 0)
		return ret;

	ret = msg_do_write_check(fd, msg);
	if (ret < 0)
		ret = -ECOMM;

	free(msg);
	return ret;
}

static int tracecmd_msg_read_extra(int fd, void *buf, u32 size, int *n)
{
	int r = 0;

	do {
		r = read(fd, buf + *n, size);
		if (r < 0) {
			if (errno == EINTR)
				continue;
			return -errno;
		} else if (!r)
			return -ENOTCONN;
		size -= r;
		*n += r;
	} while (size);

	return 0;
}

/*
 * Read header information of msg first, then read all data
 */
static int tracecmd_msg_recv(int fd, struct tracecmd_msg *msg)
{
	u32 size = 0;
	int n = 0;
	int ret;

	ret = tracecmd_msg_read_extra(fd, msg, TRACECMD_MSG_HDR_LEN, &n);
	if (ret < 0)
		return ret;

	size = ntohl(msg->size);
	if (size > TRACECMD_MSG_MAX_LEN)
		/* too big */
		goto error;
	else if (size < TRACECMD_MSG_HDR_LEN)
		/* too small */
		goto error;
	else if (size > TRACECMD_MSG_HDR_LEN) {
		size -= TRACECMD_MSG_HDR_LEN;
		return tracecmd_msg_read_extra(fd, msg, size, &n);
	}

	return 0;
error:
	plog("Receive an invalid message(size=%d)\n", size);
	return -ENOMSG;
}

#define MSG_WAIT_MSEC	5000
static int msg_wait_to = MSG_WAIT_MSEC;

void tracecmd_msg_set_debug(int debug)
{
	if (debug)
		msg_wait_to = -1;
	else
		msg_wait_to = MSG_WAIT_MSEC;
}

/*
 * A return value of 0 indicates time-out
 */
static int tracecmd_msg_recv_wait(int fd, struct tracecmd_msg *msg)
{
	struct pollfd pfd;
	int ret;

	pfd.fd = fd;
	pfd.events = POLLIN;
	ret = poll(&pfd, 1, msg_wait_to);
	if (ret < 0)
		return -errno;
	else if (ret == 0)
		return -ETIMEDOUT;

	return tracecmd_msg_recv(fd, msg);
}

static void *tracecmd_msg_buf_access(struct tracecmd_msg *msg, int offset)
{
	return (void *)msg + offset;
}

static int tracecmd_msg_wait_for_msg(int fd, struct tracecmd_msg *msg)
{
	u32 cmd;
	int ret;

	ret = tracecmd_msg_recv_wait(fd, msg);
	if (ret < 0) {
		if (ret == -ETIMEDOUT)
			warning("Connection timed out\n");
		return ret;
	}

	cmd = ntohl(msg->cmd);
	if (cmd == MSG_CLOSE)
		return -ECONNABORTED;

	return 0;
}

static int tracecmd_msg_send_and_wait_for_msg(int fd, u32 cmd, struct tracecmd_msg *msg)
{
	int ret;

	ret = tracecmd_msg_send(fd, cmd);
	if (ret < 0)
		return ret;

	ret = tracecmd_msg_wait_for_msg(fd, msg);
	if (ret < 0)
		return ret;

	return 0;
}

int tracecmd_msg_send_init_data(int fd)
{
	char buf[TRACECMD_MSG_MAX_LEN];
	struct tracecmd_msg *msg;
	int i, cpus;
	int ret;

	msg = (struct tracecmd_msg *)buf;
	ret = tracecmd_msg_send_and_wait_for_msg(fd, MSG_TINIT, msg);
	if (ret < 0)
		return ret;

	cpus = ntohl(msg->data.rinit.cpus);
	client_ports = malloc_or_die(sizeof(int) * cpus);
	for (i = 0; i < cpus; i++)
		client_ports[i] = ntohl(msg->data.rinit.port_array[i]);

	/* Next, send meta data */
	send_metadata = true;

	return 0;
}

static bool process_option(struct tracecmd_msg_opt *opt)
{
	/* currently the only option we have is to us TCP */
	if (ntohl(opt->opt_cmd) == MSGOPT_USETCP) {
		use_tcp = true;
		return true;
	}
	return false;
}

static void error_operation_for_server(struct tracecmd_msg *msg)
{
	u32 cmd;

	cmd = ntohl(msg->cmd);

	warning("Message: cmd=%d size=%d\n", cmd, ntohl(msg->size));
}

#define MAX_OPTION_SIZE 4096

int tracecmd_msg_initial_setting(int fd, int *cpus, int *pagesize)
{
	struct tracecmd_msg *msg;
	struct tracecmd_msg_opt *opt;
	char buf[TRACECMD_MSG_MAX_LEN];
	int offset = offsetof(struct tracecmd_msg, data.tinit.opt);
	int options, i, s;
	int ret;
	u32 size = 0;
	u32 cmd;

	msg = (struct tracecmd_msg *)buf;
	ret = tracecmd_msg_recv_wait(fd, msg);
	if (ret < 0) {
		if (ret == -ETIMEDOUT)
			warning("Connection timed out\n");
		return ret;
	}

	cmd = ntohl(msg->cmd);
	if (cmd != MSG_TINIT) {
		ret = -EINVAL;
		goto error;
	}

	*cpus = ntohl(msg->data.tinit.cpus);
	plog("cpus=%d\n", *cpus);
	if (*cpus < 0) {
		ret = -EINVAL;
		goto error;
	}

	*pagesize = ntohl(msg->data.tinit.page_size);
	plog("pagesize=%d\n", *pagesize);
	if (*pagesize <= 0) {
		ret = -EINVAL;
		goto error;
	}

	options = ntohl(msg->data.tinit.opt_num);
	for (i = 0; i < options; i++) {
		offset += size;
		opt = tracecmd_msg_buf_access(msg, offset);
		size = ntohl(opt->size);
		/* prevent a client from killing us */
		if (size > MAX_OPTION_SIZE) {
			plog("Exceed MAX_OPTION_SIZE\n");
			ret = -EINVAL;
			goto error;
		}
		s = process_option(opt);
		/* do we understand this option? */
		if (!s) {
			plog("Cannot understand(%d:%d:%d)\n",
			     i, ntohl(opt->size), ntohl(opt->opt_cmd));
			ret = -EINVAL;
			goto error;
		}
	}

	return 0;

error:
	error_operation_for_server(msg);
	return ret;
}

int tracecmd_msg_send_port_array(int fd, int total_cpus, int *ports)
{
	int ret;

	cpu_count = total_cpus;
	port_array = ports;

	ret = tracecmd_msg_send(fd, MSG_RINIT);
	if (ret < 0)
		return ret;

	return 0;
}

void tracecmd_msg_send_close_msg(void)
{
	tracecmd_msg_send(psfd, MSG_CLOSE);
}

static void make_meta(const char *buf, int buflen, struct tracecmd_msg *msg)
{
	int offset = offsetof(struct tracecmd_msg, data.meta.str.buf);

	msg->data.meta.str.size = htonl(buflen);
	msgcpy(msg, offset, buf, buflen);
}

int tracecmd_msg_metadata_send(int fd, const char *buf, int size)
{
	struct tracecmd_msg *msg;
	int n, len;
	int ret;
	int count = 0;

	ret = tracecmd_msg_create(MSG_SENDMETA, &msg);
	if (ret < 0)
		return ret;

	n = size;
	do {
		if (n > TRACECMD_MSG_META_MAX_LEN) {
			make_meta(buf+count, TRACECMD_MSG_META_MAX_LEN, msg);
			n -= TRACECMD_MSG_META_MAX_LEN;
			count += TRACECMD_MSG_META_MAX_LEN;
		} else {
			make_meta(buf+count, n, msg);
			/*
			 * TRACECMD_MSG_META_MAX_LEN is stored in msg->size,
			 * so update the size to the correct value.
			 */
			len = TRACECMD_MSG_META_MIN_LEN + n;
			msg->size = htonl(len);
			n = 0;
		}

		ret = msg_do_write_check(fd, msg);
		if (ret < 0)
			break;
	} while (n);

	free(msg);
	return ret;
}

int tracecmd_msg_finish_sending_metadata(int fd)
{
	int ret;

	ret = tracecmd_msg_send(fd, MSG_FINMETA);
	if (ret < 0)
		return ret;

	/* psfd will be used for closing */
	psfd = fd;
	return 0;
}

int tracecmd_msg_collect_metadata(int ifd, int ofd)
{
	struct tracecmd_msg *msg;
	char buf[TRACECMD_MSG_MAX_LEN];
	u32 s, t, n, cmd;
	int offset = TRACECMD_MSG_META_MIN_LEN;
	int ret;

	msg = (struct tracecmd_msg *)buf;

	do {
		ret = tracecmd_msg_recv_wait(ifd, msg);
		if (ret < 0) {
			if (ret == -ETIMEDOUT)
				warning("Connection timed out\n");
			else
				warning("reading client");
			return ret;
		}

		cmd = ntohl(msg->cmd);
		if (cmd == MSG_FINMETA) {
			/* Finish receiving meta data */
			break;
		} else if (cmd != MSG_SENDMETA)
			goto error;

		n = ntohl(msg->data.meta.str.size);
		t = n;
		s = 0;
		do {
			s = write(ofd, buf+s+offset, t);
			if (s < 0) {
				if (errno == EINTR)
					continue;
				warning("writing to file");
				return -errno;
			}
			t -= s;
			s = n - t;
		} while (t);
	} while (cmd == MSG_SENDMETA);

	/* check the finish message of the client */
	while (!done) {
		ret = tracecmd_msg_recv(ifd, msg);
		if (ret < 0) {
			warning("reading client");
			return ret;
		}

		msg = (struct tracecmd_msg *)buf;
		cmd = ntohl(msg->cmd);
		if (cmd == MSG_CLOSE)
			/* Finish this connection */
			break;
		else {
			warning("Not accept the message %d", ntohl(msg->cmd));
			ret = -EINVAL;
			goto error;
		}
	}

	return 0;

error:
	error_operation_for_server(msg);
	return ret;
}

/* Returns param string for "record". E.g.:
 *
 * "-e syscall:sys_enter_open -l hrtimer_interrupt"
 *
 * Now it's still hacky. We can enhance the protocol and the
 * interface in the future. Parameter needs to be freed after use.
 */
char *tracecmd_msg_svr_handle_record_req(int fd)
{
	be32 cmd;
	char buf[TRACECMD_MSG_MAX_LEN];
	int ret, size;
	char *param;
	struct tracecmd_msg *msg = (struct tracecmd_msg *)buf;

	ret = tracecmd_msg_recv_wait(fd, msg);

	cmd = ntohl(msg->cmd);
	if (cmd != MSG_SVR_RECORD_REQ) {
		plog("trace-cmd server received unknown cmd %d\n", cmd);
		return NULL;
	}

	/* We got a tracecmd msg with a tracing request */

	size = ntohl(msg->size);
	if (size <= TRACECMD_MSG_SVR_RECORD_MIN_LEN) {
		plog("MSG_SVR_RECORD_REQ size %u too small\n", size);
		return NULL;
	}

	param = msg->data.rec_req.param;

	plog("RECORD_REQUEST GOT: '%s'\n", param);

	/* send MSG_SVR_RECORD_ACK */
	ret = tracecmd_msg_send(fd, MSG_SVR_RECORD_ACK);
	if (ret < 0) {
		plog("Failed to send MSG_SVR_RECORD_ACK\n");
		return NULL;
	}

	return strdup(param);
}

int tracecmd_msg_svr_send_record_req(int fd, char *param)
{
	be32 param_len = strlen(param) + 1;
	be32 cmd;
	struct tracecmd_msg *msg;
	int ret = 0;

	ret = tracecmd_msg_create(MSG_SVR_RECORD_REQ, &msg);
	if (ret)
		return -1;

	/* fill up message body */
	msg->data.rec_req.size = htonl(param_len);
	memcpy(msg->data.rec_req.param, param, param_len);

	/* send */
	ret = msg_do_write_check(fd, msg);
	if (ret < 0)
		goto out;

	/* HACK: we did not free the msg, let's use the same buffer
	 * for recving MSG_SVR_RECORD_ACK. */
	ret = tracecmd_msg_wait_for_msg(fd, msg);
	if (ret < 0)
		goto out;

	cmd = ntohl(msg->cmd);
	if (cmd != MSG_SVR_RECORD_ACK) {
		printf("Failed to get MSG_SVR_RECORD_ACK, but %u\n", cmd);
		goto out;
	}

out:
	free(msg);
	return ret;
}

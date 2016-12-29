/*-
 * Copyright 2016 Vsevolod Stakhov
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#include "lua_common.h"
#include "dns.h"
#include "utlist.h"
#include "ref.h"
#include "unix-std.h"

/***
 * @module rspamd_tcp
 * Rspamd TCP module represents generic TCP asynchronous client available from LUA code.
 * This module hides all complexity: DNS resolving, sessions management, zero-copy
 * text transfers and so on under the hood. It can work in partial or complete modes:
 *
 * - partial mode is used when you need to call a continuation routine each time data is available for read
 * - complete mode calls for continuation merely when all data is read from socket (e.g. when a server sends reply and closes a connection)
 * @example
local logger = require "rspamd_logger"
local tcp = require "rspamd_tcp"

rspamd_config.SYM = function(task)

    local function cb(err, data)
        logger.infox('err: %1, data: %2', err, tostring(data))
    end

    tcp.request({
    	task = task,
    	host = "google.com",
    	port = 80,
    	data = {"GET / HTTP/1.0\r\n", "Host: google.com\r\n", "\r\n"},
    	callback = cb})
end
 */

LUA_FUNCTION_DEF (tcp, request);

/***
 * @method tcp:close()
 *
 * Closes TCP connection
 */
LUA_FUNCTION_DEF (tcp, close);
/***
 * @method tcp:set_timeout(seconds)
 *
 * Sets new timeout for a TCP connection in **seconds**
 * @param {number} seconds floating point value that specifies new timeout
 */
LUA_FUNCTION_DEF (tcp, set_timeout);

static const struct luaL_reg tcp_libf[] = {
	LUA_INTERFACE_DEF (tcp, request),
	{"new", lua_tcp_request},
	{"connect", lua_tcp_request},
	{NULL, NULL}
};

static const struct luaL_reg tcp_libm[] = {
	LUA_INTERFACE_DEF (tcp, close),
	LUA_INTERFACE_DEF (tcp, set_timeout),
	{"__tostring", rspamd_lua_class_tostring},
	{NULL, NULL}
};

struct lua_tcp_read_handler {
	gchar *stop_pattern;
	gint cbref;
};

struct lua_tcp_write_handler {
	struct iovec *iov;
	guint iovlen;
	guint pos;
	guint total;
	gint cbref;
};

enum lua_tcp_handler_type {
	LUA_WANT_WRITE = 0,
	LUA_WANT_READ,
};

struct lua_tcp_handler {
	union {
		struct lua_tcp_read_handler r;
		struct lua_tcp_write_handler w;
	} h;
	enum lua_tcp_handler_type type;
};

struct lua_tcp_dtor {
	rspamd_mempool_destruct_t dtor;
	void *data;
	struct lua_tcp_dtor *next;
};

#define LUA_TCP_FLAG_PARTIAL (1 << 0)
#define LUA_TCP_FLAG_SHUTDOWN (1 << 2)
#define LUA_TCP_FLAG_CONNECTED (1 << 3)

struct lua_tcp_cbdata {
	lua_State *L;
	struct rspamd_async_session *session;
	struct event_base *ev_base;
	struct timeval tv;
	rspamd_inet_addr_t *addr;
	GByteArray *in;
	GQueue *handlers;
	gint fd;
	gint connect_cb;
	guint port;
	guint flags;
	struct rspamd_async_watcher *w;
	struct event ev;
	struct lua_tcp_dtor *dtors;
	ref_entry_t ref;
};

static void lua_tcp_handler (int fd, short what, gpointer ud);
static void lua_tcp_plan_handler_event (struct lua_tcp_cbdata *cbd,
		gboolean can_read, gboolean can_write);

static const int default_tcp_timeout = 5000;

static struct rspamd_dns_resolver *
lua_tcp_global_resolver (struct event_base *ev_base)
{
	static struct rspamd_dns_resolver *global_resolver;

	if (global_resolver == NULL) {
		global_resolver = dns_resolver_init (NULL, ev_base, NULL);
	}

	return global_resolver;
}

static gboolean
lua_tcp_shift_handler (struct lua_tcp_cbdata *cbd)
{
	struct lua_tcp_handler *hdl;

	hdl = g_queue_pop_head (cbd->handlers);

	if (hdl == NULL) {
		/* We are done */
		return FALSE;
	}

	if (hdl->type == LUA_WANT_READ) {
		if (hdl->h.r.cbref) {
			luaL_unref (cbd->L, LUA_REGISTRYINDEX, hdl->h.r.cbref);
		}

		if (hdl->h.r.stop_pattern) {
			g_free (hdl->h.r.stop_pattern);
		}
	}
	else {
		if (hdl->h.w.cbref) {
			luaL_unref (cbd->L, LUA_REGISTRYINDEX, hdl->h.w.cbref);
		}

		if (hdl->h.w.iov) {
			g_free (hdl->h.w.iov);
		}
	}

	g_slice_free1 (sizeof (*hdl), hdl);

	return TRUE;
}

static void
lua_tcp_fin (gpointer arg)
{
	struct lua_tcp_cbdata *cbd = (struct lua_tcp_cbdata *)arg;
	struct lua_tcp_dtor *dtor, *dttmp;

	if (cbd->connect_cb) {
		luaL_unref (cbd->L, LUA_REGISTRYINDEX, cbd->connect_cb);
	}

	if (cbd->fd != -1) {
		event_del (&cbd->ev);
		close (cbd->fd);
	}

	if (cbd->addr) {
		rspamd_inet_address_destroy (cbd->addr);
	}

	while (lua_tcp_shift_handler (cbd)) {}

	LL_FOREACH_SAFE (cbd->dtors, dtor, dttmp) {
		dtor->dtor (dtor->data);
		g_slice_free1 (sizeof (*dtor), dtor);
	}

	g_byte_array_unref (cbd->in);
	g_slice_free1 (sizeof (struct lua_tcp_cbdata), cbd);
}

static struct lua_tcp_cbdata *
lua_check_tcp (lua_State *L, gint pos)
{
	void *ud = rspamd_lua_check_udata (L, pos, "rspamd{tcp}");
	luaL_argcheck (L, ud != NULL, pos, "'tcp' expected");
	return ud ? *((struct lua_tcp_cbdata **)ud) : NULL;
}

static void
lua_tcp_maybe_free (struct lua_tcp_cbdata *cbd)
{
	if (cbd->session) {
		rspamd_session_watcher_pop (cbd->session, cbd->w);
		rspamd_session_remove_event (cbd->session, lua_tcp_fin, cbd);
	}
	else {
		lua_tcp_fin (cbd);
	}
}

static void
lua_tcp_push_error (struct lua_tcp_cbdata *cbd, const char *err, ...)
{
	va_list ap;
	struct lua_tcp_cbdata **pcbd;
	struct lua_tcp_handler *hdl;
	gint cbref;

	hdl = g_queue_peek_head (cbd->handlers);

	g_assert (hdl != NULL);

	if (hdl->type == LUA_WANT_READ) {
		cbref = hdl->h.r.cbref;
	}
	else {
		cbref = hdl->h.w.cbref;
	}

	if (cbref != -1) {
		lua_rawgeti (cbd->L, LUA_REGISTRYINDEX, cbref);

		/* Error message */
		va_start (ap, err);
		lua_pushvfstring (cbd->L, err, ap);
		va_end (ap);

		/* Body */
		lua_pushnil (cbd->L);
		/* Connection */
		pcbd = lua_newuserdata (cbd->L, sizeof (*pcbd));
		*pcbd = cbd;
		rspamd_lua_setclass (cbd->L, "rspamd{tcp}", -1);
		REF_RETAIN (cbd);

		if (lua_pcall (cbd->L, 3, 0, 0) != 0) {
			msg_info ("callback call failed: %s", lua_tostring (cbd->L, -1));
			lua_pop (cbd->L, 1);
		}
	}

	REF_RELEASE (cbd);
}

static void
lua_tcp_push_data (struct lua_tcp_cbdata *cbd, const guint8 *str, gsize len)
{
	struct rspamd_lua_text *t;
	struct lua_tcp_cbdata **pcbd;
	struct lua_tcp_handler *hdl;
	gint cbref;

	hdl = g_queue_peek_head (cbd->handlers);

	g_assert (hdl != NULL);

	if (hdl->type == LUA_WANT_READ) {
		cbref = hdl->h.r.cbref;
	}
	else {
		cbref = hdl->h.w.cbref;
	}

	if (cbref != -1) {
		lua_rawgeti (cbd->L, LUA_REGISTRYINDEX, cbref);
		/* Error */
		lua_pushnil (cbd->L);
		/* Body */

		if (hdl->type == LUA_WANT_READ) {
			t = lua_newuserdata (cbd->L, sizeof (*t));
			rspamd_lua_setclass (cbd->L, "rspamd{text}", -1);
			t->start = (const gchar *)str;
			t->len = len;
			t->flags = 0;
		}
		/* Connection */
		pcbd = lua_newuserdata (cbd->L, sizeof (*pcbd));
		*pcbd = cbd;
		rspamd_lua_setclass (cbd->L, "rspamd{tcp}", -1);

		REF_RETAIN (cbd);

		if (lua_pcall (cbd->L, 3, 0, 0) != 0) {
			msg_info ("callback call failed: %s", lua_tostring (cbd->L, -1));
			lua_pop (cbd->L, 1);
		}
	}

	REF_RELEASE (cbd);
}

static void
lua_tcp_plan_read (struct lua_tcp_cbdata *cbd)
{
	event_del (&cbd->ev);
#ifdef EV_CLOSED
	event_set (&cbd->ev, cbd->fd, EV_READ|EV_PERSIST|EV_CLOSED,
				lua_tcp_handler, cbd);
#else
	event_set (&cbd->ev, cbd->fd, EV_READ|EV_PERSIST, lua_tcp_handler, cbd);
#endif
	event_base_set (cbd->ev_base, &cbd->ev);
	event_add (&cbd->ev, &cbd->tv);
}

static void
lua_tcp_write_helper (struct lua_tcp_cbdata *cbd)
{
	struct iovec *start;
	guint niov, i;
	gint flags = 0;
	gsize remain;
	gssize r;
	struct iovec *cur_iov;
	struct lua_tcp_handler *hdl;
	struct lua_tcp_write_handler *wh;
	struct msghdr msg;

	hdl = g_queue_peek_head (cbd->handlers);

	g_assert (hdl != NULL && hdl->type == LUA_WANT_WRITE);
	wh = &hdl->h.w;

	if (wh->pos == wh->total) {
		goto call_finish_handler;
	}

	start = &wh->iov[0];
	niov = wh->iovlen;
	remain = wh->pos;
	/* We know that niov is small enough for that */
	cur_iov = alloca (niov * sizeof (struct iovec));
	memcpy (cur_iov, wh->iov, niov * sizeof (struct iovec));

	for (i = 0; i < wh->iovlen && remain > 0; i++) {
		/* Find out the first iov required */
		start = &cur_iov[i];
		if (start->iov_len <= remain) {
			remain -= start->iov_len;
			start = &cur_iov[i + 1];
			niov--;
		}
		else {
			start->iov_base = (void *)((char *)start->iov_base + remain);
			start->iov_len -= remain;
			remain = 0;
		}
	}

	memset (&msg, 0, sizeof (msg));
	msg.msg_iov = start;
	msg.msg_iovlen = MIN (IOV_MAX, niov);
	g_assert (niov > 0);
#ifdef MSG_NOSIGNAL
	flags = MSG_NOSIGNAL;
#endif
	r = sendmsg (cbd->fd, &msg, flags);

	if (r == -1) {
		lua_tcp_push_error (cbd, "IO write error while trying to write %d "
				"bytes: %s", (gint)remain, strerror (errno));
		lua_tcp_shift_handler (cbd);
		lua_tcp_plan_handler_event (cbd, TRUE, FALSE);

		return;
	}
	else {
		wh->pos += r;
	}

	if (wh->pos >= wh->total) {
		goto call_finish_handler;
	}
	else {
		/* Want to write more */
		event_add (&cbd->ev, &cbd->tv);
	}

	return;

call_finish_handler:

	if ((cbd->flags & LUA_TCP_FLAG_SHUTDOWN)) {
		/* Half close the connection */
		shutdown (cbd->fd, SHUT_WR);
		cbd->flags &= ~LUA_TCP_FLAG_SHUTDOWN;
	}

	lua_tcp_push_data (cbd, NULL, 0);
	lua_tcp_shift_handler (cbd);
	lua_tcp_plan_handler_event (cbd, TRUE, TRUE);
}

static gboolean
lua_tcp_process_read_handler (struct lua_tcp_cbdata *cbd,
		struct lua_tcp_read_handler *rh)
{
	guint slen;
	goffset pos;

	if (rh->stop_pattern) {
		slen = strlen (rh->stop_pattern);

		if (cbd->in->len >= slen) {
			if ((pos = rspamd_substring_search (cbd->in->data, cbd->in->len,
					rh->stop_pattern, slen)) != -1) {
				lua_tcp_push_data (cbd, cbd->in->data, pos);

				if (pos + slen < cbd->in->len) {
					/* We have a leftover */
					memmove (cbd->in->data, cbd->in->data + pos + slen,
							cbd->in->len - (pos + slen));
					lua_tcp_shift_handler (cbd);
				}
				else {
					lua_tcp_shift_handler (cbd);

					return TRUE;
				}
			}
			else {
				/* Plan new read */
				lua_tcp_plan_read (cbd);
			}
		}
	}
	else {
		lua_tcp_push_data (cbd, cbd->in->data, cbd->in->len);
		lua_tcp_shift_handler (cbd);

		return TRUE;
	}

	return FALSE;
}

static void
lua_tcp_process_read (struct lua_tcp_cbdata *cbd,
		guchar *in, gssize r)
{
	struct lua_tcp_handler *hdl;
	struct lua_tcp_read_handler *rh;

	hdl = g_queue_peek_head (cbd->handlers);

	g_assert (hdl != NULL && hdl->type == LUA_WANT_READ);
	rh = &hdl->h.r;

	if (r > 0) {
		if (cbd->flags & LUA_TCP_FLAG_PARTIAL) {
			lua_tcp_push_data (cbd, in, r);
			/* Plan next event */
			lua_tcp_shift_handler (cbd);
			lua_tcp_shift_handler (cbd);
		}
		else {
			g_byte_array_append (cbd->in, in, r);

			if (!lua_tcp_process_read_handler (cbd, rh)) {
				/* Plan more read */
				lua_tcp_plan_read (cbd);
			}
			else {
				/* Go towards the next handler */
				lua_tcp_plan_handler_event (cbd, TRUE, TRUE);
			}
		}
	}
	else if (r == 0) {
		/* EOF */
		if (cbd->in->len > 0) {
			/* We have some data to process */
			lua_tcp_process_read_handler (cbd, rh);
		}
		else {
			lua_tcp_push_error (cbd, "IO read error: connection terminated");
		}

		lua_tcp_plan_handler_event (cbd, FALSE, TRUE);
	}
	else {
		/* An error occurred */
		if (errno == EAGAIN || errno == EINTR) {
			/* Restart call */
			lua_tcp_plan_read (cbd);

			return;
		}

		/* Fatal error */
		lua_tcp_push_error (cbd, "IO read error while trying to read data: %s",
				strerror (errno));

		REF_RELEASE (cbd);
	}
}

static void
lua_tcp_handler (int fd, short what, gpointer ud)
{
	struct lua_tcp_cbdata *cbd = ud;
	guchar inbuf[8192];
	gssize r;
	gint so_error = 0;
	socklen_t so_len = sizeof (so_error);

	REF_RETAIN (cbd);

	if (what == EV_READ) {
		r = read (cbd->fd, inbuf, sizeof (inbuf));
		lua_tcp_process_read (cbd, inbuf, r);
	}
	else if (what == EV_WRITE) {
		if (!(cbd->flags & LUA_TCP_FLAG_CONNECTED)) {
			if (getsockopt (fd, SOL_SOCKET, SO_ERROR, &so_error, &so_len) == -1) {
				lua_tcp_push_error (cbd, "Cannot get socket error: %s",
						strerror (errno));
				REF_RELEASE (cbd);
				goto out;
			}
			else if (so_error != 0) {
				lua_tcp_push_error (cbd, "Socket error detected: %s",
						strerror (so_error));
				REF_RELEASE (cbd);
				goto out;
			}
			else {
				cbd->flags |= LUA_TCP_FLAG_CONNECTED;

				if (cbd->connect_cb != -1) {
					struct lua_tcp_cbdata **pcbd;

					lua_rawgeti (cbd->L, LUA_REGISTRYINDEX, cbd->connect_cb);
					pcbd = lua_newuserdata (cbd->L, sizeof (*pcbd));
					*pcbd = cbd;
					REF_RETAIN (cbd);
					rspamd_lua_setclass (cbd->L, "rspamd{tcp}", -1);

					if (lua_pcall (cbd->L, 1, 0, 0) != 0) {
						msg_info ("callback call failed: %s", lua_tostring (cbd->L, -1));
						lua_pop (cbd->L, 1);
					}

					REF_RELEASE (cbd);
				}
			}
		}

		lua_tcp_write_helper (cbd);
	}
#ifdef EV_CLOSED
	else if (what == EV_CLOSED) {
		lua_tcp_push_error (cbd, "Remote peer has closed the connection");
		REF_RELEASE (cbd);
	}
#endif
	else {
		lua_tcp_push_error (cbd, "IO timeout");
		REF_RELEASE (cbd);
	}

out:
	REF_RELEASE (cbd);
}

static void
lua_tcp_plan_handler_event (struct lua_tcp_cbdata *cbd, gboolean can_read,
		gboolean can_write)
{
	struct lua_tcp_handler *hdl;

	hdl = g_queue_peek_head (cbd->handlers);

	if (hdl == NULL) {
		/* We are finished with a connection */
		REF_RELEASE (cbd);
	}
	else {
		if (hdl->type == LUA_WANT_READ) {
			/* We need to check if we have some leftover in the buffer */
			if (cbd->in->len > 0) {
				if (lua_tcp_process_read_handler (cbd, &hdl->h.r)) {
					/* We can go to the next handler */
					lua_tcp_shift_handler (cbd);
					lua_tcp_plan_handler_event (cbd, can_read, can_write);
				}
			}
			else {
				if (can_read) {
					/* We need to plan a new event */
					event_set (&cbd->ev, cbd->fd, EV_READ, lua_tcp_handler, cbd);
					event_base_set (cbd->ev_base, &cbd->ev);
					event_add (&cbd->ev, &cbd->tv);
				}
				else {
					/* Cannot read more */
					lua_tcp_push_error (cbd, "EOF, cannot read more data");
					lua_tcp_shift_handler (cbd);
					lua_tcp_plan_handler_event (cbd, can_read, can_write);
				}
			}
		}
		else {
			/*
			 * We need to plan write event if there is something in the
			 * write request
			 */
			if (hdl->h.w.pos < hdl->h.w.total) {
				if (can_write) {
					event_set (&cbd->ev, cbd->fd, EV_WRITE, lua_tcp_handler, cbd);
					event_base_set (cbd->ev_base, &cbd->ev);
					event_add (&cbd->ev, &cbd->tv);
				}
				else {
					/* Cannot read more */
					lua_tcp_push_error (cbd, "EOF, cannot read more data");
					lua_tcp_shift_handler (cbd);
					lua_tcp_plan_handler_event (cbd, can_read, can_write);
				}
			}
			else {
				/* We shouldn't have empty write handlers */
				g_assert_not_reached ();
			}
		}
	}
}

static gboolean
lua_tcp_make_connection (struct lua_tcp_cbdata *cbd)
{
	int fd;

	rspamd_inet_address_set_port (cbd->addr, cbd->port);
	fd = rspamd_inet_address_connect (cbd->addr, SOCK_STREAM, TRUE);

	if (fd == -1) {
		msg_info ("cannot connect to %s", rspamd_inet_address_to_string (cbd->addr));
		return FALSE;
	}

	cbd->fd = fd;
	lua_tcp_plan_handler_event (cbd, TRUE, TRUE);

	return TRUE;
}

static void
lua_tcp_dns_handler (struct rdns_reply *reply, gpointer ud)
{
	struct lua_tcp_cbdata *cbd = (struct lua_tcp_cbdata *)ud;
	const struct rdns_request_name *rn;

	if (reply->code != RDNS_RC_NOERROR) {
		rn = rdns_request_get_name (reply->request, NULL);
		lua_tcp_push_error (cbd, "unable to resolve host: %s",
				rn->name);
		REF_RETAIN (cbd);
	}
	else {
		if (reply->entries->type == RDNS_REQUEST_A) {
			cbd->addr = rspamd_inet_address_new (AF_INET,
					&reply->entries->content.a.addr);
		}
		else if (reply->entries->type == RDNS_REQUEST_AAAA) {
			cbd->addr = rspamd_inet_address_new (AF_INET6,
					&reply->entries->content.aaa.addr);
		}

		rspamd_inet_address_set_port (cbd->addr, cbd->port);

		if (!lua_tcp_make_connection (cbd)) {
			lua_tcp_push_error (cbd, "unable to make connection to the host %s",
					rspamd_inet_address_to_string (cbd->addr));
			REF_RETAIN (cbd);
		}
	}
}

static gboolean
lua_tcp_arg_toiovec (lua_State *L, gint pos, struct lua_tcp_cbdata *cbd,
		struct iovec *vec)
{
	struct rspamd_lua_text *t;
	gsize len;
	const gchar *str;
	struct lua_tcp_dtor *dtor;

	if (lua_type (L, pos) == LUA_TUSERDATA) {
		t = lua_check_text (L, pos);

		if (t) {
			vec->iov_base = (void *)t->start;
			vec->iov_len = t->len;

			if (t->flags & RSPAMD_TEXT_FLAG_OWN) {
				/* Steal ownership */
				t->flags = 0;
				dtor = g_slice_alloc0 (sizeof (*dtor));
				dtor->dtor = g_free;
				dtor->data = (void *)t->start;
				LL_PREPEND (cbd->dtors, dtor);
			}
		}
		else {
			msg_err ("bad userdata argument at position %d", pos);
			return FALSE;
		}
	}
	else if (lua_type (L, pos) == LUA_TSTRING) {
		str = luaL_checklstring (L, pos, &len);
		vec->iov_base = g_malloc (len);
		dtor = g_slice_alloc0 (sizeof (*dtor));
		dtor->dtor = g_free;
		dtor->data = (void *)vec->iov_base;
		memcpy (vec->iov_base, str, len);
		vec->iov_len = len;
	}
	else {
		msg_err ("bad argument at position %d", pos);
		return FALSE;
	}

	return TRUE;
}

/***
 * @function rspamd_tcp.request({params})
 * This function creates and sends TCP request to the specified host and port,
 * resolves hostname (if needed) and invokes continuation callback upon data received
 * from the remote peer. This function accepts table of arguments with the following
 * attributes
 *
 * - `task`: rspamd task objects (implies `pool`, `session`, `ev_base` and `resolver` arguments)
 * - `ev_base`: event base (if no task specified)
 * - `resolver`: DNS resolver (no task)
 * - `session`: events session (no task)
 * - `pool`: memory pool (no task)
 * - `host`: IP or name of the peer (required)
 * - `port`: remote port to use
 * - `data`: a table of strings or `rspamd_text` objects that contains data pieces
 * - `callback`: continuation function (required)
 * - `on_connect`: callback called on connection success
 * - `timeout`: floating point value that specifies timeout for IO operations in **seconds**
 * - `partial`: boolean flag that specifies that callback should be called on any data portion received
 * - `stop_pattern`: stop reading on finding a certain pattern (e.g. \r\n.\r\n for smtp)
 * - `shutdown`: half-close socket after writing (boolean: default false)
 * - `read`: read response after sending request (boolean: default true)
 * @return {boolean} true if request has been sent
 */
static gint
lua_tcp_request (lua_State *L)
{
	const gchar *host;
	gchar *stop_pattern = NULL;
	guint port;
	gint cbref, tp, conn_cbref = -1;
	struct event_base *ev_base;
	struct lua_tcp_cbdata *cbd;
	struct rspamd_dns_resolver *resolver;
	struct rspamd_async_session *session;
	struct rspamd_task *task = NULL;
	struct iovec *iov = NULL;
	guint niov = 0, total_out;
	gdouble timeout = default_tcp_timeout;
	gboolean partial = FALSE, do_shutdown = FALSE, do_read = TRUE;

	if (lua_type (L, 1) == LUA_TTABLE) {
		lua_pushstring (L, "host");
		lua_gettable (L, -2);
		host = luaL_checkstring (L, -1);
		lua_pop (L, 1);

		lua_pushstring (L, "port");
		lua_gettable (L, -2);
		if (lua_type (L, -1) == LUA_TNUMBER) {
			port = luaL_checknumber (L, -1);
		}
		else {
			/* We assume that it is a unix socket */
			port = 0;
		}

		lua_pop (L, 1);

		lua_pushstring (L, "callback");
		lua_gettable (L, -2);
		if (host == NULL || lua_type (L, -1) != LUA_TFUNCTION) {
			lua_pop (L, 1);
			msg_err ("tcp request has bad params");
			lua_pushboolean (L, FALSE);
			return 1;
		}
		cbref = luaL_ref (L, LUA_REGISTRYINDEX);

		cbd = g_slice_alloc0 (sizeof (*cbd));

		lua_pushstring (L, "task");
		lua_gettable (L, -2);
		if (lua_type (L, -1) == LUA_TUSERDATA) {
			task = lua_check_task (L, -1);
			ev_base = task->ev_base;
			resolver = task->resolver;
			session = task->s;
		}
		lua_pop (L, 1);

		if (task == NULL) {
			lua_pushstring (L, "ev_base");
			lua_gettable (L, -2);
			if (rspamd_lua_check_udata_maybe (L, -1, "rspamd{ev_base}")) {
				ev_base = *(struct event_base **)lua_touserdata (L, -1);
			}
			else {
				ev_base = NULL;
			}
			lua_pop (L, 1);

			lua_pushstring (L, "resolver");
			lua_gettable (L, -2);
			if (rspamd_lua_check_udata_maybe (L, -1, "rspamd{resolver}")) {
				resolver = *(struct rspamd_dns_resolver **)lua_touserdata (L, -1);
			}
			else {
				resolver = lua_tcp_global_resolver (ev_base);
			}
			lua_pop (L, 1);

			lua_pushstring (L, "session");
			lua_gettable (L, -2);
			if (rspamd_lua_check_udata_maybe (L, -1, "rspamd{session}")) {
				session = *(struct rspamd_async_session **)lua_touserdata (L, -1);
			}
			else {
				session = NULL;
			}
			lua_pop (L, 1);
		}

		lua_pushstring (L, "timeout");
		lua_gettable (L, -2);
		if (lua_type (L, -1) == LUA_TNUMBER) {
			timeout = lua_tonumber (L, -1) * 1000.;
		}
		lua_pop (L, 1);

		lua_pushstring (L, "stop_pattern");
		lua_gettable (L, -2);
		if (lua_type (L, -1) == LUA_TSTRING) {
			stop_pattern = g_strdup (lua_tostring (L, -1));
		}
		lua_pop (L, 1);

		lua_pushstring (L, "partial");
		lua_gettable (L, -2);
		if (lua_type (L, -1) == LUA_TBOOLEAN) {
			partial = lua_toboolean (L, -1);
		}
		lua_pop (L, 1);

		lua_pushstring (L, "shutdown");
		lua_gettable (L, -2);
		if (lua_type (L, -1) == LUA_TBOOLEAN) {
			do_shutdown = lua_toboolean (L, -1);
		}
		lua_pop (L, 1);

		lua_pushstring (L, "read");
		lua_gettable (L, -2);
		if (lua_type (L, -1) == LUA_TBOOLEAN) {
			do_read = lua_toboolean (L, -1);
		}
		lua_pop (L, 1);

		lua_pushstring (L, "on_connect");
		lua_gettable (L, -2);

		if (lua_type (L, -1) == LUA_TFUNCTION) {
			conn_cbref = luaL_ref (L, LUA_REGISTRYINDEX);
		}
		else {
			lua_pop (L, 1);
		}

		lua_pushstring (L, "data");
		lua_gettable (L, -2);
		total_out = 0;

		tp = lua_type (L, -1);
		if (tp == LUA_TSTRING || tp == LUA_TUSERDATA) {
			iov = g_malloc (sizeof (*iov));
			niov = 1;

			if (!lua_tcp_arg_toiovec (L, -1, cbd, iov)) {
				lua_pop (L, 1);
				msg_err ("tcp request has bad data argument");
				lua_pushboolean (L, FALSE);
				g_free (iov);
				g_slice_free1 (sizeof (*cbd), cbd);

				return 1;
			}

			total_out = iov[0].iov_len;
		}
		else if (tp == LUA_TTABLE) {
			/* Count parts */
			lua_pushnil (L);
			while (lua_next (L, -2) != 0) {
				niov ++;
				lua_pop (L, 1);
			}

			iov = g_malloc (sizeof (*iov) * niov);
			lua_pushnil (L);
			niov = 0;

			while (lua_next (L, -2) != 0) {
				if (!lua_tcp_arg_toiovec (L, -1, cbd, &iov[niov])) {
					lua_pop (L, 2);
					msg_err ("tcp request has bad data argument at pos %d", niov);
					lua_pushboolean (L, FALSE);
					g_free (iov);
					g_slice_free1 (sizeof (*cbd), cbd);

					return 1;
				}

				total_out += iov[niov].iov_len;
				niov ++;

				lua_pop (L, 1);
			}
		}

		lua_pop (L, 1);
	}
	else {
		msg_err ("tcp request has bad params");
		lua_pushboolean (L, FALSE);

		return 1;
	}

	cbd->L = L;

	if (total_out > 0) {
		struct lua_tcp_handler *wh;

		wh = g_slice_alloc0 (sizeof (*wh));
		wh->type = LUA_WANT_WRITE;
		wh->h.w.iov = iov;
		wh->h.w.iovlen = niov;
		wh->h.w.total = total_out;
		wh->h.w.pos = 0;
		/* Cannot set write handler here */
		wh->h.w.cbref = -1;

		if (cbref != -1 && !do_read) {
			/* We have write only callback */
			wh->h.w.cbref = cbref;
		}
		else {
			/* We have simple client callback */
			wh->h.w.cbref = -1;
		}

		g_queue_push_tail (cbd->handlers, wh);
	}

	cbd->ev_base = ev_base;
	msec_to_tv (timeout, &cbd->tv);
	cbd->fd = -1;
	cbd->port = port;

	if (do_read) {
		cbd->in = g_byte_array_sized_new (8192);
	}
	else {
		/* Save some space... */
		cbd->in = g_byte_array_new ();
	}

	if (partial) {
		cbd->flags |= LUA_TCP_FLAG_PARTIAL;
	}

	if (do_shutdown) {
		cbd->flags |= LUA_TCP_FLAG_SHUTDOWN;
	}

	if (do_read) {
		struct lua_tcp_handler *rh;

		rh = g_slice_alloc0 (sizeof (*rh));
		rh->type = LUA_WANT_READ;
		rh->h.r.cbref = cbref;
		rh->h.r.stop_pattern = stop_pattern;
		g_queue_push_tail (cbd->handlers, rh);
	}

	cbd->connect_cb = conn_cbref;
	REF_INIT_RETAIN (cbd, lua_tcp_maybe_free);

	if (session) {
		cbd->session = session;
		rspamd_session_add_event (session,
				(event_finalizer_t)lua_tcp_fin,
				cbd,
				g_quark_from_static_string ("lua tcp"));
		cbd->w = rspamd_session_get_watcher (session);
		rspamd_session_watcher_push (session);
	}

	if (rspamd_parse_inet_address (&cbd->addr, host, 0)) {
		rspamd_inet_address_set_port (cbd->addr, port);
		/* Host is numeric IP, no need to resolve */
		if (!lua_tcp_make_connection (cbd)) {
			REF_RELEASE (cbd);
			lua_pushboolean (L, FALSE);

			return 1;
		}
	}
	else {
		if (task == NULL) {
			if (!make_dns_request (resolver, session, NULL, lua_tcp_dns_handler, cbd,
					RDNS_REQUEST_A, host)) {
				lua_tcp_push_error (cbd, "cannot resolve host: %s", host);
				REF_RETAIN (cbd);
			}
		}
		else {
			if (!make_dns_request_task (task, lua_tcp_dns_handler, cbd,
					RDNS_REQUEST_A, host)) {
				lua_tcp_push_error (cbd, "cannot resolve host: %s", host);
				REF_RELEASE (cbd);
			}
		}
	}

	lua_pushboolean (L, TRUE);
	return 1;
}

static gint
lua_tcp_close (lua_State *L)
{
	struct lua_tcp_cbdata *cbd = lua_check_tcp (L, 1);

	if (cbd == NULL) {
		return luaL_error (L, "invalid arguments");
	}

	REF_RELEASE (cbd);

	return 0;
}

static gint
lua_tcp_set_timeout (lua_State *L)
{
	struct lua_tcp_cbdata *cbd = lua_check_tcp (L, 1);
	gdouble ms = lua_tonumber (L, 2);

	if (cbd == NULL) {
		return luaL_error (L, "invalid arguments");
	}

	ms *= 1000.0;
	double_to_tv (ms, &cbd->tv);

	return 0;
}

static gint
lua_load_tcp (lua_State * L)
{
	lua_newtable (L);
	luaL_register (L, NULL, tcp_libf);

	return 1;
}

void
luaopen_tcp (lua_State * L)
{
	rspamd_lua_add_preload (L, "rspamd_tcp", lua_load_tcp);
	rspamd_lua_new_class (L, "rspamd{tcp}", tcp_libm);
	lua_pop (L, 1);
}

/******************************************************************
Copyright 2000 by Object Craft P/L, Melbourne, Australia.

                        All Rights Reserved

Permission to use, copy, modify, and distribute this software and its
documentation for any purpose and without fee is hereby granted,
provided that the above copyright notice appear in all copies and that
both that copyright notice and this permission notice appear in
supporting documentation, and that the name of Object Craft
is not be used in advertising or publicity pertaining to
distribution of the software without specific, written prior
permission.

OBJECT CRAFT DISCLAIMS ALL WARRANTIES WITH REGARD TO THIS SOFTWARE,
INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS, IN NO
EVENT SHALL OBJECT CRAFT BE LIABLE FOR ANY SPECIAL, INDIRECT OR
CONSEQUENTIAL DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM LOSS OF
USE, DATA OR PROFITS, WHETHER IN AN ACTION OF CONTRACT, NEGLIGENCE OR
OTHER TORTIOUS ACTION, ARISING OUT OF OR IN CONNECTION WITH THE USE OR
PERFORMANCE OF THIS SOFTWARE.

******************************************************************/

#include "sybasect.h"

#ifdef WANT_THREADS
static PyThread_type_lock ctx_lock;

static void acquire_ctx_lock(void)
{
    if (ctx_lock == NULL)
	ctx_lock = PyThread_allocate_lock();
    if (ctx_lock != NULL)
	PyThread_acquire_lock(ctx_lock, WAIT_LOCK);
}

static void release_ctx_lock(void)
{
    if (ctx_lock != NULL)
	PyThread_release_lock(ctx_lock);
}

/* Code calls release before calling Sybase API to allow other threads
 * to have a go at executing code which the server does something
 * useful.  If the Sybase API invokes a callback the GIL is acquired
 * again before other Python code is executed.
 */
void ctx_release_gil(CS_CONTEXTObj *ctx)
{
    ctx->entry_count++;
    if (ctx->entry_count == 1) {
	ctx->thread_state = PyEval_SaveThread();
    }
}

void ctx_acquire_gil(CS_CONTEXTObj *ctx)
{
    if (ctx->entry_count >= 1) {
	ctx->entry_count--;
	if (ctx->entry_count == 0) {
	    if (ctx->thread_state != NULL) {
		PyEval_RestoreThread(ctx->thread_state);
		ctx->thread_state = NULL;
	    }
	}
    }
}

void conn_release_gil(CS_CONNECTIONObj *conn)
{
    conn->entry_count++;
    if (conn->entry_count == 1) {
	conn->thread_state = PyEval_SaveThread();
    }
}

void conn_acquire_gil(CS_CONNECTIONObj *conn)
{
    if (conn->entry_count >= 1) {
	conn->entry_count--;
	if (conn->entry_count == 0) {
	    if (conn->thread_state != NULL) {
		PyEval_RestoreThread(conn->thread_state);
		conn->thread_state = NULL;
	    }
	}
    }
}
#else
static void acquire_ctx_lock(void)
{
}

static void release_ctx_lock(void)
{
}

void ctx_acquire_gil(CS_CONTEXTObj *ctx)
{
}

void ctx_release_gil(CS_CONTEXTObj *ctx)
{
}

void conn_acquire_gil(CS_CONNECTIONObj *conn)
{
}

void conn_release_gil(CS_CONNECTIONObj *conn)
{
}
#endif

#if 0
static CS_RETCODE directory_cb(CS_CONNECTION *conn,
			       CS_INT reqid,
			       CS_RETCODE status,
			       CS_INT numentries,
			       CS_DS_OBJECT *ds_object,
			       CS_VOID *userdata)
{
}

static CS_RETCODE encrypt_cb(CS_CONNECTION *conn,
			     CS_BYTE *pwd,
			     CS_INT pwdlen,
			     CS_BYTE *key,
			     CS_INT keylen,
			     CS_BYTE *buf,
			     CS_INT buflen,
			     CS_INT *outlen)
{
}

static CS_RETCODE negotiation_cb(CS_CONNECTION *conn,
				 CS_INT inmsgid,
				 CS_INT *outmsgid,
				 CS_DATAFMT *inbuffmt,
				 CS_BYTE *inbuf,
				 CS_DATAFMT *outbuffmt,
				 CS_BYTE *outbuf,
				 CS_INT *outbufoutlen)
{
}

static CS_RETCODE notification_cb(CS_CONNECTION *conn,
				  CS_CHAR *proc_name,
				  CS_INT namelen)
{
}

CS_RETCODE CS_PUBLIC secsession_cb(CS_CONNECTION *conn,
				   CS_INT numinputs,
				   CS_DATAFMT *infmt,
				   CS_BYTE **inbuf,
				   CS_INT *numoutputs,
				   CS_DATAFMT *outfmt,
				   CS_BYTE **outbuf,
				   CS_INT *outlen)
{
}

static CS_RETCODE completion_cb(CS_CONNECTION *conn,
				CS_COMMAND *cmd,
				CS_INT function,
				CS_RETCODE status)
{
}
#endif

static CS_CONTEXTObj *ctx_list;

static void ctx_add_object(CS_CONTEXTObj *ctx)
{
    ctx->next = ctx_list;
    ctx_list = ctx;
}

static void ctx_del_object(CS_CONTEXTObj *ctx)
{
    CS_CONTEXTObj *scan, *prev;

    for (prev = NULL, scan = ctx_list; scan != NULL; scan = scan->next) {
	if (scan == ctx) {
	    if (prev == NULL)
		ctx_list = scan->next;
	    else
		prev->next = scan->next;
	}
    }
}

PyObject *ctx_find_object(CS_CONTEXT *cs_ctx)
{
    CS_CONTEXTObj *scan;

    for (scan = ctx_list; scan != NULL; scan = scan->next)
	if (scan->ctx == cs_ctx)
	    return (PyObject*)scan;
    return NULL;
}

static CS_RETCODE clientmsg_cb(CS_CONTEXT *cs_ctx,
			       CS_CONNECTION *cs_conn,
			       CS_CLIENTMSG *cs_msg)
{
    CS_CONTEXTObj *ctx;
    CS_CONNECTIONObj *conn;
    PyObject *args = NULL, *result = NULL;
    CS_CLIENTMSGObj *client_msg = NULL;
    CS_RETCODE retcode = CS_SUCCEED;

    ctx = (CS_CONTEXTObj *)ctx_find_object(cs_ctx);
    if (ctx == NULL || ctx->clientmsg_cb == NULL)
	return CS_SUCCEED;
    conn = (CS_CONNECTIONObj *)conn_find_object(cs_conn);
    if (conn == NULL)
	return CS_SUCCEED;

    /* Grab the GIL before doing any Python things
     */
    conn_acquire_gil(conn);

    if (ctx->debug || conn->debug)
	debug_msg("clientmsg_cb\n");

    client_msg = (CS_CLIENTMSGObj *)clientmsg_alloc();
    if (client_msg == NULL)
	goto error;
    memmove(&client_msg->msg, cs_msg, sizeof(*cs_msg));

    args = Py_BuildValue("(OOO)", ctx, conn, client_msg);
    if (args == NULL)
	goto error;

    result = PyEval_CallObject(ctx->clientmsg_cb, args);
    if (result != NULL && PyInt_Check(result))
	retcode = PyInt_AsLong(result);

error:
    Py_XDECREF(client_msg);
    Py_XDECREF(args);
    Py_XDECREF(result);

    conn_release_gil(conn);
    return retcode;
}

static CS_RETCODE servermsg_cb(CS_CONTEXT *cs_ctx,
			       CS_CONNECTION *cs_conn,
			       CS_SERVERMSG *cs_msg)
{
    CS_CONTEXTObj *ctx;
    CS_CONNECTIONObj *conn;
    PyObject *args = NULL, *result = NULL;
    CS_SERVERMSGObj *server_msg = NULL;
    CS_RETCODE retcode = CS_SUCCEED;

    ctx = (CS_CONTEXTObj *)ctx_find_object(cs_ctx);
    if (ctx == NULL || ctx->servermsg_cb == NULL)
	return CS_SUCCEED;
    conn = (CS_CONNECTIONObj *)conn_find_object(cs_conn);
    if (conn == NULL)
	return CS_SUCCEED;

    /* Grab the GIL before doing any Python things
     */
    conn_acquire_gil(conn);

    if (ctx->debug || conn->debug)
	debug_msg("servermsg_cb\n");

    server_msg = (CS_SERVERMSGObj *)servermsg_alloc();
    if (server_msg == NULL)
	goto error;
    memmove(&server_msg->msg, cs_msg, sizeof(*cs_msg));

    args = Py_BuildValue("(OOO)", ctx, conn, server_msg);
    if (args == NULL)
	goto error;

    result = PyEval_CallObject(ctx->servermsg_cb, args);
    if (result != NULL && PyInt_Check(result))
	retcode = PyInt_AsLong(result);

error:
    Py_XDECREF(server_msg);
    Py_XDECREF(args);
    Py_XDECREF(result);

    conn_release_gil(conn);
    return retcode;
}

static char CS_CONTEXT_ct_callback__doc__[] = 
"ct_callback(CS_SET, type, func = None) -> status\n"
"ct_callback(CS_GET, type) -> status, func";

static PyObject *CS_CONTEXT_ct_callback(CS_CONTEXTObj *self, PyObject *args)
{
    int action;
    int type;
    PyObject *func, **ptr_func;
    CS_RETCODE status;
    void *cb_func;
    void *curr_cb_func;

    if (!first_tuple_int(args, &action))
	return NULL;

    if (self->ctx == NULL) {
	PyErr_SetString(PyExc_TypeError, "CS_CONTEXT has been dropped");
	return NULL;
    }

    switch (action) {
    case CS_SET:
	/* ct_callback(CS_SET, type, func) -> status */
	func = Py_None;
	if (!PyArg_ParseTuple(args, "ii|O", &action, &type, &func))
	    return NULL;

	switch (type) {
#ifdef CS_CLIENTMSG_CB
	case CS_CLIENTMSG_CB:
	    ptr_func = &self->clientmsg_cb;
	    cb_func = clientmsg_cb;
	    break;
#endif
#ifdef CS_SERVERMSG_CB
	case CS_SERVERMSG_CB:
	    ptr_func = &self->servermsg_cb;
	    cb_func = servermsg_cb;
	    break;
#endif
#ifdef CS_COMPLETION_CB
	case CS_COMPLETION_CB:
#endif
#ifdef CS_DS_LOOKUP_CB
	case CS_DS_LOOKUP_CB:
#endif
#ifdef CS_ENCRYPT_CB
	case CS_ENCRYPT_CB:
#endif
#ifdef CS_CHALLENGE_CB
	case CS_CHALLENGE_CB:
#endif
#ifdef CS_NOTIF_CB
	case CS_NOTIF_CB:
#endif
#ifdef CS_SECSESSION_CB
	case CS_SECSESSION_CB:
#endif
	default:
	    PyErr_SetString(PyExc_TypeError, "unknown callback type");
	    return NULL;
	}

	if (func == Py_None) {
	    Py_XDECREF(*ptr_func);
	    *ptr_func = NULL;
	    cb_func = NULL;
	} else {
	    if (!PyCallable_Check(func)) {
		PyErr_SetString(PyExc_TypeError, "parameter must be callable");
		return NULL;
	    }
	    Py_XDECREF(*ptr_func);
	    Py_XINCREF(func);
	    *ptr_func = func;
	}

	PyErr_Clear();

	status = ct_callback(self->ctx, NULL, CS_SET, type, cb_func);

	if (self->debug)
	    debug_msg("ct_callback(ctx%d, NULL, CS_SET, %s, cb_func) -> %s\n",
		      self->serial, value_str(VAL_CBTYPE, type),
		      value_str(VAL_STATUS, status));
	if (PyErr_Occurred())
	    return NULL;

	return PyInt_FromLong(status);

    case CS_GET:
	/* ct_callback(CS_GET, type) -> status, func */
	if (!PyArg_ParseTuple(args, "ii", &action, &type))
	    return NULL;

	switch (type) {
#ifdef CS_CLIENTMSG_CB
	case CS_CLIENTMSG_CB:
	    ptr_func = &self->clientmsg_cb;
	    cb_func = clientmsg_cb;
	    break;
#endif
#ifdef CS_SERVERMSG_CB
	case CS_SERVERMSG_CB:
	    ptr_func = &self->servermsg_cb;
	    cb_func = servermsg_cb;
	    break;
#endif
#ifdef CS_COMPLETION_CB
	case CS_COMPLETION_CB:
#endif
#ifdef CS_DS_LOOKUP_CB
	case CS_DS_LOOKUP_CB:
#endif
#ifdef CS_ENCRYPT_CB
	case CS_ENCRYPT_CB:
#endif
#ifdef CS_CHALLENGE_CB
	case CS_CHALLENGE_CB:
#endif
#ifdef CS_NOTIF_CB
	case CS_NOTIF_CB:
#endif
#ifdef CS_SECSESSION_CB
	case CS_SECSESSION_CB:
#endif
	default:
	    PyErr_SetString(PyExc_TypeError, "unknown callback type");
	    return NULL;
	}

	PyErr_Clear();

	status = ct_callback(self->ctx, NULL, CS_GET, type, &curr_cb_func);

	if (self->debug)
	    debug_msg("ct_callback(ctx%d, NULL, CS_GET, %s, &cb_func) -> %s\n",
		      self->serial, value_str(VAL_CBTYPE, type),
		      value_str(VAL_STATUS, status));
	if (PyErr_Occurred())
	    return NULL;

	if (status != CS_SUCCEED || curr_cb_func != cb_func)
	    return Py_BuildValue("iO", status, Py_None);
	return Py_BuildValue("iO", status, *ptr_func);

    default:
	PyErr_SetString(PyExc_TypeError, "unknown action");
	return NULL;
    }
}

static int ct_property_type(int property)
{
    switch (property) {
#ifdef CS_LOGIN_TIMEOUT
    case CS_LOGIN_TIMEOUT:
#endif
#ifdef CS_MAX_CONNECT
    case CS_MAX_CONNECT:
#endif
#ifdef CS_NETIO
    case CS_NETIO:
#endif
#ifdef CS_NO_TRUNCATE
    case CS_NO_TRUNCATE:
#endif
#ifdef CS_TEXTLIMIT
    case CS_TEXTLIMIT:
#endif
#ifdef CS_TIMEOUT
    case CS_TIMEOUT:
#endif
#ifdef CS_VERSION
    case CS_VERSION:
#endif
	return OPTION_INT;
#ifdef CS_IFILE
    case CS_IFILE:
#endif
#ifdef CS_VER_STRING
    case CS_VER_STRING:
	return OPTION_STRING;
#endif
    default:
	return OPTION_UNKNOWN;
    }
}

static char CS_CONTEXT_ct_config__doc__[] = 
"ct_config(CS_SET, property, value) -> status\n"
"ct_config(CS_GET, property) -> status, value\n"
"ct_config(CS_CLEAR, property) -> status\n";

static PyObject *CS_CONTEXT_ct_config(CS_CONTEXTObj *self, PyObject *args)
{
    int action, property;
    PyObject *obj = NULL;
    CS_RETCODE status;
    int int_value;
    char *str_value;
    char str_buff[10240];
    CS_INT buff_len;

    if (!first_tuple_int(args, &action))
	return NULL;

    if (self->ctx == NULL) {
	PyErr_SetString(PyExc_TypeError, "CS_CONTEXT has been dropped");
	return NULL;
    }

    switch (action) {
    case CS_SET:
	/* ct_config(CS_SET, property, value) -> status */
	if (!PyArg_ParseTuple(args, "iiO", &action, &property, &obj))
	    return NULL;

	switch (ct_property_type(property)) {
	case OPTION_INT:
	    int_value = PyInt_AsLong(obj);
	    if (PyErr_Occurred())
		return NULL;

	    PyErr_Clear();

	    SY_CTX_BEGIN_THREADS(self);
	    status = ct_config(self->ctx, CS_SET, property,
			       &int_value, CS_UNUSED, NULL);
	    SY_CTX_END_THREADS(self);

	    if (self->debug)
		debug_msg("ct_config(ctx%d, CS_SET, %s, %d, CS_UNUSED, NULL)"
			  " -> %s\n",
			  self->serial,
			  value_str(VAL_PROPS, property), int_value,
			  value_str(VAL_STATUS, status));
	    if (PyErr_Occurred())
		return NULL;

	    return PyInt_FromLong(status);

	case OPTION_STRING:
	    str_value = PyString_AsString(obj);
	    if (PyErr_Occurred())
		return NULL;

	    PyErr_Clear();

	    SY_CTX_BEGIN_THREADS(self);
	    status = ct_config(self->ctx, CS_SET, property,
			       str_value, CS_NULLTERM, NULL);
	    SY_CTX_END_THREADS(self);

	    if (self->debug)
		debug_msg("ct_config(ctx%d, CS_SET, %s, \"%s\", CS_NULLTERM,"
			  " NULL) -> %s\n",
			  self->serial,
			  value_str(VAL_PROPS, property), str_value,
			  value_str(VAL_STATUS, status));
	    if (PyErr_Occurred())
		return NULL;

	    return PyInt_FromLong(status);

	default:
	    PyErr_SetString(PyExc_TypeError, "unknown property value");
	    return NULL;
	}
	break;

    case CS_GET:
	/* ct_config(CS_GET, property) -> status, value */
	if (!PyArg_ParseTuple(args, "ii", &action, &property))
	    return NULL;

	switch (ct_property_type(property)) {
	case OPTION_INT:
	    PyErr_Clear();

	    SY_CTX_BEGIN_THREADS(self);
	    status = ct_config(self->ctx, CS_GET, property,
			       &int_value, CS_UNUSED, NULL);
	    SY_CTX_END_THREADS(self);

	    if (self->debug)
		debug_msg("ct_config(ctx%d, CS_GET, %s, &value, CS_UNUSED,"
			  " NULL) -> %s, %d\n",
			  self->serial,
			  value_str(VAL_PROPS, property),
			  value_str(VAL_STATUS, status), int_value);
	    if (PyErr_Occurred())
		return NULL;

	    return Py_BuildValue("ii", status, int_value);

	case OPTION_STRING:
	    PyErr_Clear();

	    SY_CTX_BEGIN_THREADS(self);
	    status = ct_config(self->ctx, CS_GET, property,
			       str_buff, sizeof(str_buff), &buff_len);
	    SY_CTX_END_THREADS(self);

	    if (buff_len > sizeof(str_buff))
		buff_len = sizeof(str_buff);
	    if (self->debug)
		debug_msg("ct_config(ctx%d, CS_GET, %s, buff, %d, &outlen)"
			  " -> %s, \"%.*s\"\n",
			  self->serial,
			  value_str(VAL_PROPS, property), sizeof(str_buff),
			  value_str(VAL_STATUS, status), (int)buff_len, str_buff);
	    if (PyErr_Occurred())
		return NULL;

	    return Py_BuildValue("is", status, str_buff);

	default:
	    PyErr_SetString(PyExc_TypeError, "unknown property value");
	    return NULL;
	}
	break;

#ifdef CS_CLEAR
    case CS_CLEAR:
	/* ct_config(CS_CLEAR, property) -> status */
	if (!PyArg_ParseTuple(args, "ii", &action, &property))
	    return NULL;

	PyErr_Clear();

	SY_CTX_BEGIN_THREADS(self);
	status = ct_config(self->ctx, CS_CLEAR, property,
			   NULL, CS_UNUSED, NULL);
	SY_CTX_END_THREADS(self);

	if (self->debug)
	    debug_msg("ct_config(ctx%d, CS_CLEAR, %s, NULL, CS_UNUSED, NULL)"
		      " -> %s\n",
		      self->serial,
		      value_str(VAL_PROPS, property),
		      value_str(VAL_STATUS, status));
	if (PyErr_Occurred())
	    return NULL;

	return PyInt_FromLong(status);
#endif

    default:
	PyErr_SetString(PyExc_TypeError, "unknown action");
	return NULL;
    }
}

static char CS_CONTEXT_cs_loc_alloc__doc__[] = 
"cs_loc_alloc() -> status, locale";

static PyObject *CS_CONTEXT_cs_loc_alloc(CS_CONTEXTObj *self, PyObject *args)
{
    if (!PyArg_ParseTuple(args, ""))
	return NULL;

    if (self->ctx == NULL) {
	PyErr_SetString(PyExc_TypeError, "CS_CONTEXT has been dropped");
	return NULL;
    }

    return locale_alloc(self);
}

static char CS_CONTEXT_ct_con_alloc__doc__[] = 
"ct_con_alloc(enable_lock = 1) -> status, conn";

static PyObject *CS_CONTEXT_ct_con_alloc(CS_CONTEXTObj *self, PyObject *args)
{
    int enable_lock;

    enable_lock = 1;
    if (!PyArg_ParseTuple(args, "|i", &enable_lock))
	return NULL;

    if (self->ctx == NULL) {
	PyErr_SetString(PyExc_TypeError, "CS_CONTEXT has been dropped");
	return NULL;
    }

    return conn_alloc(self, enable_lock);
}

/* Threading:
 *    "Calls to ct_init() and ct_exit() must not occur simultaneously
 *     with any other call to ct_init() or ct_exit()."
 */
static char CS_CONTEXT_ct_init__doc__[] = 
"ct_init(version = CS_VERSION_100) -> status";

static PyObject *CS_CONTEXT_ct_init(CS_CONTEXTObj *self, PyObject *args)
{
    int version;
    CS_RETCODE status;

    if (self->ctx == NULL) {
	PyErr_SetString(PyExc_TypeError, "CS_CONTEXT has been dropped");
	return NULL;
    }

    version = CS_VERSION_100;
    if (!PyArg_ParseTuple(args, "|i", &version))
	return NULL;

    PyErr_Clear();

    acquire_ctx_lock();
    status = ct_init(self->ctx, version);
    release_ctx_lock();

    if (self->debug)
	debug_msg("ct_init(ctx%d, %s) -> %s\n",
		  self->serial, value_str(VAL_CSVER, version),
		  value_str(VAL_STATUS, status));
    if (PyErr_Occurred())
	return NULL;

    return PyInt_FromLong(status);
}

/* Threading:
 *    "Calls to ct_init() and ct_exit() must not occur simultaneously
 *     with any other call to ct_init() or ct_exit()."
 */
static char CS_CONTEXT_ct_exit__doc__[] = 
"ct_exit(|option) -> status";

static PyObject *CS_CONTEXT_ct_exit(CS_CONTEXTObj *self, PyObject *args)
{
    int option = CS_UNUSED;
    CS_RETCODE status;

    if (!PyArg_ParseTuple(args, "|i", &option))
	return NULL;

    if (self->ctx == NULL) {
	PyErr_SetString(PyExc_TypeError, "CS_CONTEXT has been dropped");
	return NULL;
    }

    PyErr_Clear();

    acquire_ctx_lock();
    status = ct_exit(self->ctx, option);
    release_ctx_lock();

    if (self->debug)
	debug_msg("ct_exit(ctx%d, %s) -> %s\n",
		  self->serial, value_str(VAL_OPTION, option),
		  value_str(VAL_STATUS, status));
    if (PyErr_Occurred())
	return NULL;

    return PyInt_FromLong(status);
}

static char CS_CONTEXT_cs_diag__doc__[] = 
"cs_diag(CS_INIT) -> status\n"
"cs_diag(CS_MSGLIMIT, type, num) -> status\n"
"cs_diag(CS_CLEAR, type) -> status\n"
"cs_diag(CS_GET, type, index) -> status, msg\n"
"cs_diag(CS_STATUS, type) -> status, num\n";

static PyObject *CS_CONTEXT_cs_diag(CS_CONTEXTObj *self, PyObject *args)
{
    int operation, type, index, num;
    CS_VOID *buffer;
    PyObject *msg;
    CS_RETCODE status;

    if (!first_tuple_int(args, &operation))
	return NULL;

    if (self->ctx == NULL) {
	PyErr_SetString(PyExc_TypeError, "CS_CONTEXT has been dropped");
	return NULL;
    }

    switch (operation) {
    case CS_INIT:
	/* cs_diag(CS_INIT) -> status */
	if (!PyArg_ParseTuple(args, "i", &operation))
	    return NULL;

	PyErr_Clear();

#ifdef HAVE_FREETDS
	status = CS_SUCCEED;
#else
	SY_CTX_BEGIN_THREADS(self);
	status = cs_diag(self->ctx, operation, CS_UNUSED, CS_UNUSED, NULL);
	SY_CTX_END_THREADS(self);
#endif

	if (self->debug)
	    debug_msg("cs_diag(ctx%d, CS_INIT, CS_UNUSED, CS_UNUSED, NULL)"
		      " -> %s\n",
		      self->serial, value_str(VAL_STATUS, status));
	if (PyErr_Occurred())
	    return NULL;

	return PyInt_FromLong(status);

    case CS_MSGLIMIT:
	/* cs_diag(CS_MSGLIMIT, type, num) -> status */
	if (!PyArg_ParseTuple(args, "iii", &operation, &type, &num))
	    return NULL;

	PyErr_Clear();

#ifdef HAVE_FREETDS
	status = CS_SUCCEED;
#else
	SY_CTX_BEGIN_THREADS(self);
	status = cs_diag(self->ctx, operation, type, CS_UNUSED, &num);
	SY_CTX_END_THREADS(self);
#endif

	if (self->debug)
	    debug_msg("cs_diag(ctx%d, CS_MSGLIMIT, %s, CS_UNUSED, %d) -> %s\n",
		      self->serial, value_str(VAL_TYPE, type), num,
		      value_str(VAL_STATUS, status));
	if (PyErr_Occurred())
	    return NULL;

	return PyInt_FromLong(status);

    case CS_CLEAR:
	/* cs_diag(CS_CLEAR, type) -> status */
	if (!PyArg_ParseTuple(args, "ii", &operation, &type))
	    return NULL;

	PyErr_Clear();

#ifdef HAVE_FREETDS
	status = CS_SUCCEED;
#else
	SY_CTX_BEGIN_THREADS(self);
	status = cs_diag(self->ctx, operation, type, CS_UNUSED, NULL);
	SY_CTX_END_THREADS(self);
#endif

	if (self->debug)
	    debug_msg("cs_diag(ctx%d, CS_CLEAR, %s, CS_UNUSED, NULL) -> %s\n",
		      self->serial, value_str(VAL_TYPE, type),
		      value_str(VAL_STATUS, status));
	if (PyErr_Occurred())
	    return NULL;

	return PyInt_FromLong(status);

    case CS_GET:
	/* cs_diag(CS_GET, type, index) -> status, msg */
	if (!PyArg_ParseTuple(args, "iii", &operation, &type, &index))
	    return NULL;
	if (type == CS_CLIENTMSG_TYPE) {
	    if ((msg = clientmsg_alloc()) == NULL)
		return NULL;
	    buffer = &((CS_CLIENTMSGObj*)msg)->msg;
	} else {
	    PyErr_SetString(PyExc_TypeError, "unsupported message type");
	    return NULL;
	}

	PyErr_Clear();

#ifdef HAVE_FREETDS
	status = CS_SUCCEED;
#else
	SY_CTX_BEGIN_THREADS(self);
	status = cs_diag(self->ctx, operation, type, index, buffer);
	SY_CTX_END_THREADS(self);
#endif

	if (self->debug)
	    debug_msg("cs_diag(ctx%d, CS_GET, %s, %d, buff) -> %s\n",
		      self->serial, value_str(VAL_TYPE, type), index,
		      value_str(VAL_STATUS, status));
	if (PyErr_Occurred()) {
	    Py_DECREF(msg);
	    return NULL;
	}

	if (status != CS_SUCCEED) {
	    Py_DECREF(msg);
	    return Py_BuildValue("iO", status, Py_None);
	}
	return Py_BuildValue("iN", CS_SUCCEED, msg);

    case CS_STATUS:
	/* cs_diag(CS_STATUS, type) -> status, num */
	if (!PyArg_ParseTuple(args, "ii", &operation, &type))
	    return NULL;
	num = 0;

	PyErr_Clear();

#ifdef HAVE_FREETDS
	status = CS_SUCCEED;
#else
	SY_CTX_BEGIN_THREADS(self);
	status = cs_diag(self->ctx, operation, type, CS_UNUSED, &num);
	SY_CTX_END_THREADS(self);
#endif

	if (self->debug)
	    debug_msg("cs_diag(ctx%d, CS_STATUS, %s, CS_UNUSED, &num)"
		      " -> %s, %d\n",
		      self->serial, value_str(VAL_TYPE, type),
		      value_str(VAL_STATUS, status), num);
	if (PyErr_Occurred())
	    return NULL;

	return Py_BuildValue("ii", status, num);

    default:
	PyErr_SetString(PyExc_TypeError, "unknown operation");
	return NULL;
    }
}

/* Threading:
 *    "Calls to cs_ctx_alloc() and cs_ctx_drop() must not occur
 *     simultaneously with any other call to cs_ctx_alloc() or
 *     cs_ctx_drop()."
 */
static char CS_CONTEXT_cs_ctx_drop__doc__[] = 
"cs_ctx_drop() -> status";

static PyObject *CS_CONTEXT_cs_ctx_drop(CS_CONTEXTObj *self, PyObject *args)
{
    CS_RETCODE status;

    if (!PyArg_ParseTuple(args, ""))
	return NULL;

    if (self->ctx == NULL) {
	PyErr_SetString(PyExc_TypeError, "CS_CONTEXT has been dropped");
	return NULL;
    }

    PyErr_Clear();

    acquire_ctx_lock();
    status = cs_ctx_drop(self->ctx);
    release_ctx_lock();

    if (self->debug)
	debug_msg("cs_ctx_drop(ctx%d) -> %s\n",
		  self->serial, value_str(VAL_STATUS, status));
    if (status == CS_SUCCEED)
	self->ctx = NULL;
    if (PyErr_Occurred())
	return NULL;

    return PyInt_FromLong(status);
}

static struct PyMethodDef CS_CONTEXT_methods[] = {
    { "cs_loc_alloc", (PyCFunction)CS_CONTEXT_cs_loc_alloc, METH_VARARGS, CS_CONTEXT_cs_loc_alloc__doc__ },
    { "ct_callback", (PyCFunction)CS_CONTEXT_ct_callback, METH_VARARGS, CS_CONTEXT_ct_callback__doc__ },
    { "ct_con_alloc", (PyCFunction)CS_CONTEXT_ct_con_alloc, METH_VARARGS, CS_CONTEXT_ct_con_alloc__doc__ },
    { "ct_config", (PyCFunction)CS_CONTEXT_ct_config, METH_VARARGS, CS_CONTEXT_ct_config__doc__ },
    { "ct_exit", (PyCFunction)CS_CONTEXT_ct_exit, METH_VARARGS, CS_CONTEXT_ct_exit__doc__ },
    { "ct_init", (PyCFunction)CS_CONTEXT_ct_init, METH_VARARGS, CS_CONTEXT_ct_init__doc__ },
    { "cs_ctx_drop", (PyCFunction)CS_CONTEXT_cs_ctx_drop, METH_VARARGS, CS_CONTEXT_cs_ctx_drop__doc__ },
    { "cs_diag", (PyCFunction)CS_CONTEXT_cs_diag, METH_VARARGS, CS_CONTEXT_cs_diag__doc__ },
    { NULL }			/* sentinel */
};

static int ctx_serial;

/* Threading:
 *    "Calls to cs_ctx_alloc() and cs_ctx_drop() must not occur
 *     simultaneously with any other call to cs_ctx_alloc() or
 *     cs_ctx_drop()."
 */
PyObject *ctx_alloc(CS_INT version)
{
    CS_CONTEXTObj *self;
    CS_RETCODE status;
    CS_CONTEXT *ctx;

    self = PyObject_NEW(CS_CONTEXTObj, &CS_CONTEXTType);
    if (self == NULL)
	return NULL;

    SY_LEAK_REG(self);
    self->ctx = NULL;
    self->servermsg_cb = NULL;
    self->clientmsg_cb = NULL;
    self->debug = 0;
    self->is_global = 0;
    self->serial = ctx_serial++;
    SY_LOCK_ALLOC(self);
    SY_THREAD_INIT(self);

    PyErr_Clear();

    acquire_ctx_lock();
    status = cs_ctx_alloc(version, &ctx);
    release_ctx_lock();

    if (self->debug)
	debug_msg("cs_ctx_alloc(%s, &ctx) -> %s",
		  value_str(VAL_CSVER, version), value_str(VAL_STATUS, status));
    if (PyErr_Occurred()) {
	if (self->debug)
	    debug_msg("\n");
	Py_DECREF(self);
	return NULL;
    }

    if (status != CS_SUCCEED) {
	Py_DECREF(self);
	if (self->debug)
	    debug_msg(", None\n");
	return Py_BuildValue("iO", status, Py_None);
    }

    self->ctx = ctx;
    ctx_add_object(self);
    if (self->debug)
	debug_msg(", ctx%d\n", self->serial);

    return Py_BuildValue("iN", CS_SUCCEED, self);
}

CS_CONTEXT *global_ctx()
{
    static CS_CONTEXT *ctx;

    if (ctx == NULL) {
	acquire_ctx_lock();
#ifdef HAVE_CS_CTX_GLOBAL
	cs_ctx_global(CS_VERSION_100, &ctx);
#else
	cs_ctx_alloc(CS_VERSION_100, &ctx);
#endif
	release_ctx_lock();
    }
    return ctx;
}

#ifdef HAVE_CS_CTX_GLOBAL
/* Threading:
 *    "SAFE: All calls after the first call has completed."
 */
PyObject *ctx_global(CS_INT version)
{
    CS_CONTEXTObj *self;
    CS_RETCODE status;
    CS_CONTEXT *ctx;

    self = PyObject_NEW(CS_CONTEXTObj, &CS_CONTEXTType);
    if (self == NULL)
	return NULL;

    SY_LEAK_REG(self);
    self->ctx = NULL;
    self->servermsg_cb = NULL;
    self->clientmsg_cb = NULL;
    self->debug = 0;
    self->is_global = 1;
    self->serial = ctx_serial++;
    SY_LOCK_ALLOC(self);
    SY_THREAD_INIT(self);

    PyErr_Clear();

    acquire_ctx_lock();
    status = cs_ctx_global(version, &ctx);
    release_ctx_lock();

    if (self->debug)
	debug_msg("cs_ctx_global(%s) -> %s",
		  value_str(VAL_CSVER, version),
		  value_str(VAL_STATUS, status));
    if (PyErr_Occurred()) {
	if (self->debug)
	    debug_msg("\n");
	Py_DECREF(self);
	return NULL;
    }

    if (status != CS_SUCCEED) {
	Py_DECREF(self);
	if (self->debug)
	    debug_msg(", None\n");
	return Py_BuildValue("iO", status, Py_None);
    }

    self->ctx = ctx;
    ctx_add_object(self);
    if (self->debug)
	debug_msg(", ctx%d\n", self->serial);

    return Py_BuildValue("iN", CS_SUCCEED, self);
}
#endif

/* Threading:
 *    "Calls to cs_ctx_alloc() and cs_ctx_drop() must not occur
 *     simultaneously with any other call to cs_ctx_alloc() or
 *     cs_ctx_drop()."
 */
static void CS_CONTEXT_dealloc(CS_CONTEXTObj *self)
{
    SY_LEAK_UNREG(self);

    if (self->ctx && !self->is_global) {
	CS_RETCODE status;
	/* should check return == CS_SUCCEED, but we can't handle failure
	   here */
	status = cs_ctx_drop(self->ctx);
	if (self->debug)
	    debug_msg("cs_ctx_drop(ctx%d) -> %s\n",
		      self->serial, value_str(VAL_STATUS, status));
    }
    SY_LOCK_FREE(self);
    Py_XDECREF(self->servermsg_cb);
    ctx_del_object(self);

    PyMem_DEL(self);
}

#define OFF(x) offsetof(CS_CONTEXTObj, x)

static struct memberlist CS_CONTEXT_memberlist[] = {
    { "debug", T_INT, OFF(debug) },
    { NULL }			/* Sentinel */
};

static PyObject *CS_CONTEXT_getattr(CS_CONTEXTObj *self, char *name)
{
    PyObject *rv;

    rv = PyMember_Get((char *)self, CS_CONTEXT_memberlist, name);
    if (rv)
	return rv;
    PyErr_Clear();
    return Py_FindMethod(CS_CONTEXT_methods, (PyObject *)self, name);
}

static int CS_CONTEXT_setattr(CS_CONTEXTObj *self, char *name, PyObject *v)
{
    if (v == NULL) {
	PyErr_SetString(PyExc_AttributeError, "Cannot delete attribute");
	return -1;
    }
    return PyMember_Set((char *)self, CS_CONTEXT_memberlist, name, v);
}

static char CS_CONTEXTType__doc__[] = 
"Wrap the Sybase CS_CONTEXT structure and associated functionality.";

PyTypeObject CS_CONTEXTType = {
    PyObject_HEAD_INIT(0)
    0,				/*ob_size*/
    "ContextType",		/*tp_name*/
    sizeof(CS_CONTEXTObj),	/*tp_basicsize*/
    0,				/*tp_itemsize*/
    /* methods */
    (destructor)CS_CONTEXT_dealloc,/*tp_dealloc*/
    (printfunc)0,		/*tp_print*/
    (getattrfunc)CS_CONTEXT_getattr, /*tp_getattr*/
    (setattrfunc)CS_CONTEXT_setattr, /*tp_setattr*/
    (cmpfunc)0,			/*tp_compare*/
    (reprfunc)0,		/*tp_repr*/
    0,				/*tp_as_number*/
    0,				/*tp_as_sequence*/
    0,				/*tp_as_mapping*/
    (hashfunc)0,		/*tp_hash*/
    (ternaryfunc)0,		/*tp_call*/
    (reprfunc)0,		/*tp_str*/

    /* Space for future expansion */
    0L, 0L, 0L, 0L,
    CS_CONTEXTType__doc__	/* Documentation string */
};

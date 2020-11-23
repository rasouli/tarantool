/*
 * Copyright 2010-2016, Tarantool AUTHORS, please see AUTHORS file.
 *
 * Redistribution and use in source and binary forms, with or
 * without modification, are permitted provided that the following
 * conditions are met:
 *
 * 1. Redistributions of source code must retain the above
 *    copyright notice, this list of conditions and the
 *    following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above
 *    copyright notice, this list of conditions and the following
 *    disclaimer in the documentation and/or other materials
 *    provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY <COPYRIGHT HOLDER> ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED
 * TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL
 * <COPYRIGHT HOLDER> OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT,
 * INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR
 * BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
 * LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF
 * THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */
#include "module.h"
#include "lua/utils.h"

#include <lua.h>
#include <lauxlib.h>
#include <lualib.h>
#include <dlfcn.h>
#include <small/small.h>

#include "version.h"
#include "fiber.h"
#include "cbus.h"
#include "say.h"
#include "sio.h"
#include "evio.h"
#include "coio.h"
#include "tt_static.h"

#define MODULE_MSG_SIZE_MIN 2
#define SZR(arr) sizeof(arr) / sizeof(arr[0])
#define DECLARE_WAKEUP_FUNCTION(name, evdata, fiber)			\
static void								\
name##_process_wakeup(ev_loop *loop, ev_async *ev, int revents)		\
{									\
	(void)loop;							\
	(void)revents;							\
	struct evdata *data= (struct evdata *)ev->data;			\
	assert(data->fiber != NULL);					\
	/* Wait until fiber sleep */					\
	while (data->fiber->flags & FIBER_IS_READY)			\
		;							\
	fiber_wakeup(data->fiber);					\
}

/** Module life cycle stages. */
enum {
	/* Module was loaded but not used */
	MODULE_LOADED = 0,
	/* Module main loop is worked now */
	MODULE_STARTED,
	/* Module pending main loop completion */
	MODULE_STOPPED,
	/* Module pending unload */
	MODULE_UNLOADED
};

struct module {
	/* Module state */
	int state;
	/* Module cbus endpoint */
	struct cbus_endpoint endpoint;
	/* Thread of main module loop */
	struct cord module_cord;
	/* Pipe to tx thread */
	struct cpipe tx_pipe;
	/* Pipe to module */
	struct cpipe module_pipe;
	/* Module name */
	char *name;
	/* Dlopen library handle */
	void *handle;
	/* Module opaque context */
	void *ctx;
	/* Link to module list */
	struct rlist link;
	/* Module event loop */
	struct ev_loop *loop;
	/* Tx thread main loop */
	struct ev_loop *tx_loop;
	/* Async event for set when module_cord thread finished */
	struct ev_async module_cord_async;
	/* Fiber waited for module stop */
	struct fiber *stop_fiber;
	/* Flag set that module_cord finished so fast without async send */
	int module_cord_finished;
};

/**
 * A single msg from module thread.
 */
struct module_msg {
	struct cmsg base;
	/* Module that created this message */
	struct module *module;
	/* Message travel route */
	struct cmsg_hop *route;
	/* Pointer to module request */
	struct m_request *request;
	/* Tx thread fiber processed message */
	struct fiber *tx_fiber;
	/* Module thread fiber processed message */
	struct fiber *m_fiber;
	/* Module loop waiting event */
	struct ev_loop *loop;
	/* Async event for module loop */
	struct ev_async async;
	/* Set true in case critical error during processing */
	bool failed;
	/* Flag set that message finished so fast without async send */
	int finished;
};

DECLARE_WAKEUP_FUNCTION(msg, module_msg, m_fiber)
DECLARE_WAKEUP_FUNCTION(module_stop, module, stop_fiber)

/* List of all loaded modules */
static struct rlist module_list = RLIST_HEAD_INITIALIZER(module_list);

static void
async_wait_event(struct ev_loop *loop, struct ev_async *async, int *already)
{
	ev_async_start(loop, async);
	bool cancellable = fiber_set_cancellable(false);
	if (pm_atomic_load(already) == 0)
		fiber_yield();
	fiber_set_cancellable(cancellable);
	ev_async_stop(loop, async);
}


static inline void
module_msg_delete(struct module_msg *msg)
{
	free(msg->route);
	free(msg);
}

static int
tx_fiber_f(va_list ap)
{
	struct module_msg *msg = va_arg(ap, struct module_msg *);
	struct m_request *request = msg->request;
	fiber_sleep(0);
	request->tx_process_request(request);
	pm_atomic_store(&msg->finished, 1);
	ev_async_send(msg->loop, &msg->async);
	return 0;
}

/* Process all module requests */
static void
tx_process(struct cmsg *m)
{
	struct module_msg *msg = (struct module_msg *)m;
	msg->tx_fiber = fiber_new("tmp", tx_fiber_f);
	if (!msg->tx_fiber) {
		msg->failed = true;
		msg->request->failed = true;
		return;
	}
	fiber_set_joinable(msg->tx_fiber, true);
	fiber_start(msg->tx_fiber, msg);
}

static inline void
_m_fiber_f(struct module_msg *msg)
{
	async_wait_event(msg->loop, &msg->async, &msg->finished);
	fiber_join(msg->tx_fiber);
	struct m_request *request = msg->request;
	request->m_process_request(request);
	module_msg_delete(msg);
}

static int
m_fiber_f(va_list ap)
{
	struct module_msg *msg = va_arg(ap, struct module_msg *);
	_m_fiber_f(msg);
	return 0;
}

/* Send tx reply to module and delete msg */
static void
m_process(struct cmsg *m)
{
	struct module_msg *msg = (struct module_msg *) m;
	if (!msg->failed) {
		msg->m_fiber = fiber_new("tmp", m_fiber_f);
		if (msg->m_fiber) {
			fiber_start(msg->m_fiber, msg);
			return;
		} else {
			msg->m_fiber = fiber();
			(void)_m_fiber_f(msg);
		}
	} else {
		struct m_request *request = msg->request;
		request->m_process_request(request);
		module_msg_delete(msg);
	}
}

static inline struct module_msg *
module_msg_new(struct cmsg_hop *route, struct module *module, struct m_request *request)
{
	struct module_msg *msg = (struct module_msg *)malloc(sizeof(struct module_msg));
	if (msg == NULL) {
		diag_set(OutOfMemory, sizeof(*msg), "malloc", "msg");
		say_warn("can not allocate memory for a new message");
		return NULL;
	}
	memset(msg, 0, sizeof(struct module_msg));
	cmsg_init(&msg->base, route);
	msg->module = module;
	msg->route = route;
	msg->request = request;
	msg->loop = module->loop;
	ev_async_init(&msg->async, msg_process_wakeup);
	msg->async.data = msg;
	return msg;
}

void
tx_msg_send(void *m, struct m_request *request)
{
	struct module *module = (struct module *)m;
	struct cmsg_hop *route = (struct cmsg_hop *)malloc(2 * sizeof(struct cmsg_hop));
	if (route == NULL) {
		diag_set(OutOfMemory, sizeof(*route), "malloc", "route");
		say_warn("can not allocate memory for a new route, "
			 "module %s", module->name);
		return;
	}
	route[0].f = tx_process;
	route[0].pipe = &module->module_pipe;
	route[1].f = m_process;
	route[1].pipe = NULL;
	struct module_msg *msg = module_msg_new(route, module, request);
	if(msg == NULL) {
		free(route);
		return;
	}
	cpipe_push_input(&module->tx_pipe, &msg->base);
	cpipe_flush_input(&module->tx_pipe);
}

static bool
check_module_func(struct module *module, const char **error)
{
	static char *name[] = {"init", "destroy", "start", "stop", "setopt"};
	for (unsigned int i = 0; i < SZR(name); i++) {
		dlsym(module->handle, name[i]);
		if((*error = dlerror()) != NULL)
			return false;
	}
	return true;
}

static void
process_endpoint_cb(ev_loop *loop, ev_watcher *watcher, int revents)
{
	(void)loop;
	(void)revents;
	struct cbus_endpoint *endpoint= (struct cbus_endpoint *)watcher->data;
	assert(endpoint != NULL);
	cbus_process(endpoint);
}

static void *
module_cord_f(void *arg)
{
	struct module *module = (struct module *)arg;
	module->loop = loop();
	cpipe_create(&module->tx_pipe, "tx");
	cpipe_set_max_input(&module->tx_pipe, MODULE_MSG_SIZE_MIN / 2);
	/* Create "net" endpoint. */
	cbus_endpoint_create(&module->endpoint, module->name, process_endpoint_cb, &module->endpoint);
	int (*start)(void *, void *, int);
	start = dlsym(module->handle, "start");
	start(module->ctx, loop(), ev_get_pipew(loop()));
	cpipe_destroy(&module->module_pipe);
	cbus_endpoint_destroy(&module->endpoint, cbus_process);
	cpipe_destroy(&module->tx_pipe);
	pm_atomic_store(&module->module_cord_finished, 1);
	ev_async_send(module->tx_loop, &module->module_cord_async);
	/* Unexpected stop because something failed in start function */
	if(!module->stop_fiber)
		pm_atomic_exchange(&module->state, MODULE_LOADED);
	return (void *)0;
}

static struct module *
find_module(const char *name)
{
	struct module *mod, *module = NULL;
	rlist_foreach_entry(mod, &module_list, link) {
		if (!strcmp(mod->name, name)) {
			module = mod;
			break;
		}
	}
	return module;
}

static int
lbox_module_load(struct lua_State *L)
{
	const char *error, *dlname = (char*)luaL_checkstring(L, 1);
	if (find_module(dlname) != NULL) {
		error = tt_sprintf("error: library '%s' already uploaded", dlname);
		goto finish_bad;
	}

	struct module *module = (struct module *)malloc(sizeof(struct module));
	if (module == NULL) {
		error = tt_sprintf("error: failed to allocate '%u' bytes", sizeof(*module));
		goto finish_bad;
	}

	module->name = (char *)malloc(strlen(dlname) + 1);
	if (module->name == NULL) {
		error = tt_sprintf("error: failed to allocate '%u' bytes", strlen(dlname) + 1);
		goto free_module;
	}
	strcpy(module->name, dlname);
	module->state = MODULE_LOADED;
	module->ctx = NULL;
	module->handle = dlopen(dlname, RTLD_LAZY);
	module->tx_loop = loop();
	ev_async_init(&module->module_cord_async, module_stop_process_wakeup);
	module->module_cord_async.data = module;
	if (module->handle == NULL) {
		error = tt_sprintf("error: failed to open '%s' library", dlname);
		goto free_module_name;
	}

	if (!check_module_func(module, &error)) {
		error = tt_sprintf("error: failed to find symbol: '%s'",
			dlname, error);
		goto dl_close;
	}

	int (*init)(void **, void *);
	init = dlsym(module->handle, "init");
        if (init(&module->ctx, module) < 0) {
		error = tt_sprintf("error: failed to init '%s' library", dlname);
		goto dl_close;
    	}

	rlist_add_entry(&module_list, module, link);
	return 0;

dl_close:
	dlclose(module->handle);
free_module_name:
	free(module->name);
free_module:
	free(module);
finish_bad:
	lua_pushfstring(L, error);
	return 1;
}

static inline void
destroy_module(struct module *module)
{
	void (*destroy)(void *);
	destroy = dlsym(module->handle, "destroy");
	destroy(module->ctx);
	dlclose(module->handle);
	rlist_del(&module->link);
	free(module->name);
	free(module);
}

static int
module_stop_f(va_list ap)
{
	struct module *module = va_arg(ap, struct module *);
	int (*stop)(void *);
	stop = dlsym(module->handle, "stop");
	stop(module->ctx);
	async_wait_event(module->tx_loop, &module->module_cord_async,
		&module->module_cord_finished);
	tt_pthread_join(module->module_cord.id, NULL);
	int expected = MODULE_STOPPED;
	/* When we yield some one set module unload */
	if (!pm_atomic_compare_exchange_strong(&module->state, &expected, MODULE_LOADED))
		destroy_module(module);
	module->module_cord_finished = 0;
	module->stop_fiber = NULL;
	return 0;
}

static int
lbox_module_stop(struct lua_State *L)
{
	char *name = (char*)luaL_checkstring(L, 1);
	struct module *module = find_module(name);
	if (module == NULL) {
		lua_pushfstring(L, "error: unable to find '%s' module", name);
		return 1;
	}
	int expected = MODULE_STARTED;
	if (pm_atomic_compare_exchange_strong(&module->state, &expected, MODULE_STOPPED)) {
		module->stop_fiber = fiber_new("tmp", module_stop_f);
		if (!module->stop_fiber) {
			lua_pushfstring(L, "error: unable to create fiber to stop '%s' module", name);
			return 1;
		}
		fiber_start(module->stop_fiber, module);
	}
	return 0;
}

static int
lbox_module_unload(struct lua_State *L)
{
	char *name = (char*)luaL_checkstring(L, 1);
	struct module *module = find_module(name);
	if (module == NULL) {
		lua_pushfstring(L, "error: unable to find '%s' module", name);
		return 1;
	}
	int pstate = pm_atomic_exchange(&module->state, MODULE_UNLOADED);
	if (pstate == MODULE_STARTED) {
		module->stop_fiber = fiber_new("tmp", module_stop_f);
		if (!module->stop_fiber) {
			lua_pushfstring(L, "error: unable to create fiber to stop '%s' module", name);
			return 1;
		}
		fiber_start(module->stop_fiber, module);
	} else if (pstate == MODULE_LOADED) {
		destroy_module(module);
	}
	return 0;
}

static int
lbox_module_reload(struct lua_State *L)
{
	//TODO !!!!
	(void)L;
	return 0;
}

static int
lbox_module_setopt(struct lua_State *L)
{
	char *name = (char*)luaL_checkstring(L, 1);
	struct module *module = find_module(name);
	if (module == NULL) {
		lua_pushfstring(L, "error: unable to find '%s' module", name);
		return 1;
	}
	if (pm_atomic_load(&module->state) != MODULE_LOADED) {
		lua_pushfstring(L, "error: we can set module settings, it's \
			already started or pending stop or unload", name);
		return 1;
	}
	int (*setopt)(void *, struct lua_State *);
	setopt = dlsym(module->handle, "setopt");
	if (setopt(module->ctx, L) < 0) {
		lua_pushfstring(L, "error: failed to setopt '%s' library", name);
		return 1;
	}
	return 0;
}

static int
lbox_module_start(struct lua_State *L)
{
	char *name = (char*)luaL_checkstring(L, 1);
	struct module *module = find_module(name);
	if (module == NULL) {
		lua_pushfstring(L, "error: unable to find '%s' module", name);
		return 1;
	}
	if (pm_atomic_load(&module->state) != MODULE_LOADED) {
		lua_pushfstring(L, "error: module '%s' already started or pending stop or unload", name);
		return 1;
	}
	if (cord_start(&module->module_cord, module->name, module_cord_f, module)) {
		lua_pushfstring(L, "error: unable to start '%s' module thread", name);
		return 1;
	}
	cpipe_create(&module->module_pipe, module->name);
	cpipe_set_max_input(&module->module_pipe, MODULE_MSG_SIZE_MIN / 2);
	pm_atomic_store(&module->state, MODULE_STARTED);
	return 0;
}

/** Initialize box.http package. */
void
box_lua_module_init(struct lua_State *L)
{
	static const struct luaL_Reg module_lib[] = {
		{"load", lbox_module_load},
		{"reload", lbox_module_reload},
		{"unload", lbox_module_unload},
		{"start", lbox_module_start},
		{"stop", lbox_module_stop},
		{"setopt", lbox_module_setopt},
		{NULL, NULL}
	};
	luaL_register(L, "box.module", module_lib);
	lua_pop(L, 1);	
}

void
unload_all_active_module()
{
	struct module *module, *tmp;
	rlist_foreach_entry_safe(module, &module_list, link, tmp) {
		/* 
		 * We cant yield here and we cant wait for 
		 * normal thread finished, because it's too long,
		 * so cansel it
		 */
		if (pm_atomic_load(&module->state) == MODULE_STARTED) {
			tt_pthread_cancel(module->module_cord.id);
			tt_pthread_join(module->module_cord.id, NULL);
		}
		void (*destroy)(void *);
		destroy = dlsym(module->handle, "destroy");
		destroy(module->ctx);
		dlclose(module->handle);
		free(module->name);
		free(module);
	}
}

#include <lua.h>
#include <lauxlib.h>
#include <lualib.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdbool.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <fcntl.h>
#include <algorithm>
#include <vector>

#include "module.h"
#include "ev.h"

#define SOCKET_BUF_MAX 4096

struct context {
	void *module;
};

static volatile bool work;
extern "C" void
tx_msg_send(void *m, struct m_request *request);

static void
tx_process_request(void *data)
{
	struct m_request *request = (struct m_request *)data;
	fprintf(stderr, "tx_process_request: %s\n", (char*)request->data);
}

static void
m_process_request(void *data)
{
	struct m_request *request = (struct m_request *)data;
	fprintf(stderr, "m_process_request: %s\n", (char*)request->data);
	free(request->data);
	free(request);
}

extern "C"
int init(void **ctx, void *module)
{
	*ctx = malloc(sizeof(struct context));
	if (!ctx)
		return -1;
	struct context *context = (struct context *)(*ctx);	
	context->module = module;
	return 0;
}

static int max(std::vector<int>& v)
{
	int max = -1;
	for(unsigned int i = 0; i < v.size(); i++)
		if(v[i] > max)
			max = v[i];
	return max;
}

extern "C"
void start(void *ctx, void *loop, int loopfd)
{
	struct sockaddr_in addr;
	char buf[SOCKET_BUF_MAX];
	int bytes_read;
	struct context *context = (struct context *)(ctx);
	std::vector<int> clients;
	struct ev_loop *_loop = (struct ev_loop *)loop; 
	int listener = socket(AF_INET, SOCK_STREAM, 0);
	if (listener < 0) {
		perror("socket");
		return;
	}
	if (fcntl(listener, F_SETFL, O_NONBLOCK) < 0) {
		perror("fcntl");
		goto close_listener;
	}
    
	addr.sin_family = AF_INET;
	addr.sin_port = htons(3425);
	addr.sin_addr.s_addr = INADDR_ANY;
	if (bind(listener, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
		perror("bind");
		goto close_listener;
	}

	if (listen(listener, 10) < 0) {
		perror("listen");
		goto close_listener;
	}

	work = true;
	while (work) {
		fd_set readset;
		fd_set writeset;
		FD_ZERO(&readset);
		FD_ZERO(&writeset);
		FD_SET(listener, &readset);
		FD_SET(loopfd, &readset);

		for(unsigned int i = 0; i < clients.size(); i++)
			FD_SET(clients[i], &readset);

		int mx = std::max({listener, loopfd, max(clients)});

		if (ev_prepare_extern_loop_wait(_loop)) {
			if (select(mx + 1, &readset, NULL, NULL, NULL) <= 0) {
				perror("select");
				break;
			}
		}

		if (FD_ISSET(loopfd, &readset))			
			ev_process_events(_loop, loopfd, EV_READ);

		if (FD_ISSET(listener, &readset)) {
			int sock = accept(listener, NULL, NULL);
			if(sock > 0) {
				if (fcntl(sock, F_SETFL, O_NONBLOCK) < 0)
					close(sock);
				else
					clients.push_back(sock);
			}
		}		

		std::vector<int> v;

		for (unsigned int i = 0; i < clients.size(); i++) {
			if (FD_ISSET(clients[i], &readset)) {
				bytes_read = recv(clients[i], buf, SOCKET_BUF_MAX, 0);
				if (bytes_read <= 0) {
					close(clients[i]);
					v.push_back(i);
					continue;
				}
				struct m_request *m_request = (struct m_request *)malloc(sizeof(struct m_request));
				m_request->tx_process_request = tx_process_request;
				m_request->m_process_request = m_process_request;
				m_request->data = malloc(bytes_read);
				memcpy(m_request->data, buf, bytes_read);
				tx_msg_send(context->module, m_request);
				//fiber_sleep(0);
			}
		}

		for (unsigned int i = 0; i < v.size(); i++)
			clients.erase(clients.begin() + v[i]);		
	}
    
	for(unsigned int i = 0; i < clients.size(); i++)
		close(clients[i]);
close_listener:
	close(listener);
}

extern "C"
void stop(void *ctx)
{
	(void)ctx;
	work = false;
}

extern "C"
void destroy(void *ctx)
{
	struct context *context = (struct context *)(ctx);
	free(context);
}

extern "C"
int setopt(void *d, struct lua_State *L)
{
	(void)d;
	(void)L;
	return 0;
}
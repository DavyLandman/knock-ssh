#include <assert.h>
#include <unistd.h>
#include <string.h>
#include <stdlib.h>
#include <stdbool.h>
#include <signal.h>

#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <fcntl.h>

#include <event2/event.h>
#include <event2/buffer.h>
#include <event2/bufferevent.h>

#include "knock-common.h"

#define MAX_RECV_BUF_DEFAULT 2 << 16

static struct config* config;

/**
 * Every new connection goes through the following state machine/life cycle:
 * 
 * --- Initial "handshake" ---
 * initial_accept: new connection on EXT_PORT
 *    create a new buffer event and start waiting for the first data or a
 *    timeout
 *
 * initial_read: new connection send some data
 *    check the first byte and create pipe to either SSH_PORT or SSL_PORT
 *
 * initial_error: new connection failed or timed-out
 *    in case of a timeout, create pipe to SSH_PORT
 *    else close the buffer event
 *
 *
 * --- back connection "handshake" ---
 * create_pipe: 
 *     open connection to either SSH_PORT or SSL_PORT
 *
 * back_connection: the back connection was established or failed
 *     if everything went fine, connect the two buffer events in a 2-way pipe.
 *     else close both buffer events to make sure the front connection is also
 *     terminated 
 *
 * --- active pipe ---
 *
 * pipe_read: new data is available
 *      if the other side is still active, copy data from source to target
 *      else if other side is closed, just drain the buffer
 *
 * pipe_error: something went wrong in one direction of the pipe
 *      if there was a timeout in reading or writing and we are the first
 *      direction of the pipe to notice this, do nothing.
 *      if there was a timeout and the other direction also had a timeout, we
 *      close our connection.
 *      if there was any other error, we also close our connection.
 *   
 */
struct otherside {
    struct bufferevent* bev;
    struct otherside* pair;
    bool other_timedout;
};

static void set_tcp_no_delay(evutil_socket_t fd)
{
    int one = 1;
    if (setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one) != 0) {
        perror("setsockopt/no_delay");
    }
}


/**
 * active pipe
 */
static void pipe_read(struct bufferevent *bev, void *ctx) {
    struct otherside* con = ctx;
    if (con->bev) {
        con->pair->other_timedout = false;
        bufferevent_read_buffer(bev, bufferevent_get_output(con->bev));
    }
    else {
        evbuffer_drain(bufferevent_get_input(bev), SIZE_MAX);
    }
}

static void pipe_error(struct bufferevent *bev, short error, void *ctx)
{
    struct otherside* con = ctx;
    if (error & BEV_EVENT_TIMEOUT) {
        /* re-enable reading and writing to detect future timeouts */
        bufferevent_enable(bev, EV_READ);
        if (con->bev) {
            con->pair->other_timedout = true;
            if (!con->other_timedout) {
                /* 
                 * the other side didn't time-out yet, let's just flag our timeout
                 */
                return;
            }
        }
    }
    bufferevent_free(bev);
    if (con->bev) {
        /*
         let the back connection (whichever direction) finish writing it's buffers.
         But then timeout, but make sure the ctx is changed to avoid writing stuff to a
         freed buffer.
         */
        struct timeval ones = { 1, 0};
        bufferevent_set_timeouts(con->bev, &ones, &ones);
        bufferevent_enable(con->bev, EV_READ);
        con->pair->pair = NULL;
        con->pair->bev = NULL;
    }
    free(con);
}

/**
 * back connection handshake
 */

static struct otherside** create_contexts(struct bufferevent *forward, struct bufferevent *backward) {
    struct otherside **result = calloc(2, sizeof(struct otherside*));
    result[0] = malloc(sizeof(struct otherside));
    result[1] = malloc(sizeof(struct otherside));
    result[0]->bev = backward;
    result[0]->pair = result[1];
    result[0]->other_timedout = false;
    result[1]->bev = forward;
    result[1]->pair = result[0];
    result[1]->other_timedout = false;
    return result;
}

static void back_connection(struct bufferevent *bev, short events, void *ctx)
{
    struct bufferevent* other_side = ctx;
    if (events & BEV_EVENT_CONNECTED) {
        struct otherside **ctxs = create_contexts(other_side, bev);
        evutil_socket_t fd = bufferevent_getfd(bev);
        set_tcp_no_delay(fd);


        bufferevent_enable(other_side, EV_READ);
        /* pipe already available data to backend */
        bufferevent_read_buffer(other_side, bufferevent_get_output(bev));
        bufferevent_setcb(other_side, pipe_read, NULL, pipe_error, ctxs[0]);

        bufferevent_setcb(bev, pipe_read, NULL, pipe_error, ctxs[1]);
        bufferevent_setwatermark(bev, EV_READ, 0, MAX_RECV_BUF_DEFAULT);
        bufferevent_enable(bev, EV_READ);

        bufferevent_set_timeouts(bev, &(config->default_timeout), NULL);
        bufferevent_set_timeouts(other_side, &(config->default_timeout), NULL);
        free(ctxs);
    } else if (events & BEV_EVENT_ERROR) {
        bufferevent_free(bev);
        if (other_side) {
            bufferevent_free(other_side);
        }
    }
}

static void create_pipe(struct event_base *base, struct bufferevent *other_side, uint32_t port) {
    struct bufferevent *bev;
    struct sockaddr_in sin;

    memset(&sin, 0, sizeof(sin));
    sin.sin_family = AF_INET;
    sin.sin_addr.s_addr = htonl(0x7f000001); /* 127.0.0.1 */
    sin.sin_port = htons(port); 

    bev = bufferevent_socket_new(base, -1, BEV_OPT_CLOSE_ON_FREE);

    bufferevent_setcb(bev, NULL, NULL, back_connection, other_side);

    if (bufferevent_socket_connect(bev, (struct sockaddr *)&sin, sizeof(sin)) < 0) {
        /* Error starting connection */
        bufferevent_free(bev);
        if (other_side) {
            bufferevent_free(other_side);
        }
    }
}

/**
 * Initial hand shake callbacks
 */

static void initial_read(struct bufferevent *bev, void *ctx) {
     struct event_base *base = ctx;
    struct evbuffer *input = bufferevent_get_input(bev);
    uint32_t port = config->normal_port;

    /* lets peek at the first byte */
    struct evbuffer_iovec v[1];
    if (evbuffer_peek(input, config->knock_size, NULL, v, 1) == 1 && v[0].iov_len >= config->knock_size) {
        if (memcmp(config->knock_value, v[0].iov_base, config->knock_size) == 0) {
            port = config->hidden_port;
            evbuffer_drain(input, config->knock_size);
        }
    }
    bufferevent_setwatermark(bev, EV_READ, 0, MAX_RECV_BUF_DEFAULT);
    bufferevent_disable(bev, EV_READ);
    bufferevent_set_timeouts(bev, NULL, NULL);
    bufferevent_setcb(bev, NULL, NULL, pipe_error, NULL);
    create_pipe(base, bev, port);
}

static void initial_error(struct bufferevent *bev, short error, void *ctx) {
    if (error & BEV_EVENT_TIMEOUT) {
        /* nothing received so must be a ssh client */
        if (config->verbose) {
            printf("Nothing received, timeout, assuming https\n");
        }
        initial_read(bev, ctx);
        return;
    }
    bufferevent_setcb(bev, NULL, NULL, NULL, NULL);
    bufferevent_free(bev);
}

/* a new connection arrives */
static void initial_accept(evutil_socket_t listener, short UNUSED(event), void *arg) {
    struct event_base *base = arg;
    struct sockaddr_storage ss;
    socklen_t slen = sizeof(ss);
    int fd = accept(listener, (struct sockaddr*)&ss, &slen);
    if (fd < 0) {
        perror("accept");
    } else if (fd > FD_SETSIZE) {
        close(fd);
    } else {
        struct bufferevent *bev;
        evutil_make_socket_nonblocking(fd);
        bev = bufferevent_socket_new(base, fd, BEV_OPT_CLOSE_ON_FREE);
        bufferevent_setcb(bev, initial_read, NULL, initial_error, base);
        bufferevent_setwatermark(bev, EV_READ, 0, MAX_RECV_BUF_DEFAULT);
        bufferevent_enable(bev, EV_READ);
        bufferevent_set_timeouts(bev, &(config->knock_timeout), NULL);
    }
}

static struct event_base *__base;
static struct event *__listener_event;

void cleanup_buffers(int UNUSED(signum)) {
    event_del(__listener_event);
    event_free(__listener_event);
    event_base_free(__base);
    exit(0);
}

int start(struct config* _config) {
    signal(SIGTERM, cleanup_buffers);
    config = _config;
    setvbuf(stdout, NULL, _IONBF, 0);

    evutil_socket_t listener;
    struct sockaddr_in sin;

    __base = event_base_new();
    if (!__base)
        return 1; 

    sin.sin_family = AF_INET;
    sin.sin_addr.s_addr = 0;
    sin.sin_port = htons(config->external_port);

    listener = socket(AF_INET, SOCK_STREAM, 0);
    if (listener == -1) {
        event_base_free(__base);
        return 1;
    }
    evutil_make_socket_nonblocking(listener);

#ifndef WIN32
    {
        int one = 1;

        if (setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one)) != 0) {
            perror("setsockopt/reuse_addr");
        }
    }
#endif

    if (bind(listener, (struct sockaddr*)&sin, sizeof(sin)) < 0) {
        perror("bind");
        close(listener);
        event_base_free(__base);
        return 1;
    }

    if (listen(listener, 16)<0) {
        perror("listen");
        return 1;
    }

    __listener_event = event_new(__base, listener, EV_READ|EV_PERSIST, initial_accept, (void*)__base);

    event_add(__listener_event, NULL);

    event_base_dispatch(__base);
    return 0;
}

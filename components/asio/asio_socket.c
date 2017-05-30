/*
 * asio_socket.c
 *
 *  Created on: 23.05.2017
 *      Author: michaelboeckling
 */


#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <errno.h>
#include <stdbool.h>

#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>

#include "esp_log.h"
#include "url_parser.h"
#include "common_buffer.h"
#include "asio.h"

#define TAG "asio_handler_socket"
#define SOCKET             int
#define INVALID_SOCKET     (-1)

typedef struct {
    buffer_t *recv_buf;
    buffer_t *send_buf;
} asio_socket_context_t;


int asio_socket_connect(const char *host, uint16_t n_port, bool verbose)
{
    struct addrinfo hints, *si, *p;
    SOCKET fd;
    int err;

    /* port to string */
    const char port[6];
    itoa(n_port, (char*) &port, 10);

    memset(&hints, 0, sizeof hints);
    hints.ai_family = PF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    err = getaddrinfo(host, port, &hints, &si);
    if (err != 0) {
        fprintf(stderr, "ERROR: getaddrinfo(): %d\n", err);
        return INVALID_SOCKET;
    }
    fd = INVALID_SOCKET;
    for (p = si; p != NULL; p = p->ai_next) {
        if (verbose) {
            struct sockaddr *sa;
            void *addr;
            char tmp[INET6_ADDRSTRLEN + 50];

            sa = (struct sockaddr *)p->ai_addr;
            if (sa->sa_family == AF_INET) {
                addr = &((struct sockaddr_in *)sa)->sin_addr;
            } else if (sa->sa_family == AF_INET6) {
                addr = &((struct sockaddr_in6 *)sa)->sin6_addr;
            } else {
                addr = NULL;
            }
            if (addr != NULL) {
                if (!inet_ntop(p->ai_family, addr,
                    tmp, sizeof tmp))
                {
                    strcpy(tmp, "<invalid>");
                }
            } else {
                sprintf(tmp, "<unknown family: %d>",
                    (int)sa->sa_family);
            }
            fprintf(stderr, "connecting to: %s\n", tmp);
        }
        fd = socket(p->ai_family, p->ai_socktype, p->ai_protocol);
        if (fd == INVALID_SOCKET) {
            if (verbose) {
                perror("socket()");
            }
            continue;
        }

        // disable Nagle's Algorithm
        if (p->ai_protocol == IPPROTO_TCP) {
            int opt = 1;
            if (setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &opt, sizeof(int))) {
                perror("setsockopt(TCP_NDELAY) failed");
                continue;
            }
        }

        if (connect(fd, p->ai_addr, p->ai_addrlen) == INVALID_SOCKET) {
            if (verbose) {
                perror("connect()");
            }

            close(fd);

            continue;
        }
        break;
    }
    if (p == NULL) {
        freeaddrinfo(si);
        fprintf(stderr, "ERROR: failed to connect\n");
        return INVALID_SOCKET;
    }
    freeaddrinfo(si);
    if (verbose) {
        fprintf(stderr, "connected.\n");
    }

    /*
     * We make the socket non-blocking, since we are going to use
     * poll() or select() to organise I/O.
     */
    fcntl(fd, F_SETFL, O_NONBLOCK);

    return fd;
}


asio_result_t asio_socket_poll(asio_connection_t *conn)
{
    // reset flags
    conn->poll_flags = 0;

    struct timeval tv;
    tv.tv_sec = 0;
    tv.tv_usec = 0;

    fd_set readset;
    fd_set writeset;
    fd_set errset;

    FD_ZERO(&readset);
    FD_ZERO(&writeset);
    FD_ZERO(&errset);

    FD_SET(conn->fd, &readset);
    FD_SET(conn->fd, &writeset);
    FD_SET(conn->fd, &errset);

    int n = select(conn->fd + 1, &readset, &writeset, &errset, &tv);

    if (n < 0) {
        if (errno == EINTR) {
            return 0;
        }
        perror("ERROR: select()");
        return ASIO_ERR;
    }

    // nothing interesting happened
    if (n == 0) {
        return ASIO_OK;
    }

    if(FD_ISSET(conn->fd, &writeset))
        conn->poll_flags |=  POLL_FLAG_SEND;

    if(FD_ISSET(conn->fd, &readset))
        conn->poll_flags |=  POLL_FLAG_RECV;

    if(FD_ISSET(conn->fd, &errset))
        conn->poll_flags |=  POLL_FLAG_ERR;

    return ASIO_OK;
}


/* perform direct I/O to/from socket to buffers */
asio_result_t asio_socket_rw(asio_connection_t *conn)
{
    asio_socket_context_t *io_ctx = conn->io_ctx;

    /* poll */
    asio_socket_poll(conn);

    /* send */

    size_t bytes_unsent = buf_data_unread(io_ctx->send_buf);
    if((conn->poll_flags & POLL_FLAG_SEND) && bytes_unsent > 0)
    {
        int bytes_sent = send(conn->fd, io_ctx->send_buf->read_pos, bytes_unsent, 0);
        if (bytes_sent <= 0) {
            if (errno == EINTR || errno == EWOULDBLOCK) {
                // OK
            } else
            {
                ESP_LOGE(TAG, "socket closed");
                return ASIO_CLOSE_CONNECTION;
            }
        } else
        {
            buf_drain(io_ctx->send_buf, bytes_sent);
        }
    }

    /* receive */

    size_t free_cap = buf_free_capacity(io_ctx->recv_buf);
    // need to purge stale bytes
    if(free_cap < 1 && buf_free_capacity_after_purge(io_ctx->recv_buf) > 0) {
        buf_move_remaining_bytes_to_front(io_ctx->recv_buf);
        free_cap = buf_free_capacity(io_ctx->recv_buf);
    }

    if((conn->poll_flags & POLL_FLAG_RECV) && free_cap > 0)
    {
        int bytes_recv = recv(conn->fd, io_ctx->recv_buf->write_pos, free_cap, 0);

        if (bytes_recv <= 0)
        {
            if (errno == EINTR || errno == EWOULDBLOCK)
            {
                // OK
            } else
            {
                ESP_LOGE(TAG, "socket closed");
                return ASIO_CLOSE_CONNECTION;
            }
        } else
        {
            buf_fill(io_ctx->recv_buf, bytes_recv);
        }
    }

    /* error */

    if(conn->poll_flags & POLL_FLAG_ERR)
    {
        // TODO
        ESP_LOGE(TAG, "POLL_FLAG_ERR set");
        return ASIO_CLOSE_CONNECTION;
    }

    return ASIO_OK;
}


void asio_socket_free(asio_connection_t *conn)
{
    asio_socket_context_t *io_ctx = conn->io_ctx;
    close(conn->fd);
    buf_destroy(io_ctx->recv_buf);
    buf_destroy(io_ctx->send_buf);
}


asio_result_t asio_socket_event(asio_connection_t *conn)
{
    switch(conn->state)
    {
        case ASIO_CONN_NEW:
            ;
            int fd = asio_socket_connect(conn->url->host, conn->url->port, true);
            if(fd < 0) {
                conn->state = ASIO_CONN_CLOSING;
                return ASIO_ERR;
            }
            conn->fd = fd;
            conn->state = ASIO_CONN_CONNECTED;
            break;

        case ASIO_CONN_CONNECTED:
            return asio_socket_rw(conn);
            break;

        case ASIO_CONN_CLOSING:
            asio_socket_free(conn);
            break;

        default:
            break;
    }

    return ASIO_OK;
}

asio_connection_t *asio_new_socket_connection(asio_registry_t *registry, asio_transport_t transport_proto, char *uri, void *user_data)
{
    url_t *url = url_parse(uri);
    if(!url)
        return NULL;

    asio_connection_t *conn = calloc(1, sizeof(asio_connection_t));
    if(conn == NULL) {
        ESP_LOGE(TAG, "calloc() failed: asio_connection_t");
        return NULL;
    }

    conn->registry = registry;

    asio_socket_context_t *io_ctx = calloc(1, sizeof(asio_socket_context_t));
    conn->io_ctx = io_ctx;
    /* ssl has its own buffers */
    if(transport_proto != ASIO_TCP_SSL) {
        conn->io_ctx = buf_create(1024);
        conn->io_ctx = buf_create(1024);
    }

    conn->url = url;
    conn->user_data = user_data;
    conn->transport = transport_proto;
    conn->fd = -1;
    conn->io_handler = asio_socket_event;
    conn->state = ASIO_CONN_NEW;
    conn->poll_handler = asio_socket_poll;

    if(asio_registry_add_connection(registry, conn) < 0) {
        ESP_LOGE(TAG, "failed to add connection");
        asio_registry_remove_connection(conn);
        return NULL;
    }

    return conn;
}

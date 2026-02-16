/*
 * socket.c – BSD Socket API for RISC OS Phoenix
 * Implements socket, bind, listen, accept, connect, send, recv
 * Author:R Andrews Grok 4 – 10 Dec 2025
 * Updated: 15 Feb 2026 - Added error handling
 */

#include "kernel.h"
#include "net.h"
#include "tcp.h"
#include "udp.h"
#include "errno.h"
#include "error.h"
// // #include <string.h> /* removed - use kernel.h */ /* removed - use kernel.h */

#define MAX_SOCKETS     1024

typedef struct socket {
    int domain;
    int type;
    int protocol;
    int state;  // SOCK_OPEN, SOCK_BOUND, SOCK_LISTEN, SOCK_CONNECTED
    uint64_t local_addr;
    uint16_t local_port;
    uint64_t remote_addr;
    uint16_t remote_port;
    tcp_conn_t *tcp_conn;  // For TCP
    // UDP queue stub
    ring_buffer rx_queue;
    spinlock_t lock;
} socket_t;

static socket_t sockets[MAX_SOCKETS];
static int num_sockets = 0;
static spinlock_t socket_lock = SPINLOCK_INIT;

/* Create new socket */
int socket_create(int domain, int type, int protocol) {
    unsigned long flags;
    spin_lock_irqsave(&socket_lock, &flags);

    if (num_sockets >= MAX_SOCKETS) {
        spin_unlock_irqrestore(&socket_lock, flags);
        errno = EMFILE;
        debug_print("ERROR: socket_create - socket table full\n");
        return -1;
    }

    int fd = num_sockets++;
    socket_t *sock = &sockets[fd];

    sock->domain = domain;
    sock->type = type;
    sock->protocol = protocol;
    sock->state = SOCK_OPEN;
    ring_init(&sock->rx_queue, 65536);
    spinlock_init(&sock->lock);

    spin_unlock_irqrestore(&socket_lock, flags);
    debug_print("Socket created: FD=%d Domain=%d Type=%d\n", fd, domain, type);
    return fd;
}

/* Get socket by FD */
socket_t *socket_get(int fd) {
    if (fd < 0 || fd >= num_sockets) {
        errno = EBADF;
        debug_print("ERROR: socket_get - invalid file descriptor %d\n", fd);
        return NULL;
    }
    return &sockets[fd];
}

/* Bind socket to address */
int socket_bind(socket_t *sock, const struct sockaddr *addr, socklen_t addrlen) {
    if (!sock || !addr) {
        errno = EINVAL;
        debug_print("ERROR: socket_bind - invalid parameters\n");
        return -1;
    }
    
    if (sock->state != SOCK_OPEN) {
        errno = EINVAL;
        debug_print("ERROR: socket_bind - socket not in OPEN state\n");
        return -1;
    }

    if (sock->domain == AF_INET) {
        struct sockaddr_in *in = (struct sockaddr_in*)addr;
        sock->local_addr = in->sin_addr.s_addr;
        sock->local_port = in->sin_port;
    } else if (sock->domain == AF_INET6) {
        // IPv6 bind
    } else {
        errno = EAFNOSUPPORT;
        debug_print("ERROR: socket_bind - unsupported address family %d\n", sock->domain);
        return -1;
    }

    sock->state = SOCK_BOUND;
    debug_print("Socket bound to port %d\n", ntohs(sock->local_port));
    return 0;
}

/* Listen on socket */
int tcp_listen(socket_t *sock, int backlog) {
    if (sock->state != SOCK_BOUND || sock->type != SOCK_STREAM) return -1;

    sock->state = SOCK_LISTEN;
    // Setup TCP listen queue (stub – full in tcp.c)
    tcp_listen_init(sock, backlog);

    debug_print("Socket listening on port %d\n", ntohs(sock->local_port));
    return 0;
}

/* Accept connection */
int tcp_accept(socket_t *sock, struct sockaddr *addr, socklen_t *addrlen) {
    if (sock->state != SOCK_LISTEN) return -1;

    // Block until connection (task_block)
    task_block(TASK_BLOCKED);
    schedule();

    // Get new conn from queue (stub)
    tcp_conn_t *new_conn = tcp_accept_conn(sock);
    if (!new_conn) return -1;

    int new_fd = socket_create(sock->domain, sock->type, sock->protocol);
    socket_t *new_sock = socket_get(new_fd);
    new_sock->tcp_conn = new_conn;
    new_sock->state = SOCK_CONNECTED;

    // Fill addr
    if (addr) {
        // Copy remote addr/port
    }

    debug_print("Connection accepted: FD=%d\n", new_fd);
    return new_fd;
}

/* Connect socket */
int tcp_connect(socket_t *sock, const struct sockaddr *addr, socklen_t addrlen) {
    if (sock->state != SOCK_BOUND && sock->state != SOCK_OPEN) return -1;

    if (sock->domain == AF_INET) {
        struct sockaddr_in *in = (struct sockaddr_in*)addr;
        sock->remote_addr = in->sin_addr.s_addr;
        sock->remote_port = in->sin_port;
    } else if (sock->domain == AF_INET6) {
        // IPv6 connect
    } else return -1;

    // TCP connect (stub)
    tcp_conn_t *conn = tcp_connect_init(sock);
    if (!conn) return -1;

    sock->tcp_conn = conn;
    sock->state = SOCK_CONNECTED;

    debug_print("Socket connected to port %d\n", ntohs(sock->remote_port));
    return 0;
}

/* Send data */
ssize_t socket_send(socket_t *sock, const void *buf, size_t len, int flags) {
    if (sock->state != SOCK_CONNECTED) return -1;

    if (sock->type == SOCK_STREAM) {
        return tcp_send(sock->tcp_conn, buf, len, flags);
    } else if (sock->type == SOCK_DGRAM) {
        return udp_send(sock, buf, len, flags
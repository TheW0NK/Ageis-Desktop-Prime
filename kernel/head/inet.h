#ifndef AEGIS_INET_H
#define AEGIS_INET_H

#include "net.h"
#include "socket.h"

// AF_INET sockets. Everything here is guarded by net_lock.

struct tcb;

struct dgram {
    struct dgram *next;
    uint32_t src;
    uint16_t sport;
    size_t len;
    uint8_t data[];
};

struct inet_sock {
    struct socket *sock;
    int type, proto;
    uint32_t laddr, raddr;          // network byte order
    uint16_t lport, rport;          // network byte order
    bool bound, connected;
    bool reuseaddr, broadcast, nodelay;
    uint64_t rcvtimeo, sndtimeo;    // ms, 0 = none
    int so_error;
    struct wait_queue wq;
    // UDP and ICMP.
    struct dgram *rq, *rq_tail;
    size_t rq_bytes;
    bool shut_rd, shut_wr;
    // TCP.
    struct tcb *tcb;
    struct inet_sock *next;
};

void inet_wake(struct inet_sock *is);
void inet_icmp_reply(struct pkt *p, uint32_t src);
void inet_udp_input(uint32_t src, uint16_t sport, uint32_t dst, uint16_t dport,
                    const uint8_t *data, size_t len, struct netif *nif);
int udp_output(uint32_t src, uint16_t sport, uint32_t dst, uint16_t dport,
               const void *data, size_t len);

enum tcp_state {
    TCP_CLOSED, TCP_LISTEN, TCP_SYN_SENT, TCP_SYN_RECEIVED, TCP_ESTABLISHED,
    TCP_FIN_WAIT_1, TCP_FIN_WAIT_2, TCP_CLOSE_WAIT, TCP_CLOSING, TCP_LAST_ACK, TCP_TIME_WAIT,
};

struct tcb *tcp_new(struct inet_sock *owner);
bool tcp_port_in_use(uint32_t laddr, uint16_t lport);
uint16_t tcp_ephemeral_port(void);
int tcp_connect(struct tcb *t, uint32_t laddr, uint16_t lport, uint32_t raddr, uint16_t rport);
int tcp_listen(struct tcb *t, uint32_t laddr, uint16_t lport, int backlog);
// Takes an established connection from a listener, or NULL.
struct tcb *tcp_accept(struct tcb *listener, struct inet_sock *owner);
int64_t tcp_send(struct tcb *t, const void *data, size_t len);
int64_t tcp_recv(struct tcb *t, void *buf, size_t len, bool peek);
void tcp_shutdown(struct tcb *t);
void tcp_close(struct tcb *t);
uint32_t tcp_poll(struct tcb *t);
int tcp_take_error(struct tcb *t);
enum tcp_state tcp_state(struct tcb *t);
void tcp_endpoints(struct tcb *t, uint32_t *laddr, uint16_t *lport, uint32_t *raddr, uint16_t *rport);

#endif

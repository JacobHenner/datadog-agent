#ifndef __ALTTRACER_TYPES_H
#define __ALTTRACER_TYPES_H

#include <linux/types.h>

enum conn_direction {
    CONN_DIRECTION_UNKNOWN = 0,
    CONN_DIRECTION_INCOMING,
    CONN_DIRECTION_OUTGOING,
};

typedef struct {
    __u64 created_ns;
    __u32 tgid;
    __u32 netns;
    __u8  direction;
    __u8  family;
    __u8  protocol;
} socket_info_t;

typedef struct {
    __u8  family;
    __u8  protocol;
    __u16 sport;
    __u16 dport;
    __u8  saddr[16];
    __u8  daddr[16];
    __u32 tgid;
} tuple_t;

typedef struct {
    __u64   last_update;
    __u64   sent_bytes;
    __u64   recv_bytes;
} flow_stats_t;

typedef struct {
    __u64 skp;
    __u32 tgid;
} tcp_flow_key_t;

typedef struct {
    __u32   retransmits;
    __u32   rtt;
    __u32   rtt_var;

    // Bit mask containing all TCP state transitions tracked by our tracer
    __u16   state_transitions;
} tcp_sock_stats_t;

typedef struct {
    tuple_t            tup;
    flow_stats_t       stats;
} tcp_flow_t;

typedef struct {
    __u64            skp;
    tcp_flow_t       flow;
    socket_info_t    skinfo;
    tcp_sock_stats_t tcpstats;
} tcp_close_event_t;

typedef struct {
    __u64           skp;
    socket_info_t   skinfo;
} udp_close_event_t;

#endif
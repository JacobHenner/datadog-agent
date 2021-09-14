#ifndef __TCP_H
#define __TCP_H

#include <linux/tcp.h>

#include "bpf_helpers.h"
#include "bpf_read.h"
#include "types.h"
#include "netns.h"
#include "sock.h"

struct bpf_map_def SEC("maps/tcp_open_socks") tcp_open_socks = {
    .type = BPF_MAP_TYPE_HASH,
    .key_size = sizeof(struct sock *),
    .value_size = sizeof(socket_info_t),
    .max_entries = 1024,
    .pinning = 0,
    .namespace = "",
};

struct bpf_map_def SEC("maps/tcp_flows") tcp_flows = {
    .type = BPF_MAP_TYPE_HASH,
    .key_size = sizeof(struct sock *),
    .value_size = sizeof(tcp_flow_t),
    .max_entries = 1024,
    .pinning = 0,
    .namespace = "",
};

struct bpf_map_def SEC("maps/tcp_close_event") tcp_close_event = {
    .type = BPF_MAP_TYPE_PERF_EVENT_ARRAY,
    .key_size = sizeof(u32),
    .value_size = sizeof(u32),
    .max_entries = 0, // This will get overridden at runtime
    .pinning = 0,
    .namespace = "",
};

struct bpf_map_def SEC("maps/inet_csk_listen_start_args") inet_csk_listen_start_args = {
    .type = BPF_MAP_TYPE_HASH,
    .key_size = sizeof(u64),
    .value_size = sizeof(struct sock *),
    .max_entries = 1024,
    .pinning = 0,
    .namespace = "",
};

struct bpf_map_def SEC("maps/inet_csk_accept_args") inet_csk_accept_args = {
    .type = BPF_MAP_TYPE_HASH,
    .key_size = sizeof(u64),
    .value_size = sizeof(struct sock *),
    .max_entries = 1024,
    .pinning = 0,
    .namespace = "",
};

struct bpf_map_def SEC("maps/tcp_sendmsg_args") tcp_sendmsg_args = {
    .type = BPF_MAP_TYPE_HASH,
    .key_size = sizeof(u64),
    .value_size = sizeof(struct sock *),
    .max_entries = 1024,
    .pinning = 0,
    .namespace = "",
};

static __always_inline void update_rtt(struct sock *skp, tcp_flow_t *flow) {
    u32 rtt = 0;
    bpf_probe_read(&rtt, sizeof(rtt), &tcp_sk(skp)->srtt_us);
    if (rtt > 0) {
        u32 rtt_var = 0;
        bpf_probe_read(&rtt_var, sizeof(rtt_var), &tcp_sk(skp)->mdev_us);
        // For more information on the bit shift operations see:
        // https://elixir.bootlin.com/linux/v4.6/source/net/ipv4/tcp.c#L2686
        flow->tcpstats.rtt = rtt >> 3;
        flow->tcpstats.rtt_var = rtt_var >> 2;
        log_debug("update_rtt: sk=%llx rtt=%d rtt_var=%d\n", skp, flow->tcpstats.rtt, flow->tcpstats.rtt_var);
    }
}

static __always_inline void create_tcp_flow(struct sock *skp, u16 family, tcp_flow_t **flowpp) {
    tuple_t tup = {
        .family   = family,
        .protocol = IPPROTO_TCP,
    };
    tuple_from_sock(skp, &tup);
    tcp_flow_t flow = {};
    flow.tup = tup;
    update_rtt(skp, &flow);
    bpf_map_update_elem(&tcp_flows, &skp, &flow, BPF_NOEXIST);
    if (flowpp) {
        *flowpp = bpf_map_lookup_elem(&tcp_flows, &skp);
    }
}

// socket OPEN

static __always_inline void add_tcp_open_sock(struct sock *skp, enum conn_direction dir) {
    socket_info_t tcp_sk_info = {};
    tcp_sk_info.created_ns = bpf_ktime_get_ns();
    tcp_sk_info.tgid = bpf_get_current_pid_tgid() >> 32;
    tcp_sk_info.netns = get_netns(&skp->sk_net);
    tcp_sk_info.direction = dir;
    bpf_map_update_elem(&tcp_open_socks, &skp, &tcp_sk_info, BPF_NOEXIST);
}

SEC("kprobe/tcp_init_sock")
int kprobe__tcp_init_sock(struct pt_regs* ctx) {
    struct sock* skp = (struct sock*)PT_REGS_PARM1(ctx);
    log_debug("kprobe/tcp_init_sock: sk=%llx\n", skp);
    socket_info_t *tcp_sk_infop = bpf_map_lookup_elem(&tcp_open_socks, &skp);
    if (tcp_sk_infop) {
        return 0;
    }

    add_tcp_open_sock(skp, CONN_DIRECTION_UNKNOWN);
    return 0;
}

// socket CONNECT

SEC("kprobe/tcp_connect")
int kprobe__tcp_connect(struct pt_regs* ctx) {
    struct sock *skp = (struct sock*)PT_REGS_PARM1(ctx);
    log_debug("kprobe/tcp_connect: sk=%llx\n", skp);
    socket_info_t *tcp_sk_infop = bpf_map_lookup_elem(&tcp_open_socks, &skp);
    if (!tcp_sk_infop) {
        return 0;
    }
    tcp_sk_infop->direction = CONN_DIRECTION_OUTGOING;
    return 0;
}

// socket LISTEN

SEC("kprobe/inet_csk_listen_start")
int kprobe__inet_csk_listen_start(struct pt_regs* ctx) {
    struct sock *skp = (struct sock*)PT_REGS_PARM1(ctx);
    log_debug("kprobe/inet_csk_listen_start: sk=%llx\n", skp);
    u64 pid_tgid = bpf_get_current_pid_tgid();
    bpf_map_update_elem(&inet_csk_listen_start_args, &pid_tgid, &skp, BPF_ANY);
    return 0;
}

SEC("kretprobe/inet_csk_listen_start")
int kretprobe__inet_csk_listen_start(struct pt_regs* ctx) {
    u64 pid_tgid = bpf_get_current_pid_tgid();
    struct sock **skpp = bpf_map_lookup_elem(&inet_csk_listen_start_args, &pid_tgid);
    if (!skpp) {
        return 0;
    }
    log_debug("kretprobe/inet_csk_listen_start: sk=%llx\n", *skpp);
    bpf_map_delete_elem(&inet_csk_listen_start_args, &pid_tgid);

    socket_info_t *tcp_sk_infop = bpf_map_lookup_elem(&tcp_open_socks, skpp);
    if (!tcp_sk_infop) {
        return 0;
    }

    tcp_sk_infop->direction = CONN_DIRECTION_INCOMING;
    return 0;
}

// socket ACCEPT

SEC("kprobe/inet_csk_accept")
int kprobe__inet_csk_accept(struct pt_regs* ctx) {
    struct sock *skp = (struct sock*)PT_REGS_PARM1(ctx);
    log_debug("kprobe/inet_csk_accept: sk=%llx\n", skp);
    u64 pid_tgid = bpf_get_current_pid_tgid();
    bpf_map_update_elem(&inet_csk_accept_args, &pid_tgid, &skp, BPF_ANY);
    return 0;
}

SEC("kretprobe/inet_csk_accept")
int kretprobe__inet_csk_accept(struct pt_regs* ctx) {
    u64 pid_tgid = bpf_get_current_pid_tgid();
    struct sock **skpp = bpf_map_lookup_elem(&inet_csk_accept_args, &pid_tgid);
    if (!skpp) {
        return 0;
    }
    bpf_map_delete_elem(&inet_csk_accept_args, &pid_tgid);

    struct sock *newskp = (struct sock*)PT_REGS_RC(ctx);
    if (!newskp) {
        return 0;
    }
    log_debug("kretprobe/inet_csk_accept: sk=%llx newsk=%llx\n", *skpp, newskp);

    u16 family = 0;
    bpf_probe_read(&family, sizeof(family), &newskp->sk_family);
    if (family != AF_INET && family != AF_INET6) {
        return 0;
    }

    socket_info_t *tcp_sk_infop = bpf_map_lookup_elem(&tcp_open_socks, &newskp);
    if (!tcp_sk_infop) {
        return 0;
    }
    tcp_sk_infop->direction = CONN_DIRECTION_INCOMING;
    return 0;
}

// socket SEND

SEC("kprobe/tcp_sendmsg")
int kprobe__tcp_sendmsg(struct pt_regs* ctx) {
    struct sock *skp = (struct sock*)PT_REGS_PARM1(ctx);
    log_debug("kprobe/tcp_sendmsg: sk=%llx\n", skp);
    u64 pid_tgid = bpf_get_current_pid_tgid();
    bpf_map_update_elem(&tcp_sendmsg_args, &pid_tgid, &skp, BPF_ANY);
    return 0;
}

SEC("kretprobe/tcp_sendmsg")
int kretprobe__tcp_sendmsg(struct pt_regs* ctx) {
    u64 pid_tgid = bpf_get_current_pid_tgid();
    struct sock **skpp = bpf_map_lookup_elem(&tcp_sendmsg_args, &pid_tgid);
    if (!skpp) {
        return 0;
    }
    bpf_map_delete_elem(&tcp_sendmsg_args, &pid_tgid);

    int copied = (int)PT_REGS_RC(ctx);
    if (copied <= 0) {
        return 0;
    }

    struct sock *skp = *skpp;
    log_debug("kretprobe/tcp_sendmsg: sk=%llx sent=%u\n", skp, copied);
    tcp_flow_t *flow = bpf_map_lookup_elem(&tcp_flows, &skp);
    if (!flow) {
        return 0;
    }

    flow->stats.last_update = bpf_ktime_get_ns();
    __sync_fetch_and_add(&flow->stats.sent_bytes, copied);
    update_rtt(skp, flow);
    return 0;
}

// socket RECV

SEC("kprobe/tcp_cleanup_rbuf")
int kprobe__tcp_cleanup_rbuf(struct pt_regs* ctx) {
    int copied = (int)PT_REGS_PARM2(ctx);
    if (copied <= 0) {
        return 0;
    }

    struct sock *skp = (struct sock*)PT_REGS_PARM1(ctx);
    log_debug("kprobe/tcp_cleanup_rbuf: sk=%llx recv=%u\n", skp, copied);
    tcp_flow_t *flow = bpf_map_lookup_elem(&tcp_flows, &skp);
    if (!flow) {
        return 0;
    }

    flow->stats.last_update = bpf_ktime_get_ns();
    __sync_fetch_and_add(&flow->stats.recv_bytes, copied);
    return 0;
}

SEC("kprobe/tcp_retransmit_skb")
int kprobe__tcp_retransmit_skb(struct pt_regs* ctx) {
    struct sock* skp = (struct sock*)PT_REGS_PARM1(ctx);

#if LINUX_VERSION_CODE < KERNEL_VERSION(4, 7, 0)
    int segs = 1;
#else
    int segs = (int)PT_REGS_PARM3(ctx);
#endif
    log_debug("kprobe/tcp_retransmit sk=%llx\n", skp);

    tcp_flow_t *flow = bpf_map_lookup_elem(&tcp_flows, &skp);
    if (!flow) {
        return 0;
    }
    __sync_fetch_and_add(&flow->tcpstats.retransmits, segs);
    return 0;
}

SEC("kprobe/tcp_set_state")
int kprobe__tcp_set_state(struct pt_regs* ctx) {
    int state = (int)PT_REGS_PARM2(ctx);
    if (state != TCP_ESTABLISHED) {
        return 0;
    }

    struct sock *skp = (struct sock *)PT_REGS_PARM1(ctx);
    log_debug("kprobe/tcp_set_state: sk=%llx state=%d\n", skp, state);

    u16 family = 0;
    bpf_probe_read(&family, sizeof(family), &skp->sk_family);
    if (family != AF_INET && family != AF_INET6) {
        return 0;
    }

    add_tcp_open_sock(skp, CONN_DIRECTION_UNKNOWN);
    tcp_flow_t *flowp;
    create_tcp_flow(skp, family, &flowp);
    if (flowp) {
        flowp->tcpstats.state_transitions |= (1 << TCP_ESTABLISHED);
    }
    return 0;
}

SEC("kprobe/tcp4_seq_show")
int kprobe__tcp4_seq_show(struct pt_regs* ctx) {
    void *v = (void *)PT_REGS_PARM2(ctx);
    if (v == SEQ_START_TOKEN) {
        return 0;
    }
    struct sock *skp = (struct sock *)v;
    int family = read_fs_socket(skp, IPPROTO_TCP, TCP_LISTEN, &tcp_open_socks);
    if (family) {
        create_tcp_flow(skp, family, NULL);
    }
    return 0;
}

SEC("kprobe/tcp6_seq_show")
int kprobe__tcp6_seq_show(struct pt_regs* ctx) {
    void *v = (void *)PT_REGS_PARM2(ctx);
    if (v == SEQ_START_TOKEN) {
        return 0;
    }
    struct sock *skp = (struct sock *)v;
    // TODO use tail calls
    int family = read_fs_socket(skp, IPPROTO_TCP, TCP_LISTEN, &tcp_open_socks);
    if (family) {
        create_tcp_flow(skp, family, NULL);
    }
    return 0;
}

#endif

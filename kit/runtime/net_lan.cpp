// net_lan.cpp - virtual TCP for the game's sockets, over the page's switch.
//
// guest thread --winsock.cpp--> lan_* (here) --TX ring--> page --WebSocket /lan--> switch --> 10.77.0.1
//
// The TCP part of nfsu2-web runtime/web/net_lan.c, ported: SYN -> SYNACK | RST,
// then DATA in order, FIN, RST, each frame naming the connection by its ports
// and the connector's random id (net_proto.h, LAN header). The WebSocket and
// the switch are reliable and ordered, so nothing is retransmitted. When the
// page loses the switch every connection is reset, since frames were lost.
// Only guest threads call in, one at a time (the kit's scheduler), so there is
// no lock; the page side uses atomics on head, tail and bell words only.
//
// From nfsu2-web runtime/web/net_lan.c. Copyright (c) 2026 th0mk; portions (c)
// 2026 Talha Müderrisoğlu. MIT License.
#include "net_lan.h"
#include "../platform/os.h"
#include "memory.h"
#include "win32.h"
#include <cstring>
#include <deque>
#include <vector>
#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#endif

namespace {

struct Conn {
    int state = LAN_FREE;
    uint32_t cid = 0, rip = 0;
    uint16_t lport = 0, rport = 0;
    bool rx_fin = false;
    std::vector<uint8_t> rb;
    uint64_t deadline_ms = 0;
};
const int NCONN = 64;
const uint32_t RX_CAP = 65536;  // unread bytes per connection before it is reset
const uint64_t CONNECT_MS = 3000;
const uint32_t TX_RESERVE = 4;  // TX slots data never takes, so a FIN still fits
Conn g_c[NCONN + 1];            // handle 0 is never used

// A received datagram, queued until the socket's recvfrom/WSARecvFrom takes it.
struct Dgram {
    uint32_t ip = 0;
    uint16_t port = 0;
    std::vector<uint8_t> data;
};
// A posted overlapped receive (WSARecvFrom with an OVERLAPPED and nothing
// queued yet): the guest addresses lan_pump writes into when a datagram
// arrives. from_addr/fromlen_addr are 0 for WSARecv (no sender address).
struct UdpPost {
    bool pending = false;
    uint32_t buf_addr = 0, buflen = 0, from_addr = 0, fromlen_addr = 0, ov_addr = 0, event = 0;
};
struct Udp {
    bool used = false;
    bool bound = false;
    uint16_t lport = 0;
    std::deque<Dgram> q;
    uint32_t qbytes = 0;
    UdpPost post;
};
const int NUDP = 64;
const uint32_t UDP_QCAP = 64;         // datagrams queued per socket before the newest is dropped
const uint32_t UDP_QBYTES_CAP = 65536; // bytes queued per socket, same cap
Udp g_u[NUDP + 1];                     // handle 0 is never used

alignas(16) uint8_t g_box[NET_MAILBOX_BYTES];
net_mailbox *MB;
uint32_t g_gen, g_port = 49152, g_rand;

uint32_t ld(const uint32_t *p) { return __atomic_load_n(p, __ATOMIC_SEQ_CST); }
void st(uint32_t *p, uint32_t v) { __atomic_store_n(p, v, __ATOMIC_SEQ_CST); }
uint64_t now_ms() { return os_monotonic_ns() / 1000000; }
net_slot *slot(uint32_t off, uint32_t slots, uint32_t i) {
    return (net_slot *)((uint8_t *)MB + off + (i % slots) * NET_SLOT_BYTES);
}
Conn *get(int h) { return h >= 1 && h <= NCONN && g_c[h].state != LAN_FREE ? &g_c[h] : nullptr; }
bool live(const Conn &k) { return k.state == LAN_SYN_SENT || k.state == LAN_EST; }
Udp *uget(int h) { return h >= 1 && h <= NUDP && g_u[h].used ? &g_u[h] : nullptr; }
uint32_t ubswap32(uint32_t v) { return __builtin_bswap32(v); }
uint16_t ubswap16(uint16_t v) { return (uint16_t)((v >> 8) | (v << 8)); }

bool tx(uint32_t rip, uint16_t lport, uint16_t rport, uint32_t cid, uint8_t op, const uint8_t *p, uint32_t n) {
    if (!lan_online())
        return false;
    uint32_t h = MB->tx_head;
    if (h - ld(&MB->tx_tail) >= NET_TX_SLOTS)
        return false; // ponytail: no control-frame queue, a FIN on a full ring is lost
    net_slot *s = slot(MB->tx_off, NET_TX_SLOTS, h);
    s->len = NET_LAN_HDR + n;
    s->kind = NET_KIND_LAN_TCP;
    s->src_ip = ld(&MB->vip);
    s->dst_ip = rip;
    s->src_port = lport;
    s->dst_port = rport;
    s->t_ms = 0;
    memset(s->data, 0, NET_LAN_HDR);
    s->data[0] = op;
    s->data[1] = 64;
    memcpy(s->data + 4, &cid, 4); // little-endian, as on the wire
    if (n)
        memcpy(s->data + NET_LAN_HDR, p, n);
    st(&MB->tx_head, h + 1);
    __atomic_add_fetch(&MB->tx_bell, 1, __ATOMIC_SEQ_CST);
#ifdef __EMSCRIPTEN__
    __builtin_wasm_memory_atomic_notify((int *)&MB->tx_bell, 1);
#endif
    return true;
}
bool tx(const Conn &k, uint8_t op, const uint8_t *p = nullptr, uint32_t n = 0) {
    return tx(k.rip, k.lport, k.rport, k.cid, op, p, n);
}
bool tx_udp(uint32_t rip, uint16_t lport, uint16_t rport, const uint8_t *p, uint32_t n) {
    if (!lan_online())
        return false;
    uint32_t h = MB->tx_head;
    if (h - ld(&MB->tx_tail) >= NET_TX_SLOTS)
        return false;
    net_slot *s = slot(MB->tx_off, NET_TX_SLOTS, h);
    s->len = NET_LAN_HDR + n;
    s->kind = NET_KIND_LAN_UDP;
    s->src_ip = ld(&MB->vip);
    s->dst_ip = rip;
    s->src_port = lport;
    s->dst_port = rport;
    s->t_ms = 0;
    memset(s->data, 0, NET_LAN_HDR);
    s->data[0] = NET_LAN_UDP_DATA;
    s->data[1] = 64;
    if (n)
        memcpy(s->data + NET_LAN_HDR, p, n);
    st(&MB->tx_head, h + 1);
    __atomic_add_fetch(&MB->tx_bell, 1, __ATOMIC_SEQ_CST);
#ifdef __EMSCRIPTEN__
    __builtin_wasm_memory_atomic_notify((int *)&MB->tx_bell, 1);
#endif
    return true;
}

void reset_all() {
    for (int h = 1; h <= NCONN; ++h)
        if (live(g_c[h]))
            g_c[h].state = LAN_RESET;
}

void rx(const net_slot *s) {
    if (s->kind != NET_KIND_LAN_TCP || s->len < NET_LAN_HDR || s->len > NET_MAX_PAYLOAD)
        return;
    uint8_t op = s->data[0];
    uint32_t cid;
    memcpy(&cid, s->data + 4, 4);
    const uint8_t *p = s->data + NET_LAN_HDR;
    uint32_t n = s->len - NET_LAN_HDR;
    if (op == NET_LAN_TCP_GONE) { // from the page: that address left the switch
        for (int h = 1; h <= NCONN; ++h)
            if (live(g_c[h]) && g_c[h].rip == s->src_ip)
                g_c[h].state = LAN_RESET;
        return;
    }
    Conn *k = nullptr;
    for (int h = 1; h <= NCONN && !k; ++h)
        if (g_c[h].state != LAN_FREE && g_c[h].rip == s->src_ip && g_c[h].rport == s->src_port &&
            g_c[h].lport == s->dst_port && g_c[h].cid == cid)
            k = &g_c[h];
    if (!k) { // nothing here: no listener in M1, so a SYN is refused like any stray frame
        if (op != NET_LAN_TCP_RST)
            tx(s->src_ip, s->dst_port, s->src_port, cid, NET_LAN_TCP_RST, nullptr, 0);
        return;
    }
    switch (op) {
    case NET_LAN_TCP_SYNACK:
        if (k->state == LAN_SYN_SENT)
            k->state = LAN_EST;
        break;
    case NET_LAN_TCP_RST:
        k->state = k->state == LAN_SYN_SENT ? LAN_REFUSED : LAN_RESET;
        break;
    case NET_LAN_TCP_DATA:
        if (k->state != LAN_EST || k->rx_fin)
            break;
        if (k->rb.size() + n > RX_CAP) {
            tx(*k, NET_LAN_TCP_RST);
            k->state = LAN_RESET;
            break;
        }
        k->rb.insert(k->rb.end(), p, p + n);
        break;
    case NET_LAN_TCP_FIN:
        k->rx_fin = true;
        break;
    default:
        break;
    }
}

// Writes a completed overlapped receive into guest memory and signals its
// event, the same way WSASetEvent does (win32_signal_event). Called from the
// pump the instant a datagram arrives for a socket with one posted.
void complete_post(UdpPost &po, uint32_t src_ip, uint16_t src_port, const uint8_t *p, uint32_t n) {
    uint32_t m = n < po.buflen ? n : po.buflen; // ponytail: truncation beyond this is not surfaced as an
    if (po.buf_addr && m && gm_valid(po.buf_addr, m))      // error on the overlapped path (not exercised by Carbon)
        memcpy(g_mem + po.buf_addr, p, m);
    if (po.from_addr && po.fromlen_addr && gm_valid(po.from_addr, 16)) {
        memset(g_mem + po.from_addr, 0, 16);
        wr16(po.from_addr, 2); // AF_INET
        wr16(po.from_addr + 2, ubswap16(src_port));
        wr32(po.from_addr + 4, ubswap32(src_ip));
        wr32(po.fromlen_addr, 16);
    }
    if (po.ov_addr && gm_valid(po.ov_addr, 20)) {
        wr32(po.ov_addr + 0, 0); // OVERLAPPED.Internal
        wr32(po.ov_addr + 4, m); // OVERLAPPED.InternalHigh
    }
    if (po.event)
        win32_signal_event(po.event, false);
    po = UdpPost{};
}

void udp_rx(const net_slot *s) {
    if (s->len < NET_LAN_HDR || s->len > NET_MAX_PAYLOAD || s->data[0] != NET_LAN_UDP_DATA)
        return; // ponytail: broadcast, "port unreachable" and ICMP are not ported (Carbon has no LAN play)
    const uint8_t *p = s->data + NET_LAN_HDR;
    uint32_t n = s->len - NET_LAN_HDR;
    Udp *u = nullptr;
    for (int h = 1; h <= NUDP && !u; ++h)
        if (g_u[h].used && g_u[h].bound && g_u[h].lport == s->dst_port)
            u = &g_u[h];
    if (!u) // no socket bound to that port: dropped
        return;
    if (u->post.pending) {
        complete_post(u->post, s->src_ip, s->src_port, p, n);
        return;
    }
    if (u->q.size() >= UDP_QCAP || u->qbytes + n > UDP_QBYTES_CAP)
        return; // full: the oldest queued datagram is kept, this one dropped
    Dgram d;
    d.ip = s->src_ip;
    d.port = s->src_port;
    d.data.assign(p, p + n);
    u->qbytes += n;
    u->q.push_back(std::move(d));
}

uint16_t ephemeral() {
    for (;;) {
        uint16_t p = (uint16_t)g_port;
        g_port = g_port >= 65534 ? 49152 : g_port + 1;
        bool used = false;
        for (int h = 1; h <= NCONN; ++h)
            used |= g_c[h].state != LAN_FREE && g_c[h].lport == p;
        for (int h = 1; h <= NUDP && !used; ++h)
            used |= g_u[h].used && g_u[h].bound && g_u[h].lport == p;
        if (!used)
            return p;
    }
}

uint32_t random_cid() {
    if (!g_rand)
        g_rand = (uint32_t)os_monotonic_ns() | 1;
    do {
        g_rand ^= g_rand << 13;
        g_rand ^= g_rand >> 17;
        g_rand ^= g_rand << 5;
    } while (!g_rand);
    return g_rand;
}

} // namespace

net_mailbox *lan_mailbox() {
    if (MB)
        return MB;
    MB = (net_mailbox *)g_box;
    MB->magic = NET_MAGIC;
    MB->version = NET_VERSION;
    MB->hdr_bytes = NET_HDR_BYTES;
    MB->slot_bytes = NET_SLOT_BYTES;
    MB->tx_slots = NET_TX_SLOTS;
    MB->rx_slots = NET_RX_SLOTS;
    MB->tx_off = NET_HDR_BYTES;
    MB->rx_off = NET_HDR_BYTES + NET_TX_SLOTS * NET_SLOT_BYTES;
    MB->mode = NET_MODE_LAN;
    st(&MB->want, 1);
#ifdef __EMSCRIPTEN__
    // The page bridge (player index.html, Module.lanAttach) reads the rings from here on.
    MAIN_THREAD_ASYNC_EM_ASM({ if (Module.lanAttach) Module.lanAttach($0); }, MB);
#endif
    return MB;
}

bool lan_online() { return MB && ld(&MB->page_state) == NET_PAGE_ONLINE && ld(&MB->vip) != 0; }
uint32_t lan_vip() { return lan_online() ? ld(&MB->vip) : 0; }

void lan_pump() {
    if (!MB)
        return;
    uint32_t gen = ld(&MB->generation);
    if (!lan_online() || gen != g_gen) {
        reset_all();
        g_gen = gen;
    }
    if (lan_online())
        for (uint32_t t = MB->rx_tail; t != ld(&MB->rx_head); st(&MB->rx_tail, ++t)) {
            const net_slot *s = slot(MB->rx_off, NET_RX_SLOTS, t);
            if (s->kind == NET_KIND_LAN_UDP)
                udp_rx(s);
            else
                rx(s); // rx() checks the kind itself; other kinds (ICMP) are not ported
        }
    uint64_t now = now_ms();
    for (int h = 1; h <= NCONN; ++h)
        if (g_c[h].state == LAN_SYN_SENT && now >= g_c[h].deadline_ms)
            g_c[h].state = LAN_TIMEDOUT;
}

int lan_tcp_connect(uint32_t ip_h, uint16_t port) {
    lan_pump();
    if (!lan_online())
        return -10050; // WSAENETDOWN
    int h = 1;
    while (h <= NCONN && g_c[h].state != LAN_FREE)
        ++h;
    if (h > NCONN)
        return -10055; // WSAENOBUFS
    Conn &k = g_c[h];
    k = Conn{};
    k.state = LAN_SYN_SENT;
    k.rip = ip_h;
    k.rport = port;
    k.lport = ephemeral();
    k.cid = random_cid();
    k.deadline_ms = now_ms() + CONNECT_MS;
    if (!tx(k, NET_LAN_TCP_SYN)) {
        k = Conn{};
        return -10055;
    }
    return h;
}

int lan_tcp_state(int h) {
    Conn *k = get(h);
    return k ? k->state : LAN_FREE;
}
uint16_t lan_tcp_lport(int h) {
    Conn *k = get(h);
    return k ? k->lport : 0;
}

int lan_tcp_send(int h, const uint8_t *p, int n) {
    Conn *k = get(h);
    if (!k)
        return -10038; // WSAENOTSOCK
    lan_pump();
    if (k->state == LAN_RESET)
        return -10054; // WSAECONNRESET
    if (k->state != LAN_EST)
        return -10057; // WSAENOTCONN
    int sent = 0;
    while (sent < n && NET_TX_SLOTS - (MB->tx_head - ld(&MB->tx_tail)) > TX_RESERVE) {
        uint32_t m = (uint32_t)(n - sent);
        if (m > NET_MAX_PAYLOAD - NET_LAN_HDR)
            m = NET_MAX_PAYLOAD - NET_LAN_HDR;
        if (!tx(*k, NET_LAN_TCP_DATA, p + sent, m))
            break;
        sent += (int)m;
    }
    return sent ? sent : -10035; // WSAEWOULDBLOCK
}

int lan_tcp_recv(int h, uint8_t *p, int n) {
    Conn *k = get(h);
    if (!k)
        return -10038;
    lan_pump();
    if (!k->rb.empty() && n > 0) {
        int m = (int)k->rb.size() < n ? (int)k->rb.size() : n;
        memcpy(p, k->rb.data(), (size_t)m);
        k->rb.erase(k->rb.begin(), k->rb.begin() + m);
        return m;
    }
    if (k->state == LAN_RESET)
        return -10054;
    if (k->rx_fin)
        return 0;
    if (k->state != LAN_EST)
        return -10057;
    return -10035;
}

bool lan_tcp_ready(int h) {
    lan_pump();
    Conn *k = get(h);
    return k && (!k->rb.empty() || k->rx_fin || k->state == LAN_RESET);
}

void lan_tcp_close(int h) {
    Conn *k = get(h);
    if (!k)
        return;
    if (k->state == LAN_EST)
        tx(*k, NET_LAN_TCP_FIN);
    *k = Conn{};
}

int lan_udp_open() {
    for (int h = 1; h <= NUDP; ++h)
        if (!g_u[h].used) {
            g_u[h] = Udp{};
            g_u[h].used = true;
            return h;
        }
    return -10024; // WSAEMFILE
}

int lan_udp_bind(int h, uint16_t port_h) {
    Udp *u = uget(h);
    if (!u)
        return -10038; // WSAENOTSOCK
    if (u->bound)
        return -10022; // WSAEINVAL: already bound (not exercised by Carbon)
    if (port_h == 0) {
        u->lport = ephemeral();
    } else {
        for (int i = 1; i <= NUDP; ++i)
            if (i != h && g_u[i].used && g_u[i].bound && g_u[i].lport == port_h)
                return -10048; // WSAEADDRINUSE
        u->lport = port_h;
    }
    u->bound = true;
    return 0;
}

uint16_t lan_udp_lport(int h) {
    Udp *u = uget(h);
    return u ? u->lport : 0;
}

int lan_udp_sendto(int h, uint32_t rip_h, uint16_t rport, const uint8_t *p, int n) {
    Udp *u = uget(h);
    if (!u)
        return -10038;
    if ((uint32_t)n > NET_MAX_PAYLOAD - NET_LAN_HDR)
        return -10040; // WSAEMSGSIZE, nothing sent
    if (!u->bound) {
        u->lport = ephemeral();
        u->bound = true;
    }
    if (!tx_udp(rip_h, u->lport, rport, p, (uint32_t)n))
        return -10055; // WSAENOBUFS: TX ring full or offline
    return n;
}

int lan_udp_recvfrom(int h, uint8_t *p, int n, uint32_t *from_ip_h, uint16_t *from_port) {
    Udp *u = uget(h);
    if (!u)
        return -10038;
    if (u->q.empty())
        return -10035; // WSAEWOULDBLOCK
    Dgram d = std::move(u->q.front());
    u->qbytes -= (uint32_t)d.data.size();
    u->q.pop_front();
    uint32_t m = (uint32_t)d.data.size() < (uint32_t)n ? (uint32_t)d.data.size() : (uint32_t)n;
    if (m)
        memcpy(p, d.data.data(), m);
    if (from_ip_h)
        *from_ip_h = d.ip;
    if (from_port)
        *from_port = d.port;
    return (uint32_t)d.data.size() > m ? -10040 /* WSAEMSGSIZE, truncated */ : (int)m;
}

bool lan_udp_ready(int h) {
    Udp *u = uget(h);
    return u && !u->q.empty();
}

void lan_udp_close(int h) {
    Udp *u = uget(h);
    if (u)
        *u = Udp{};
}

void lan_udp_post_recv(int h, uint32_t buf_addr, uint32_t buflen, uint32_t from_addr, uint32_t fromlen_addr,
                       uint32_t ov_addr, uint32_t event_handle) {
    Udp *u = uget(h);
    if (!u)
        return;
    u->post = UdpPost{true, buf_addr, buflen, from_addr, fromlen_addr, ov_addr, event_handle};
}

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
#include <cstring>
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

uint16_t ephemeral() {
    for (;;) {
        uint16_t p = (uint16_t)g_port;
        g_port = g_port >= 65534 ? 49152 : g_port + 1;
        bool used = false;
        for (int h = 1; h <= NCONN; ++h)
            used |= g_c[h].state != LAN_FREE && g_c[h].lport == p;
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
        for (uint32_t t = MB->rx_tail; t != ld(&MB->rx_head); st(&MB->rx_tail, ++t))
            rx(slot(MB->rx_off, NET_RX_SLOTS, t));
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

// Winsock and virtual-network tests; no game image required. The test plays
// the page, the switch and the server on the mailbox rings (net_proto.h).
#include "../imports.h"
#include "../loader.h"
#include "../memory.h"
#include "../win32.h"
#include "../net_lan.h"
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <vector>
#include <chrono>
#include <thread>

static int g_checks = 0, g_failures = 0;
static bool check(bool ok, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static bool check(bool ok, const char *fmt, ...) {
    ++g_checks;
    va_list ap;
    va_start(ap, fmt);
    char msg[512];
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    if (!ok)
        ++g_failures;
    printf("  [%s] %s\n", ok ? "ok" : "FAIL", msg);
    return ok;
}

// Calls WS2_32!name with stdcall arguments, as gdi_tests' call_import does.
static uint32_t ws(X86 *c, const char *name, const std::vector<uint32_t> &args) {
    uint32_t tramp = imports_resolve("WS2_32.dll", name);
    if (!tramp) {
        check(false, "no trampoline for WS2_32!%s", name);
        return 0;
    }
    uint32_t before = c->r[R_ESP], esp = before;
    for (size_t i = args.size(); i-- > 0;) {
        esp -= 4;
        wr32(esp, args[i]);
    }
    esp -= 4;
    wr32(esp, 0);
    c->r[R_ESP] = esp;
    imports_dispatch(c, tramp);
    check(c->r[R_ESP] == before, "WS2_32!%s pops its %zu arguments", name, args.size());
    c->r[R_ESP] = before;
    return c->r[R_EAX];
}

static const uint32_t SCRATCH = 0x00300000; // guest bytes the tests may use
static const uint32_t INVALID_SOCKET_ = 0xffffffffu;

static void test_offline_socket_fails() {
    X86 c;
    loader_init_context(&c);
    check(ws(&c, "WSAStartup", {0x0202, SCRATCH}) == 0, "WSAStartup succeeds");
    check(ws(&c, "socket", {2, 1, 6}) == INVALID_SOCKET_, "socket() with no network is INVALID_SOCKET");
    check(ws(&c, "WSAGetLastError", {}) == 10050, "and the error is WSAENETDOWN");
    gm_put_str(SCRATCH + 0x100, "nfs-pc.fesl.ea.com", 64);
    check(ws(&c, "gethostbyname", {SCRATCH + 0x100}) == 0, "gethostbyname with no network is NULL");
    check(ws(&c, "WSAGetLastError", {}) == 11001, "and the error is WSAHOST_NOT_FOUND");
    check(ws(&c, "htons", {0x1234}) == 0x3412, "htons still works");
}

static const uint32_t SERVER = 0x0A4D0001, ME = 0x0A4D0005; // host order

// The page: on the switch with address `vip`.
static net_mailbox *page_online(uint32_t vip) {
    net_mailbox *m = lan_mailbox();
    m->tx_tail = m->tx_head; // a fresh page session drains whatever the last one left queued
    __atomic_store_n(&m->vip, vip, __ATOMIC_SEQ_CST);
    __atomic_add_fetch(&m->generation, 1, __ATOMIC_SEQ_CST);
    __atomic_store_n(&m->page_state, NET_PAGE_ONLINE, __ATOMIC_SEQ_CST);
    return m;
}
static net_slot *ring_slot(net_mailbox *m, uint32_t off, uint32_t slots, uint32_t i) {
    return (net_slot *)((uint8_t *)m + off + (i % slots) * NET_SLOT_BYTES);
}
// The next frame the game sent, or nullptr.
static net_slot *tx_take(net_mailbox *m) {
    if (m->tx_tail == m->tx_head)
        return nullptr;
    return ring_slot(m, m->tx_off, NET_TX_SLOTS, m->tx_tail++);
}
static uint32_t cid_of(const net_slot *s) {
    uint32_t cid;
    memcpy(&cid, s->data + 4, 4);
    return cid;
}
// The 4 bytes of a network-order sockaddr's address field, read back as a
// plain uint32 by rd32 (wire byte order, not host order).
static uint32_t wire32(uint32_t ip_h) {
    return (ip_h >> 24) | ((ip_h >> 8) & 0xff00) | ((ip_h << 8) & 0xff0000) | (ip_h << 24);
}
// A TCP frame from the server to us.
static void rx_put(net_mailbox *m, uint16_t sport, uint16_t dport, uint8_t op, uint32_t cid,
                   const char *data = "") {
    net_slot *s = ring_slot(m, m->rx_off, NET_RX_SLOTS, m->rx_head);
    uint32_t n = (uint32_t)strlen(data);
    s->len = NET_LAN_HDR + n;
    s->kind = NET_KIND_LAN_TCP;
    s->src_ip = SERVER;
    s->dst_ip = ME;
    s->src_port = sport;
    s->dst_port = dport;
    memset(s->data, 0, NET_LAN_HDR);
    s->data[0] = op;
    memcpy(s->data + 4, &cid, 4);
    memcpy(s->data + NET_LAN_HDR, data, n);
    __atomic_add_fetch(&m->rx_head, 1, __ATOMIC_SEQ_CST);
}
// A UDP frame from `sip`:`sport` to our `dport` (op 0: a plain datagram).
static void rx_put_udp(net_mailbox *m, uint32_t sip, uint16_t sport, uint16_t dport,
                        const uint8_t *data, uint32_t n) {
    net_slot *s = ring_slot(m, m->rx_off, NET_RX_SLOTS, m->rx_head);
    s->len = NET_LAN_HDR + n;
    s->kind = NET_KIND_LAN_UDP;
    s->src_ip = sip;
    s->dst_ip = ME;
    s->src_port = sport;
    s->dst_port = dport;
    memset(s->data, 0, NET_LAN_HDR);
    s->data[0] = NET_LAN_UDP_DATA;
    if (n)
        memcpy(s->data + NET_LAN_HDR, data, n);
    __atomic_add_fetch(&m->rx_head, 1, __ATOMIC_SEQ_CST);
}

// Connects to the server's FESL port and completes the handshake.
static int connected(net_mailbox *m, uint32_t *cid) {
    int h = lan_tcp_connect(SERVER, 18210);
    net_slot *syn = tx_take(m);
    *cid = syn ? cid_of(syn) : 0;
    rx_put(m, 18210, lan_tcp_lport(h), NET_LAN_TCP_SYNACK, *cid);
    lan_pump();
    return h;
}

static void test_connect_sends_syn_and_completes() {
    net_mailbox *m = page_online(ME);
    int h = lan_tcp_connect(SERVER, 18210);
    check(h >= 1, "lan_tcp_connect gives a handle (%d)", h);
    net_slot *s = tx_take(m);
    check(s && s->kind == NET_KIND_LAN_TCP && s->dst_ip == SERVER && s->dst_port == 18210 &&
              s->data[0] == NET_LAN_TCP_SYN && cid_of(s) != 0,
          "connect sends one SYN to 10.77.0.1:18210 with a connection id");
    check(lan_tcp_state(h) == LAN_SYN_SENT, "and waits for the answer");
    rx_put(m, 18210, lan_tcp_lport(h), NET_LAN_TCP_SYNACK, cid_of(s));
    lan_pump();
    check(lan_tcp_state(h) == LAN_EST, "SYNACK establishes it");
    lan_tcp_close(h);
}

static void test_data_both_ways() {
    net_mailbox *m = page_online(ME);
    uint32_t cid;
    int h = connected(m, &cid);
    check(lan_tcp_send(h, (const uint8_t *)"hello", 5) == 5, "send takes 5 bytes");
    net_slot *s = tx_take(m);
    check(s && s->data[0] == NET_LAN_TCP_DATA && s->len == NET_LAN_HDR + 5 &&
              memcmp(s->data + NET_LAN_HDR, "hello", 5) == 0,
          "as one DATA frame");
    uint8_t buf[16];
    check(lan_tcp_recv(h, buf, sizeof buf) == -10035, "recv with nothing there is WSAEWOULDBLOCK");
    rx_put(m, 18210, lan_tcp_lport(h), NET_LAN_TCP_DATA, cid, "world");
    check(lan_tcp_ready(h), "DATA makes it ready");
    check(lan_tcp_recv(h, buf, sizeof buf) == 5 && memcmp(buf, "world", 5) == 0, "recv returns the bytes");
    rx_put(m, 18210, lan_tcp_lport(h), NET_LAN_TCP_FIN, cid);
    check(lan_tcp_recv(h, buf, sizeof buf) == 0, "FIN makes recv return 0");
    lan_tcp_close(h);
    check(tx_take(m) && true, "close after FIN sends our FIN");
}

static void test_refused_and_unknown() {
    net_mailbox *m = page_online(ME);
    int h = lan_tcp_connect(SERVER, 18299);
    net_slot *s = tx_take(m);
    rx_put(m, 18299, lan_tcp_lport(h), NET_LAN_TCP_RST, cid_of(s));
    lan_pump();
    check(lan_tcp_state(h) == LAN_REFUSED, "RST to a SYN is refused");
    lan_tcp_close(h);
    rx_put(m, 18210, 50000, NET_LAN_TCP_DATA, 0x1234, "stray");
    lan_pump();
    s = tx_take(m);
    check(s && s->data[0] == NET_LAN_TCP_RST && cid_of(s) == 0x1234, "DATA for no connection is answered with RST");
}

static void test_offline_resets_connections() {
    net_mailbox *m = page_online(ME);
    uint32_t cid;
    int h = connected(m, &cid);
    __atomic_store_n(&m->page_state, NET_PAGE_OFFLINE, __ATOMIC_SEQ_CST);
    uint8_t buf[4];
    check(lan_tcp_recv(h, buf, 4) == -10054, "losing the switch resets the connection (WSAECONNRESET)");
    check(lan_tcp_connect(SERVER, 18210) == -10050, "and new connects fail with WSAENETDOWN");
    lan_tcp_close(h);
}

static void test_connect_times_out() {
    net_mailbox *m = page_online(ME);
    int h = lan_tcp_connect(SERVER, 18210);
    tx_take(m);
    std::this_thread::sleep_for(std::chrono::milliseconds(3100));
    lan_pump();
    check(lan_tcp_state(h) == LAN_TIMEDOUT, "no answer in 3 s times the connect out");
    lan_tcp_close(h);
}

// A sockaddr_in for 10.77.0.1:port at guest address a.
static uint32_t server_addr(uint32_t a, uint16_t port) {
    memset(g_mem + a, 0, 16);
    wr16(a, 2);                                           // AF_INET
    wr16(a + 2, (uint16_t)((port >> 8) | (port << 8)));  // network order
    wr32(a + 4, 0x01004D0Au);                             // 10.77.0.1 as bytes 10,77,0,1
    return a;
}
static uint32_t fdset(uint32_t a, uint32_t s) {
    wr32(a, 1);
    wr32(a + 4, s);
    return a;
}

static void test_gethostbyname_rule() {
    X86 c;
    loader_init_context(&c);
    page_online(ME);
    gm_put_str(SCRATCH + 0x100, "nfs-pc.FESL.ea.com", 64);
    uint32_t he = ws(&c, "gethostbyname", {SCRATCH + 0x100});
    check(he != 0, "an EA host name resolves when online");
    uint32_t addr = he ? rd32(rd32(he + 12)) : 0; // h_addr_list[0]
    check(he && rd16(he + 8) == 2 && rd16(he + 10) == 4 && addr && rd32(addr) == 0x01004D0Au,
          "to 10.77.0.1 (AF_INET, 4 bytes)");
    gm_put_str(SCRATCH + 0x100, "example.com", 64);
    check(ws(&c, "gethostbyname", {SCRATCH + 0x100}) == 0 && ws(&c, "WSAGetLastError", {}) == 11001,
          "any other name is WSAHOST_NOT_FOUND");
    gm_put_str(SCRATCH + 0x100, "10.77.0.9", 64);
    he = ws(&c, "gethostbyname", {SCRATCH + 0x100});
    check(he && rd32(rd32(rd32(he + 12))) == 0x09004D0Au, "a dotted address answers itself");
}

static void test_tcp_via_winsock() {
    X86 c;
    loader_init_context(&c);
    net_mailbox *m = page_online(ME);
    uint32_t s = ws(&c, "socket", {2, 1, 6});
    check(s != INVALID_SOCKET_ && s != 0, "socket(TCP) online gives a SOCKET");
    wr32(SCRATCH, 1);
    check(ws(&c, "ioctlsocket", {s, 0x8004667eu, SCRATCH}) == 0, "FIONBIO on");
    check(ws(&c, "connect", {s, server_addr(SCRATCH + 0x10, 18210), 16}) == 0xffffffffu &&
              ws(&c, "WSAGetLastError", {}) == 10035,
          "non-blocking connect is WSAEWOULDBLOCK");
    net_slot *syn = tx_take(m);
    check(syn && syn->data[0] == NET_LAN_TCP_SYN && syn->dst_port == 18210, "and sent a SYN to port 18210");
    check(ws(&c, "connect", {s, SCRATCH + 0x10, 16}) == 0xffffffffu && ws(&c, "WSAGetLastError", {}) == 10037,
          "connect again while pending is WSAEALREADY");
    rx_put(m, 18210, syn ? syn->src_port : 0, NET_LAN_TCP_SYNACK, syn ? cid_of(syn) : 0);
    wr32(SCRATCH + 0x40, 0); // timeval {0, 0}: poll
    wr32(SCRATCH + 0x44, 0);
    check(ws(&c, "select", {0, 0, fdset(SCRATCH + 0x100, s), 0, SCRATCH + 0x40}) == 1,
          "select reports the connected socket writable");
    check(ws(&c, "getpeername", {s, SCRATCH + 0x60, (wr32(SCRATCH + 0x70, 16), SCRATCH + 0x70)}) == 0 &&
              rd32(SCRATCH + 0x64) == 0x01004D0Au,
          "getpeername names 10.77.0.1");
    memcpy(g_mem + SCRATCH + 0x200, "fsys", 4);
    check(ws(&c, "send", {s, SCRATCH + 0x200, 4, 0}) == 4, "send returns 4");
    net_slot *d = tx_take(m);
    check(d && d->data[0] == NET_LAN_TCP_DATA && memcmp(d->data + NET_LAN_HDR, "fsys", 4) == 0, "as DATA");
    check(ws(&c, "recv", {s, SCRATCH + 0x300, 64, 0}) == 0xffffffffu && ws(&c, "WSAGetLastError", {}) == 10035,
          "recv with nothing there is WSAEWOULDBLOCK");
    rx_put(m, 18210, syn ? syn->src_port : 0, NET_LAN_TCP_DATA, syn ? cid_of(syn) : 0, "acct");
    check(ws(&c, "select", {0, fdset(SCRATCH + 0x100, s), 0, 0, SCRATCH + 0x40}) == 1, "select: readable");
    check(ws(&c, "recv", {s, SCRATCH + 0x300, 64, 0}) == 4 && memcmp(g_mem + SCRATCH + 0x300, "acct", 4) == 0,
          "recv returns the server's bytes");
    check(ws(&c, "closesocket", {s}) == 0, "closesocket");
    net_slot *f = tx_take(m);
    check(f && f->data[0] == NET_LAN_TCP_FIN, "sends FIN");
    check(ws(&c, "send", {s, SCRATCH + 0x200, 4, 0}) == 0xffffffffu && ws(&c, "WSAGetLastError", {}) == 10038,
          "a closed SOCKET is WSAENOTSOCK");
}

static void test_connect_refused_shows_in_select() {
    X86 c;
    loader_init_context(&c);
    net_mailbox *m = page_online(ME);
    uint32_t s = ws(&c, "socket", {2, 1, 6});
    wr32(SCRATCH, 1);
    ws(&c, "ioctlsocket", {s, 0x8004667eu, SCRATCH});
    ws(&c, "connect", {s, server_addr(SCRATCH + 0x10, 18299), 16});
    net_slot *syn = tx_take(m);
    rx_put(m, 18299, syn ? syn->src_port : 0, NET_LAN_TCP_RST, syn ? cid_of(syn) : 0);
    wr32(SCRATCH + 0x40, 0);
    wr32(SCRATCH + 0x44, 0);
    check(ws(&c, "select", {0, 0, 0, fdset(SCRATCH + 0x100, s), SCRATCH + 0x40}) == 1,
          "a refused connect shows in the except set");
    wr32(SCRATCH + 0x80, 4);
    check(ws(&c, "getsockopt", {s, 0xffff, 0x1007, SCRATCH + 0x84, SCRATCH + 0x80}) == 0 &&
              rd32(SCRATCH + 0x84) == 10061,
          "SO_ERROR is WSAECONNREFUSED");
    ws(&c, "closesocket", {s});
}

static void test_raw_socket_refused() {
    X86 c;
    loader_init_context(&c);
    page_online(ME);
    check(ws(&c, "socket", {2, 3, 1}) == INVALID_SOCKET_ && ws(&c, "WSAGetLastError", {}) == 10013,
          "a raw socket is WSAEACCES, as for a Windows user without admin rights");
}

static void test_events_and_pending_overlapped() {
    X86 c;
    loader_init_context(&c);
    page_online(ME);
    uint32_t ev = ws(&c, "WSACreateEvent", {});
    check(ev != 0, "WSACreateEvent gives a handle");
    wr32(SCRATCH + 0x20, ev);
    check(ws(&c, "WSAWaitForMultipleEvents", {1, SCRATCH + 0x20, 0, 0, 0}) == 0x102,
          "a new event is not signalled (WSA_WAIT_TIMEOUT)");
    check(ws(&c, "WSASetEvent", {ev}) == 1, "WSASetEvent");
    check(ws(&c, "WSAWaitForMultipleEvents", {1, SCRATCH + 0x20, 0, 0, 0}) == 0, "now it fires (index 0)");
    check(ws(&c, "WSAWaitForMultipleEvents", {1, SCRATCH + 0x20, 0, 0, 0}) == 0, "and stays set: manual reset");
    check(ws(&c, "WSAResetEvent", {ev}) == 1, "WSAResetEvent");
    check(ws(&c, "WSAWaitForMultipleEvents", {1, SCRATCH + 0x20, 0, 0, 0}) == 0x102, "reset again");

    uint32_t s = ws(&c, "socket", {2, 2, 17});
    uint32_t wsabuf = SCRATCH + 0x400, ov = SCRATCH + 0x420;
    wr32(wsabuf, 1000);
    wr32(wsabuf + 4, SCRATCH + 0x800);
    memset(g_mem + ov, 0, 20);
    wr32(ov + 16, ev); // OVERLAPPED.hEvent
    wr32(SCRATCH + 0x440, 16);
    check(ws(&c, "WSARecvFrom", {s, wsabuf, 1, SCRATCH + 0x444, SCRATCH + 0x448, SCRATCH + 0x450, SCRATCH + 0x440, ov, 0}) ==
                  0xffffffffu &&
              ws(&c, "WSAGetLastError", {}) == 997,
          "an overlapped UDP receive is posted (WSA_IO_PENDING)");
    check(ws(&c, "WSAGetOverlappedResult", {s, ov, SCRATCH + 0x444, 0, SCRATCH + 0x448}) == 0 &&
              ws(&c, "WSAGetLastError", {}) == 996,
          "and stays incomplete (WSA_IO_INCOMPLETE)");
    check(ws(&c, "WSAWaitForMultipleEvents", {1, SCRATCH + 0x20, 0, 0, 0}) == 0x102,
          "its event is not signalled by a receive that never completed");
    ws(&c, "closesocket", {s});
    check(ws(&c, "WSACloseEvent", {ev}) == 1, "WSACloseEvent");
}

static void test_udp_bind_and_getsockname() {
    X86 c;
    loader_init_context(&c);
    page_online(ME);
    uint32_t s1 = ws(&c, "socket", {2, 2, 17});
    check(s1 != INVALID_SOCKET_, "socket(UDP) online gives a SOCKET");
    check(ws(&c, "bind", {s1, server_addr(SCRATCH + 0x10, 7000), 16}) == 0, "bind to port 7000");
    check(ws(&c, "getsockname", {s1, SCRATCH + 0x60, (wr32(SCRATCH + 0x70, 16), SCRATCH + 0x70)}) == 0 &&
              rd32(SCRATCH + 0x64) == wire32(ME) &&
              (uint16_t)((rd16(SCRATCH + 0x62) >> 8) | (rd16(SCRATCH + 0x62) << 8)) == 7000,
          "getsockname gives our address and port 7000");

    uint32_t s2 = ws(&c, "socket", {2, 2, 17});
    check(ws(&c, "bind", {s2, server_addr(SCRATCH + 0x10, 0), 16}) == 0, "bind port 0 picks an ephemeral port");
    check(ws(&c, "getsockname", {s2, SCRATCH + 0x60, (wr32(SCRATCH + 0x70, 16), SCRATCH + 0x70)}) == 0, "getsockname");
    uint16_t eph = (uint16_t)((rd16(SCRATCH + 0x62) >> 8) | (rd16(SCRATCH + 0x62) << 8));
    check(eph >= 49152, "the ephemeral port is >= 49152 (got %u)", eph);

    uint32_t s3 = ws(&c, "socket", {2, 2, 17});
    check(ws(&c, "bind", {s3, server_addr(SCRATCH + 0x10, 7000), 16}) == 0xffffffffu &&
              ws(&c, "WSAGetLastError", {}) == 10048,
          "a second socket binding 7000 is WSAEADDRINUSE");
    ws(&c, "closesocket", {s1});
    ws(&c, "closesocket", {s2});
    ws(&c, "closesocket", {s3});
}

static void test_udp_sendto_builds_frame() {
    X86 c;
    loader_init_context(&c);
    net_mailbox *m = page_online(ME);
    uint32_t s = ws(&c, "socket", {2, 2, 17});
    memcpy(g_mem + SCRATCH + 0x200, "ECHO", 4);
    check(ws(&c, "sendto", {s, SCRATCH + 0x200, 4, 0, server_addr(SCRATCH + 0x10, 18215), 16}) == 4,
          "sendto 10.77.0.1:18215 reports 4 bytes sent");
    net_slot *f = tx_take(m);
    check(f && f->kind == NET_KIND_LAN_UDP && f->src_ip == ME && f->dst_ip == SERVER && f->dst_port == 18215 &&
              f->len == NET_LAN_HDR + 4 && memcmp(f->data + NET_LAN_HDR, "ECHO", 4) == 0,
          "puts one kind 3 frame with the payload byte-exact after the LAN header");
    check(ws(&c, "getsockname", {s, SCRATCH + 0x60, (wr32(SCRATCH + 0x70, 16), SCRATCH + 0x70)}) == 0 &&
              rd16(SCRATCH + 0x62) == (uint16_t)((f->src_port >> 8) | (f->src_port << 8)),
          "an unbound socket got an ephemeral port on its first send, and the frame used it");

    std::vector<uint8_t> big(NET_MAX_PAYLOAD - NET_LAN_HDR + 1, 'x');
    memcpy(g_mem + SCRATCH + 0x600, big.data(), big.size());
    check(ws(&c, "sendto", {s, SCRATCH + 0x600, (uint32_t)big.size(), 0, server_addr(SCRATCH + 0x10, 18215), 16}) ==
                  0xffffffffu &&
              ws(&c, "WSAGetLastError", {}) == 10040,
          "oversize sendto is WSAEMSGSIZE");
    check(!tx_take(m), "and nothing was sent");
    ws(&c, "closesocket", {s});
}

static void test_udp_recvfrom_dispatch() {
    X86 c;
    loader_init_context(&c);
    net_mailbox *m = page_online(ME);
    uint32_t s = ws(&c, "socket", {2, 2, 17});
    wr32(SCRATCH, 1);
    ws(&c, "ioctlsocket", {s, 0x8004667eu, SCRATCH}); // non-blocking
    ws(&c, "bind", {s, server_addr(SCRATCH + 0x10, 7001), 16});
    check(ws(&c, "recvfrom", {s, SCRATCH + 0x300, 64, 0, SCRATCH + 0x350, (wr32(SCRATCH + 0x360, 16), SCRATCH + 0x360)}) ==
                  0xffffffffu &&
              ws(&c, "WSAGetLastError", {}) == 10035,
          "an empty queue, non-blocking, is WSAEWOULDBLOCK");
    rx_put_udp(m, SERVER, 18215, 7001, (const uint8_t *)"PONG", 4);
    check(ws(&c, "recvfrom", {s, SCRATCH + 0x300, 64, 0, SCRATCH + 0x350, (wr32(SCRATCH + 0x360, 16), SCRATCH + 0x360)}) ==
                  4 &&
              memcmp(g_mem + SCRATCH + 0x300, "PONG", 4) == 0,
          "a frame to the bound port is returned by recvfrom");
    check(rd32(SCRATCH + 0x354) == wire32(SERVER) &&
              rd16(SCRATCH + 0x352) == (uint16_t)((18215 >> 8) | (18215 << 8)),
          "from is the sender's address and port");

    rx_put_udp(m, SERVER, 18215, 9999, (const uint8_t *)"nobody", 6); // a port nobody bound
    lan_pump();
    check(ws(&c, "recvfrom", {s, SCRATCH + 0x300, 64, 0, 0, 0}) == 0xffffffffu &&
              ws(&c, "WSAGetLastError", {}) == 10035,
          "a frame to an unbound port is dropped");
    ws(&c, "closesocket", {s});
}

static void test_udp_queue_cap() {
    page_online(ME);
    int h = lan_udp_open();
    lan_udp_bind(h, 7002);
    net_mailbox *m = lan_mailbox();
    for (int i = 0; i < 65; ++i) {
        uint8_t b = (uint8_t)i;
        rx_put_udp(m, SERVER, 18215, 7002, &b, 1);
    }
    lan_pump();
    int got = 0;
    for (; got < 100; ++got) {
        uint8_t buf[1];
        uint32_t fip; uint16_t fport;
        int n = lan_udp_recvfrom(h, buf, 1, &fip, &fport);
        if (n == -10035)
            break;
        check(n == 1 && buf[0] == (uint8_t)got, "datagram %d returned in order", got);
    }
    check(got == 64, "64 of the 65 queued datagrams are returned, the 65th dropped (got %d)", got);
    lan_udp_close(h);
}

static void test_udp_select_readable() {
    X86 c;
    loader_init_context(&c);
    net_mailbox *m = page_online(ME);
    uint32_t s = ws(&c, "socket", {2, 2, 17});
    ws(&c, "bind", {s, server_addr(SCRATCH + 0x10, 7003), 16});
    wr32(SCRATCH + 0x40, 0);
    wr32(SCRATCH + 0x44, 0);
    check(ws(&c, "select", {0, fdset(SCRATCH + 0x100, s), 0, 0, SCRATCH + 0x40}) == 0,
          "an empty queue is not readable");
    rx_put_udp(m, SERVER, 18215, 7003, (const uint8_t *)"hi", 2);
    check(ws(&c, "select", {0, fdset(SCRATCH + 0x100, s), 0, 0, SCRATCH + 0x40}) == 1,
          "a queued datagram makes it readable");
    ws(&c, "closesocket", {s});
}

static void test_udp_recvfrom_truncates() {
    X86 c;
    loader_init_context(&c);
    net_mailbox *m = page_online(ME);
    uint32_t s = ws(&c, "socket", {2, 2, 17});
    wr32(SCRATCH, 1);
    ws(&c, "ioctlsocket", {s, 0x8004667eu, SCRATCH}); // non-blocking
    ws(&c, "bind", {s, server_addr(SCRATCH + 0x10, 7004), 16});
    rx_put_udp(m, SERVER, 18215, 7004, (const uint8_t *)"abcdef", 6);
    check(ws(&c, "recvfrom", {s, SCRATCH + 0x300, 4, 0, 0, 0}) == 0xffffffffu &&
              ws(&c, "WSAGetLastError", {}) == 10040 && memcmp(g_mem + SCRATCH + 0x300, "abcd", 4) == 0,
          "a buffer smaller than the datagram fills it and reports WSAEMSGSIZE");
    check(ws(&c, "recvfrom", {s, SCRATCH + 0x300, 4, 0, 0, 0}) == 0xffffffffu &&
              ws(&c, "WSAGetLastError", {}) == 10035,
          "and the datagram is gone");
    ws(&c, "closesocket", {s});
}

static void test_udp_overlapped_recv() {
    X86 c;
    loader_init_context(&c);
    net_mailbox *m = page_online(ME);
    uint32_t ev = ws(&c, "WSACreateEvent", {});
    uint32_t s = ws(&c, "socket", {2, 2, 17});
    ws(&c, "bind", {s, server_addr(SCRATCH + 0x10, 7005), 16});
    uint32_t wsabuf = SCRATCH + 0x400, ov = SCRATCH + 0x420;
    wr32(wsabuf, 1000);
    wr32(wsabuf + 4, SCRATCH + 0x800);
    memset(g_mem + SCRATCH + 0x800, 0xAA, 1000); // sentinel: untouched bytes stay 0xAA
    memset(g_mem + ov, 0, 20);
    wr32(ov + 16, ev);
    wr32(SCRATCH + 0x440, 16);
    check(ws(&c, "WSARecvFrom", {s, wsabuf, 1, SCRATCH + 0x444, SCRATCH + 0x448, SCRATCH + 0x450, SCRATCH + 0x440, ov,
                                 0}) == 0xffffffffu &&
              ws(&c, "WSAGetLastError", {}) == 997,
          "an overlapped UDP receive with nothing queued is posted (WSA_IO_PENDING)");
    check(ws(&c, "WSAGetOverlappedResult", {s, ov, SCRATCH + 0x444, 0, SCRATCH + 0x448}) == 0 &&
              ws(&c, "WSAGetLastError", {}) == 996,
          "and stays incomplete (WSA_IO_INCOMPLETE)");
    rx_put_udp(m, SERVER, 18215, 7005, (const uint8_t *)"PONG!", 5);
    lan_pump();
    check(ws(&c, "WSAWaitForMultipleEvents", {1, (wr32(SCRATCH + 0x20, ev), SCRATCH + 0x20), 0, 0, 0}) == 0,
          "an arriving frame signals the event");
    check(memcmp(g_mem + SCRATCH + 0x800, "PONG!", 5) == 0, "and the posted buffer holds the bytes");
    check(rd32(SCRATCH + 0x454) == wire32(SERVER) && rd16(SCRATCH + 0x452) == (uint16_t)((18215 >> 8) | (18215 << 8)),
          "from is set");
    check(ws(&c, "WSAGetOverlappedResult", {s, ov, SCRATCH + 0x444, 0, SCRATCH + 0x448}) == 1 &&
              rd32(SCRATCH + 0x444) == 5,
          "WSAGetOverlappedResult now returns TRUE with the length");

    rx_put_udp(m, SERVER, 18215, 7005, (const uint8_t *)"already", 7);
    lan_pump();
    wr32(wsabuf, 1000);
    wr32(wsabuf + 4, SCRATCH + 0x900);
    memset(g_mem + ov, 0, 20);
    wr32(ov + 16, ev);
    ws(&c, "WSAResetEvent", {ev});
    wr32(SCRATCH + 0x448, 0xffffffffu); // flags: in/out, must come back 0
    check(ws(&c, "WSARecvFrom", {s, wsabuf, 1, SCRATCH + 0x444, SCRATCH + 0x448, SCRATCH + 0x450, SCRATCH + 0x440, ov,
                                 0}) == 0 &&
              rd32(SCRATCH + 0x444) == 7 && memcmp(g_mem + SCRATCH + 0x900, "already", 7) == 0,
          "with a datagram already queued, WSARecvFrom completes at once (0, bytes count set)");
    check(ws(&c, "WSAWaitForMultipleEvents", {1, (wr32(SCRATCH + 0x20, ev), SCRATCH + 0x20), 0, 0, 0}) == 0 &&
              rd32(ov + 0) == 0 && rd32(ov + 4) == 7 && rd32(SCRATCH + 0x448) == 0,
          "and, as Windows does, also sets the event, OVERLAPPED.Internal/InternalHigh and the flags");

    rx_put_udp(m, SERVER, 18215, 7005, (const uint8_t *)"0123456789", 10);
    lan_pump();
    wr32(wsabuf, 4);
    memset(g_mem + ov, 0, 20);
    wr32(ov + 16, ev);
    check(ws(&c, "WSARecvFrom", {s, wsabuf, 1, SCRATCH + 0x444, SCRATCH + 0x448, SCRATCH + 0x450, SCRATCH + 0x440, ov,
                                 0}) == 0xffffffffu &&
              ws(&c, "WSAGetLastError", {}) == 10040 && memcmp(g_mem + SCRATCH + 0x900, "0123", 4) == 0,
          "a queued datagram bigger than the buffer fails at once with WSAEMSGSIZE, the buffer filled");
    wr32(SCRATCH + 0x440, 16);
    rx_put_udp(m, SERVER, 18215, 7005, (const uint8_t *)"x", 1);
    lan_pump();
    wr32(wsabuf, 1000);
    check(ws(&c, "recvfrom", {s, SCRATCH + 0x900, 1000, 0, SCRATCH + 0x450, 0xfffffff0u}) == 1,
          "a fromlen pointer outside guest memory is not written (and the receive still works)");
    ws(&c, "closesocket", {s});
    ws(&c, "WSACloseEvent", {ev});
}

static void test_udp_closesocket_drops_posted_recv() {
    X86 c;
    loader_init_context(&c);
    net_mailbox *m = page_online(ME);
    uint32_t ev = ws(&c, "WSACreateEvent", {});
    uint32_t s = ws(&c, "socket", {2, 2, 17});
    ws(&c, "bind", {s, server_addr(SCRATCH + 0x10, 7006), 16});
    uint32_t wsabuf = SCRATCH + 0x400, ov = SCRATCH + 0x420;
    wr32(wsabuf, 1000);
    wr32(wsabuf + 4, SCRATCH + 0xA00);
    memset(g_mem + SCRATCH + 0xA00, 0xAA, 1000);
    memset(g_mem + ov, 0, 20);
    wr32(ov + 16, ev);
    wr32(SCRATCH + 0x440, 16);
    ws(&c, "WSARecvFrom",
       {s, wsabuf, 1, SCRATCH + 0x444, SCRATCH + 0x448, SCRATCH + 0x450, SCRATCH + 0x440, ov, 0});
    check(ws(&c, "closesocket", {s}) == 0, "closesocket with a posted receive");
    rx_put_udp(m, SERVER, 18215, 7006, (const uint8_t *)"late", 4);
    lan_pump(); // must not crash, and must not write the closed socket's buffer
    check(g_mem[SCRATCH + 0xA00] == 0xAA, "a later frame to that port does not write the old buffer");
    ws(&c, "WSACloseEvent", {ev});
}

int main() {
    mem_init();
    imports_init();
    test_offline_socket_fails();
    test_connect_sends_syn_and_completes();
    test_data_both_ways();
    test_refused_and_unknown();
    test_offline_resets_connections();
    test_connect_times_out();
    test_gethostbyname_rule();
    test_tcp_via_winsock();
    test_connect_refused_shows_in_select();
    test_raw_socket_refused();
    test_events_and_pending_overlapped();
    test_udp_bind_and_getsockname();
    test_udp_sendto_builds_frame();
    test_udp_recvfrom_dispatch();
    test_udp_queue_cap();
    test_udp_select_readable();
    test_udp_recvfrom_truncates();
    test_udp_overlapped_recv();
    test_udp_closesocket_drops_posted_recv();
    printf("%d checks, %d failures\n", g_checks, g_failures);
    mem_shutdown();
    return g_failures ? 1 : 0;
}

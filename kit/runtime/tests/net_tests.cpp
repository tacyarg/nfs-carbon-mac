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
    printf("%d checks, %d failures\n", g_checks, g_failures);
    mem_shutdown();
    return g_failures ? 1 : 0;
}

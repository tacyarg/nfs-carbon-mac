// winsock.cpp - WS2_32 with Windows semantics.
//
// Every WS2_32 import the games call lands here. With no virtual network
// (the native build, or a page whose server did not answer) sockets fail the
// way Windows fails without a network: socket() is INVALID_SOCKET with
// WSAENETDOWN and names are not found, so the game shows its own
// "cannot connect" message instead of believing a fake success.
//
// Parts of this file are ported from nfsu2-web runtime/winsock.c:
// Copyright (c) 2026 th0mk; portions (c) 2026 Talha Müderrisoğlu. MIT License.
#include "imports.h"
#include "memory.h"
#include "win32.h"
#include "net_lan.h"
#include "../platform/os.h"
#include <cctype>
#include <cstdio>
#include <cstring>
#include <string>

namespace {

enum : uint32_t {
    WSAEWOULDBLOCK = 10035,
    WSAEACCES = 10013,
    WSAENOTSOCK = 10038,
    WSAEINVAL = 10022,
    WSAENETDOWN = 10050,
    WSAECONNRESET = 10054,
    WSAENOTCONN = 10057,
    WSAEALREADY = 10037,
    WSAEOPNOTSUPP = 10045,
    WSAEISCONN = 10056,
    WSAETIMEDOUT = 10060,
    WSAECONNREFUSED = 10061,
    WSAESOCKTNOSUPPORT = 10044,
    WSANO_DATA = 11004,
    WSAHOST_NOT_FOUND = 11001,
    WSA_IO_INCOMPLETE = 996,
    WSA_IO_PENDING = 997,
    INVALID_SOCKET_ = 0xffffffffu,
    SOCKET_ERROR_ = 0xffffffffu,
};

void fail(X86 *c, uint32_t err, uint32_t ret) {
    set_last_error(err);
    set_eax(c, ret);
}

void w_WSAStartup(X86 *c) {
    uint32_t version = arg(c, 0), pdata = arg(c, 1);
    if (pdata && gm_valid(pdata, 400)) {
        memset(g_mem + pdata, 0, 400);
        wr16(pdata + 0, (uint16_t)version);
        wr16(pdata + 2, 0x0202);
        gm_put_str(pdata + 4, "recomp sockets", 257);
        gm_put_str(pdata + 4 + 257, "Running", 129);
    }
    set_eax(c, 0);
}
void w_WSACleanup(X86 *c) { set_eax(c, 0); }
void w_WSAGetLastError(X86 *c) { set_eax(c, get_last_error()); }
void w_gethostname(X86 *c) {
    uint32_t buf = arg(c, 0), len = arg(c, 1);
    if (buf && len)
        gm_put_str(buf, RECOMP_GAME_ID, len);
    set_eax(c, 0);
}
void w_htons(X86 *c) {
    uint32_t v = arg(c, 0) & 0xffff;
    set_eax(c, ((v & 0xff) << 8) | (v >> 8));
}
void w_htonl(X86 *c) {
    uint32_t v = arg(c, 0);
    set_eax(c, (v >> 24) | ((v >> 8) & 0xff00) | ((v << 8) & 0xff0000) | (v << 24));
}
void w_inet_addr(X86 *c) {
    unsigned a, b, cc, d;
    char tail;
    std::string text = gm_str(arg(c, 0), 64);
    if (sscanf(text.c_str(), "%u.%u.%u.%u%c", &a, &b, &cc, &d, &tail) == 4 && a < 256 && b < 256 &&
        cc < 256 && d < 256)
        set_eax(c, a | (b << 8) | (cc << 16) | (d << 24));
    else
        set_eax(c, 0xffffffffu); // INADDR_NONE
}
void w_inet_ntoa(X86 *c) {
    static uint32_t buf = 0; // reused across calls, as Windows does
    if (!buf || !heap_owns(buf))
        buf = heap_alloc(32, true);
    uint32_t a = arg(c, 0);
    char tmp[32];
    snprintf(tmp, sizeof tmp, "%u.%u.%u.%u", a & 0xff, (a >> 8) & 0xff, (a >> 16) & 0xff, a >> 24);
    if (buf)
        gm_put_str(buf, tmp, 32);
    set_eax(c, buf);
}

// One guest SOCKET. TCP sockets map to a lan_* handle once they connect.
struct Sock {
    bool used = false;
    int type = 0;  // 1 stream, 2 datagram
    bool nb = false;
    int h = 0;     // lan_* handle (TCP, after connect)
    uint32_t lport_n = 0; // UDP: the port bind() named, network order
};
const uint32_t SOCK_BASE = 0x100; // SOCKET n is SOCK_BASE + index: never 0, never INVALID_SOCKET
const int NSOCK = 64;
Sock g_socks[NSOCK];

Sock *sock_of(uint32_t s) {
    uint32_t i = s - SOCK_BASE;
    return i < (uint32_t)NSOCK && g_socks[i].used ? &g_socks[i] : nullptr;
}
uint32_t bswap32(uint32_t v) { return __builtin_bswap32(v); }
uint16_t bswap16(uint16_t v) { return (uint16_t)((v >> 8) | (v << 8)); }
// One polling slice of a blocking call: frames in, then let the other guest threads run.
void pump_wait(uint32_t ms) {
    lan_pump();
    guest_sleep_ms(ms);
}
uint32_t conn_error(const Sock &k) {
    switch (lan_tcp_state(k.h)) {
    case LAN_REFUSED: return WSAECONNREFUSED;
    case LAN_TIMEDOUT: return WSAETIMEDOUT;
    case LAN_RESET: return WSAECONNRESET;
    default: return 0;
    }
}

void w_WSAStartup2(X86 *c) {
    w_WSAStartup(c);
    if (recomp_env("LAN") && !lan_online()) {
        lan_mailbox(); // published to the page, which attaches and goes online
        for (int i = 0; i < 200 && !lan_online(); ++i)
            guest_sleep_ms(10);
        if (!lan_online())
            log_once("lan-attach", "winsock: the page did not attach the virtual network in 2 s");
    }
}

void w_socket(X86 *c) {
    uint32_t type = arg(c, 1);
    if (!lan_online())
        return fail(c, WSAENETDOWN, INVALID_SOCKET_);
    if (type == 3)
        return fail(c, WSAEACCES, INVALID_SOCKET_);
    if (type != 1 && type != 2)
        return fail(c, WSAESOCKTNOSUPPORT, INVALID_SOCKET_);
    for (int i = 0; i < NSOCK; ++i)
        if (!g_socks[i].used) {
            g_socks[i] = Sock{};
            g_socks[i].used = true;
            g_socks[i].type = (int)type;
            set_eax(c, SOCK_BASE + (uint32_t)i);
            return;
        }
    fail(c, 10024 /* WSAEMFILE */, INVALID_SOCKET_);
}

void w_closesocket(X86 *c) {
    Sock *k = sock_of(arg(c, 0));
    if (!k)
        return fail(c, WSAENOTSOCK, SOCKET_ERROR_);
    if (k->h)
        lan_tcp_close(k->h);
    *k = Sock{};
    set_eax(c, 0);
}

void w_ioctlsocket(X86 *c) {
    Sock *k = sock_of(arg(c, 0));
    uint32_t cmd = arg(c, 1), p = arg(c, 2);
    if (!k)
        return fail(c, WSAENOTSOCK, SOCKET_ERROR_);
    if (cmd == 0x8004667eu && p) // FIONBIO
        k->nb = rd32(p) != 0;
    else if (cmd == 0x4004667fu && p) // FIONREAD
        wr32(p, 0); // ponytail: reports 0; DirtySock's polled recv does not need the count
    else
        return fail(c, WSAEINVAL, SOCKET_ERROR_);
    set_eax(c, 0);
}

void w_connect(X86 *c) {
    Sock *k = sock_of(arg(c, 0));
    uint32_t sa = arg(c, 1);
    if (!k)
        return fail(c, WSAENOTSOCK, SOCKET_ERROR_);
    if (k->type != 1) {
        log_once("udp-connect", "winsock: connect on a UDP socket is not modelled yet");
        return set_eax(c, 0);
    }
    if (k->h) {
        int st = lan_tcp_state(k->h);
        if (st == LAN_SYN_SENT)
            return fail(c, WSAEALREADY, SOCKET_ERROR_);
        if (st == LAN_EST)
            return fail(c, WSAEISCONN, SOCKET_ERROR_);
        lan_tcp_close(k->h); // a failed earlier attempt: try again
        k->h = 0;
    }
    if (!sa || !gm_valid(sa, 8))
        return fail(c, 10014 /* WSAEFAULT */, SOCKET_ERROR_);
    int h = lan_tcp_connect(bswap32(rd32(sa + 4)), bswap16(rd16(sa + 2)));
    if (h < 0)
        return fail(c, (uint32_t)-h, SOCKET_ERROR_);
    k->h = h;
    if (k->nb)
        return fail(c, WSAEWOULDBLOCK, SOCKET_ERROR_);
    while (lan_tcp_state(h) == LAN_SYN_SENT)
        pump_wait(5);
    if (uint32_t e = conn_error(*k))
        return fail(c, e, SOCKET_ERROR_);
    set_eax(c, 0);
}

void w_send(X86 *c) {
    Sock *k = sock_of(arg(c, 0));
    uint32_t buf = arg(c, 1), len = arg(c, 2);
    if (!k)
        return fail(c, WSAENOTSOCK, SOCKET_ERROR_);
    if (!k->h)
        return fail(c, WSAENOTCONN, SOCKET_ERROR_);
    if (!gm_valid(buf, len))
        return fail(c, 10014, SOCKET_ERROR_);
    int n = lan_tcp_send(k->h, g_mem + buf, (int)len);
    while (n == -(int)WSAEWOULDBLOCK && !k->nb) {
        pump_wait(5);
        n = lan_tcp_send(k->h, g_mem + buf, (int)len);
    }
    if (n < 0)
        return fail(c, (uint32_t)-n, SOCKET_ERROR_);
    set_eax(c, (uint32_t)n);
}

void w_recv(X86 *c) {
    Sock *k = sock_of(arg(c, 0));
    uint32_t buf = arg(c, 1), len = arg(c, 2);
    if (!k)
        return fail(c, WSAENOTSOCK, SOCKET_ERROR_);
    if (k->type == 2)
        return fail(c, WSAEWOULDBLOCK, SOCKET_ERROR_); // ponytail: UDP receives nothing in M1
    if (!k->h)
        return fail(c, WSAENOTCONN, SOCKET_ERROR_);
    if (!gm_valid(buf, len))
        return fail(c, 10014, SOCKET_ERROR_);
    int n = lan_tcp_recv(k->h, g_mem + buf, (int)len);
    while (n == -(int)WSAEWOULDBLOCK && !k->nb) {
        pump_wait(5);
        n = lan_tcp_recv(k->h, g_mem + buf, (int)len);
    }
    if (n < 0)
        return fail(c, (uint32_t)-n, SOCKET_ERROR_);
    set_eax(c, (uint32_t)n);
}

// recvfrom/sendto: TCP behaves as recv/send; UDP is the M1 stub.
void w_recvfrom(X86 *c) {
    Sock *k = sock_of(arg(c, 0));
    if (k && k->type == 1)
        return w_recv(c);
    if (!k)
        return fail(c, WSAENOTSOCK, SOCKET_ERROR_);
    log_once("udp-recv", "winsock: UDP receive is not modelled yet (M2)");
    fail(c, WSAEWOULDBLOCK, SOCKET_ERROR_);
}
void w_sendto(X86 *c) {
    Sock *k = sock_of(arg(c, 0));
    if (k && k->type == 1)
        return w_send(c);
    if (!k)
        return fail(c, WSAENOTSOCK, SOCKET_ERROR_);
    log_once("udp-send", "winsock: UDP send dropped, not modelled yet (M2)");
    set_eax(c, arg(c, 2)); // ponytail: reported sent, as a datagram into the void would be
}

void w_bind(X86 *c) {
    Sock *k = sock_of(arg(c, 0));
    uint32_t sa = arg(c, 1);
    if (!k)
        return fail(c, WSAENOTSOCK, SOCKET_ERROR_);
    if (sa && gm_valid(sa, 4))
        k->lport_n = rd16(sa + 2);
    set_eax(c, 0); // TCP picks its own port at connect; UDP is the M1 stub
}
void w_listen_accept(X86 *c) {
    log_once("listen", "winsock: listen/accept are not modelled yet (M2)");
    fail(c, WSAEOPNOTSUPP, SOCKET_ERROR_);
}
void w_ok(X86 *c) { set_eax(c, 0); } // shutdown, setsockopt: accepted, nothing to change in M1

void w_getsockopt(X86 *c) {
    Sock *k = sock_of(arg(c, 0));
    uint32_t level = arg(c, 1), opt = arg(c, 2), val = arg(c, 3), plen = arg(c, 4);
    if (!k)
        return fail(c, WSAENOTSOCK, SOCKET_ERROR_);
    if (!val || !plen || !gm_valid(val, 4))
        return fail(c, 10014, SOCKET_ERROR_);
    uint32_t v = 0;
    if (level == 0xffff && opt == 0x1007) // SO_ERROR
        v = k->h ? conn_error(*k) : 0;
    else if (level == 0xffff && (opt == 0x1001 || opt == 0x1002)) // SO_SNDBUF, SO_RCVBUF
        v = 65536;
    wr32(val, v);
    wr32(plen, 4);
    set_eax(c, 0);
}

void put_addr(uint32_t sa, uint32_t plen, uint32_t ip_h, uint16_t port) {
    if (!sa || !plen || !gm_valid(sa, 16))
        return;
    memset(g_mem + sa, 0, 16);
    wr16(sa, 2);
    wr16(sa + 2, bswap16(port));
    wr32(sa + 4, bswap32(ip_h));
    wr32(plen, 16);
}
void w_getsockname(X86 *c) {
    Sock *k = sock_of(arg(c, 0));
    if (!k)
        return fail(c, WSAENOTSOCK, SOCKET_ERROR_);
    put_addr(arg(c, 1), arg(c, 2), lan_vip(), k->h ? lan_tcp_lport(k->h) : bswap16((uint16_t)k->lport_n));
    set_eax(c, 0);
}
void w_getpeername(X86 *c) {
    Sock *k = sock_of(arg(c, 0));
    if (!k)
        return fail(c, WSAENOTSOCK, SOCKET_ERROR_);
    if (!k->h || lan_tcp_state(k->h) != LAN_EST)
        return fail(c, WSAENOTCONN, SOCKET_ERROR_);
    // Only the server is reachable in M1, so the peer is the address connect() named.
    uint32_t sa = arg(c, 1), plen = arg(c, 2);
    if (sa && plen && gm_valid(sa, 16)) {
        memset(g_mem + sa, 0, 16);
        wr16(sa, 2);
        wr32(sa + 4, 0x01004D0Au); // ponytail: 10.77.0.1; keep the real peer once peers connect (M3)
        wr32(plen, 16);
    }
    set_eax(c, 0);
}

// select: polls until something is ready or the timeout passes.
bool ready(uint32_t s, int which) {
    Sock *k = sock_of(s);
    if (!k)
        return false;
    if (k->type == 2)
        return which == 1; // UDP: always writable, never readable (M1 stub)
    if (!k->h)
        return false;
    int st = lan_tcp_state(k->h);
    if (which == 0)
        return lan_tcp_ready(k->h);
    if (which == 1)
        return st == LAN_EST;
    return st == LAN_REFUSED || st == LAN_TIMEDOUT; // except: a failed connect
}
void w_select(X86 *c) {
    uint32_t sets[3] = {arg(c, 1), arg(c, 2), arg(c, 3)}, tv = arg(c, 4);
    uint64_t wait_ms = tv ? (uint64_t)rd32(tv) * 1000 + rd32(tv + 4) / 1000 : UINT64_MAX;
    uint64_t start = os_monotonic_ns() / 1000000;
    for (;;) {
        lan_pump();
        uint32_t total = 0;
        uint32_t out[3][64];
        uint32_t nout[3] = {0, 0, 0};
        for (int w = 0; w < 3; ++w) {
            if (!sets[w])
                continue;
            uint32_t n = rd32(sets[w]);
            for (uint32_t i = 0; i < n && i < 64; ++i) {
                uint32_t s = rd32(sets[w] + 4 + 4 * i);
                if (ready(s, w))
                    out[w][nout[w]++] = s;
            }
            total += nout[w];
        }
        uint64_t now = os_monotonic_ns() / 1000000;
        if (total || now - start >= wait_ms) {
            for (int w = 0; w < 3; ++w)
                if (sets[w]) {
                    wr32(sets[w], nout[w]);
                    for (uint32_t i = 0; i < nout[w]; ++i)
                        wr32(sets[w] + 4 + 4 * i, out[w][i]);
                }
            return set_eax(c, total);
        }
        guest_sleep_ms(wait_ms - (now - start) < 5 ? (uint32_t)(wait_ms - (now - start)) : 5);
    }
}

// The one hostent gethostbyname hands out, reused across calls as Windows does:
// hostent (16 bytes) | name (64) | aliases {0} | addr_list {addr, 0} | addr (4)
uint32_t g_hostent;
void w_gethostbyname(X86 *c) {
    std::string name = gm_str(arg(c, 0), 256);
    std::string lower = name;
    for (char &ch : lower)
        ch = (char)tolower((unsigned char)ch);
    unsigned a, b, cc, d;
    char tail;
    uint32_t ip_n = 0;
    if (sscanf(name.c_str(), "%u.%u.%u.%u%c", &a, &b, &cc, &d, &tail) == 4 && a < 256 && b < 256 && cc < 256 &&
        d < 256)
        ip_n = a | (b << 8) | (cc << 16) | (d << 24);
    else if (lan_online() && lower.size() > 7 && lower.compare(lower.size() - 7, 7, ".ea.com") == 0)
        ip_n = 0x01004D0Au; // 10.77.0.1: the server's own host (carbon-online)
    if (!ip_n) {
        log_once(("gethostbyname:" + name).c_str(), "gethostbyname(\"%s\"): not found", name.c_str());
        return fail(c, WSAHOST_NOT_FOUND, 0);
    }
    if (!g_hostent || !heap_owns(g_hostent))
        g_hostent = heap_alloc(128, true);
    uint32_t h = g_hostent;
    gm_put_str(h + 16, name.c_str(), 64);
    wr32(h + 80, 0);          // aliases: none
    wr32(h + 84, h + 92);     // addr_list[0]
    wr32(h + 88, 0);          // addr_list[1]
    wr32(h + 92, ip_n);       // the address, network order
    wr32(h + 0, h + 16);      // h_name
    wr32(h + 4, h + 80);      // h_aliases
    wr16(h + 8, 2);           // h_addrtype AF_INET
    wr16(h + 10, 4);          // h_length
    wr32(h + 12, h + 84);     // h_addr_list
    set_eax(c, h);
}
void w_nodata(X86 *c) { fail(c, WSANO_DATA, 0); } // getservbyname/port, gethostbyaddr

// WSAEVENT is a kernel32 event: manual reset, not signalled (WSACreateEvent's contract).
void w_WSACreateEvent(X86 *c) {
    uint32_t h = win32_create_event(true, false);
    if (!h)
        return fail(c, 10055 /* WSA_NOT_ENOUGH_MEMORY maps here */, 0);
    set_eax(c, h);
}
// WaitForMultipleObjects plus an unused alertable flag; the same return values.
void w_WSAWaitForMultipleEvents(X86 *c) {
    uint32_t n = arg(c, 0), list = arg(c, 1), all = arg(c, 2), ms = arg(c, 3);
    if (n == 0 || n > 64 || !gm_valid(list, n * 4))
        return fail(c, WSAEINVAL, 0xffffffffu); // WSA_WAIT_FAILED
    uint32_t handles[64];
    for (uint32_t i = 0; i < n; ++i)
        handles[i] = rd32(list + 4 * i);
    lan_pump();
    set_eax(c, guest_wait_objects(handles, n, all != 0, ms));
}
// Overlapped receive: only the UDP and raw sockets of the socket thread use it,
// and the UDP stub never receives, so the receive stays posted. M2 completes it.
void w_WSARecvAny(X86 *c) {
    Sock *k = sock_of(arg(c, 0));
    if (!k)
        return fail(c, WSAENOTSOCK, SOCKET_ERROR_);
    if (k->type == 1) {
        log_once("tcp-overlapped-recv", "winsock: WSARecv/WSARecvFrom called on a TCP socket");
        return fail(c, WSAEOPNOTSUPP, SOCKET_ERROR_); // ponytail: TCP overlapped is unused by Carbon
    }
    fail(c, WSA_IO_PENDING, SOCKET_ERROR_);
}
void w_WSAGetOverlappedResult(X86 *c) {
    if (!sock_of(arg(c, 0)))
        return fail(c, WSAENOTSOCK, 0);
    fail(c, WSA_IO_INCOMPLETE, 0);
}

const ImportShim shims[] = {
    {"WS2_32.dll", "WSAStartup", 2, w_WSAStartup2},
    {"WS2_32.dll", "ord115", 2, w_WSAStartup2},
    {"WS2_32.dll", "WSACleanup", 0, w_WSACleanup},
    {"WS2_32.dll", "ord116", 0, w_WSACleanup},
    {"WS2_32.dll", "WSAGetLastError", 0, w_WSAGetLastError},
    {"WS2_32.dll", "ord111", 0, w_WSAGetLastError},
    {"WS2_32.dll", "socket", 3, w_socket},
    {"WS2_32.dll", "ord23", 3, w_socket},
    {"WS2_32.dll", "gethostbyname", 1, w_gethostbyname},
    {"WS2_32.dll", "ord52", 1, w_gethostbyname},
    {"WS2_32.dll", "gethostname", 2, w_gethostname},
    {"WS2_32.dll", "ord57", 2, w_gethostname},
    {"WS2_32.dll", "htons", 1, w_htons},
    {"WS2_32.dll", "ntohs", 1, w_htons},
    {"WS2_32.dll", "htonl", 1, w_htonl},
    {"WS2_32.dll", "ntohl", 1, w_htonl},
    {"WS2_32.dll", "inet_addr", 1, w_inet_addr},
    {"WS2_32.dll", "inet_ntoa", 1, w_inet_ntoa},
    {"WS2_32.dll", "accept", 3, w_listen_accept}, {"WS2_32.dll", "ord1", 3, w_listen_accept},
    {"WS2_32.dll", "bind", 3, w_bind},            {"WS2_32.dll", "ord2", 3, w_bind},
    {"WS2_32.dll", "closesocket", 1, w_closesocket}, {"WS2_32.dll", "ord3", 1, w_closesocket},
    {"WS2_32.dll", "connect", 3, w_connect},      {"WS2_32.dll", "ord4", 3, w_connect},
    {"WS2_32.dll", "getpeername", 3, w_getpeername}, {"WS2_32.dll", "ord5", 3, w_getpeername},
    {"WS2_32.dll", "getsockname", 3, w_getsockname}, {"WS2_32.dll", "ord6", 3, w_getsockname},
    {"WS2_32.dll", "getsockopt", 5, w_getsockopt}, {"WS2_32.dll", "ord7", 5, w_getsockopt},
    {"WS2_32.dll", "ioctlsocket", 3, w_ioctlsocket}, {"WS2_32.dll", "ord10", 3, w_ioctlsocket},
    {"WS2_32.dll", "listen", 2, w_listen_accept}, {"WS2_32.dll", "ord13", 2, w_listen_accept},
    {"WS2_32.dll", "recv", 4, w_recv},            {"WS2_32.dll", "ord16", 4, w_recv},
    {"WS2_32.dll", "recvfrom", 6, w_recvfrom},    {"WS2_32.dll", "ord17", 6, w_recvfrom},
    {"WS2_32.dll", "select", 5, w_select},        {"WS2_32.dll", "ord18", 5, w_select},
    {"WS2_32.dll", "send", 4, w_send},            {"WS2_32.dll", "ord19", 4, w_send},
    {"WS2_32.dll", "sendto", 6, w_sendto},        {"WS2_32.dll", "ord20", 6, w_sendto},
    {"WS2_32.dll", "setsockopt", 5, w_ok},        {"WS2_32.dll", "ord21", 5, w_ok},
    {"WS2_32.dll", "shutdown", 2, w_ok},          {"WS2_32.dll", "ord22", 2, w_ok},
    {"WS2_32.dll", "WSAIoctl", 9, w_listen_accept},
    {"WS2_32.dll", "WSARecv", 7, w_WSARecvAny},
    {"WS2_32.dll", "WSARecvFrom", 9, w_WSARecvAny},
    {"WS2_32.dll", "WSAGetOverlappedResult", 5, w_WSAGetOverlappedResult},
    {"WS2_32.dll", "WSACreateEvent", 0, w_WSACreateEvent},
    {"WS2_32.dll", "WSAWaitForMultipleEvents", 5, w_WSAWaitForMultipleEvents},
    {"WS2_32.dll", "getservbyport", 2, w_nodata},
    {"WS2_32.dll", "getservbyname", 2, w_nodata},
    {"WS2_32.dll", "gethostbyaddr", 3, w_nodata},
};

} // namespace

void winsock_register() {
    imports_register(shims, sizeof shims / sizeof shims[0]);
}

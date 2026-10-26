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
#include <cstdio>
#include <cstring>

namespace {

enum : uint32_t {
    WSAEWOULDBLOCK = 10035,
    WSAENOTSOCK = 10038,
    WSAEINVAL = 10022,
    WSAENETDOWN = 10050,
    WSAECONNRESET = 10054,
    WSAENOTCONN = 10057,
    WSAHOST_NOT_FOUND = 11001,
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
void w_socket(X86 *c) { fail(c, WSAENETDOWN, INVALID_SOCKET_); }
void w_gethostbyname(X86 *c) {
    log_once("gethostbyname", "gethostbyname(\"%s\"): no network", gm_str(arg(c, 0), 256).c_str());
    fail(c, WSAHOST_NOT_FOUND, 0);
}
void w_gethostname(X86 *c) {
    uint32_t buf = arg(c, 0), len = arg(c, 1);
    if (buf && len)
        gm_put_str(buf, RECOMP_GAME_ID, len);
    set_eax(c, 0);
}
// Any call on a SOCKET the table does not know: Windows answers WSAENOTSOCK.
void w_notsock(X86 *c) { fail(c, WSAENOTSOCK, SOCKET_ERROR_); }
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

const ImportShim shims[] = {
    {"WS2_32.dll", "WSAStartup", 2, w_WSAStartup},
    {"WS2_32.dll", "ord115", 2, w_WSAStartup},
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
    // SOCKET calls: nothing can hold a SOCKET yet. Tasks 3 and 4 replace these.
    {"WS2_32.dll", "accept", 3, w_notsock},      {"WS2_32.dll", "ord1", 3, w_notsock},
    {"WS2_32.dll", "bind", 3, w_notsock},        {"WS2_32.dll", "ord2", 3, w_notsock},
    {"WS2_32.dll", "closesocket", 1, w_notsock}, {"WS2_32.dll", "ord3", 1, w_notsock},
    {"WS2_32.dll", "connect", 3, w_notsock},     {"WS2_32.dll", "ord4", 3, w_notsock},
    {"WS2_32.dll", "getpeername", 3, w_notsock}, {"WS2_32.dll", "ord5", 3, w_notsock},
    {"WS2_32.dll", "getsockname", 3, w_notsock}, {"WS2_32.dll", "ord6", 3, w_notsock},
    {"WS2_32.dll", "getsockopt", 5, w_notsock},  {"WS2_32.dll", "ord7", 5, w_notsock},
    {"WS2_32.dll", "ioctlsocket", 3, w_notsock}, {"WS2_32.dll", "ord10", 3, w_notsock},
    {"WS2_32.dll", "listen", 2, w_notsock},      {"WS2_32.dll", "ord13", 2, w_notsock},
    {"WS2_32.dll", "recv", 4, w_notsock},        {"WS2_32.dll", "ord16", 4, w_notsock},
    {"WS2_32.dll", "recvfrom", 6, w_notsock},    {"WS2_32.dll", "ord17", 6, w_notsock},
    {"WS2_32.dll", "select", 5, w_notsock},      {"WS2_32.dll", "ord18", 5, w_notsock},
    {"WS2_32.dll", "send", 4, w_notsock},        {"WS2_32.dll", "ord19", 4, w_notsock},
    {"WS2_32.dll", "sendto", 6, w_notsock},      {"WS2_32.dll", "ord20", 6, w_notsock},
    {"WS2_32.dll", "setsockopt", 5, w_notsock},  {"WS2_32.dll", "ord21", 5, w_notsock},
    {"WS2_32.dll", "shutdown", 2, w_notsock},    {"WS2_32.dll", "ord22", 2, w_notsock},
    {"WS2_32.dll", "WSAIoctl", 9, w_notsock},
    {"WS2_32.dll", "WSARecv", 7, w_notsock},
    {"WS2_32.dll", "WSARecvFrom", 9, w_notsock},
    {"WS2_32.dll", "WSAGetOverlappedResult", 5, w_notsock},
    // Events and the database calls: Task 4 and Task 3 fill these in.
    {"WS2_32.dll", "WSACreateEvent", 0, nullptr},
    {"WS2_32.dll", "WSACloseEvent", 1, nullptr},
    {"WS2_32.dll", "WSASetEvent", 1, nullptr},
    {"WS2_32.dll", "WSAResetEvent", 1, nullptr},
    {"WS2_32.dll", "WSAWaitForMultipleEvents", 5, nullptr},
    {"WS2_32.dll", "getservbyport", 2, nullptr},
    {"WS2_32.dll", "getservbyname", 2, nullptr},
    {"WS2_32.dll", "gethostbyaddr", 3, nullptr},
};

} // namespace

void winsock_register() {
    imports_register(shims, sizeof shims / sizeof shims[0]);
}

// Winsock and virtual-network tests; no game image required. The test plays
// the page, the switch and the server on the mailbox rings (net_proto.h).
#include "../imports.h"
#include "../loader.h"
#include "../memory.h"
#include "../win32.h"
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <vector>

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

int main() {
    mem_init();
    imports_init();
    test_offline_socket_fails();
    printf("%d checks, %d failures\n", g_checks, g_failures);
    mem_shutdown();
    return g_failures ? 1 : 0;
}

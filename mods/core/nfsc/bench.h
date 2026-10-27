/* bench.h - the game flow's state changes on the log, for the race benchmark (included by nfsc.c).
   RECOMP_BENCH=1 only: a line "core.nfsc: flow <name>" each time the GameFlowManager enters another
   state, so a script waits for the front end and the race to finish loading instead of sleeping a
   guessed time. The manager is two words: [0xa99b9c] the state's callback, run every frame while the
   state lasts, and [0xa99ba4] its name, both written in place by many routines. The callbacks are only
   reached through that pointer, so they cannot be hooked (POP_E_NOSYMBOL); the words are read after
   cFEng::Service (00600c90) instead, which runs once a frame while the menus or the race HUD do. */
#define GFM_CALLBACK 0x00a99b9cu
#define GFM_NAME 0x00a99ba4u

static uint32_t g_flow_cb = 1, g_flow_name = 1; /* what was last logged */

static void bench_flow(const PopModApi *api, pop_cpu_v1 *cpu, PopHookInvocation *inv, void *user) {
    (void)cpu;
    (void)inv;
    (void)user;
    const uint32_t cb = get_u32(GFM_CALLBACK), name = get_u32(GFM_NAME);
    if (cb == g_flow_cb && name == g_flow_name)
        return;
    g_flow_cb = cb;
    g_flow_name = name;
    void *p = NULL;
    char line[96];
    snprintf(line, sizeof line, "core.nfsc: flow %.40s (state %08x)",
             name && api->guest_ptr(api, name, 40, &p) == POP_OK ? (const char *)p : "-", cb);
    log_line(line);
}

static void bench_install(void) {
    if (!getenv("RECOMP_BENCH"))
        return;
    install(0x00600c90u, bench_flow, POP_HOOK_AFTER, 28);
}

/* online.h - hide the main menu's online entries (setting hide_online, default on).
 *
 * The main menu carousel is built by 0083c420: ten items, each made by 005869c0
 * (label hash, choice id) and added with 005b2320 (thiscall scroller, item; RET 4;
 * its result is ignored). Quick Match (MENU_MAIN_QUICK_MATCH, id 4) and Custom
 * Match (MENU_MAIN_CUSTOM_MATCH, id 5) are EA Online modes whose servers are gone;
 * picking either asks to connect. Their two add calls (returning to 0083c611 and
 * 0083c5c3) are skipped, so the carousel never has them; every other menu built
 * with 005b2320 is untouched. */
#define FE_ADD_ITEM 0x005b2320u
#define ADD_QUICK_MATCH_RET 0x0083c611u
#define ADD_CUSTOM_MATCH_RET 0x0083c5c3u

/* One REPLACE hook filtering on the return address: the kit takes a single
 * REPLACE per function, so two callsite hooks could not both be installed. */
static void online_add_item(const PopModApi *api, pop_cpu_v1 *cpu, PopHookInvocation *inv, void *user) {
    (void)inv;
    (void)user;
    const uint32_t ret = get_u32(cpu->esp);
    if (ret == ADD_QUICK_MATCH_RET || ret == ADD_CUSTOM_MATCH_RET) {
        api->hook_return(api, cpu, 0, 4); /* thiscall, one argument */
        return;
    }
    api->call_original(api, cpu->target, cpu);
}

/* FESL as plain TCP: 008df480 returns the title parameters (this + 0x4c); 0093e210
 * logs their options word at +0x10, bit 0x1 being SSL_OVERRIDE_OFF. Set it each
 * time the struct is handed out (idempotent) when the page asked (fesl_plain). */
#define TITLE_PARAMS_GET 0x008df480u
#define TITLE_OPTIONS_OFF 0x10u
#define TITLE_SSL_OVERRIDE_OFF 0x1u

/* ponytail: FUN_008df480 has ~25 call sites and the decompile does not show
 * whether every caller passes the same object in ECX; writing bit 0x1 into
 * the wrong object's +0x10 would corrupt unrelated memory. Until Task 8's
 * live capture settles it, the hook only ever writes to the FIRST pointer it
 * observes (set below, only while fesl_plain is on) and just logs once -
 * separately - any different pointer it sees, without touching it. */
static uint32_t g_title_params_first;
static int g_title_params_other_logged;

static void online_title_params(const PopModApi *api, pop_cpu_v1 *cpu, PopHookInvocation *inv, void *user) {
    (void)api;
    (void)inv;
    (void)user;
    const uint32_t ptr = cpu->eax;
    char line[96];
    if (!g_title_params_first) {
        g_title_params_first = ptr;
        snprintf(line, sizeof line, "core.nfsc: title params struct first seen at %08x", ptr);
        log_line(line);
    }
    if (ptr != g_title_params_first) {
        if (!g_title_params_other_logged) {
            g_title_params_other_logged = 1;
            snprintf(line, sizeof line, "core.nfsc: FUN_008df480 also returned %08x (first was %08x), not touched",
                     ptr, g_title_params_first);
            log_line(line);
        }
        return;
    }
    const uint32_t at = ptr + TITLE_OPTIONS_OFF;
    put_u32(at, get_u32(at) | TITLE_SSL_OVERRIDE_OFF);
}

static void online_install(void) {
    /* FESL as plain TCP: 008df480 returns the title parameters (this + 0x4c);
     * 0093e210 logs their options word at +0x10, bit 0x1 being SSL_OVERRIDE_OFF.
     * Set it each time the struct is handed out (idempotent) when the page
     * asked (fesl_plain).
     *
     * ponytail: FUN_008df480 has ~25 call sites and the decompile does not
     * show whether every caller passes the same object in ECX (the returned
     * pointer); writing the wrong object's +0x10 would corrupt unrelated
     * memory. Until Task 8's live capture settles it, the hook only ever
     * writes to the FIRST pointer it observes and just logs (once) any
     * different pointer it sees, without touching it. */
    if (setting("fesl_plain", 0) && install(TITLE_PARAMS_GET, online_title_params, POP_HOOK_AFTER, 24) == POP_OK)
        log_line("core.nfsc: FESL set to plain TCP (SSL_OVERRIDE_OFF)");
    if (!setting("hide_online", 1))
        return;
    if (install(FE_ADD_ITEM, online_add_item, POP_HOOK_REPLACE, 18) != POP_OK)
        log_line("core.nfsc: could not hide the online menu entries");
    else
        log_line("core.nfsc: online menu entries hidden");
}

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

static void online_skip_item(const PopModApi *api, pop_cpu_v1 *cpu, PopHookInvocation *inv, void *user) {
    (void)inv;
    (void)user;
    api->hook_return(api, cpu, 0, 4); /* thiscall, one argument */
}

static void online_install(void) {
    if (!setting("hide_online", 1))
        return;
    if (!g_api->hook_install_at_callsite ||
        g_api->hook_install_at_callsite(g_api, FE_ADD_ITEM, ADD_QUICK_MATCH_RET, online_skip_item,
                                        POP_HOOK_REPLACE, 0, &g_hooks[18]) != POP_OK ||
        g_api->hook_install_at_callsite(g_api, FE_ADD_ITEM, ADD_CUSTOM_MATCH_RET, online_skip_item,
                                        POP_HOOK_REPLACE, 0, &g_hooks[19]) != POP_OK)
        log_line("core.nfsc: could not hide the online menu entries");
    else
        log_line("core.nfsc: online menu entries hidden");
}

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

/* FESL as plain TCP: FUN_00938f50 calls FUN_00977d10(state, 1) when secure != 0,
 * which does `*(u16*)(state+0x90) = 1` (DirtySock's secure flag, cdecl, one caller).
 * Replace it with a no-op so the flag stays off and the login dials plain TCP. */
#define FESL_SET_SECURE 0x00977d10u

static void online_fesl_set_secure(const PopModApi *api, pop_cpu_v1 *cpu, PopHookInvocation *inv, void *user) {
    (void)inv;
    (void)user;
    api->hook_return(api, cpu, cpu->eax, 0); /* cdecl, caller pops; leave state+0x90 untouched */
}

/* Quick online (planning/specs/2026-10-26-quick-online-and-cloud-saves-design.md; the spike's evidence:
 * planning/research/2026-10-26-signin-skip-spike.md). The page passes the player's name and the login code its
 * key gives (RECOMP_ONLINE_NAME, RECOMP_ONLINE_CODE); Carbon's own "Automatically Sign in" logs in with them.
 * FUN_0071fcc0 (cdecl, two arguments, plain RET) creates the login flow for all six callers, and the flow's Start
 * (0x5a7e00, not hookable) runs right after it. The options struct is *(*(u32*)0xa8acfc + 8): account name at
 * +0x10 (32 bytes), password at +0x30 (32 bytes), the auto sign-in flag at +0x6b. A refused login makes Carbon
 * clear the flag itself (0x723ff0, case 0x1b) and show EA Sign In; from then on this session leaves the fields
 * to the player. */
#define LOGIN_FLOW_CREATE 0x0071fcc0u
#define ONLINE_OPTS_ROOT 0x00a8acfcu

static int g_quick_written, g_quick_off;

static void quick_put(uint32_t at, const char *s, uint32_t cap) {
    uint32_t i = 0;
    for (; s[i] && i < cap - 1; i++)
        g_api->guest_write_u8(g_api, at + i, (uint8_t)s[i]);
    for (; i < cap; i++)
        g_api->guest_write_u8(g_api, at + i, 0);
}

static void online_login_flow(const PopModApi *api, pop_cpu_v1 *cpu, PopHookInvocation *inv, void *user) {
    (void)inv;
    (void)user;
    const char *name = getenv("RECOMP_ONLINE_NAME"), *code = getenv("RECOMP_ONLINE_CODE");
    const uint32_t root = get_u32(ONLINE_OPTS_ROOT), opts = root ? get_u32(root + 8) : 0;
    if (opts && name && *name && code && *code && !g_quick_off) {
        uint8_t flag = 0;
        api->guest_read_u8(api, opts + 0x6b, &flag);
        if (g_quick_written && !flag) {
            g_quick_off = 1; /* Carbon turned it off after a refusal: hands off */
            log_line("core.nfsc: quick online off for this session (the login was refused)");
        } else {
            quick_put(opts + 0x10, name, 32);
            quick_put(opts + 0x30, code, 32);
            api->guest_write_u8(api, opts + 0x6b, 1);
            g_quick_written = 1;
        }
    }
    api->call_original(api, cpu->target, cpu);
}

/* First run: the page's name becomes the alias, autosave on, with no prompts. A new player is asked
   "Create a new alias?" (memcard state 0xb), then for a name, then "enable Autosave and save your alias
   now?" (0x14). Both dialogs go through the memcard state manager's ShowDialog (vtable+0x10c); this hook
   answers Yes for them, with the page's name. A returning player's save loads without either dialog.
   Trace: planning/research/2026-10-27-profile-skip-spike.md. */
#define MC_SHOW_DIALOG 0x005d0d40u  /* thiscall, no arguments, plain RET */
#define MC_CHANGE_STATE 0x00579200u /* thiscall (state) */
#define MC_SETTINGS 0x00a8ad30u
#define MC_SET_INT 0x004a04e0u      /* thiscall (id, value, -1, -1, -1) */
#define MC_AUTOSAVE 0x13aeu
#define MC_PROFILE_ROOT 0x00a97a7cu /* profile = [[[root]+0xd4]], alias char[32] at +0x10 */

/* The page's name if it can be the alias: 1 to 15 of A-Z a-z 0-9 _ -. The alias also names the save
   folder, and any RECOMP_ URL parameter reaches the environment, so any other name keeps the prompts. */
static const char *alias_name(void) {
    const char *n = getenv("RECOMP_ONLINE_NAME");
    unsigned i = 0;
    for (; n && n[i]; ++i) {
        const char c = n[i];
        if (i == 15 || !((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                         c == '_' || c == '-'))
            return NULL;
    }
    return i ? n : NULL;
}

static int g_alias_busy; /* the state change below shows 0x14 again through this hook */

static void online_first_alias(const PopModApi *api, pop_cpu_v1 *cpu, PopHookInvocation *inv, void *user) {
    (void)inv;
    (void)user;
    const uint32_t sm = cpu->ecx;
    const char *name = alias_name();
    uint32_t state = get_u32(sm + 4), eax = 0;
    if (get_u32(sm + 8) != 0 || !name || (state != 0x0b && state != 0x14)) {
        api->call_original(api, cpu->target, cpu); /* not the boot flow's first-run dialogs */
        return;
    }
    if (!g_alias_busy) {
        g_alias_busy = 1;
        if (state == 0x0b) { /* Yes, as the game's own answer does; skip the notices and the NAME box */
            api->guest_write_u8(api, sm + 0xd5, 1);
            uint32_t s = 0x1d; /* "name accepted": sets the (empty) name, then asks 0x14 */
            api->guest_call(api, MC_CHANGE_STATE, sm, &s, 1, &eax);
            state = get_u32(sm + 4);
        }
        if (state == 0x14) { /* Yes, with the page's name */
            quick_put(get_u32(get_u32(get_u32(MC_PROFILE_ROOT) + 0xd4)) + 0x10, name, 32);
            const uint32_t on[5] = {MC_AUTOSAVE, 1, ~0u, ~0u, ~0u};
            api->guest_call(api, MC_SET_INT, MC_SETTINGS, on, 5, &eax);
            uint32_t s = 0x19; /* the game's own save */
            api->guest_call(api, MC_CHANGE_STATE, sm, &s, 1, &eax);
            log_line("core.nfsc: first-run alias created from the page name, autosave on");
        }
        g_alias_busy = 0;
    }
    api->hook_return(api, cpu, cpu->eax, 0);
}

static void online_install(void) {
    if (setting("fesl_plain", 0) && install(FESL_SET_SECURE, online_fesl_set_secure, POP_HOOK_REPLACE, 24) == POP_OK)
        log_line("core.nfsc: FESL set to plain TCP (DirtySock secure flag kept off)");
    if (getenv("RECOMP_ONLINE_NAME") && install(LOGIN_FLOW_CREATE, online_login_flow, POP_HOOK_REPLACE, 25) == POP_OK)
        log_line("core.nfsc: quick online: signing in under the page's name");
    if (alias_name() && install(MC_SHOW_DIALOG, online_first_alias, POP_HOOK_REPLACE, 20) == POP_OK)
        log_line("core.nfsc: first run: the page's name becomes the alias, autosave on");
    if (!setting("hide_online", 1))
        return;
    if (install(FE_ADD_ITEM, online_add_item, POP_HOOK_REPLACE, 18) != POP_OK)
        log_line("core.nfsc: could not hide the online menu entries");
    else
        log_line("core.nfsc: online menu entries hidden");
}

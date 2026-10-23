/* options.h - the port's settings as rows in the game's own Options screens (included by nfsc.c).
 *
 * Carbon's options are numbered (the PC ones 0x1388..0x13eb). An Options screen's setup routine adds
 * its rows one by one with FeOptionsScreen::AddOption (005c1d70, thiscall screen, RET 4), which makes
 * the widget for that number and attaches it as the next OPTION_n object (005711c0 "OPTION_%d" hash,
 * 005bca70 stdcall (name, widget)); the screen is [0x00a97ba8] while it is being set up. Widgets read
 * and change their value through the options manager at 0x00a8ad30, whose methods pass
 * (id, a, b, c, d) to the handler for id / 1000 (table at +8):
 *   004a0490 get     (+0x18): *a = value, AL = 1       choice rows show value index 0..4
 *   004a04e0 set     (+0x1c): value a                  toggles write !get, "reset" writes default
 *   004a0530 step    (+0x20): a = -1 / +1              choice rows' left / right
 *   004a0670 default (+0x30): *a = default value
 * The choice widget (0058c0e0, thiscall (uint32 *label, id, five value-name hashes), RET 0x1c; 0x8c
 * bytes; +0x64 = usable, +0x74 id) shows the name hash for the index get returns (00594f10), or the
 * number when that hash is 0x881f8efa.
 * Our rows use numbers 0x1400.. (still the 5000s handler, which refuses numbers it does not know, so
 * a manager method not hooked here stays harmless). The hooks answer for our numbers and pass every
 * other one to the game. Labels are string hashes answered by localized_wide (nfsc.c).
 */
#include <stddef.h>
static uint32_t string_hash(const char *t);

#define OPT_MANAGER 0x00a8ad30u
#define OPT_GET 0x004a0490u
#define OPT_SET 0x004a04e0u
#define OPT_STEP 0x004a0530u
#define OPT_DEFAULT 0x004a0670u
#define OPT_ADD_ROW 0x005c1d70u
#define OPT_SCREEN 0x00a97ba8u
#define OPT_ROW_COUNT 0x164u      /* screen: rows added so far (the n of OPTION_n) */
#define GAME_MALLOC 0x006a1560u
#define FORMAT_HASH 0x005711c0u   /* cdecl (format, ...) -> bStringHash of the text */
#define FMT_OPTION_N 0x009d54e8u  /* "OPTION_%d" */
#define CHOICE_WIDGET 0x0058c0e0u
#define ATTACH_ROW 0x005bca70u
#define CHOICE_BYTES 0x8cu
#define WIDGET_USABLE 0x64u
#define VALUE_AS_NUMBER 0x881f8efau
#define VIDEO_LAST_ROW 0x13dbu    /* Brightness: the last row of the Video screen (005c68a0) */

typedef struct {
    uint32_t id;
    uint32_t after; /* the game's row these follow */
    const char *label;
    int count; /* choices */
    const char *values[5];
    int def;
    int (*get)(void);
    void (*set)(int);
} OptRow;

/* Host display rows (kit display_get / display_set). */
static int display_value(const char *key, int fallback) {
    int64_t v = fallback;
    if (g_api->size >= offsetof(PopModApi, display_set) + sizeof g_api->display_set && g_api->display_get)
        g_api->display_get(g_api, key, &v);
    return (int)v;
}
static void display_change(const char *key, int value) {
    if (g_api->size < offsetof(PopModApi, display_set) + sizeof g_api->display_set || !g_api->display_set)
        return;
    const PopModStatus s = g_api->display_set(g_api, key, value);
    if (s != POP_OK) {
        char line[96];
        snprintf(line, sizeof line, "core.nfsc: options: %s = %d refused (%d)", key, value, (int)s);
        log_line(line);
    }
}
static int window_get(void) { return display_value("window", 2); }
static void window_set(int v) { display_change("window", v); }

static OptRow g_opt_rows[] = {
    {0x1400u, VIDEO_LAST_ROW, "Display Mode", 3, {"Windowed", "Borderless", "Full Screen"}, 2, window_get, window_set},
};
#define OPT_ROWS (sizeof g_opt_rows / sizeof g_opt_rows[0])
static uint32_t g_opt_label_hash[OPT_ROWS], g_opt_value_hash[OPT_ROWS][5];
static uint32_t g_opt_scratch;  /* guest dword: the label hash the widget constructor reads */
static uint32_t g_opt_adding;   /* the number AddOption was asked for */

static OptRow *opt_row(uint32_t id) {
    for (unsigned i = 0; i < OPT_ROWS; ++i)
        if (g_opt_rows[i].id == id)
            return &g_opt_rows[i];
    return NULL;
}

/* Text for our label and value hashes (localized_wide). */
static const char *options_text(uint32_t hash) {
    for (unsigned i = 0; i < OPT_ROWS; ++i) {
        if (g_opt_label_hash[i] == hash)
            return g_opt_rows[i].label;
        for (int k = 0; k < g_opt_rows[i].count; ++k)
            if (g_opt_value_hash[i][k] == hash)
                return g_opt_rows[i].values[k];
    }
    return NULL;
}

/* One of our rows on the screen being set up, made the way AddOption makes the game's. */
static void opt_add_row(const PopModApi *api, OptRow *r, unsigned index) {
    const uint32_t screen = get_u32(OPT_SCREEN);
    if (!screen || !g_opt_scratch)
        return;
    const uint32_t n = get_u32(screen + OPT_ROW_COUNT) + 1;
    api->guest_write_u32(api, screen + OPT_ROW_COUNT, n);
    uint32_t name = 0, widget = 0;
    const uint32_t fmt[2] = {FMT_OPTION_N, n};
    api->guest_call(api, FORMAT_HASH, 0, fmt, 2, &name);
    const uint32_t size = CHOICE_BYTES;
    if (api->guest_call(api, GAME_MALLOC, 0, &size, 1, &widget) != POP_OK || !widget)
        return;
    api->guest_write_u32(api, g_opt_scratch, g_opt_label_hash[index]);
    uint32_t args[7] = {g_opt_scratch, r->id, 0, 0, 0, 0, 0};
    for (int k = 0; k < 5; ++k)
        args[2 + k] = k < r->count ? g_opt_value_hash[index][k] : VALUE_AS_NUMBER;
    api->guest_call(api, CHOICE_WIDGET, widget, args, 7, NULL);
    uint32_t usable = get_u32(widget + WIDGET_USABLE);
    api->guest_write_u32(api, widget + WIDGET_USABLE, (usable & 0xffffff00u) | 1u);
    const uint32_t attach[2] = {name, widget};
    api->guest_call(api, ATTACH_ROW, 0, attach, 2, NULL);
}

static void opt_add_enter(const PopModApi *api, pop_cpu_v1 *cpu, PopHookInvocation *inv, void *user) {
    (void)api;
    (void)inv;
    (void)user;
    g_opt_adding = get_u32(cpu->esp + 4);
}

static void opt_add_leave(const PopModApi *api, pop_cpu_v1 *cpu, PopHookInvocation *inv, void *user) {
    (void)cpu;
    (void)inv;
    (void)user;
    const uint32_t after = g_opt_adding;
    g_opt_adding = 0;
    for (unsigned i = 0; i < OPT_ROWS; ++i)
        if (g_opt_rows[i].after == after)
            opt_add_row(api, &g_opt_rows[i], i);
}

/* Manager methods: thiscall (id, a, b, c, d), RET 0x14, AL = handled. */
static void opt_get(const PopModApi *api, pop_cpu_v1 *cpu, PopHookInvocation *inv, void *user) {
    (void)inv;
    (void)user;
    OptRow *r = opt_row(get_u32(cpu->esp + 4));
    if (!r) {
        api->call_original(api, cpu->target, cpu);
        return;
    }
    const uint32_t out = get_u32(cpu->esp + 8);
    if (out)
        api->guest_write_u32(api, out, (uint32_t)r->get());
    api->hook_return(api, cpu, (cpu->eax & 0xffffff00u) | 1u, 0x14);
}

static void opt_set(const PopModApi *api, pop_cpu_v1 *cpu, PopHookInvocation *inv, void *user) {
    (void)inv;
    (void)user;
    OptRow *r = opt_row(get_u32(cpu->esp + 4));
    if (!r) {
        api->call_original(api, cpu->target, cpu);
        return;
    }
    const int v = (int)get_u32(cpu->esp + 8);
    if (v >= 0 && v < r->count)
        r->set(v);
    api->hook_return(api, cpu, (cpu->eax & 0xffffff00u) | 1u, 0x14);
}

static void opt_step(const PopModApi *api, pop_cpu_v1 *cpu, PopHookInvocation *inv, void *user) {
    (void)inv;
    (void)user;
    OptRow *r = opt_row(get_u32(cpu->esp + 4));
    if (!r) {
        api->call_original(api, cpu->target, cpu);
        return;
    }
    const int delta = (int)get_u32(cpu->esp + 8) < 0 ? -1 : 1;
    r->set((r->get() + delta + r->count) % r->count);
    api->hook_return(api, cpu, (cpu->eax & 0xffffff00u) | 1u, 0x14);
}

static void opt_default(const PopModApi *api, pop_cpu_v1 *cpu, PopHookInvocation *inv, void *user) {
    (void)inv;
    (void)user;
    OptRow *r = opt_row(get_u32(cpu->esp + 4));
    if (!r) {
        api->call_original(api, cpu->target, cpu);
        return;
    }
    const uint32_t out = get_u32(cpu->esp + 8);
    if (out)
        api->guest_write_u32(api, out, (uint32_t)r->def);
    api->hook_return(api, cpu, (cpu->eax & 0xffffff00u) | 1u, 0x14);
}

static void options_install(const PopModApi *api) {
    char key[48];
    for (unsigned i = 0; i < OPT_ROWS; ++i) {
        snprintf(key, sizeof key, "NFSC_MAC_OPTION_%04X", g_opt_rows[i].id);
        g_opt_label_hash[i] = string_hash(key);
        for (int k = 0; k < g_opt_rows[i].count; ++k) {
            snprintf(key, sizeof key, "NFSC_MAC_OPTION_%04X_%d", g_opt_rows[i].id, k);
            g_opt_value_hash[i][k] = string_hash(key);
        }
    }
    if (api->guest_alloc(api, 4, &g_opt_scratch) != POP_OK)
        g_opt_scratch = 0;
    if (install(OPT_ADD_ROW, opt_add_enter, POP_HOOK_BEFORE, 24) != POP_OK ||
        install(OPT_ADD_ROW, opt_add_leave, POP_HOOK_AFTER, 25) != POP_OK ||
        install(OPT_GET, opt_get, POP_HOOK_REPLACE, 26) != POP_OK ||
        install(OPT_SET, opt_set, POP_HOOK_REPLACE, 27) != POP_OK ||
        install(OPT_STEP, opt_step, POP_HOOK_REPLACE, 28) != POP_OK ||
        install(OPT_DEFAULT, opt_default, POP_HOOK_REPLACE, 29) != POP_OK)
        log_line("core.nfsc: settings in the Options menus unavailable");
}

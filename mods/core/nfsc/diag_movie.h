/* diag_movie.h - TEMPORARY (2026-10-10): trail for the start-up black screen.
 * A live capture showed the game on FeBlackScreen.fng + FEAnyMovie.fng with
 * "EA_Bumper" queued and no movie running. Each frame (before cFEng::Service)
 * this logs a line whenever one of these changes: the pause reasons
 * (0x00a8bae4, set by 0050c370; window focus = bit 16, the movie screen = bit
 * 11), the focus flags (0x00ab0ad0 WM_ACTIVATEAPP, 0x00ab0b3c WM_ACTIVATE), the
 * queued movie (0x00a97a88) and the front end's package list (**0x00a97a78
 * +0xe0, next +4, name pointer +0xc). Plus a line per movie_done (00595300). */
static uint32_t g_dm_pause = 0xffffffffu, g_dm_app = 0xffffffffu, g_dm_act = 0xffffffffu;
static char g_dm_movie[64], g_dm_pkgs[256];
static unsigned g_dm_frames;

static void dm_cstr(uint32_t addr, char *out, size_t size) {
    out[0] = 0;
    void *p = NULL;
    if (addr && g_api->guest_ptr(g_api, addr, (uint32_t)size - 1, &p) == POP_OK && p) {
        memcpy(out, p, size - 1);
        out[size - 1] = 0;
    }
}

static void dm_service(const PopModApi *api, pop_cpu_v1 *cpu, PopHookInvocation *inv, void *user) {
    (void)cpu;
    (void)inv;
    (void)user;
    ++g_dm_frames;
    const uint32_t pause = get_u32(0x00a8bae4u), app = get_u32(0x00ab0ad0u);
    uint8_t act = 0;
    api->guest_read_u8(api, 0x00ab0b3cu, &act);
    char movie[64], pkgs[256] = {0};
    dm_cstr(0x00a97a88u, movie, sizeof movie);
    const uint32_t feng = get_u32(get_u32(0x00a97a78u));
    size_t n = 0;
    for (uint32_t p = feng ? get_u32(feng + 0xe0) : 0, k = 0; p && k < 12 && n + 40 < sizeof pkgs; p = get_u32(p + 4), ++k) {
        char name[40];
        dm_cstr(get_u32(p + 0xc), name, sizeof name);
        n += snprintf(pkgs + n, sizeof pkgs - n, "%s%s", n ? "," : "", name);
    }
    if (pause != g_dm_pause || app != g_dm_app || act != g_dm_act || strcmp(movie, g_dm_movie) || strcmp(pkgs, g_dm_pkgs)) {
        char line[512];
        snprintf(line, sizeof line, "core.nfsc diag: frame %u pause=%08x app_active=%u activate_state=%u movie=%s fe=[%s]",
                 g_dm_frames, pause, app, act, movie, pkgs);
        log_line(line);
        g_dm_pause = pause;
        g_dm_app = app;
        g_dm_act = act;
        memcpy(g_dm_movie, movie, sizeof movie);
        memcpy(g_dm_pkgs, pkgs, sizeof pkgs);
    }
}

static void dm_movie_done(const PopModApi *api, pop_cpu_v1 *cpu, PopHookInvocation *inv, void *user) {
    (void)inv;
    (void)user;
    const uint32_t self = cpu->ecx;
    char line[128];
    snprintf(line, sizeof line, "core.nfsc diag: movie_done check state=%u requested=%u", get_u32(self + 0x120),
             get_u32(self + 0x124));
    static uint32_t last = 0xffffffffu;
    const uint32_t now = get_u32(self + 0x120) << 8 | get_u32(self + 0x124);
    if (now != last) {
        api->log(api, line);
        last = now;
    }
}

static void diag_movie_install(void) {
    install(0x00600c90u, dm_service, POP_HOOK_BEFORE, 18);
    install(0x00595300u, dm_movie_done, POP_HOOK_BEFORE, 19);
}

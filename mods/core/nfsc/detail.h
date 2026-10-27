/* detail.h - the game's own detail settings, logged at boot and overridable (included by nfsc.c).
   The game loads them from the registry in 0x711810 into these globals (destinations read from that
   function's listing). "core.nfsc: detail loaded ..." shows what it loaded; RECOMP_SETTING_gfx="refl=0,env=1"
   then writes them, for the race bench and lighter defaults on phones
   (planning/plans/2026-10-27-lighter-frames.md). */
static const struct {
    const char *name;
    uint32_t addr;
} k_detail[] = {
    {"env", 0x00a65340u},    /* g_CarEnvironmentMapEnable */
    {"refl", 0x00a65358u},   /* g_RoadReflectionEnable */
    {"blur", 0x00a65360u},   /* g_MotionBlurEnable */
    {"part", 0x00a6536cu},   /* g_ParticleSystemEnable */
    {"world", 0x00a65370u},  /* g_WorldLodLevel */
    {"car", 0x00a65378u},    /* g_CarLodLevel */
    {"rain", 0x00a65380u},   /* g_RainEnable */
    {"fx", 0x00a65394u},     /* g_VisualTreatment */
    {"shadow", 0x00a65398u}, /* g_ShadowDetail */
    {"shader", 0x00a63e60u}, /* g_ShaderDetailLevel */
    {"perf", 0x00ab0ae8u},   /* g_PerformanceLevel */
    {"first", 0x00a6538cu},  /* FirstTime */
};
#define DETAIL_COUNT (sizeof k_detail / sizeof k_detail[0])

static void detail_log(const char *what) {
    char line[200];
    int n = snprintf(line, sizeof line, "core.nfsc: detail %s", what);
    for (size_t i = 0; i < DETAIL_COUNT && n > 0 && n < (int)sizeof line; ++i)
        n += snprintf(line + n, sizeof line - (size_t)n, " %s=%u", k_detail[i].name, get_u32(k_detail[i].addr));
    log_line(line);
}

static void detail_loaded(const PopModApi *api, pop_cpu_v1 *cpu, PopHookInvocation *inv, void *user) {
    (void)api;
    (void)cpu;
    (void)inv;
    (void)user;
    detail_log("loaded");
    const char *s = getenv("RECOMP_SETTING_gfx");
    if (!s || !*s)
        return;
    while (*s) {
        char name[16];
        unsigned v;
        int used = 0;
        if (sscanf(s, "%15[a-z]=%u%n", name, &v, &used) == 2 && used > 0) {
            for (size_t i = 0; i < DETAIL_COUNT; ++i)
                if (!strcmp(name, k_detail[i].name))
                    g_api->guest_write_u32(g_api, k_detail[i].addr, v);
            s += used;
        } else {
            ++s; /* not name=value: skip a character */
        }
        if (*s == ',')
            ++s;
    }
    detail_log("applied"); /* read back from the globals: the check that the writes landed */
}

static void detail_install(void) {
    install(0x00711810u, detail_loaded, POP_HOOK_AFTER, 29);
}

/* The sound mixer's two inner loops, run natively. EA's audio thread spends most of its time in
 * them (~50 ms/s of guest time in a race), and every guest thread shares one baton, so that time
 * came straight out of the game thread's frame.
 *   008ad305 cdecl void (float *dst, const float *src, float gain, int n): dst[i] += src[i] * gain
 *   008ad12e cdecl void (float *dst, const float *src, float gain, int n): dst[i] = src[i] * gain
 * The guest's SSE path rounds each multiply and each add to float, as this does; its x87 path
 * (unaligned buffers) is the same sum. */
#define MIX_ADD 0x008ad305u
#define MIX_SCALE 0x008ad12eu

static int mix_buffers(const PopModApi *api, pop_cpu_v1 *cpu, float **dst, const float **src,
                       float *gain, uint32_t *n) {
    const uint32_t d = get_u32(cpu->esp + 4), s = get_u32(cpu->esp + 8);
    *gain = bits_float(get_u32(cpu->esp + 12));
    *n = get_u32(cpu->esp + 16);
    void *dp = 0, *sp = 0;
    if ((int32_t)*n <= 0)
        return 1; /* nothing to mix: dst stays null and the loop is skipped */
    if (api->guest_ptr(api, d, *n * 4, &dp) != POP_OK || api->guest_ptr(api, s, *n * 4, &sp) != POP_OK)
        return 0;
    *dst = (float *)dp;
    *src = (const float *)sp;
    return 1;
}

static void mix_add(const PopModApi *api, pop_cpu_v1 *cpu, PopHookInvocation *inv, void *user) {
    (void)inv;
    (void)user;
    float *dst = 0, gain;
    const float *src = 0;
    uint32_t n;
    if (!mix_buffers(api, cpu, &dst, &src, &gain, &n)) {
        api->call_original(api, cpu->target, cpu);
        return;
    }
    for (uint32_t i = 0; dst && i < n; i++)
        dst[i] += src[i] * gain;
    api->hook_return(api, cpu, cpu->eax, 0);
}

static void mix_scale(const PopModApi *api, pop_cpu_v1 *cpu, PopHookInvocation *inv, void *user) {
    (void)inv;
    (void)user;
    float *dst = 0, gain;
    const float *src = 0;
    uint32_t n;
    if (!mix_buffers(api, cpu, &dst, &src, &gain, &n)) {
        api->call_original(api, cpu->target, cpu);
        return;
    }
    for (uint32_t i = 0; dst && i < n; i++)
        dst[i] = src[i] * gain;
    api->hook_return(api, cpu, cpu->eax, 0);
}

static void mix_install(void) {
    if (install(MIX_ADD, mix_add, POP_HOOK_REPLACE, 14) != POP_OK ||
        install(MIX_SCALE, mix_scale, POP_HOOK_REPLACE, 16) != POP_OK)
        log_line("core.nfsc: native mixer unavailable, the game's own loops run");
}

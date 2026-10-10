/* movie.h - NFSCarbon.WidescreenFix's MoviePlaybackFix (Misc.ixx), on always.
 *
 * Movie start-up (00888908) asks the audio reader once how many bytes it has
 * buffered (thiscall 008d3ca0, returning to 00888917). If the audio has not
 * arrived yet the game never connects its decoder; the unread audio then fills
 * the shared stream and starves the video, so the movie never starts (a black
 * screen with no sound). Here that one call waits, up to ~5 s, until audio is
 * buffered or the stream reaches its end state (2, thiscall 008d3cf0), sleeping
 * through the game's own SleepEx import (alertable: file reads complete through
 * completion routines) so the reading threads keep running. */
#define MOVIE_BUFFERED_BYTES 0x008d3ca0u
#define MOVIE_STREAM_STATE 0x008d3cf0u
#define MOVIE_START_RET 0x00888917u
#define IAT_SLEEPEX 0x009c1140u

static void movie_wait_for_audio(const PopModApi *api, pop_cpu_v1 *cpu, PopHookInvocation *inv, void *user) {
    (void)inv;
    (void)user;
    const uint32_t reader = cpu->ecx, sleep_ex = get_u32(IAT_SLEEPEX);
    uint32_t bytes = 0, state = 0;
    api->guest_call(api, MOVIE_BUFFERED_BYTES, reader, NULL, 0, &bytes);
    for (int i = 0; bytes == 0 && i < 5000; ++i) {
        api->guest_call(api, MOVIE_STREAM_STATE, reader, NULL, 0, &state);
        if (state == 2)
            break;
        const uint32_t args[2] = {1, 1}; /* SleepEx(1 ms, alertable) */
        if (sleep_ex)
            api->guest_call(api, sleep_ex, 0, args, 2, NULL);
        api->guest_call(api, MOVIE_BUFFERED_BYTES, reader, NULL, 0, &bytes);
    }
    api->hook_return(api, cpu, bytes, 0); /* thiscall, no arguments */
}

static void movie_install(void) {
    if (!g_api->hook_install_at_callsite ||
        g_api->hook_install_at_callsite(g_api, MOVIE_BUFFERED_BYTES, MOVIE_START_RET, movie_wait_for_audio,
                                        POP_HOOK_REPLACE, 0, &g_hooks[20]) != POP_OK)
        log_line("core.nfsc: movie playback fix unavailable");
}

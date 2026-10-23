/* movie.h - NFSCarbon.WidescreenFix's MoviePlaybackFix (Misc.ixx), on always (included by nfsc.c).
 *
 * Movie start-up (00888908) asks the audio reader ([esi+0x180]; null when the
 * movie has no audio, and then the check is skipped) once how many bytes it has
 * buffered (thiscall 008d3ca0, returning to 00888917). If the audio has not
 * arrived yet the game never connects the audio, the unread audio then fills
 * the shared stream and starves the video: the movie never starts and the game
 * waits on it forever. Video and subtitles are already waited for in a loop
 * (0088896f: SleepEx(0, alertable) until data or stream state 2). Seen on a
 * player's external drive (2026-10-15): the EA logo froze three times without
 * the video decoder threads ever starting, and a story movie hung after Kenji.
 * Here that one call waits the same way, up to 5 s (ThirteenAG's limit), until
 * audio is buffered or the stream reaches its end state (2, thiscall
 * 008d3cf0), sleeping alertably through the game's own SleepEx import (file
 * reads complete through completion routines). Every movie start is logged with
 * how long it waited, so a wait on a fast drive shows up in the log.
 */
#include <time.h>
#define MOVIE_BUFFERED_BYTES 0x008d3ca0u
#define MOVIE_STREAM_STATE 0x008d3cf0u
#define MOVIE_START_RET 0x00888917u
#define IAT_SLEEPEX 0x009c1140u
#define MOVIE_WAIT_MS 5000.0

static double movie_now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}

static void movie_wait_for_audio(const PopModApi *api, pop_cpu_v1 *cpu, PopHookInvocation *inv, void *user) {
    (void)inv;
    (void)user;
    const uint32_t reader = cpu->ecx, sleep_ex = get_u32(IAT_SLEEPEX);
    uint32_t bytes = 0, state = 0;
    api->guest_call(api, MOVIE_BUFFERED_BYTES, reader, NULL, 0, &bytes);
    const int waited = bytes == 0;
    const double start = movie_now_ms();
    const char *why = "audio arrived";
    while (bytes == 0) {
        api->guest_call(api, MOVIE_STREAM_STATE, reader, NULL, 0, &state);
        if (state == 2) {
            why = "stream ended";
            break;
        }
        if (movie_now_ms() - start >= MOVIE_WAIT_MS || !sleep_ex) {
            why = "gave up";
            break;
        }
        const uint32_t args[2] = {1, 1}; /* SleepEx(1 ms, alertable) */
        api->guest_call(api, sleep_ex, 0, args, 2, NULL);
        api->guest_call(api, MOVIE_BUFFERED_BYTES, reader, NULL, 0, &bytes);
    }
    char name[64] = {0};
    void *p = NULL;
    if (api->guest_ptr(api, 0x00a97a88u, sizeof name - 1, &p) == POP_OK && p)
        memcpy(name, p, sizeof name - 1);
    char line[160];
    if (waited)
        snprintf(line, sizeof line, "core.nfsc: movie %s: waited %.0f ms for its audio (%s, %u bytes)",
                 name[0] ? name : "?", movie_now_ms() - start, why, bytes);
    else
        snprintf(line, sizeof line, "core.nfsc: movie %s: audio already buffered (%u bytes)", name[0] ? name : "?",
                 bytes);
    log_line(line);
    api->hook_return(api, cpu, bytes, 0); /* thiscall, no arguments */
}

static void movie_install(void) {
    if (!g_api->hook_install_at_callsite ||
        g_api->hook_install_at_callsite(g_api, MOVIE_BUFFERED_BYTES, MOVIE_START_RET, movie_wait_for_audio,
                                        POP_HOOK_REPLACE, 0, &g_hooks[20]) != POP_OK)
        log_line("core.nfsc: movie playback fix unavailable");
}

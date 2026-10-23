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
 * reads complete through completion routines).
 * Some movies have an audio reader but no audio at all: Attract (the title
 * screen's background) is only MVhd + MV0F chunks, so ThirteenAG's wait always
 * ran its full 5 s there (the pause after the intro, measured 2026-10-23). The
 * stream ([reader+4]) keeps every chunk read so far in one list at +0x50 in
 * file order (node: +0 next, +8 size, +0xc chunk bytes, +0x14 reader index;
 * 008d4740, 008d3bf0, and 00888884 reads the MVhd tag through +0xc). Movies with
 * audio put SCHl before their first video frame, so a frame (MV0K/MV0F) read
 * with no SC* chunk before it means the movie has no audio: no wait.
 * Every movie start is logged with how long it waited and why it stopped.
 */
#include <time.h>
#define MOVIE_BUFFERED_BYTES 0x008d3ca0u
#define MOVIE_STREAM_STATE 0x008d3cf0u
#define MOVIE_START_RET 0x00888917u
#define IAT_SLEEPEX 0x009c1140u
#define MOVIE_WAIT_MS 5000.0
#define MOVIE_STREAM_CHUNKS 0x50u
#define TAG_MV0K 0x4b30564du /* "MV0K" */
#define TAG_MV0F 0x4630564du /* "MV0F" */
#define TAG_SC 0x4353u        /* "SC" (SCHl, SCCl, SCDl: audio) */

static double movie_now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}

/* What the chunks read so far say about the movie's audio: 1 an audio chunk was
 * read, 0 a video frame came first (no audio track), -1 not known yet. */
static int movie_has_audio(uint32_t reader) {
    uint32_t node = get_u32(get_u32(reader + 4) + MOVIE_STREAM_CHUNKS);
    int frame = 0;
    for (int i = 0; node && i < 256; ++i, node = get_u32(node)) {
        const uint32_t tag = get_u32(get_u32(node + 0xc));
        if ((tag & 0xffffu) == TAG_SC)
            return 1;
        if (tag == TAG_MV0K || tag == TAG_MV0F)
            frame = 1;
    }
    return frame ? 0 : -1;
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
        if (movie_has_audio(reader) == 0) {
            why = "no audio in this movie";
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
    static const char *const track[] = {"not known yet", "none", "yes"};
    const char *audio_track = track[movie_has_audio(reader) + 1];
    char line[192];
    if (waited)
        snprintf(line, sizeof line, "core.nfsc: movie %s: waited %.0f ms for its audio (%s, %u bytes; audio track: %s)",
                 name[0] ? name : "?", movie_now_ms() - start, why, bytes, audio_track);
    else
        snprintf(line, sizeof line, "core.nfsc: movie %s: audio already buffered (%u bytes; audio track: %s)",
                 name[0] ? name : "?", bytes, audio_track);
    log_line(line);
    api->hook_return(api, cpu, bytes, 0); /* thiscall, no arguments */
}

static void movie_install(void) {
    if (!g_api->hook_install_at_callsite ||
        g_api->hook_install_at_callsite(g_api, MOVIE_BUFFERED_BYTES, MOVIE_START_RET, movie_wait_for_audio,
                                        POP_HOOK_REPLACE, 0, &g_hooks[20]) != POP_OK)
        log_line("core.nfsc: movie playback fix unavailable");
}

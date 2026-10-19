/* cheats.h - infinite nitrous and speedbreaker, engine power (included by nfsc.c).
 * Settings infinite_nitrous, infinite_speedbreaker (off), engine_power and
 * opponent_power (Normal); F10 Mods tab, live. Canyon duels switch nitrous and
 * speedbreaker off; the power settings still work there.
 *
 * NITROUS. The engine's per-step update 006e3df0 (thiscall engine, 3 args, RET 0xc)
 * keeps the tank at [engine + 0xf0] (0..1). Its own "infinite nitrous" reward branch
 * (card 10) refills the tank only when [[engine + 0x2c]]->vfunc 0x20 returns a
 * player, so that call is the test used here: after the original runs, a player's
 * tank is filled; AI cars are left alone.
 *
 * SPEEDBREAKER. The game's Tweak_InfiniteRaceBreaker byte 0x00b4d86c (address from
 * berkayylmao's NFSPluginSDK, checked at 00761dcd) stops the gauge draining.
 */
#define ENGINE_UPDATE 0x006e3df0u
#define INFINITE_RACEBREAKER 0x00b4d86cu

static void cheats_speedbreaker(void) {
    g_api->guest_write_u8(g_api, INFINITE_RACEBREAKER, setting("infinite_speedbreaker", 0) ? 1 : 0);
}

/* The same test the game's reward branch uses: does this engine belong to a player? */
static uint32_t engine_is_player(const PopModApi *api, uint32_t engine) {
    const uint32_t owner = get_u32(engine + 0x2c);
    const uint32_t fn = owner ? get_u32(get_u32(owner) + 0x20) : 0;
    uint32_t player = 0;
    static int told;
    const PopModStatus st = fn ? api->guest_call(api, fn, owner, NULL, 0, &player) : POP_E_NOTFOUND;
    if (!told && (st != POP_OK || player)) {
        told = 1;
        log_line(st == POP_OK ? "core.nfsc: cheats found the player's car"
                              : "core.nfsc: cheats could not ask whose car it is");
    }
    return player;
}

/* POWER. 006e3df0 returns the engine's power multiplier (1.0, more while nitrous
 * fires); the drivetrain 006ffe60 stores it at [engine + 0xf4] and the torque
 * curve 006e31e0 multiplies by it. So the returned ST0 is scaled: the player's by
 * engine_power, every other car's (opponents, traffic, cops) by opponent_power.
 * Canyon duels switch nitrous off but not this. */
static double power_choice(const char *key, const double *scale, int n) {
    const int64_t c = setting(key, 0);
    return c > 0 && c < n ? scale[c] : 1.0;
}

static void engine_update(const PopModApi *api, pop_cpu_v1 *cpu, PopHookInvocation *inv, void *user) {
    (void)inv;
    (void)user;
    const uint32_t engine = cpu->ecx;
    static const double mine[] = {1.0, 1.5, 2.0, 3.0}, theirs[] = {1.0, 0.5, 0.25};
    const double up = power_choice("engine_power", mine, 4);
    const double down = power_choice("opponent_power", theirs, 3);
    const int nitrous = setting("infinite_nitrous", 0) != 0;
    const uint32_t player = (nitrous || up != 1.0 || down != 1.0) ? engine_is_player(api, engine) : 0;
    api->call_original(api, cpu->target, cpu);
    if (player && nitrous)
        put_float(engine + 0xf0, 1.0f);
    const double k = player ? up : down;
    if (k != 1.0)
        cpu->st[cpu->fpu_top & 7u] *= k; /* ST0 */
    cheats_speedbreaker();
}

/* CATCH-UP (rubberbanding). GRaceParameters' catch-up getter 0063ead0 (thiscall,
 * no arguments, plain RET, flag in AL) reads race attribute 0x10db04e6 and treats a
 * race that does not set it as ON; Quick Race's "Catch Up" option writes it
 * (00626440), career races keep their own. With no_catchup the getter answers off
 * for every race, so opponents no longer slow down or speed up to stay near you. */
#define RACE_CATCH_UP 0x0063ead0u
static void race_catch_up(const PopModApi *api, pop_cpu_v1 *cpu, PopHookInvocation *inv, void *user) {
    (void)inv;
    (void)user;
    if (setting("no_catchup", 0)) {
        api->hook_return(api, cpu, cpu->eax & 0xffffff00u, 0);
        return;
    }
    api->call_original(api, cpu->target, cpu);
}

static void cheats_install(void) {
    cheats_speedbreaker();
    if (install(ENGINE_UPDATE, engine_update, POP_HOOK_REPLACE, 19) != POP_OK)
        log_line("core.nfsc: cheats unavailable (hook 006e3df0)");
    if (install(RACE_CATCH_UP, race_catch_up, POP_HOOK_REPLACE, 23) != POP_OK)
        log_line("core.nfsc: catch-up switch unavailable (hook 0063ead0)");
}

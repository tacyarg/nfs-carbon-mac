# NFS Carbon → native Apple Silicon (static recompilation)

Port of Need for Speed: Carbon (PC v1.4, unwrapped NFSC.exe) to macOS using
veritr1x's recomp-kit, taken from the NFS Most Wanted Mac port
(github.com/elforeign/nfs-most-wanted-mac, local clone ~/nfs-most-wanted-mac).
The kit's copy lives in `kit/` with our changes on top. Developed on macOS 27, Apple Silicon.
Explain findings plainly and keep commands copy-pasteable.

## Working rules
- Get evidence before patching: read the listing (`analysis/decompiled/<image>/functions/<addr>.asm`),
  the bytes (`xxd`), or the log line first. Guessing cost several rounds on `_initterm`.
- Kit changes go in `kit/` directly; commit them in git with a message saying why.
- Never commit game files or anything derived from them (original/, rebased/, analysis/,
  build/, dist/ are ignored).
- Rebuild: `source .venv/bin/activate && python tools/build.py` (add `--regenerate`
  only when `kit/tools/recomp/translate.py` or `game.toml` changed).
- Never run the build or game from inside `original/`'s parent via `..`:
  `original/` is a symlink to your game folder.

## Layout
- `game.toml`: exe identity (sha256 baaaaec3…fd3, base 0x400000, entry 0x87e926),
  1 GB arena, `[modules.aux.a_msvcr71]` / `[modules.aux.b_msvcp71]`, entry points,
  data_ranges. Sentinel hooks point into padding 0x00c2b400-0x00c2bfff.
- `rebased/`: msvcr71.dll → 0x10000000, msvcp71.dll → 0x10060000 (tools/rebase_pe.py;
  the kit has no relocation support and the originals want 0x7c34/0x7c3a0000).
- `tools/`: analyze.py (Ghidra over exe + both DLLs), fetch_tools.py (Ghidra 12.1.3 +
  JDK 26 into ~/Library/NFSC-Native-Setup), rebase_pe.py, who_names.py (who names an
  address), inspect_exe.py, build.py/setup.py wrappers.
- `docs/kit-patches/carbon-kit*.patch`: history of kit changes 1-15, all already applied to
  `kit/`. Since 2026-10-10 the project is a git repo: kit changes are commits, no new patch files.

## Packaging (tools/setup_kit/)
- `package.py --game DIR [--install]`: build/CarbonRecomp.app + your game folder ->
  `dist/Need for Speed Carbon.app` (and /Applications with --install). Checks NFSC.exe's
  SHA-256, copies the game with APFS clones minus game.toml [bundle] exclude + setup.exe
  etc., puts the rebased msvcr71/msvcp71 beside NFSC.exe, Info.plist: name, icon,
  public.app-category.racing-games + LSSupportsGameMode (Game Mode), ad-hoc signs.
- `launch.c` -> Contents/MacOS/Launch (CFBundleExecutable): profile
  ~/Library/Application Support/NFS Carbon (saves in Documents/NFS Carbon/<profile>/,
  registry.json, mod-settings.json seeded from Resources/Defaults on first run),
  Logs/<date>.log + Logs/latest.log (10 kept), RECOMP_EXE=Resources/Game/NFSC.exe,
  RECOMP_LOG=1; any RECOMP_* already set wins. Then execs CarbonRecomp.
- `defaults/`: registry.json (working video settings), mod-settings.json (perf overlay
  off, host.display/window 2 = full screen; 0 windowed, 1 borderless), fallback
  AppIcon.icns from `make_icon.py` (original art, no EA logos).
- Icon: package.py uses `--icon PICTURE` if given, else the game's own icon read from the
  player's NFSC.exe at build time (only a 32x32 8-bit one exists; bicubic+sharpen up to the
  macOS tile), else the fallback. No EA art is stored in the repo.
- Launcher sets RECOMP_D3D9_SHADER_WARM_DIR=<profile>/ShaderWarm. First launch of the
  installed app (cold Metal cache, 0.66 s of shader compiles at the movies) went black
  after the EA splash; later launches fine. Warm-up helps every launch after the first;
  the root cause of the black (why a slow first frame stalls the game) is NOT found.
- Run from /Applications, not dist/: inside the checkout the runtime sees game.toml
  above it and behaves as a developer build.

## Mods (mods/core/nfsc, game.toml)
- `mods/CMakeLists.txt` builds every mods/core/<id>/ plugin (kit build_core.py) into the
  app's Resources/mods/core. `NOTICE` credits ThirteenAG (MIT) for the widescreen port.
- core.nfsc widescreen stage 1 (2026-10-10, built + packaged, not yet run): replaces
  00712ac0 (racing resolution -> screen shape, <=1080 rows); BEFORE hook on 0071b780
  (projection; view id at [[esp+4]+8]) fills slots 0x00c2b400..0x00c2b410 that 5
  operand_redirects in game.toml point at (aspect int num/den at 0071b858/0071b864,
  1.0 at 0071b8da, 0.5 at 0071b8ec, 1.0 at 0071b923), values from ThirteenAG's
  Rendering.ixx FOVHook (views 1/4 Hor+, mirror 3 = 0.925). 00712ba0 is dead code.
  Settings: widescreen (bool), fov_scaling 0/1/2.
  Fixed since: mods need POP_MOD_DECLARE_ABI(); screen size is read at device creation
  (kit change 13, desktop host never published it to mods); game size = screen / k
  (k = round(rows/1080)) because host_present_fit scales by whole numbers only:
  1668x1080 left a border, 1710x1107 fills 3420x2214. Bars confirmed gone 06:2x.
- Widescreen stage 2, HUD (confirmed: fills the screen, HUD scaled; 2026-10-10): mod writes HUD_SCALE_X
  009e8f8c, world-map X 00a604ac, FE X 009c778c, race-route X 009d5f3c (runtime reads,
  checked in gen), slot 0xc2b418 = HUD centre for 6 MOV 320.0 instruction_patches,
  slot 0xc2b414 = 240*aspect-320 for SetWideScreenMode's 120.0 (2 redirects); widescreen
  HUD forced on (SETZ 005dc507/005d52b2, JNZ 005b6bae/005b6b5b); BEFORE/AFTER hooks on
  SetWideScreenMode 005d5280 move groups 0x1603009E/0x5D0101F1 via FindObject 005a0250 /
  GetCenter 00597900 / SetCenter 00597a70. NOT done: mirror/PiP (PUSH 00750de6/00750df4 +
  MirrorFix at 00750c23 needs a mid-function hook), FE scaling (FEScaleHook1/2 at
  00730e4d/00730ece), loading-screen pillarboxes, rain droplets, autosculpt/arrest blur,
  options resolution list. `tools/find_pattern.py` finds the fix's byte patterns.
- Background pause/mute: the game already does it (user-confirmed 2026-10-10: music
  pauses on Cmd-Tab) via WM_ACTIVATE/WM_ACTIVATEAPP the host posts (WndProc 00712060:
  007120ab -> 0050c370; 007121d5 -> 0081e6a0). A host-side pause was tried and reverted.
- Controller / XtendedInput (STARTED 2026-10-10). User has a Switch Pro pad; it already
  works through the kit's DInput joystick, but with Carbon's generic PC layout (Y gas,
  A reverse, B brake, X speedbreaker, R2 pause, L2 nitro, L1 look back, right stick
  nothing). Goal: the Xbox 360 layout XtendedInput (xan1242/Berkay Yigit, MIT;
  github.com/xan1242/NFS-XtendedInput) defaults to: RT gas, LT brake, LS steer, A e-brake,
  B nitro, X speedbreaker, Y crew, LB look back, RB camera, RS Y shift, Start pause, Back
  reset; plus console-style FE prompts. Plan (MW port's way, mods/core/nfsmw/pad_mapping.h
  + frontend_pad.h): write the console layout as the binding DEFAULTS in the game's own
  table (keep Controls-screen remaps), triggers as analog pedals, console FE objects shown
  while the pad is in use. Button icons: XtendedInput's XtendedInputButtons.tpk is Aero_'s
  art with no stated licence -> do NOT ship; Setup app downloads Release-Carbon-Pack.zip
  from xan1242's GitHub release (opt-in checkbox) and copies the TPK into the user's app;
  fallback: our own drawn glyphs. XtendedInput's Carbon addresses (src/Main.hpp +
  Definitions.h, #ifdef GAME_CARBON, match v1.4): INPUTMAPPING_CONSTRUCTOR_CALL 0x00696A74
  (in 006969a0: per-port mapping object, data from 006964a0), INPUTDEVICE_FACTORY_INIT
  0x9B629D, DEADZONE ptr 0x696071, FE deadzone ptr 0x59FF6E, FENG_HIDEPCOBJ 0x583D70->
  0x583D99 (vt 0x9D0680), CFENG_RENDEROBJ_NOP 0x5B092D/flag 0x5B0929, FENG_FINDOBJECT
  0x5A0250, FENG_SETOBJECTVISIBILITY 0x570460, CFENG_INSTANCE 0xA97A78, PRESS_START hook
  0x8577B6, DINPUT_KILL 0x6892EB, OnLoadDone 0x58E250 (profile loaded).
  STAGE 1 CONFIRMED by the user 2026-10-10 (Switch Pro: layout, analog triggers, names, remaps) (mods/core/nfsc/pad.h): bindings = 72 rows
  at 0xa98700 {name, triples*} (template 0xa61c88, 52-byte rows; names GAME_ACTION_GAS...);
  triples 0/1 keyboard, 2p+2/2p+3 port p {kind, port, control}. 00689550 (thiscall port, RET 4)
  = per-device defaults for rows 0..46 (callers: 00694c50 device create, 005c8960 Controls
  Restore Defaults, 004b01f0 reset). REPLACE hook remembers the game's generic rows then
  writes the console layout; per frame (REPLACE 00692240, values float[72] at [this+0x20])
  a port whose rows == generic gets the layout (old saves, Restore Defaults); remaps kept.
  Triggers (kind 7 control 0/1 = Rx/Ry) get full travel after the update (state +0x20c/+0x210,
  range 0..65535). Names: REPLACE 00679ad0 (cdecl out, device, row, primary, port), family
  from setting controller_buttons or SDL_GetGamepadTypeForID via dlsym (SDL exported by host).
  Settings: console_layout, controller_buttons.
  STAGE 2 console menu prompts (fe_pad.h) CONFIRMED working by the user 2026-10-10 (needed
  00583d70 in entry_points: hooks need a listed function). Career map overlap fix (groups
  0x17b1f254 console / 0xa2601d59 PC via FindObject on [0xa977f0]+0xc, only while the map or
  FeWorldMapQuickList.fng is in control (00572350); quick list focus hooks 005cdbf0/005c3880;
  "Free Roam" -> Back in the FE) CONFIRMED 2026-10-10. Details:
  REPLACE 00583d70 (load-time hide of console objects, arg obj, RET 4), REPLACE 005b0920
  (render object; its TEST 0x40/JNZ at 005b092d NOP'd in game.toml), BEFORE 00600c90
  cFEng::Service walks menus ([this+0xe0] next +4; objects +0x4c next +4; group children +0x64)
  hiding PC (0x1008) or console (0x40) set, mark in UserParam +0x28 bit 0x01000000. Console
  set after pad input, PC after key/click. Icons: console images use PS2 names (CROSS...) not
  in the PC data -> retargeted to XBONE_/PS4_/SWITCH_ names in GLOBAL/XtendedInputButtons.tpk,
  loaded via 006b32c0/006b5910/006b68d0. No pack -> keyboard prompts stay. package.py
  --button-icons downloads XtendedInput 1.22 Release-Carbon-Pack.zip (sha256 pinned, cached in
  ~/Library/Caches/NFS Carbon Setup). Setting console_prompts. Offline fallback (our own TPK) TODO.
  F10 page: game.toml [settings] page = "tabs" (choice names; Controls tab), touch rows removed.
- Remove online options: NOT done. Main menu state 00843690 (FeMainMenu.fng), carousel
  FeMainMenu_Sub.fng actions in 005a5560 (switch on choice 1..9; 2/3 -> 00845640 arg 1/2,
  vtable 009f82d0, maybe Online/LAN). Item labels are not bStringHash'd names found in the
  exe (tried MM_*/MAINMENU_*); likely defined in the FE package data. Next: find the
  carousel item list (who fills FeMainMenu_Sub's options) or hook the choice handler.
- Full resolution list + Display mode (git 579ed7d, CONFIRMED by the user 2026-10-10: 1920x1080
  letterboxed correctly, no stretch; F10 (fn+F10) Window row switches modes): list =
  screen/1..4 ("(native)" on k=1) + common 4:3/16:10/16:9/21:9 sizes <= screen, sorted by
  width; g_aspect follows the chosen size (FOV/HUD); needs kit change 16. Window mode =
  the kit's F10 settings page (game.toml [settings] rows = MW's list). Unknown: whether
  gameplay frames keep aspect for non-screen shapes (compositor's non-legacy gameplay
  path draws the world to the whole drawable -> could stretch).
- Resolution menu (CONFIRMED working 2026-10-10 by the user): list = full-screen size / 1..4
  (>=480 rows), default nearest 1080 rows; option index 00a65388; tables at 0xc2b500
  (supported) / 0xc2b5a0 (refresh) via 8 redirects (indexed operands need kit change 15);
  NOP 007115f0/007116df; save PUSH at 00711d3f -> slot 0xc2b640 (packed 0x80000000|w<<16|h);
  REPLACE hooks IncScreenRes 004a51f0, GetLocg_RacingResolution 004a3830,
  GetLocalizedWideString 00588f20 (labels, keys NFSC_NATIVE_RES_WxH). Unlisted readers
  00712b85/00712c65/0072bee0 keep the original tables. HUD re-applied per size change.
- Source of the fix: github.com/ThirteenAG/WidescreenFixesPack source/NFSCarbon.WidescreenFix
  (Resolution.ixx, Rendering.ixx, Frontend.ixx...). Its addresses match our exe.

## Kit changes made so far (all in kit/)
1. Loader: aux DLLs map before the exe's IAT is bound; exe imports bind to their
   exports; statically imported DLLs get DllMain(PROCESS_ATTACH) in key order before
   the entry; a stock DLL beside the exe with the wrong hash no longer shadows the
   rebased one.
2. Shims: kernel32 (InterlockedCompareExchange/ExchangeAdd, DisableThreadLibraryCalls,
   GetComputerNameA, console/pipe stubs…), advapi32 (not admin; CryptGenRandom),
   ole32 CoCreateGuid, rpcrt4 UUID strings, ws2_32 byte-order helpers.
   D3DX shims also registered as d3dx9_30.dll; 6 texture/surface functions fail cleanly.
   `CreateVolumeTexture` in d3d9 works (A8R8G8B8/X8R8G8B8/A8/L8); see kit change 7.
3. Translator: JCXZ; CRT exits through the IAT never return (not RaiseException);
   `data_ranges` config; masked jump-table slots that overlap an instruction are holes
   (msvcr71 memmove); `_initterm` found when called via IAT, a register, or an
   `FF 25` import thunk (NFSC.exe uses thunk 0087f1a0, table 0xa4a000-0xa4bf68).
4. Runtime: RaiseException(0x406D1388) = SetThreadName, returns immediately.
5. Translator (`carbon-kit-xmmword.patch`): 16-byte memory operands spelt `xmmword`, so
   unlisted functions with an SSE path recover (SSE ops become recomp_unmodelled traps;
   CPUID advertises no SSE so the game takes its x87 path). Fixed 008ab884 + 4 others;
   recovery errors 19 -> 8 (remaining 8 are width-6 decodes of data).
6. d3d9 shadergen (`carbon-kit-psfog.patch`): ps_3_0 inputs declared with usage 11
   (FOG) read the fog varying instead of failing the whole pixel shader.
7. d3dx9 (`carbon-kit-fillvolume.patch`): D3DXFillVolumeTexture implemented (helper
   d3d9_fill_volume in d3d9.cpp calls the guest LPD3DXFILL3D per texel). Carbon's
   colour-grading lookup (0073c1a0 creates an N^3 A8R8G8B8 volume, callback 0073c100)
   was all zeros, so IDI_VISUALTREATMENT_FX turned every 3D pixel black.
8. d3dx9 (`carbon-kit-fxshared-refs.patch`): shared effect-pool parameters
   (share_parameter / join_pool) now take their own texture reference
   (set_param_texture); fx_destroy released one per effect, so destroying effects at
   race load freed the colour-grading LUT the game still used. Fx_GetTexture returns
   a volume texture as IDirect3DVolumeTexture9.
9. d3dx9 (`carbon-kit-fxsampler-refs.patch`): effect passes binding device sampler
   slots (bind_constants sampler, pass SAMPLER_TEXTURE state) go through
   bind_sampler_slot, which takes/releases the volume-texture reference the way
   Dev_SetTexture does. Backtraces proved Dev_SetTexture released the LUT twice
   (refs 2->1->0) for slots the effect had filled without a reference.
10. d3d9/d3dx9 (`carbon-kit-dds-cube-volume.patch`): D3DXCreateCubeTextureFromFileInMemory
   and D3DXCreateVolumeTextureFromFileInMemory load DDS (DXT1-5, A8R8G8B8, X8R8G8B8,
   A8, L8, mips). Called by GLOBALB.BUN chunk handlers 0070f6c0 (0x30300201: five
   256^2 DXT1 environment cubes into 0xab09d0[]) and 0070f670 (0x30300200: 32^3
   A8R8G8B8 volume, 6 mips, into 0xab09e8). volume_create() split out of
   Dev_CreateVolumeTexture.
11. d3d9/d3dx9 (`carbon-kit-loadsurface.patch`): D3DXLoadSurfaceFromSurface for whole
   surfaces (no rects): same format+size copies rows, otherwise decode (DXT1/3/5,
   A8R8G8B8, X8R8G8B8, R5G6B5, X1/A1R5G5B5, A4R4G4B4, A8, L8) -> nearest-neighbour
   scale -> colour key -> encode (simple bounding-box DXT encoder). Source is the
   surface's CPU copy. Callers: 007d7580 (texture copy when formats differ),
   00759040 (per mip), 00732ca0 (boot).
15. Translator (`carbon-kit-indexed-redirects.patch`): operand_redirects also match an
   address that ends an indexed operand ([EAX*0x4 + 0xADDR]).
14. Host (`carbon-kit-notch-fullscreen.patch`): note_screen_size also records the
   full-screen size = display minus NSScreen.safeAreaInsets.top (helper
   host_mac_safe_top_points in metal_surface.mm); mods' screen_size prefers it. On the
   13" notched MacBook Air: display 3420x2214, full screen 3420x2146 (notch strip 68 px), so the game is
   1710x1073 and fills the window; 1710x1107 was shrunk to fit -> side borders.
13. Runtime (`carbon-kit-screen-size.patch`): host_display_screen falls back to
   host_display_screen_size, so mods' screen_size() works on the desktop host.
12. Translator (`carbon-kit-module-redirects.patch`): configure_module() clears the exe's
   operand_redirects / instruction_patches / data_seeds, so an aux DLL translation no
   longer fails with "operand redirects matched nothing".
16. Presenter (git 0b704cd): game.toml `[settings] scaling = "smooth"` (kit default
   "integer") makes the compositor's whole-frame rect and host_present_fit take the
   fractional aspect-preserving scale, so any game resolution fills the screen. Carbon
   sets it. Built 2026-10-10, no visible change yet (the menu list is still screen/k);
   groundwork for a full resolution list + Display Mode (windowed/borderless/full).

## Where we are
- All three images translate (exe 49947/49970 functions) and the app builds:
  `build/CarbonRecomp.app`.
- 008dacdd is fixed (`carbon-kit-raisereturns.patch`, applied and built 2026-10-10 00:52).
- 2026-10-10 ~05:10: RACE RENDERS. 3D world in races and the menu garage/car both
  render, including after returning from a race (kit changes 7-9). Game is playable:
  profile, career, races, sound, HUD, 60 fps.
  Cube maps + file volume: kit change 10 — CONFIRMED working 05:19 (car reflections
  visible, all 6 DDS loaded). D3DXLoadSurfaceFromSurface: kit change 11, built, not
  yet run. SetGammaRamp deferred: the presenter has no gamma stage (needs a Metal
  present-shader change). Then: SetGammaRamp
  (brightness), LightEnable; thread calling address 0 at boot; registry.json install
  keys; then the user's mods (XtendedInput, widescreen, Extra Options, HD reflections).
- Run 2026-10-10 03:10-04:54: menu 3D renders; race black and menu black after a race.
  Refcount diagnostics showed the first LUT (volume 1292) freed at race load by kit
  internal releases only (no guest Release): over-release of shared effect params
  -> kit change 8. Not enough (05:01 run still black): backtrace diag showed the real
  over-release in Dev_SetTexture -> kit change 9. All temporary diag removed. Built,
  not yet run.
- Run 2026-10-10 03:00: Options > Audio crash fixed. Still no 3D anywhere (menu
  background car/scene and race world black; HUD, minimap, FE 2D fine, 60 fps).
  Cause found: D3DXFillVolumeTexture unimplemented -> black colour-grading LUT
  (kit change 7). Built, not yet run. Next if still black: cube maps
  (D3DXCreateCubeTextureFromFileInMemory, reflections/sky), D3DXLoadSurfaceFromSurface.
- Run 2026-10-10 02:41: BIG progress. Profile/alias created, main menu works, career
  boots and the race progresses with sound and HUD, but no 3D world renders (also some
  menu elements missing). Options > Audio crashed/hung. Said No to EA servers.
  Log: unknown targets 00555450, 005507a0 (translator recovered then withdrew both
  as "data") and 005a69b0 -> all added to entry_points; one pixel shader failed
  ("pixel shader input usage 11" = FOG) -> kit change 6. Rebuilt, not yet run.
  The "guest thunk ... fast-path miss" lines are NOT a problem: heap code is audio-bank
  routines linked by loader 00898a13 (helper table 0xa7d6c0, 40 fns from 00897ba8);
  after the thunk miss, interp_call runs them (no unknown-target follows).
  If 3D is still missing, next suspects: D3DXCreateCubeTextureFromFileInMemory /
  D3DXCreateVolumeTextureFromFileInMemory / D3DXFillVolumeTexture /
  D3DXLoadSurfaceFromSurface (all "not implemented") and CreateVolumeTexture stub.
- Run 2026-10-10 02:14 (with 0089777e): pressing a key at press-start now opens a
  yes/no prompt, which then hangs. Log: unknown target 008ab884 on the audio thread
  (return 008b65fe) — an audio decoder the translator refused ("unknown operand width
  16"); fixed by kit change 5, added to entry_points, rebuilt, not yet run. Still open:
  `guest thunk 016b0b64 fast-path miss` — the game runs code from HEAP memory (bytes
  `56 8b 74 24 08 56 e8 ..`, call resolving to exe 00897ba8; no copy of it in the exe).
  The runtime only runs tiny jump stubs from the heap and returns 0 otherwise. If the
  prompt still hangs, find who writes/calls 016b0b64 next.
- Bulk static scan FAILED (2026-10-10 01:51): `tools/find_missed_functions.py` found
  42k "function start after RET/INT3/JMP" candidates (analysis/scanned-entries.UNUSED-broke-boot.txt);
  building with them (`--discovered`) crashed on boot (the scanner, tools/find_missed_functions.py, was deleted in the cleanup): real functions such as the
  WndProc 00712060 became unknown targets, then SIGBUS at EIP 0. Don't feed the
  translator a static guess list. Reverted after the 02:06 crash: build is back to the good one
  (58164 functions, 18 recovery errors) plus 0x0089777e in game.toml (unknown target
  right after the press-start key press, run 01:32 at LOG=1: 60 fps, key registers,
  nothing happens). Not yet run. Faster option to try: `kit/tools/discover.py`
  (records only addresses a run really reaches).
- Run 2026-10-10 01:24: EA splash, street-racing warning video and sound all play
  correctly; reaches "press any key", but a key press does nothing and the menu runs
  ~3 fps. Keys do reach the game (TranslateMessage/MapVirtualKeyExA/GetKeyState
  after the press). One new unknown target before the press: 0059d6d0 (vtable +0x1c,
  called from 00589093 on a freshly allocated object) — added to entry_points,
  rebuilt, not yet run. That run was with RECOMP_LOG=2: 1.1 GB / 16.8M log lines,
  every import call and d3d9 draw traced — likely a big part of the 3 fps; measure
  speed with RECOMP_LOG=1 before suspecting the renderer.
- Run 2026-10-10 01:12 (after adding the 4 entry points below): ~2000 frames, loads
  GLOBAL/FRONTEND/LANGUAGES/CARS/TRACKS/SOUND, plays MOVIES\EA_Bumper_ENGLISH_ntsc.vp6:
  user saw a glitchy EA splash, then an all-green screen. VideoDecodeThread called
  unknown targets 0088510e and 00884a1d (MMX code Ghidra folded into 008849c8), and
  the main thread called 0054bac0. All three were added to entry_points and the app
  rebuilt; not yet run. Also new in that log: D3DXLoadSurfaceFromSurface,
  D3DXCreateCubeTextureFromFileInMemory, D3DXCreateVolumeTextureFromFileInMemory,
  D3DXFillVolumeTexture not implemented; SetGammaRamp not implemented; SendInput
  unimplemented; dinput force feedback unsupported (harmless).
- Earlier run (2026-10-10 01:01, window closed by hand): gets much further.
  The window opens, D3D9 device is created on Metal (640x480), all ~30 shader effects
  (IDI_*_FX) compile, cube maps/render targets are created, DirectSound starts and
  plays (silent) audio, and threads 2-7 start (incl. "RWAudioCore Dac"). Then the main
  thread stalls: only 2 Present calls, 0 ReadFile calls, no game data is ever loaded.
- Next problem (evidence): 4 "call to unknown target ... returning 0":
  0089f604, 0089f07c, 0089f494 (called via `CALL EBX` in the vector-constructor
  helper 00403a60, i.e. object constructors) and 008999ee (audio thread). Raw bytes
  show each starts right after a RET/JMP of the previous function with a normal
  prologue (`mov eax,ecx` / `push ebp; mov ebp,esp`): real functions Ghidra missed
  because they are only reached through pointers. Fix = add them to
  `[translate] entry_points` in game.toml and rebuild with `--regenerate`
  (or let `kit/tools/discover.py` collect more like them). Constructors "returning 0"
  leave objects unbuilt, the likely cause of the stall.
- Also seen, lower priority: one thread calls address 00000000 (return 0fdfff00,
  all registers 0) → access violation, unhandled; registry install key missing, so
  `Install Dir` (read in 007346b0, along with CD Drive, FirstTime, CacheSize,
  SwapSize, Language, StreamingInstall) falls back; it then looks for
  `C:\Program Files\EA GAMES\Need for Speed Carbon/NFS\ZDIR.BIN` (guest_root maps
  to original/, which has no NFS folder); WSAStartup/WSACleanup unimplemented;
  d3d9 LightEnable unimplemented; DirectSound rejects a 22050 Hz 6-ch buffer.
- Known next items after that: registry.json with Carbon's install keys
  (HKLM\Software\Electronic Arts\Need for Speed Carbon, values above); real hooks
  instead of sentinels; volume textures; then the user's mods (XtendedInput,
  widescreen, Extra Options, HD reflections; MW's mods/core/nfsmw is the template).

## Run the game (from Terminal, logs to a file)
```
cd ~/nfsc-mac/original
RECOMP_EXE="$PWD/NFSC.exe" \
RECOMP_PROFILE_DIR="$HOME/Library/Application Support/NFS Carbon Dev" \
RECOMP_LOG=2 RECOMP_IMPORT_STATS=1 RECOMP_MAX_SECONDS=90 \
~/nfsc-mac/build/CarbonRecomp.app/Contents/MacOS/CarbonRecomp > ~/nfsc-mac/run-log.txt 2>&1
grep -v '^\[recomp\] [-<]> ' ~/nfsc-mac/run-log.txt | awk '!seen[$0]++' > ~/nfsc-mac/run-summary.txt
```
LOG=2 logs every import call (100k+ lines); read run-summary.txt first.
`kit/tools/discover.py --game-dir ~/nfsc-mac` batches "unknown target" addresses.

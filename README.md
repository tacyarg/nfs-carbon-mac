# nfsc-mac — Need for Speed: Carbon on Apple Silicon (recomp-kit bring-up)

Built on the MW port's copy of veritr1x's recomp-kit. Bring your own PC game
(v1.4 NFSC.exe, sha256 baaaaec3...fd3, with msvcr71.dll and msvcp71.dll).

## First run

    GAME="/Users/vasisht11/Downloads/Need-for-Speed-Carbon/Need for Speed - Carbon"
    cd ~/nfsc-mac
    cp -R ~/nfs-most-wanted-mac/kit kit
    (cd kit && patch -p1 < ../carbon-kit.patch)
    python3 -m venv .venv && .venv/bin/pip install -r kit/requirements-dev.txt
    .venv/bin/python tools/setup.py --install "$GAME" --link-only
    .venv/bin/python tools/rebase_pe.py "$GAME"
    .venv/bin/python tools/analyze.py --ghidra-home <Ghidra 12.1.3 folder> --java-home <JDK folder>
    .venv/bin/python tools/build.py --regenerate

Ghidra and the JDK are already downloaded by the MW setup, under
~/Library/NFSMW-Native-Setup/.

## What carbon-kit.patch changes

- runtime/loader.*: aux DLLs load before the exe's imports are bound, the
  exe's imports bind to them, their DllMains run before the game's entry, and
  a stock DLL beside the exe no longer shadows the rebased copy.
- runtime/kernel32_wide.cpp, runtime/misc.cpp: kernel32, advapi32, ole32,
  rpcrt4 and ws2_32 functions Carbon and msvcr71/msvcp71 import.
- dx/d3dx9.cpp: D3DX shims also registered as d3dx9_30.dll; six missing
  texture/surface functions fail cleanly and log once.

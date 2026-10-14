# Panxcz LibTool — universal il2cpp external tool

An **external ELF** (runs standalone, not an injected `.so`) for reading & manipulating
Unity/il2cpp games: ImGui + OpenGL ES overlay, game process memory access via
`/proc/<pid>/mem`. One binary for **all games** — the target game is selected at
runtime (typed in), not baked at build time.

by **Panxcz & Freebuff**

```
panxcz-libtool/
├── build.sh                     # build via ndk-build (NDK auto-detected)
├── tools/
│   ├── ndk_detect.sh            # find any NDK (r21..r30, Termux, zip)
│   ├── install_onphone.sh       # push + run on device
│   ├── push_dump.sh             # send .cs dump to dumper folder on device
│   └── dump_native.sh           # run --dumpout from PC (adb), optional
├── jni/
│   ├── Android.mk / Android-dbg.mk
│   ├── include/
│   │   ├── core/{mem,il2cpp,target,explore,scanner,safe,keys,log}.h
│   │   └── ui/ui.h
│   └── src/
│       ├── main.cpp             # boot: overlay + attach + (target from argv/cfg)
│       ├── core/
│       │   ├── mem.cpp          # /proc/<pid>/mem read/write, badPage cache
│       │   ├── il2cpp.cpp       # metadata, class/field/method lookup
│       │   ├── target.cpp       # per-game layout (MLBB/FF/CODM/Subway)
│       │   ├── explore.cpp      # class tree explorer, RVA resolution
│       │   ├── scanner.cpp      # memory scan (first/refine/freeze)
│       │   ├── safe.cpp         # read/write guards, anti-drift
│       │   ├── keys.cpp         # keybinds (custom keypad overlay)
│       │   ├── log.cpp           # log capture
│       │   └── gencode.h/memview.h  # hex/asm viewer + code gen
│       └── ui/
│           └── ui.cpp            # ImGui overlay + OpenGL ES 3.0
└── .github/workflows/build-release.yml   # CI: build + verify + auto-release
```

## Features

- **External ELF** — runs independently, reads/writes game memory via `/proc/<pid>/mem`. No injection, no Frida required.
- **Runtime IL2CPP dumper** (`--dumpout`) — native dump, no PC/Frida needed. Output to `/sdcard/panxcz-tools/dumper/<pkg>/<ver>/`.
- **Class/field/method explorer** — offset/RVA resolution, per-game layout auto-detection.
- **Runtime patch + force-return** (bool/int/float/void) with anti-drift.
- **Memory scanner** (first/refine/freeze) + hex/asm viewer.
- **Universal catalog merge** — dump offline (Il2CppDumper/Frida) + runtime pool.
- **Layout auto-detection per game** (MLBB/FF/Subway/CODM) — no hardcoded offsets.
- **16KB page alignment enforced**, exports verified via map files.
- **Three output names**: `.sh` / `.elf` / bare (identical ELF).

## Quick Start

```bash
# Clone + build locally
git clone https://github.com/rotteng749-lgtm/panxcz-libtool
cd panxcz-libtool
bash build.sh

# Or grab a release binary
wget https://github.com/rotteng749-lgtm/panxcz-libtool/releases/download/v1.0.8/panxcz-libtool-v1.0.8-arm64.tar.gz
tar -xzf panxcz-libtool-v1.0.8-arm64.tar.gz
```

## Usage

### Auto mode (finds game process automatically)

```bash
su -c '/data/local/tmp/panxcz_libtool'
```

### Manual mode (specify package)

```bash
su -c '/data/local/tmp/panxcz_libtool.sh com.mobile.legends libcsharp.so'
```

### By PID

```bash
su -c '/data/local/tmp/panxcz_libtool.sh com.mobile.legends:UnityKillsMe --pid 1234'
```

### Native dump (no Frida)

```bash
su -c '/data/local/tmp/panxcz_libtool.sh com.mobile.legends:UnityKillsMe --dumpout --all-libs --json --header --ptr --exit'
```

Output: `/sdcard/panxcz-tools/dumper/<pkg>/<ver>/`

### Memory scan

```bash
su -c '/data/local/tmp/panxcz_libtool.sh com.mobile.legends --search <value>'
```

### Runtime patch

```bash
su -c '/data/local/tmp/panxcz_libtool.sh com.mobile.legends --patch <offset> --bool'
```

## Build

```bash
bash build.sh
```

Options:

| Flag | Description |
|------|-------------|
| `--ndk /path/to/ndk` | Override NDK path |
| `--abi arm64-v8a` | Target ABI |
| `--jobs 8` | Parallel jobs |
| `--debug` | Debug build (Android-dbg.mk) |
| `--clean` | Clean build artifacts |

NDK is auto-detected from: `NDK_HOME` / `ANDROID_NDK_HOME` / `ANDROID_NDK_ROOT`, `sdk/ndk/`, `/opt`, Termux, NDK zips in `/sdcard/Download`. Any NDK r21..r30 works.

## CI/CD

GitHub Actions builds arm64-v8a on `v*` tags:

1. Install NDK r28b via `nttld/setup-ndk`
2. Build via `ndk-build`
3. Verify **16KB page alignment** (`readelf` parsing)
4. Verify **symbol exports** against `menu.map.txt` / `dumper.map.txt` / `zygisk.map.txt`
5. Package `tar.gz` → upload artifact + auto-release via `softprops/action-gh-release`

```bash
git tag v1.0.9 && git push origin v1.0.9
```

## License

MIT License — Copyright (c) 2025 Panxcz & Freebuff
#!/usr/bin/env bash
# ============================================================================
#  build.sh - build PANXCZ LIBTOOL (external ELF, universal il2cpp tool)
#
#  Hasil: out/panxcz_libtool.sh   <- ELF arm64 (nama .sh-nya SENGAJA: biar bisa
#          dikirim/disimpan sebagai satu file)
#          Jalanin LANGSUNG, jangan lewat sh:
#            su -c '/data/local/tmp/panxcz_libtool.sh com.mobile.legends'
#          (shell Android = mksh, nggak nge-exec ELF: `sh <elf>` => syntax error)
#
#  NDK dicari otomatis (tools/ndk_detect.sh): NDK_HOME / ANDROID_NDK_HOME /
#  ANDROID_NDK_ROOT, sdk/ndk/<versi>, /opt, Termux, zip NDK di /sdcard/Download.
#  Versi NDK apa pun (r21..r30) -- ndk-build yang dipakai, jadi build-nya sama
#  seperti Android.mk biasa.
#
#  Usage:
#    bash build.sh                       # arm64-v8a
#    bash build.sh --ndk /path/android-ndk-r25c
#    bash build.sh --debug               # log verbose (Android-dbg.mk)
#    bash build.sh --jobs 8
# ============================================================================
set -uo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ABI="arm64-v8a"
MODE="release"
JOBS="$( (nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4) )"
NDK_OVERRIDE=""

while [ $# -gt 0 ]; do
    case "$1" in
        --ndk)      NDK_OVERRIDE="$2"; shift 2 ;;
        --abi|-a)   ABI="$2"; shift 2 ;;
        --jobs|-j)  JOBS="$2"; shift 2 ;;
        --debug)    MODE="debug"; shift ;;
        --clean)    rm -rf "$HERE/obj" "$HERE/libs" "$HERE/out"; echo "[*] clean"; shift ;;
        -h|--help)  sed -n '2,25p' "$0"; exit 0 ;;
        *) echo "option nggak dikenal: $1" >&2; exit 1 ;;
    esac
done

if [ -n "$NDK_OVERRIDE" ]; then
    export NDK_HOME="$NDK_OVERRIDE"
fi

# Tool ini dibangun pakai ndk-build (Android.mk + BUILD_EXECUTABLE), jadi NDK
# yang dicari HARUS yang punya ndk-build. Detector umum (ndk_detect.sh) sengaja
# nerima NDK "tipis" (clang+sysroot doang, buat build manual) -- itu nggak cukup
# di sini, jadi dicari NDK lengkap dulu.
find_full_ndk() {
    local c
    for c in \
        "${NDK_HOME:-}" "${ANDROID_NDK_HOME:-}" "${ANDROID_NDK_ROOT:-}" \
        "$HERE/ndk/android-ndk-r"* "$HERE/../ndk/android-ndk-r"* \
        "$HOME/android-ndk-r"* "$HOME/Android/android-ndk-r"* \
        "$HOME/Android/Sdk/ndk/"* "$HOME/android-sdk/ndk/"* \
        "${ANDROID_HOME:-}/ndk/"* "${ANDROID_SDK_ROOT:-}/ndk/"* \
        "$HOME/ci-toolchain/android-ndk-"* \
        /opt/android-ndk-r* /usr/local/android-ndk-r* \
        "${PREFIX:-}/opt/android-ndk-r"*; do
        [ -n "$c" ] || continue
        [ -x "$c/ndk-build" ] && { echo "$c"; return 0; }
    done
    return 1
}

FULL_NDK="$(find_full_ndk || true)"

# shellcheck source=tools/ndk_detect.sh
source "$HERE/tools/ndk_detect.sh"
if [ -n "$FULL_NDK" ]; then
    NDK_HOME="$FULL_NDK"          # detektor hormati env ini duluan
fi
if ! lt_ndk_detect; then
    echo "[x] NDK nggak ketemu -- lihat pesan di atas." >&2
    exit 1
fi
lt_ndk_info

if [ ! -x "$LT_NDK_DIR/ndk-build" ]; then
    echo "[x] NDK yang kepilih (\$LT_NDK_DIR) nggak punya ndk-build:"
    echo "    $LT_NDK_DIR"
    echo "    Tool ini butuh NDK LENGKAP (ada ndk-build-nya)."
    echo "    Cara: ndk-build ada di NDK resmi dari developer.android.com."
    echo "      taruh zip-nya di /sdcard/Download atau set NDK_HOME=/path/ke/android-ndk-rXX"
    exit 1
fi

MK="jni/Android.mk"
[ "$MODE" = "debug" ] && [ -f "$HERE/jni/Android-dbg.mk" ] && MK="jni/Android-dbg.mk"

echo "[*] NDK    : $LT_NDK_DIR (r$LT_NDK_VER)"
echo "[*] ABI    : $ABI"
echo "[*] mode   : $MODE ($MK)"
echo "[*] jobs   : $JOBS"

"$LT_NDK_DIR/ndk-build" \
    NDK_PROJECT_PATH="$HERE" \
    APP_BUILD_SCRIPT="$HERE/$MK" \
    NDK_APPLICATION_MK="$HERE/jni/Application.mk" \
    NDK_OUT="$HERE/obj" \
    NDK_LIBS_OUT="$HERE/libs" \
    APP_ABI="$ABI" \
    -j"$JOBS" || exit 1

SUF=""
[ "$MODE" = "debug" ] && SUF="dbg"

BIN="$HERE/libs$SUF/$ABI/panxcz_libtool"
[ -f "$BIN" ] || BIN="$HERE/libs/$ABI/panxcz_libtool"
[ -f "$BIN" ] || { echo "[x] binary nggak keproduksi" >&2; exit 1; }

mkdir -p "$HERE/out"
# TIGA nama dari SATU biner yang sama (isinya identik -- cuma beda nama pakai):
#   .sh            : nama lama, biar script/perintah yang udah ada tetap jalan
#   .elf           : jelas-jelas "ini ELF", buat dikirim/di-upload/inspeksi
#   tanpa ekstensi: buat dijadikan PAYLOAD ala tool client-server Free Fire
#                    (assets daemon: app nge-copy dari assets, `chmod 777`,
#                    terus `su -c <path>` -- mis. aset "Fucker" di HKBOSS)
cp -f "$BIN" "$HERE/out/panxcz_libtool.sh"
cp -f "$BIN" "$HERE/out/panxcz_libtool.elf"
cp -f "$BIN" "$HERE/out/panxcz_libtool"
chmod 755 "$HERE/out/panxcz_libtool.sh" "$HERE/out/panxcz_libtool.elf" "$HERE/out/panxcz_libtool"

# Cek cepat: hasilnya harus PIE aarch64 (bukan script, bukan arsip).
if command -v file >/dev/null 2>&1; then
    printf '[*] jenis file : %s\n' "$(file -b "$HERE/out/panxcz_libtool" | cut -c1-60)"
fi

echo
echo "[+] selesai:"
ls -lh "$HERE/out/panxcz_libtool" "$HERE/out/panxcz_libtool.elf" "$HERE/out/panxcz_libtool.sh" | awk '{printf "    %10s  %s\n", $5, $9}'
echo "    CARA PAKAI     : /data/local/tmp/panxcz_libtool            <- CUKUP INI"
echo "                     (nama .sh/.elf sama aja -- isinya biner yang sama)"
echo "    PAYLOAD (assets): taruh out/panxcz_libtool di assets app, terus aplikasinya"
echo "                     nge-copy + chmod 777 + su -c <path>  (pola daemon yang sama)"
echo "    contoh daemon  : su -c '/data/local/tmp/panxcz_libtool'   <-- tanpa argumen = auto"
echo "                     (belum root? tool naikin sendiri lewat su; terus cari"
echo "                      game sendiri, attach, dump sendiri -- tanpa argumen)"
echo "    manual (ops.)  : su -c '/data/local/tmp/panxcz_libtool.sh <package> [lib]'"
echo "    contoh         : su -c '/data/local/tmp/panxcz_libtool.sh com.mobile.legends libcsharp.so'"
echo "    pakai pid      : su -c '/data/local/tmp/panxcz_libtool.sh com.mobile.legends:UnityKillsMe --pid 1234'"
echo "    pakai dump     : su -c '/data/local/tmp/panxcz_libtool.sh com.mobile.legends --dump /data/local/tmp/panxcz_dump.cs --probe BattleManager --exit'"
echo "    DUMP SENDIRI   : su -c '/data/local/tmp/panxcz_libtool.sh com.mobile.legends:UnityKillsMe --dumpout --all-libs --json --header --ptr --exit'"
echo "                     (hasil di /sdcard/panxcz-tools/dumper/<pkg>/<versi>/, tanpa frida/PC)"
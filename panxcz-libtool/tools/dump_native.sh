#!/usr/bin/env bash
# ============================================================================
#  dump_native.sh - jalanin DUMP SENDIRI (--dumpout) di HP lewat adb
#
#  Nggak butuh frida / python: yang nge-dump itu tool-nya sendiri (root doang).
#  Hasilnya di HP: /sdcard/panxcz-tools/dumper/<pkg>/<versi>/<pkg>_native_full.cs
#  (+ .json / .h / .ptrs.tsv kalau diminta).
#
#  Pakai:
#    bash tools/dump_native.sh <serial> [package] [opsi tambahan...]
#
#  Contoh:
#    bash tools/dump_native.sh 192.168.1.61:40729
#    bash tools/dump_native.sh 192.168.1.61:40729 com.mobile.legends:UnityKillsMe --json --header
#    bash tools/dump_native.sh 192.168.1.61:40729 --like BattleManager --classes 5 --diag 1
#    PULL=1 bash tools/dump_native.sh 192.168.1.61:40729        # tarik hasilnya ke ./dumps
#
#  Default package: com.mobile.legends:UnityKillsMe (engine MLBB ada di
#  sub-proses ini, bukan di com.mobile.legends).
# ============================================================================
set -uo pipefail

SER="${1:-}"
[ -n "$SER" ] || { echo "pakai: bash tools/dump_native.sh <serial> [package] [opsi...]" >&2; exit 1; }
shift || true

PKG="com.mobile.legends:UnityKillsMe"
if [ $# -gt 0 ] && [ "${1#--}" = "$1" ]; then PKG="$1"; shift; fi

# --check : DIAGNOSA SAJA (root/izin /proc/pid/mem, region, baca blok 4 MB).
# Dipakai kalau dump gagal di HP lain -- hasilnya baris [check] + verdict.
MODE="dump"
if [ "${1:-}" = "--check" ]; then MODE="check"; shift; fi

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BIN="$ROOT/out/panxcz_libtool.sh"
REMOTE="/data/local/tmp/panxcz_libtool.sh"
ADB=(adb -s "$SER")

[ -f "$BIN" ] || { echo "[x] $BIN belum ada -- build dulu (bash build.sh)"; exit 1; }
"${ADB[@]}" get-state >/dev/null 2>&1 || { echo "[x] HP nggak kebaca adb ($SER)"; exit 1; }

echo "[1/4] push binary + chmod"
"${ADB[@]}" push "$BIN" "$REMOTE" >/dev/null || exit 1
"${ADB[@]}" shell "su -c 'chmod 755 $REMOTE'" >/dev/null

if [ "$MODE" = "check" ]; then
    echo "[2/4] jalanin DIAGNOSA: $PKG --check"
    "${ADB[@]}" shell "su -c 'logcat -c'"
    "${ADB[@]}" shell "su -c 'cd /data/local/tmp && timeout 120 ./panxcz_libtool.sh $PKG --check --exit'" >/dev/null 2>&1
    echo "[3/4] log:"
    "${ADB[@]}" shell "su -c 'logcat -d -s panxcz_libtool'" 2>&1 | tr -d '\r' \
        | grep -aE "check\]|proses target|boot\] exit" | sed 's/^.*panxcz_libtool: //' | sed 's/^/    /'
    echo "[4/4] (mode --check: nggak ada file hasil)"
    exit 0
fi

echo "[2/4] jalanin dumpout: $PKG --dumpout --all-libs $*"
"${ADB[@]}" shell "su -c 'logcat -c'"
"${ADB[@]}" shell "su -c 'cd /data/local/tmp && timeout 600 ./panxcz_libtool.sh $PKG --dumpout --all-libs $* --exit'" >/dev/null 2>&1

echo "[3/4] log:"
"${ADB[@]}" shell "su -c 'logcat -d -s panxcz_libtool'" 2>&1 | tr -d '\r' \
    | grep -aE "proses target|fase|dumpout|check\]|boot\] exit" | sed 's/^.*panxcz_libtool: //' | sed 's/^/    /'

echo "[4/4] hasil di HP:"
BASE="$(echo "$PKG" | cut -d: -f1)"
"${ADB[@]}" shell "su -c 'ls -la \$(find /sdcard/panxcz-tools/dumper/$BASE -name \"*_native*\" 2>/dev/null)'" 2>&1 \
    | tr -d '\r' | sed 's/^/    /'

if [ "${PULL:-0}" = "1" ]; then
    DIR="$ROOT/dumps/$BASE"
    mkdir -p "$DIR"
    echo "    tarik ke $DIR ..."
    "${ADB[@]}" shell "su -c 'tar -cf - -C /sdcard/panxcz-tools/dumper/$BASE .'" 2>/dev/null | tar -xf - -C "$DIR" 2>/dev/null \
        && echo "    ok: $(find "$DIR" -name '*_native*' | wc -l) file"
fi

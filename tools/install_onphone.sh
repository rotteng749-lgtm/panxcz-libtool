#!/usr/bin/env bash
# ============================================================================
#  install_onphone.sh - kirim tool ke HP + (opsional) langsung jalanin
#
#  Usage:
#    bash tools/install_onphone.sh <serial> [package] [lib]
#
#  Contoh:
#    bash tools/install_onphone.sh 192.168.1.61:35313
#    bash tools/install_onphone.sh H894X6833B012345 com.mobile.legends liblogic.so
#
#  Yang dilakukan:
#    1. push out/panxcz_libtool.sh -> /data/local/tmp/panxcz_libtool.sh
#    2. chmod 755
#    3. tulis target ke /data/local/tmp/panxcz_libtool.cfg (kalau package dikasih)
#    4. (kalau package dikasih) kill yang lama + jalanin di latar belakang
#    5. tampilkan 25 baris log terakhir (tag: panxcz_libtool)
# ============================================================================
set -uo pipefail

SER="${1:?serial HP (mis. 192.168.1.61:35313)}"
PKG="${2:-}"
LIB="${3:-}"
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
ADB=(adb -s "$SER")
BIN="$ROOT/out/panxcz_libtool.sh"

[ -f "$BIN" ] || { echo "[x] $BIN belum ada -- build dulu (bash build.sh)"; exit 1; }

"${ADB[@]}" get-state >/dev/null 2>&1 || { echo "[x] HP nggak kebaca adb"; exit 1; }

echo "[1/5] push binary ..."
"${ADB[@]}" push "$BIN" /data/local/tmp/panxcz_libtool.sh >/dev/null || exit 1
"${ADB[@]}" shell "su -c 'chmod 755 /data/local/tmp/panxcz_libtool.sh'" >/dev/null

if [ -n "$PKG" ]; then
    echo "[2/5] tulis cfg: package=$PKG lib=${LIB:-auto}"
    "${ADB[@]}" shell "su -c 'printf \"package=%s\\nlib=%s\\n\" \"$PKG\" \"${LIB:-auto}\" > /data/local/tmp/panxcz_libtool.cfg; chmod 666 /data/local/tmp/panxcz_libtool.cfg'" >/dev/null
else
    echo "[2/5] package nggak dikasih -- pilih game-nya di tab TARGET"
fi

echo "[3/5] matiin instance lama ..."
"${ADB[@]}" shell "su -c 'pkill -f panxcz_libtool; sleep 1'" >/dev/null 2>&1

if [ -n "$PKG" ]; then
    echo "[4/5] jalanin di latar belakang ..."
    "${ADB[@]}" shell "su -c 'cd /data/local/tmp && logcat -c; setsid /data/local/tmp/panxcz_libtool.sh $PKG ${LIB:-} </dev/null >/dev/null 2>&1 &'" >/dev/null
    sleep 6
    echo "[5/5] log terakhir:"
    "${ADB[@]}" shell "su -c 'logcat -d -s panxcz_libtool | tail -25'" 2>&1 | tr -d '\r'
else
    echo "[4/5] jalanin manual (butuh terminal di HP):"
    echo "      su -c '/data/local/tmp/panxcz_libtool.sh <package> [lib]'"
    echo "[5/5] selesai."
fi

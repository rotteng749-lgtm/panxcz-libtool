#!/usr/bin/env bash
# ============================================================================
#  push_dump.sh - kirim dump .cs ke HP, ke folder dumper:
#
#      /sdcard/panxcz-tool/dumper/<pkgname>/<versionname>/dump.cs
#
#  Tool nyari sendiri file-nya (urutan):
#    1. cfg `dump=` / argumen --dump
#    2. <dumpdir>/<pkg>/<versi>/*.cs     <- dumpdir default = /sdcard/panxcz-tool/dumper
#    3. /data/local/tmp/panxcz_dump.cs, /sdcard/panxcz/dump.cs, ...
#
#  Dump dipakai buat tabel "class-slot":
#      libbase + slot  ->  Il2CppClass*  ->  +0xa8 static_fields  ->  +offset
#  Itu jalur yang bikin kelas di MLBB ketemu (di sana scan-string mentok).
#
#  Pakai:
#    bash tools/push_dump.sh <serial> <dump.cs> [package] [versi]
#    bash tools/push_dump.sh 192.168.1.61:40729 "/home/panxcz/Documents/kont/dump (2).cs"
#    bash tools/push_dump.sh 192.168.1.61:40729 dump.cs com.mobile.legends 22.1.97.12061
#
#  Kalau package/versi nggak dikasih, script coba nebak dari HP (dumpsys).
# ============================================================================
set -uo pipefail

SERIAL="${1:-}"
DUMP="${2:-}"
PKG="${3:-}"
VER="${4:-}"
ROOT="${DUMP_ROOT:-/sdcard/panxcz-tool/dumper}"

if [ -z "$SERIAL" ] || [ -z "$DUMP" ]; then
    echo "pakai: bash tools/push_dump.sh <serial> <file-dump.cs> [package] [versi]" >&2
    exit 1
fi
[ -f "$DUMP" ] || { echo "[x] dump nggak ada: $DUMP" >&2; exit 1; }

ADB=(adb -s "$SERIAL")
SIZE=$(stat -c%s "$DUMP" 2>/dev/null || echo 0)
echo "[1/5] dump $DUMP ($((SIZE / 1024 / 1024)) MB)"

if [ -z "$PKG" ]; then
    # tebak: proses yang punya lib engine il2cpp -> ambil NAMA prosesnya
    # (cmdline), bukan pid-nya.
    PKG=$("${ADB[@]}" shell "su -c 'for p in \$(pidof com.mobile.legends) \$(pidof com.mobile.legends:UnityKillsMe); do
        grep -qE \"lib(csharp|logic|il2cpp)\\.so\" /proc/\$p/maps 2>/dev/null && { tr -d \"\\0\" < /proc/\$p/cmdline; echo; break; }
      done'" 2>/dev/null | tr -d '\r' | head -1)
    # nama folder = package tanpa sub-proses (dumper-nya nulis per package)
    PKG="${PKG%%:*}"
    [ -n "$PKG" ] || PKG="com.mobile.legends"
    echo "      package nggak dikasih -- nebak dari HP: $PKG"
fi

if [ -z "$VER" ]; then
    VER=$("${ADB[@]}" shell "su -c 'dumpsys package $PKG | grep -m1 versionName'" 2>/dev/null \
          | tr -d '\r' | sed 's/.*versionName=//')
    # paket nggak ketemu -> dumpsys balikin sampah; minta versi yang masuk akal
    case "$VER" in
        [0-9]*.[0-9]*) ;;
        *) echo "      versi nggak kebaca dari HP -- pakai 'unknown' (kasih argumen ke-4 kalau perlu)"; VER="unknown" ;;
    esac
    echo "      versi dari HP: $VER"
fi

DEST_DIR="$ROOT/$PKG/$VER"
echo "[2/5] folder tujuan: $DEST_DIR"
"${ADB[@]}" shell "su -c 'mkdir -p \"$DEST_DIR\"'" >/dev/null 2>&1 || true

echo "[3/5] push ..."
"${ADB[@]}" push "$DUMP" "$DEST_DIR/dump.cs" >/dev/null || exit 1
# salinan kedua di /data/local/tmp: tool jalan sebagai root & selalu bisa baca
"${ADB[@]}" shell "su -c 'cp \"$DEST_DIR/dump.cs\" /data/local/tmp/panxcz_dump.cs'" >/dev/null 2>&1 || true

echo "[4/5] cek cepat isinya ..."
F="$DEST_DIR/dump.cs"
printf '  kelas      : '; "${ADB[@]}" shell "su -c 'grep -ac \"^class \" \"$F\"'" | tr -d '\r'
printf '  punya slot : '; "${ADB[@]}" shell "su -c 'grep -ac \"Class-slot candidates: 0x\" \"$F\"'" | tr -d '\r'
echo "  module     :"
# (sed/sort-nya di PC aja: bikin quoting di sisi HP simpel)
"${ADB[@]}" shell "grep -a '^// Module ' '$F' | sort -u" 2>/dev/null \
    | tr -d '\r' | sed 's/; TypeDefIndex.*//' | head -5 | sed 's/^/    /'
printf '  md5(dump)  : '; "${ADB[@]}" shell "su -c 'md5sum \"$F\"'" 2>/dev/null | cut -d' ' -f1 | tr -d '\r'

echo "[5/5] tes cepat (probe 1 kelas, 30-60 detik):"
"${ADB[@]}" shell "su -c 'logcat -c; cd /data/local/tmp && timeout 90 ./panxcz_libtool.sh $PKG --probe BattleManager --exit'" >/dev/null 2>&1
"${ADB[@]}" shell "su -c 'logcat -d -s panxcz_libtool'" 2>&1 | grep -aE "dump\]|il2/dump|probe\] OK|exit:" | tr -d '\r' | sed 's/^/    /'
echo "[+] selesai."

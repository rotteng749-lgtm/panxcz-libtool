#!/usr/bin/env bash
# ============================================================
# ndk_detect.sh -- cari NDK Android yang BENERAN ADA di mesin ini
# ------------------------------------------------------------
# Dipakai (di-source) oleh build_direct.sh / build_plain.sh / build_obf.sh /
# build_all.sh. Semua skrip itu dulu cuma ngeliat dua tempat:
#     $NDK_HOME  atau  $HOME/ci-toolchain/android-ndk-r29
# Jadi kalau NDK-nya beda versi (r25/r26/r27/r28/r30), beda layout
# (sdk/ndk/<versi>), beda OS (darwin-*), atau lu build di HP (Termux), build-nya
# nyerah walau NDK-nya jelas-jelas ada. Sekarang SEMUA kandidat disisir.
#
# Yang di-support:
#   - $NDK_HOME / $ANDROID_NDK_HOME / $ANDROID_NDK_ROOT (dihormati duluan)
#   - $ANDROID_HOME/ndk/<ver>, $ANDROID_SDK_ROOT/ndk/<ver>
#   - ci-toolchain/android-ndk-*, /opt, /usr/local, /usr/lib, ~/android-ndk-*
#   - Termux ($PREFIX/opt/android-ndk*, ~/../usr/opt/...)
#   - /tmp/ndk-sys/android-ndk-* (hasil extract lama)
#   - zip NDK resmi di /sdcard/Download/**, /tmp -- otomatis di-extract
#     seperlunya (cuma bagian arch-independent: sysroot + builtins + unwind)
#   - versi clang APA PUN: folder lib/clang/<n> dideteksi, jadi nggak kepaku di
#     clang/21 kayak build lama.
#   - prebuilt tag apa pun (linux-x86_64, linux-aarch64, darwin-*).
#
# Hasil (semua diekspor):
#   LT_NDK_DIR         akar NDK
#   LT_NDK_VER         versi (source.properties)
#   LT_NDK_PREBUILT    .../toolchains/llvm/prebuilt/<tag>
#   LT_NDK_SYSROOT     .../<tag>/sysroot
#   LT_NDK_BIN         .../<tag>/bin
#   LT_NDK_CLANGXX     clang++ yang dipakai
#   LT_NDK_CLANG       clang (C) yang dipakai -- mbedTLS/zip itu C
#   LT_NDK_AR          llvm-ar yang dipakai
#   LT_NDK_TARGET_ARGS ("--target=aarch64-linux-android21 --sysroot=..." atau kosong)
#   LT_NDK_RT_BUILTINS file libclang_rt.builtins-aarch64-android.a
#   LT_NDK_RT_UNWIND   file libunwind.a (disiapin kalau cuma ada di clang/)
#   LT_NDK_HOST_SYSROOT 1 = clang host dipakai sama sysroot NDK (mode HP)
#   LT_NDK_STRIP        llvm-strip (kalau ada)
#
# Pemakaian:
#     source "$(dirname "$0")/tools/ndk_detect.sh"
#     lt_ndk_detect || exit 1        # dia udah nge-print alasannya
# ============================================================

# Jangan di-`set -e` di sini: skrip ini cuma nyari, dan gagal-nya dilaporkan
# lewat nilai balik.

_lt_ndk_dbg() { [[ -n "${LT_NDK_VERBOSE:-}" ]] && echo "[ndk] $*" >&2; return 0; }

# Host arch -> bentuk yang dipakai nama tag prebuilt NDK.
_lt_host_arch() {
    case "$(uname -m 2>/dev/null)" in
        x86_64|amd64)      echo "x86_64" ;;
        aarch64|arm64|armv8*) echo "arm64" ;;
        *) echo "unknown" ;;
    esac
}

# Tag prebuilt -> arch yang dibutuhin host-nya.
_lt_tag_arch() {
    case "$1" in
        *x86_64*) echo "x86_64" ;;
        *aarch64*|*arm64*) echo "arm64" ;;
        *) echo "unknown" ;;
    esac
}

# Ngecek satu kandidat NDK. Kalau valid -> set semua variabel LT_NDK_*.
# Balikin 0 kalau dipakai.
_lt_ndk_try() {
    local dir="$1"
    [[ -n "$dir" && -d "$dir" ]] || return 1
    [[ -d "$dir/toolchains/llvm/prebuilt" ]] || return 1

    local prebuilt tag host hostarch tagarch
    # Ambil prebuilt yang arch-nya cocok sama host dulu (kalau ada), baru
    # fallback ke yang pertama ketemu (buat mode host-clang + sysroot).
    hostarch="$(_lt_host_arch)"
    prebuilt=""
    for p in "$dir"/toolchains/llvm/prebuilt/*; do
        [[ -d "$p" ]] || continue
        tag="$(basename "$p")"
        tagarch="$(_lt_tag_arch "$tag")"
        if [[ "$tagarch" == "$hostarch" ]]; then prebuilt="$p"; break; fi
        [[ -z "$prebuilt" ]] && prebuilt="$p"
    done
    [[ -n "$prebuilt" ]] || return 1

    local sysroot="$prebuilt/sysroot"
    [[ -d "$sysroot" ]] || return 1

    # clang: wrapper per-target lebih enak (nggak perlu --target), tapi cuma
    # bisa dipakai kalau arch-nya arch host. Di HP (arm64) prebuilt-nya x86_64
    # -> wrapper-nya nggak bisa jalan, jadi pakai clang host + sysroot NDK.
    local clangxx="" targetargs="" hostsysroot=0 bin="$prebuilt/bin"
    if [[ -x "$bin/aarch64-linux-android21-clang++" ]] &&
       [[ "$(_lt_tag_arch "$(basename "$prebuilt")")" == "$hostarch" ]]; then
        clangxx="$bin/aarch64-linux-android21-clang++"
    elif [[ -x "$bin/clang++" ]] &&
         [[ "$(_lt_tag_arch "$(basename "$prebuilt")")" == "$hostarch" ]]; then
        clangxx="$bin/clang++"
        targetargs="--target=aarch64-linux-android21 --sysroot=$sysroot"
    else
        # MODE HP / cross-arch: clang host + sysroot NDK.
        local hostclang=""
        for c in clang++ /usr/bin/clang++ /usr/lib/llvm-*/bin/clang++ "${PREFIX:-}/bin/clang++"; do
            if command -v "$c" >/dev/null 2>&1; then hostclang="$(command -v "$c")"; break; fi
            [[ -x "$c" ]] && { hostclang="$c"; break; }
        done
        [[ -n "$hostclang" ]] || return 1
        clangxx="$hostclang"
        targetargs="--target=aarch64-linux-android21 --sysroot=$sysroot"
        hostsysroot=1
    fi

    # Versi folder clang (dulu dipaku di "21" -- NDK r26 = 17, r27 = 18, dst).
    # Kalau ada beberapa versi, ambil yang paling tinggi.
    #
    # PENTING: NDK r25c dan sebelumnya menaruh runtime-nya di  lib64/clang/... ,
    # r26+ pindah ke  lib/clang/... . Dulu di sini cuma lib/clang yang dilihat,
    # jadi NDK lama yang SEHAT (mis. r25c resmi, 1,7 GB) DITOLAK mentah-mentah --
    # detektornya lompat ke NDK lain yang "kelihatan" lengkap padahal cuma
    # clang+sysroot. Sekarang dua layout dicari.
    local cver="" v croot
    for croot in "$prebuilt/lib/clang" "$prebuilt/lib64/clang"; do
        [[ -d "$croot" ]] || continue
        for c in "$croot"/*/lib/linux; do
            [[ -d "$c" ]] || continue
            v="$(basename "$(dirname "$(dirname "$c")")")"
            if [[ -z "$cver" || "$(printf '%s\n%s\n' "$cver" "$v" | sort -V | tail -1)" == "$v" ]]; then
                cver="$v"
            fi
        done
    done

    # builtins/unwind CUMA dipakai jalur clang-manual (build di HP). Kalau NDK-nya
    # punya ndk-build, dua file ini nggak dibutuhin -- jadi JANGAN dijadikan
    # syarat mutlak, cukup diisi kosong.
    local builtins="" unwind=""
    if [[ -n "$cver" ]]; then
        for croot in "$prebuilt/lib/clang" "$prebuilt/lib64/clang"; do
            [[ -d "$croot" ]] || continue
            if [[ -f "$croot/$cver/lib/linux/libclang_rt.builtins-aarch64-android.a" ]]; then
                builtins="$croot/$cver/lib/linux/libclang_rt.builtins-aarch64-android.a"
                break
            fi
        done
        local u1="$sysroot/usr/lib/aarch64-linux-android/21/libunwind.a"
        if [[ -f "$u1" ]]; then
            unwind="$u1"
        else
            for croot in "$prebuilt/lib/clang" "$prebuilt/lib64/clang"; do
                [[ -d "$croot" ]] || continue
                if [[ -f "$croot/$cver/lib/linux/aarch64/libunwind.a" ]]; then
                    mkdir -p "$(dirname "$u1")" 2>/dev/null
                    cp -f "$croot/$cver/lib/linux/aarch64/libunwind.a" "$u1" 2>/dev/null || true
                    [[ -f "$u1" ]] && unwind="$u1"
                    break
                fi
            done
        fi
    fi

    # ---- C compiler ---------------------------------------------------------
    # mbedTLS (dan sebagian sumber lain) itu C. Wajib clang, BUKAN clang++:
    # kalau clang++ yang dipakai, file .c dikompilasi sebagai C++ dan mbedtls-nya
    # gagal (nama symbol beda / error C). Dulu tiap skrip nyari sendiri-sendiri
    # dan hasilnya beda-beda; sekarang satu tempat.
    local clangc=""
    if [[ "$hostsysroot" != "1" && -x "$bin/aarch64-linux-android21-clang" ]]; then
        clangc="$bin/aarch64-linux-android21-clang"        # wrapper NDK (paling enak)
    elif [[ "$hostsysroot" != "1" && -x "$bin/clang" ]] &&
         [[ "$(_lt_tag_arch "$(basename "$prebuilt")")" == "$hostarch" ]]; then
        clangc="$bin/clang"                                # clang NDK + --target
    else
        clangc="$(command -v clang 2>/dev/null || true)"     # MODE HP / cross-arch
        [[ -n "$clangc" ]] || clangc="clang"
    fi
    export LT_NDK_CLANG="$clangc"

    # ---- archiver ----------------------------------------------------------
    local ar=""
    if [[ "$hostsysroot" != "1" && -x "$bin/llvm-ar" ]]; then ar="$bin/llvm-ar"; fi
    [[ -n "$ar" ]] || ar="$(command -v llvm-ar 2>/dev/null || true)"
    [[ -n "$ar" ]] || ar="$(command -v ar 2>/dev/null || true)"
    export LT_NDK_AR="${ar:-ar}"

    export LT_NDK_DIR="$dir"
    export LT_NDK_PREBUILT="$prebuilt"
    export LT_NDK_SYSROOT="$sysroot"
    export LT_NDK_BIN="$bin"
    export LT_NDK_CLANGXX="$clangxx"
    export LT_NDK_TARGET_ARGS="$targetargs"
    export LT_NDK_RT_BUILTINS="$builtins"
    export LT_NDK_RT_UNWIND="$unwind"
    export LT_NDK_HOST_SYSROOT="$hostsysroot"
    export LT_NDK_CLANG_VER="$cver"
    if [[ -f "$dir/source.properties" ]]; then
        LT_NDK_VER="$(sed -n 's/^Pkg\.Revision *= *//p' "$dir/source.properties" | head -1)"
    fi
    # Kalau source.properties nggak ada / nggak punya Pkg.Revision, ambil dari
    # nama foldernya (android-ndk-r27 -> 27). Dulu di kasus itu keluar "runknown"
    # dan itu bikin bingung pas nyari masalah versi.
    if [[ -z "${LT_NDK_VER:-}" || "$LT_NDK_VER" == "unknown" ]]; then
        local dirver
        dirver="$(basename "$dir" | sed -n 's/.*android-ndk-r\([0-9][0-9]*\).*/\1/p')"
        # Layout SDK: foldernya langsung nomor versi (sdk/ndk/27.0.12077973).
        [[ -n "$dirver" ]] || dirver="$(basename "$dir" | sed -n 's/^\([0-9][0-9.]*\).*/\1/p')"
        [[ -n "$dirver" ]] && LT_NDK_VER="$dirver"
    fi
    export LT_NDK_VER="${LT_NDK_VER:-unknown}"
    if [[ -x "$bin/llvm-strip" ]] && [[ "$hostsysroot" == "0" ]]; then
        export LT_NDK_STRIP="$bin/llvm-strip"
    else
        export LT_NDK_STRIP="$(command -v llvm-strip 2>/dev/null || true)"
    fi
    return 0
}

# Nyisir daftar kandidat, urut prioritas. Yang pertama valid dipakai.
lt_ndk_detect() {
    local p
    # 0. Kalau skrip INDUK-nya udah nemu NDK, pakai itu apa adanya. Penting buat
    #    skrip anak (mis. build_mbedtls.sh dipanggil dari build_obf.sh): hasilnya
    #    dijamin sama, dan nggak nyisir ulang.
    if [[ -n "${LT_NDK_DIR:-}" && -d "${LT_NDK_DIR:-}" ]]; then
        _lt_ndk_try "$LT_NDK_DIR" && { _lt_ndk_dbg "dari induk: $LT_NDK_DIR"; return 0; }
    fi

    # 1. env var yang emang buat itu (paling dihormati)
    for p in "${NDK_HOME:-}" "${ANDROID_NDK_HOME:-}" "${ANDROID_NDK_ROOT:-}"; do
        [[ -n "$p" ]] && _lt_ndk_try "$p" && { _lt_ndk_dbg "dari env: $p"; return 0; }
    done

    # 2. SDK-style (sdk/ndk/<versi>) -- ambil versi TERBESAR
    local sdkroot
    for sdkroot in "${ANDROID_HOME:-}" "${ANDROID_SDK_ROOT:-}" \
                   "$HOME/Android/Sdk" "$HOME/android-sdk" \
                   "${PREFIX:-}/opt/android-sdk" "/opt/android-sdk"; do
        [[ -n "$sdkroot" && -d "$sdkroot/ndk" ]] || continue
        local best="" bestver=""
        for p in "$sdkroot"/ndk/*; do
            [[ -d "$p" ]] || continue
            local v; v="$(basename "$p")"
            if [[ -z "$bestver" || "$(printf '%s\n%s\n' "$bestver" "$v" | sort -V | tail -1)" == "$v" ]]; then
                best="$p"; bestver="$v"
            fi
        done
        [[ -n "$best" ]] && _lt_ndk_try "$best" && { _lt_ndk_dbg "dari SDK: $best"; return 0; }
    done

    # 3. folder-folder yang umum dipakai (versi apa pun)
    local pats=(
        "$HOME/ci-toolchain/android-ndk-*"
        "$HOME/android-ndk-*"
        "$HOME/Android/android-ndk-*"
        "/opt/android-ndk-*"
        "/opt/android-ndk"
        "/usr/local/android-ndk*"
        "/usr/lib/android-ndk*"
        "${PREFIX:-}/opt/android-ndk*"
        "$HOME/../usr/opt/android-ndk*"
        "/tmp/ndk-sys/android-ndk-*"
        "$HOME/.ndk/*"
    )
    for p in "${pats[@]}"; do
        local cand
        for cand in $p; do                 # sengaja tanpa kutip: glob
            [[ -d "$cand" ]] || continue
            _lt_ndk_try "$cand" && { _lt_ndk_dbg "dari glob: $cand"; return 0; }
        done
    done

    # 4. zip NDK resmi -> extract seperlunya. Di HP biasanya di /sdcard/Download,
    #    di PC di /tmp. Yang diambil cuma yang dibutuhin (sysroot + builtins +
    #    unwind), soalnya NDK resmi r23+ itu ~600 MB.
    local zip
    for zip in /sdcard/Download/1DMP/Compressed/android-ndk-*.zip \
               /sdcard/Download/*/android-ndk-*.zip \
               /sdcard/Download/android-ndk-*.zip \
               /storage/emulated/0/Download/android-ndk-*.zip \
               /tmp/android-ndk-*.zip "$HOME/Downloads/android-ndk-"*.zip; do
        [[ -f "$zip" ]] || continue
        echo "[ndk] sysroot belum ada -- extract dari zip: $zip"
        local top; top="$(basename "$zip" .zip)"            # android-ndk-r29
        local dest="${LT_NDK_ZIP_DEST:-/tmp/ndk-sys}"
        mkdir -p "$dest" || { echo "[ndk] ✗ nggak bisa nulis ke $dest"; continue; }
        # Tag prebuilt: ikut host (linux-x86_64 / linux-aarch64 / darwin-*).
        local tag="linux-x86_64"
        case "$(uname -s 2>/dev/null)" in Darwin*) tag="darwin-x86_64";; esac
        case "$(_lt_host_arch)" in arm64) tag="${tag%x86_64}aarch64";; esac
        command -v unzip >/dev/null 2>&1 || { echo "[ndk] ✗ unzip nggak ada"; break; }
        unzip -qo "$zip" \
            "$top/toolchains/llvm/prebuilt/$tag/sysroot/*" \
            "$top/toolchains/llvm/prebuilt/$tag/lib/clang/*/lib/linux/*aarch64*" \
            "$top/toolchains/llvm/prebuilt/$tag/lib/clang/*/lib/linux/aarch64/*" \
            "$top/source.properties" \
            -d "$dest" || { echo "[ndk] ✗ extract gagal"; continue; }
        _lt_ndk_try "$dest/$top" && { _lt_ndk_dbg "dari zip: $dest/$top"; return 0; }
    done

    echo "[ndk] ✗ NDK Android nggak ketemu."
    echo "[ndk]   Yang disisir: \$NDK_HOME, \$ANDROID_NDK_HOME, \$ANDROID_NDK_ROOT,"
    echo "[ndk]   \$ANDROID_HOME/ndk/*, \$HOME/ci-toolchain/android-ndk-*, /opt, /usr/local,"
    echo "[ndk]   \$PREFIX/opt, /tmp/ndk-sys, dan zip NDK di /sdcard/Download."
    echo "[ndk]   Cara paling gampang: taruh zip NDK resmi di /sdcard/Download,"
    echo "[ndk]   atau: NDK_HOME=/path/ke/android-ndk-rXX bash build_all.sh"
    return 1
}

# Print ringkas buat log build.
lt_ndk_info() {
    echo "[*] NDK      : $LT_NDK_DIR (r$LT_NDK_VER, clang/$LT_NDK_CLANG_VER)"
    echo "[*] prebuilt : $(basename "$LT_NDK_PREBUILT")"
    echo "[*] clang    : $LT_NDK_CLANGXX"
    echo "[*] clang(C) : $LT_NDK_CLANG   (buat mbedTLS)"
    echo "[*] ar       : $LT_NDK_AR"
    if [[ "$LT_NDK_HOST_SYSROOT" == "1" ]]; then
        echo "[*] mode     : clang HOST + sysroot NDK (build di HP tanpa NDK host-nya)"
    fi
}

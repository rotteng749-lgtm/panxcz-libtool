// ============================================================================
//  libdump.h - "dump LIB"-nya: gambar memori + laporan isi sebuah .so
//
//  KENAPA MODUL INI ADA
//  --------------------
//  Dump il2cpp (core/dumpout.h) cuma ngomong soal kelas/field/method GAME.
//  Yang nggak pernah kelihatan justru LIB ANTI-CHEAT-nya (Free Fire: libanogs.so
//  + libanort.so) -- padahal itu yang nentuin apa aja yang harus di-hide.
//
//  Modul ini bikin DUA hal, dua-duanya READ-ONLY (nggak nulis ke memori game):
//
//    1. GAMBAR MEMORI (.img)
//       Semua segmen lib yang ke-map di proses ditulis ke satu file dengan
//       susunan FILE OFFSET (bukan susunan alamat). Jadi offset di file itu sama
//       dengan offset di .so aslinya -> bisa dibuka di IDA/Ghidra (load as
//       binary, sesuaikan base-nya), termasuk bagian yang di memori udah
//       di-relocate/di-decrypt sama game-nya.
//       Plus file .regions.txt: tiap segmen (start-end, izin, file offset)
//       -> dipakai buat ngubah alamat runtime ke alamat file dan sebaliknya.
//
//       Khusus lib anti-cheat (yang biasanya di-strip total), gambar memori ini
//       yang jadi sumber utama: di file yang sama alamatnya tetap kayak di
//       memori, jadi offset di .img = offset di .so-nya (tinggal rebase).
//
//    2. LAPORAN (.txt + .json)
//       Baca ELF-nya langsung dari memori: header, segmen, NEEDED (lib apa aja
//       yang dia pakai), EXPORT function (API anti-cheat-nya), IMPORT (contoh:
//       ptrace/inotify -> itu alat deteksinya), terus STRING-nya dipindai dan
//       ditandai: mana yang nyebut frida/magisk/KSU/xposed/emulator/dumper,
//       dan mana yang nyebut JEJAK TOOL KITA SENDIRI (mis. "/data/local/tmp").
//       Daftar string lengkap ditulis ke .strings.txt.
//
//       LOGIC-NYA (ini yang bikin dump-nya kepakai, bukan cuma kumpulan byte):
//         - [API YANG DIPANGGIL] dari RELA/JMPREL: nama fungsi luar (ptrace,
//           inotify_add_watch, dl_iterate_phdr, fopen, getpid, ...) tetap kepas
//           walau .dynsym/DT_HASH dibuang, plus rva slot GOT-nya masing-masing.
//         - [CONSTRUCTOR] DT_INIT + DT_INIT_ARRAY: fungsi yang jalan saat lib
//           di-load -- di lib anti-cheat dari sini dia nyalain thread pemantau
//           dan pasang hook, jadi rva-nya yang pertama diburu.
//         - [FUNGSI] dari .eh_frame_hdr (nggak bisa di-strip: exception C++ butuh
//           itu): daftar SEMUA fungsi. Ditulis dalam 4 bentuk dari data yang sama,
//           jadi nggak wajib punya IDA:
//             <lib>.fns.txt       : rva + ukuran (jarak ke fungsi berikutnya)
//             <lib>.fns.r2        : script radare2 -> r2 -a arm -b 64 <lib>.img -i <lib>.fns.r2
//             <lib>.fns.ghidra.py : post-script Ghidra (img di-import raw binary, base 0)
//             <lib>.fns.map       : alamat+nama (kode/data) buat tool/editor apa pun
//
//  Ini yang dipakai buat jawab "apa aja yang butuh di-hide", bukan nebak-nebak.
// ============================================================================
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace libdump {

struct Options {
    // Kosong = otomatis: lib engine il2cpp + SEMUA lib anti-cheat yang ke-load.
    std::vector<std::string> libs;
    // Kosong = <dumpdir>/<package>/<versi>/ac
    std::string outDir;
    bool image  = true;      // tulis gambar memori (.img)
    bool report = true;      // tulis laporan (.txt + .json + .strings.txt)
    // allSo: SELAIN engine + anti-cheat, ikut juga lib milik game-nya sendiri
    // (path-nya nyebut package-nya, mis. /data/app/...com.mobile.legends.../lib/).
    // Lib sistem Android (HAL kamera, codec, mediacodec, ...) NGGAK ikut -- di
    // Free Fire jumlahnya 400-an dan isinya nggak ada hubungannya sama game.
    bool allSo  = false;
    // systemLibs: pakai kalau memang mau SEMUA .so di proses, termasuk sistem.
    bool systemLibs = false;
    size_t maxImageMB = 768;      // batas gambar memori PER LIB (MB)
    // Batas TOTAL gambar memori buat semua lib dalam satu sesi -- biar
    // `--libdump-all` di game ber-lib banyak nggak ngabisin storage.
    size_t maxImageTotalMB = 1024;
    size_t maxStrings = 300000;  // batas jumlah string yang ditulis
};

struct Lib {
    std::string name;         // basename, mis. libanogs.so
    std::string path;         // path lengkapnya di /proc/<pid>/maps
    uintptr_t   base = 0;     // alamat map paling bawah
    size_t      mappedBytes = 0;
    size_t      segments = 0;
    size_t      exports = 0;  // jumlah simbol yang didefinisikan (FUNC dll)
    size_t      imports = 0;  // jumlah simbol undefined (import)
    size_t      stringsTotal = 0;
    size_t      hits = 0;     // string yang kena kata kunci (vektor + jejak tool)
    std::vector<std::string> vectors;   // deteksi vektor yang ketemu
    std::vector<std::string> traces;    // JEJAK TOOL (mis. /data/local/tmp)
    // "logic"-nya: dari relokasi + unwind, tetap kebaca walau lib-nya di-strip
    // total (libanogs/libanort nggak punya tabel simbol sama sekali).
    size_t      relocUses = 0;   // berapa kali API luar dipanggil (relokasi)
    size_t      ctors = 0;       // DT_INIT + DT_INIT_ARRAY (pintu masuk logic)
    size_t      funcs = 0;       // jumlah fungsi dari .eh_frame_hdr
    // path hasil (kosong = nggak ditulis). fnsPath = .fns.txt, r2Path = .fns.r2
    //   (.fns.ghidra.py dan .fns.map dihitung dari fnsPath dengan ekstensi lain)
    std::string imagePath, reportPath, stringsPath, jsonPath, fnsPath, r2Path;
};

struct Result {
    bool ok = false;
    std::string dir;                 // folder hasil
    std::vector<Lib> libs;
};

// Lib yang dianggap "menarik": engine il2cpp + lib anti-cheat yang ke-load di
// proses ini. allSo=true -> ikut lib milik game-nya sendiri (systemLibs=true
// baru ikut SEMUA .so, termasuk lib sistem Android).
std::vector<std::string> autoLibs(bool allSo = false, bool systemLibs = false);

// Jalanin. Berat (baca puluhan MB) -> panggil dari thread kerja, bukan render loop.
bool run(const Options& o, Result& r);

// Nama-nama kata kunci yang dipakai buat nandain string (biar bisa ditampilin
// di UI / dicatat di README).
const std::vector<std::string>& vectorKeys();

}  // namespace libdump

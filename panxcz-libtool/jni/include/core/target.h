// ============================================================================
//  target.h - game mana + lib il2cpp mana (DIPILIH SAAT RUNTIME)
//
//  Tool-nya universal: bukan cuma Subway/MLBB. Yang nentuin:
//    * package  -> nama paket game (mis. com.mobile.legends)
//    * lib      -> lib yang punya API/metadata il2cpp
//                  MLBB        = liblogic.so
//                  Free Fire   = libil2cpp.so (atau libanogs.so buat anti-cheat)
//                  Unity umum  = libil2cpp.so
//
//  Sumber nilainya, urut prioritas:
//    1. argumen .sh    : panxcz_libtool.sh <package> [lib]
//    2. /data/local/tmp/panxcz_libtool.cfg   (ditulis menu, dibaca tool)
//    3. /sdcard/panxcz/libtool.cfg
//    lib boleh dikosongkan -> auto-detect dari maps proses yang ke-attach
//    (cari lib yang punya "global-metadata" / nama il2cpp / export standar).
// ============================================================================
#pragma once

#include <string>
#include <vector>

namespace target {

struct Proc {
    int         pid = 0;
    std::string pkg;
    int         score = 0;   // seberapa yakin proses ini punya lib il2cpp
};

// daftar proses yang punya nama paket (buat dipilih di menu)
std::vector<Proc> listProcesses();

// Cari SENDIRI game yang lagi jalan: proses dengan skor il2cpp tertinggi
// (skor > 0 = beneran ada lib il2cpp / global-metadata). Nama sub-proses
// dipotong jadi package dasar ("com.x:UnityKillsMe" -> "com.x") supaya masih
// bisa milih pid terbaik sendiri. Kosong = nggak ada game yang kelihatan.
// Dipakai MODE OTOMATIS: `./panxcz_libtool.sh` tanpa argumen apa pun.
std::string autoDetectPackage(int* scoreOut = nullptr);

// ---- konfigurasi ----
bool load();                      // baca cfg (sekali di awal; aman dipanggil ulang)
bool save();                      // tulis cfg ke /data/local/tmp + /sdcard/panxcz
const std::string& package();
const std::string& lib();
// Nama proses yang BENERAN punya lib il2cpp (cmdline lengkap). Unity sering
// naruh engine-nya di proses anak -- MLBB: com.mobile.legends:UnityKillsMe.
// Diisi otomatis setelah attach & disimpan di cfg, jadi jalan berikutnya tool
// langsung nempel ke proses itu (nggak nebak dari proses utama lagi).
const std::string& engine();
void setEngine(const std::string& e);
bool  autoLib();                  // true kalau lib dikosongkan (mode auto)
void  setPackage(const std::string& p);
void  setLib(const std::string& l);   // "" = auto

// ---- dump offline (.cs) ----------------------------------------------------
// Dump ngasih "Class-slot candidates" = RVA cell yang isinya Il2CppClass*.
// Itu satu-satunya jalur yang jalan di game yang metadata-nya dipecah
// (MLBB: liblogic.so + libfirst.so + libcsharp.so) -- lihat core/offlinedump.h.
void  setDump(const std::string& p);          // dari cfg / argumen --dump
const std::string& dump();                    // path yang diminta (boleh kosong)
// Root folder dumper: /sdcard/panxcz-tool/dumper/<pkg>/<versi>/*.cs
// (setDumpDir buat ganti, mis. kalau foldernya bukan di /sdcard)
void        setDumpDir(const std::string& p);
std::string dumpDir();

// Lib anti-cheat/proteksi (libanogs, libanort, libtersafe, libtprt, ...)?
// Ini BUKAN engine il2cpp: nggak pernah dipilih buat scanning kelas, tapi bisa
// dibaca/di-dump buat lihat logic deteksinya (lihat core/libdump.h).
bool isAntiCheatLib(const std::string& name);
// yang DIKETIK user (kosong = belum pernah diganti, alias pakai default)
std::string customDumpDir();
// Cari sendiri file dump di lokasi yang biasa dipakai
// (/data/local/tmp/panxcz_dump.cs, /sdcard/panxcz/dump.cs, ...).
// Balikin "" kalau nggak ada.
std::string findDumpFile();
// Sama, tapi balikin SEMUA file dump yang layak buat package target: biasanya
// satu file, tapi kalau dump-nya kepecah per lib (com.x_full.cs +
// com.x_liblogic.so.cs + com.x_libcsharp.so.cs) semuanya -- nanti digabung
// (offlinedump::loadMany) supaya katalog kelas/method-nya lengkap.
// Dump milik game lain NGGAK pernah ikut (dulu cfg `dump=` yang nunjuk ke game
// lain bikin kelas + RVA-nya ketuker diam-diam).
std::vector<std::string> findDumpFiles();
// PID tertentu (0 = cari sendiri). Perlu karena satu package bisa punya
// beberapa proses dan engine-nya nggak selalu di pid yang pertama ketemu.
void  setSelPid(int pid);
int   selPid();
std::string configPath();         // path yang kepakai ("-" kalau belum ada)

// ---- attach ----
bool attach();                    // cari pid package + buka /proc/pid/mem
bool attached();
int  pid();

// Cari lib il2cpp di proses yang sudah ke-attach:
//   1. nama yang diketik (kalau ada)
//   2. kandidat umum: libil2cpp.so, liblogic.so, libunity.so, libgame.so ...
//   3. lib mana pun yang region-nya membuat "global-metadata" muncul dekat sana
// Balikin nama lib (kosong kalau tidak ketemu).
std::string detectLib(bool force = false);

}  // namespace target

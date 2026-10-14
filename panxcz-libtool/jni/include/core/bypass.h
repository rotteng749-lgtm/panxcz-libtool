// ============================================================================
//  bypass.h - anti-deteksi buat tool EXTERNAL (nggak nyuntik ke game)
//
//  JUJUR DULU: tool ini proses TERPISAH yang cuma baca /proc/<pid>/mem. Karena
//  nggak ada kode kita yang jalan di dalam game, yang bisa dilakukan cuma
//  soal JEJAK DIRI SENDIRI -- bukan nge-patch pengecekan root di dalam game.
//
//  Yang beneran jalan di sini:
//    1. NAMA PROSES disamarkan (prctl PR_SET_NAME + timpa /proc/self/cmdline),
//       jadi kalau game nyisir daftar proses buat nyari nama aneh
//       ("panxcz", "libtool", "frida", "gameguardian") yang keliatan cuma nama
//       netral (default: "mali_render").
//    2. File kerja (config/log/dump) ditulis 0600 dan disimpan di path netral,
//       bukan folder yang namanya "khas cheat".
//    3. PANTAU BALIK: karena kita root, kita bisa baca tabel fd proses game
//       (/proc/<game>/fd). Kalau ADA fd game yang nunjuk ke /proc/<pid-kita>
//       atau ke file kita, artinya game kebukti lagi ngecek kita -> tool lepas
//       overlay + keluar sendiri (atau cuma dicatat, tergantung mode).
//
//  Yang TIDAK bisa (dan nggak diklaim):
//    - nyembunyiin root/Magisk/su dari game (butuh hook di dalam proses game)
//    - nge-block anti-cheat yang jalan di kernel (SECCOMP/overlay scan ROM tertentu)
//    - nyembunyiin /proc/<pid-kita>/maps: itu hak kernel, harus di-mount ulang
//
//  Jadi anggap ini pengurang risiko, bukan jaminan aman.
// ============================================================================
#pragma once

#include <string>

namespace bypass {

// Nama proses samaran (default "mali_render").
std::string procName();
void setProcName(const std::string& n);

// on/off (dibaca dari cfg, bisa diubah dari menu tab Info).
bool& enabled();

// Terapkan: rename proses + rapikan izin file kerja.
// Balikin true kalau minimal rename-nya kejalan.
bool apply();

// Timpa argv (yang dibaca /proc/<pid>/cmdline) supaya `ps` / pemindai proses
// di game nggak lihat path tool. Cuma bisa kalau nama samarannya nggak lebih
// panjang dari argv[0] ("mali_render" vs "/data/local/tmp/panxcz_libtool.sh").
void setTitleFromArgv(int argc, char** argv);

// Cek sekali: apakah proses game nunjuk ke kita (fd/maps)?
//   exitOnHit = true -> minta tool keluar sendiri (panic)
// Balikin true kalau ketemu jejak.
bool scanOnce(bool exitOnHit);

// true = tool minta keluar karena kedeteksi (dicek main loop).
bool panicRequested();
void clearPanic();

// Hasil cek terakhir (buat ditampilin di menu).
const std::string& lastReport();
void setLastReport(const std::string& s);

// dipanggil timer ringan dari loop render: cek tiap ~10 detik
void tick();

}  // namespace bypass

// ============================================================================
//  metapool.h - baca DAFTAR METHOD lengkap dari runtime (tanpa dump, tanpa frida)
//
//  KENAPA MODUL INI ADA
//  --------------------
//  Di MLBB, Il2CppClass TIDAK menyimpan array MethodInfo yang lengkap: array di
//  klass+0x90 cuma terisi sebagian (contoh nyata: BattleManager punya 121 method
//  di metadata, tapi array-nya cuma nunjuk 20). Jadi nge-dump method lewat
//  array kelas = hasilnya bolong-bolong ("RVA: unknown" di mana-mana).
//
//  Yang dipakai MLBB: SEMUA MethodInfo dialokasikan di SATU POOL global, deret
//  rapat, stride 0x50 byte. Isi satu entry (dibaca dari game yang jalan):
//
//      +0x00 methodPointer (alamat kode absolut, mis. 0x7a82702cc8)
//      +0x08 virtualMethodPointer
//      +0x10 Il2CppClass* (pemilik method, ada MTE tag -> harus di-untag)
//      +0x18 invoker / pemetaan lain
//      +0x20 rgctx (0 = nggak ada)
//      +0x28 -> record metadata 0x20 byte; dword pertamanya = INDEKS NAMA
//      +0x30..+0x4F sisa entry
//
//  Nama methodnya sendiri TIDAK ada di struktur itu, tapi diheap string yang di
//  file metadata. Index nama dikodekan sebagai: byte paling atas = NOMOR FILE
//  metadata (1 = global-metadata.dat, 2 = global-first-metadata.dat,
//  3 = global-csharp-metadata.dat), 3 byte bawah = offset nama DI FILE ASLI.
//  Jadi:
//
//      nama = mapping[space].start + (index - delta[space])
//
//  Nilai delta itu konstanta layout file (bukan alamat runtime) -- dan TIDAK
//  di-hardcode di sini: dia DIKALIBRASI waktu jalan. Caranya: ambil beberapa
//  kelas yang method-nya kebaca dari dump offline (nama + RVA), cocokin entry
//  pool-nya lewat RVA, terus index nama yang ketemu dipakai buat nyari delta
//  (hasilnya divote, jadi salah satu sampel nggak bikin kalibrasinya meleset).
//// Kalau game-nya il2cpp biasa (tanpa pool seperti ini), modul ini bilang
// "nggak siap" dan jalur lama (kalibrasi array kelas / dump) tetap dipakai.
//
//  typeId -> KELAS (biar SEMUA record kepakai)
//  -------------------------------------------
//  Record method-def nunjuk tipe PEMILIKnya lewat satu dword (typeId). Peta
//  typeId->kelas awalnya cuma bisa dibangun dari MethodInfo yang udah dibikin
//  engine -- di MLBB nyata cuma ~2.000 tipe padahal tabel recordnya 17.770 tipe
//  (jadi ribuan kelas punya daftar method bolong). Ada TIGA jalur, semuanya
//  "cari sendiri saat jalan" (nggak ada offset yang di-hardcode):
//
//   (a) TOKEN TypeDef di dalam Il2CppClass (byte atas 0x02). Ini yang nyata
//       jalan di MLBB. Indeks record beda tipis dari token, dan geserannya
//       KONSTAN PER FILE METADATA:
//
//           indeks = (token & 0xFFFFFF) + K[space]
//
//       contoh terverifikasi (MLBB 22.1.97.12061, dicocokin ke dump offline):
//         SafeFileHandle (space 1/liblogic.so): token 0x2000067, indeks 0x66
//         kelas space 3 (libcsharp.so)        : token 0x2000e4d, indeks 4032
//
//       Offset token-nya DICARI (bukan di-hardcode): kelas yang typeId-nya udah
//       diketahui dipakai sampel, lalu tiap offset dinilai dari "berapa kelas
//       yang dword-nya bertoken TypeDef DAN selisihnya konsisten per space".
//
//   (b) field "indeks tipe" (nilai = indeks + geseran) -- jalur lama, dipakai
//       kalau game-nya nyimpen indeks mentah (tanpa token).
//
//   (c) TABEL TIPE: array Il2CppClass* yang diindeks indeks tipe (engine pakai
//       ini buat cari kelas dari type id). Tabelnya nggak diekspor, jadi
//       ditemukan dengan VOTE: tiap kelas yang udah dikenal dicari di memori,
//       dan kalau pointer-nya nongol di alamat A, berarti
//
//           base[space] = A - indeks * 8
//
//       Base yang menang (dan menang jelas) = tabelnya. Ini yang bikin tipe
//       yang MethodInfo-nya BELUM dibikin engine ikut kepetakan -- tahan ASLR
//       dan tahan game update karena nggak ada alamat yang ditanam.
// ============================================================================
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "core/explore.h"          // explore::Method

namespace metapool {

struct MethodEnt {
    uint32_t    token = 0;     // token MethodDef dari record metadata (0 = nggak ada)
    uintptr_t   klass = 0;
    uintptr_t   mi    = 0;     // 0 = MethodInfo-nya belum dibikin game (RVA unknown)
    uintptr_t   code  = 0;
    uint64_t    rva   = 0;
    std::string module;
    std::string name;      // kosong = index-nya nggak bisa di-resolve
};

struct Stats {
    size_t     spaces = 0, entries = 0, named = 0, classes = 0;
    size_t     scannedMB = 0, cand = 0, phase = 0;
    // peta typeId -> kelas
    size_t     typeMapped = 0;             // total tipe yang kepetakan ke kelas
    size_t     typeFromTok = 0;            // dari token TypeDef di kelas
    size_t     typeFromTable = 0;          // dari tabel tipe (tipe tanpa MethodInfo)
    size_t     typeRecords = 0;            // tipe yang punya record di metadata
    std::string layout;                    // layout MethodInfo yang kepakai
    std::vector<std::string> spaceInfo;    // 1 baris per space (buat log)
};

// Cari mapping metadata + pool MethodInfo + kalibrasi delta nama.
// Berat (baca memori puluhan MB) -> panggil dari thread, bukan render loop.
bool init();
// batas byte heap yang dipindai buat nyari pool (default 2048 MB)
void setScanCapMB(size_t mb);
// Paksa satu layout MethodInfo (index dari layoutTag()), -1 = otomatis.
// Dipakai `--pool-layout=N` buat diagnosa.
void   setForcedLayout(int idx);
size_t layoutCount();
const char* layoutTag(size_t idx);
bool ready();
const Stats& stats();
const char* lastError();

// Semua method yang berhasil dibaca.
const std::vector<MethodEnt>& all();
// SEMUA Il2CppClass* yang berhasil dipetakan pool -- gabungan kelas yang punya
// MethodInfo runtime (g_byKlass) DAN kelas yang cuma kepetakan lewat typeId/
// token/tabel tipe (g_methodsByKlass). Di MLBB ini ~24 rb kelas padahal
// g_entries cuma nyentuh ~8 rb: sisanya nggak punya MethodInfo runtime tapi
// pointer kelasnya SUDAH ke-resolve, jadi tetep bisa di-dump (nama/field dari
// runtime, method dari dump offline). Dumper pakai ini biar kelasnya lengkap.
const std::vector<uintptr_t>& allClasses();
// Method milik satu kelas (ganti isi `out`; false = kelasnya nggak ada di pool).
bool forKlass(uintptr_t klass, std::vector<explore::Method>& out, size_t max = 6144);

// Nama kelas dari pointer Il2CppClass (dibaca dari runtime, bukan dari dump).
// Dipakai buat nyocokin "Class::Method" pas method-nya dicari lewat pool
// (di MLBB jalur cari-nama-kelas-di-memori nggak jalan, pool yang jalan).
bool klassName(uintptr_t klass, std::string& out);
// Nama dari satu index mentah (dipakai buat diagnosa / tes).
bool nameOfIndex(uint32_t idx, std::string& out);
// Lengkapi method satu kelas dari dump offline: RVA buat method yang
// MethodInfo-nya belum dibikin engine, nama buat yang index-nya nggak kebaca.
// Balikin jumlah method yang dilengkapi.
size_t enrichFromDump(uintptr_t klass, const std::string& kname);
// Kalau `rva` + `module` ketemu di pool: balikin entry-nya (buat cek cepat).
const MethodEnt* byRva(uint64_t rva, const std::string& module);

// ---- typeId -> Il2CppClass* ------------------------------------------------
// Jumlah tipe yang kepetakan ke kelas (sesudah peta lengkap dibangun di init()).
size_t   typeMapSize();
// Offset di dalam Il2CppClass tempat typeId-nya disimpan (0 = nggak kedeteksi).
uint32_t typeOffset();
// Offset di dalam Il2CppClass tempat TOKEN TypeDef-nya disimpan
// (0 = nggak kedeteksi). Ini yang dipakai MLBB (lihat catatan di atas).
uint32_t typeTokenOffset();
// Berapa tipe yang bisa dipetakan LEWAT TABEL TIPE (kelas yang MethodInfo-nya
// belum dibikin engine sekalipun) -- 0 = tabelnya nggak ketemu.
size_t   typeTableSize();
// Diagnosa: tulis histogram offset Il2CppClass yang isinya typeId kelas
// (dipakai buat verifikasi deteksi otomatis di atas).
void     dumpTypeProbe(int samples = 400, int topN = 20);

}  // namespace metapool

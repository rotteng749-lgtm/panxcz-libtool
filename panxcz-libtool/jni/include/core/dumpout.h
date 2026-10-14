// ============================================================================
//  dumpout.h - DUMP SENDIRI, di dalam tool (root doang -- tanpa frida, tanpa PC)
//
//  KENAPA
//  ------
//  Dulu dump cuma bisa dari luar (frida + Python di PC). Padahal tool ini jalan
//  sebagai ROOT dan udah bisa baca /proc/<pid>/mem -- jadi dia bisa nge-dump
//  sendiri, langsung di HP.
//
//  CARANYA (nggak butuh simbol / global-metadata)
//  ----------------------------------------------
//  Di module il2cpp ada tabel cell yang isinya Il2CppClass* ("class-slot" di
//  dump Il2CppDumper). Tabel itu panjang dan berderet rapat, jadi bisa dicari
//  dari bentuknya: pindai region data module-nya, cari qword yang nunjuk ke
//  struktur Il2CppClass (nama kelas printable di +0x10, image *.dll di +0x00),
//  dan ambil yang punya tetangga 8-byte (tanda dia anggota tabel, bukan
//  kebetulan).
//
//  Tiap kelas yang ketemu dibaca:
//      +0x00 image (nama *.dll)   +0x10 nama   +0x18 namespace
//      FieldInfo[]  -> nama, offset, token, tipe
//      MethodInfo** -> nama + signature, RVA, module pemilik kode
//
//  Hasilnya ditulis sebagai .cs format Il2CppDumper -- termasuk baris
//  "// Class-slot candidates" -- jadi tool ini (dan Il2CppDumper, IDA script,
//  dst) bisa baca balik file-nya. Artinya: tool-nya jadi mandiri, nggak
//  kebergantungan dump dari luar lagi.
//
//  JUJUR: field statis TIDAK ditandai di dump ini (metadata runtime nggak
//  nyimpen penandanya). Offset instance + RVA method-nya asli.
// ============================================================================
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace dumpout {

struct Options {
    bool allLibs = false;              // pindai semua module il2cpp (MLBB: 3 lib)
    std::vector<std::string> libs;     // kalau diisi: cuma module ini
    bool ptr    = false;               // tulis alamat runtime tiap method (--ptr)
    bool json   = false;               // tulis .json
    bool header = false;               // tulis .h (tabel siap-patch)
    bool tips   = true;                // tulis .ptrs.tsv kalau --ptr
    size_t maxClasses = 0;             // 0 = semua (--classes N buat tes cepat)
    size_t diag = 0;                   // --diag N: tulis qword mentah N kelas pertama
    bool   noPool = false;             // --no-pool: jangan pakai pool MethodInfo global
    bool   noEnrich = false;           // --no-enrich: jangan lengkapi dari dump offline
    // --no-dump-methods: jangan isi daftar method dari dump offline buat kelas
    // yang pool runtime-nya nggak punya MethodInfo (nyata di MLBB: BattleManager
    // 0 method dari pool, padahal 121 di metadata). Default: diisi.
    bool   noDumpMethods = false;
    // --no-dump-classes: jangan tambah kelas dari KATALOG dump offline. Default:
    // ditambah. Pindai class-slot cuma nemu kelas yang cell-nya udah ke-resolve
    // runtime; di MLBB itu cuma ~5 rb dari ~28 rb kelas -- sisanya ada di katalog
    // dump offline (slot-nya dari sana) dan di sini di-resolve runtime.
    bool   noDumpClasses = false;
    bool   dryRun = false;             // --scan-only: scan & hitung aja (nggak nulis file)

    // kolom yang ditulis (--cols): field,method,slot,token,flags,parent
    // kosong = semua yang didukung
    std::vector<std::string> cols;
    std::string nsFilter;              // --ns Battle   (namespace persis)
    std::string likeFilter;            // --like Data   (nama mengandung)

    std::string outDir;                // kosong = <dumpdir>/<pkg>/<versi>/
    std::string fileTag;               // nama file: <pkg>_<tag>.cs
};

struct Result {
    size_t classes = 0, withSlot = 0, fields = 0, methods = 0, modules = 0;
    size_t candCells = 0, scannedMB = 0;
    bool   fieldLayoutOk = false, methodLayoutOk = false, parentOk = false;
    bool   aborted = false;            // dihentikan dari menu (tombol Stop)
    std::string path, jsonPath, headerPath, ptrsPath, dir;
};

// Jalanin dump (berat -- panggil dari thread). false = gagal (lihat log).
bool run(const Options& o, Result& res);

// Versi package (dari `dumpsys package`) buat nama folder <pkg>/<versi>/.
// "unknown" kalau nggak kebaca. Dipakai dumpout + libdump biar sejajar.
std::string pkgVersion(const std::string& pkg);

// --- Stop ---------------------------------------------------------------
// Pemindaian mengecek flag ini di loop chunk + loop per-kelas, jadi tombol
// Stop nggak nunggu dump selesai.
void requestAbort();
void clearAbort();
bool abortRequested();

}  // namespace dumpout

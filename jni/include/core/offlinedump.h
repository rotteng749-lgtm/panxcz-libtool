// ============================================================================
//  offlinedump.h - baca dump .cs (format Il2CppDumper / panxcz_dump_full.js)
//
//  KENAPA INI ADA
//  --------------
//  Di sebagian game cara "scan nama class -> pointer balik" NGGAK JALAN, dan
//  yang paling parah: MLBB. Di sana metadata-nya dipecah tiga bagian
//  (liblogic.so / libfirst.so / libcsharp.so) dan nama class-nya nggak punya
//  pointer balik ke Il2CppClass -- jadi jalur scan mentok.
//
//  Tapi dump punya baris yang justru menjawab persis masalah itu:
//
//      // Module 3: libcsharp.so / Assembly-CSharp.dll
//      // Namespace: ; TypeDefIndex: 4032; Token: 0x2000e4d
//      // Class-slot candidates: 0x635b290
//      class BattleManager
//      {
//          static BattleManager Instance; // static_fields offset: 0x0; token: 0x4007c74
//          // RVA: 0x2EAF818
//          void Update();
//      }
//
//  "Class-slot candidate" = RVA (relatif base module) dari sebuah cell yang
//  ISINYA Il2CppClass*. Jadi rantainya tinggal:
//
//      libbase + slot  ->  Il2CppClass*  ->  +0xa8 static_fields  ->  +offset
//
//  File-nya besar (MLBB ~36 MB / 28 rb kelas), jadi dibaca SEKALI ke memori
//  terus di-index: lookup kelas jadi O(1) dan nggak perlu scan apa-apa lagi.
//
//  Dump cuma "katalog alamat". Semua isi cell-nya tetap divalidasi runtime
//  (nama kelas di +0x10 harus sama) -- kalau game update dan slotnya geser,
//  yang kejadian cuma "kandidat ditolak", bukan crash.
// ============================================================================
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace dump {

// satu kandidat cell: "<module> 0x<rva>" (module kosong = pakai module baris
// "// Module" di atasnya, itu format dump offline yang polos).
struct Slot {
    std::string module;
    uint64_t    rva = 0;
};

// Jenis deklarasi di dump (kwitansi: kelas biasa / struct / enum / interface).
// Il2CppDumper nulis salah satu dari empat keyword ini; kalau kita nggak
// nyimpen, dump ulang kita cuma bisa nulis "class" buat semuanya.
enum class Kind : uint8_t { Class = 0, Struct, Enum, Interface };

struct ClassEntry {
    std::string name;        // nama kelas (pake backtick buat generic: List`1)
    std::string ns;          // namespace (boleh kosong)
    std::string assembly;    // *.dll pemiliknya
    std::string module;      // lib .so yang di-dump (liblogic.so / libcsharp.so)
    uint32_t    typeDefIndex = 0;
    uint64_t    token = 0;
    Kind        kind = Kind::Class;
    std::vector<Slot> slots; // kandidat cell Il2CppClass*
    size_t      bodyOff = 0; // offset awal blok tubuh kelas di buffer
    // Kualitas tubuh kelas ini buat "RVA method": dump hasil tool lain suka
    // nulis `// RVA: unknown (null_entry)` (metadata-nya nggak bisa di-resolve
    // offline), dump kita nulis `// RVA: 0x...`. Kalau satu kelas ada di dua
    // file, yang dipakai yang RVA-nya nyata -- kalau nggak, kelas yang punya
    // RVA asli ketutup body yang "unknown" cuma karena filenya lebih dulu.
    uint32_t    rvaReal    = 0;   // jumlah baris "// RVA: 0x..." di tubuhnya
    uint32_t    rvaUnknown = 0;   // jumlah baris "// RVA: unknown"/kosong
};

struct Field {
    std::string name;
    std::string type;        // tipe tertulis di dump (mis. "System.Int32")
    uint32_t    offset = 0;  // relatif instance, atau relatif static_fields
    uint32_t    token  = 0;
    bool        isStatic = false;
};

struct Method {
    std::string name;        // nama + parameter (apa adanya dari dump)
    uint64_t    rva = 0;
    bool        hasRva = false;   // false = "unknown (null_entry)" / abstract
    // Module pemilik kode method ini kalau dump-nya nulis tag di belakang RVA:
    //     // RVA: 0x1219718 (liblogic.so)
    // Kosong = RVA-nya relatif module baris "// Module" di atas kelas itu
    // (dump offline lama / dump kita yang v3.3). Di MLBB method satu kelas bisa
    // nyebar ke beberapa lib, jadi ini yang bikin alamat absolutnya ketemu.
    std::string module;
};

struct ClassDetail {
    std::vector<Field>  fields;
    std::vector<Method> methods;
};

// ---- lifecycle -------------------------------------------------------------
// Baca + index file dump. Berat sekali saja (36 MB) -- panggil dari thread.
bool load(const char* path);
// Baca BEBERAPA dump sekaligus dan GABUNG jadi satu katalog (kelasnya digabung,
// duplikat dibuang). Dipakai buat dump yang kepecah per lib
// (com.x_full.cs + com.x_liblogic.so.cs + com.x_libcsharp.so.cs): satu file
// aja nggak lengkap, gabungannya lengkap. Hanya file dari versi game yang sama
// yang layak digabung (RVA-nya beda kalau versinya beda) -- itu diurus
// target::findDumpFiles().
bool loadMany(const std::vector<std::string>& paths);
// Cari sendiri file dump-nya (cfg `dump=`, terus lokasi standar di
// /data/local/tmp & /sdcard) baru load. Dipakai pas boot + tombol di menu.
bool loadAuto(std::string* triedPath = nullptr);
void unload();
bool loaded();
const std::string& path();          // "file1.cs +2" kalau digabung
const std::vector<std::string>& files();   // semua file yang ikut kebaca

// ---- statistik (buat ditampilin di menu / log) ------------------------------
size_t classCount();
size_t slotCount();                 // jumlah kelas yang punya kandidat slot
size_t moduleCount();
std::string moduleSummary();        // "libcsharp.so -> Assembly-CSharp.dll (20003)"
const std::vector<ClassEntry>& classes();

// ---- lookup -----------------------------------------------------------------
// Cari kelas berdasarkan nama (dan namespace kalau diisi). Cocok PERSIS, dan
// kalau nggak ada baru coba longgar (buang namespace / suffix generic).
bool find(const char* name, const char* ns, std::vector<const ClassEntry*>& out);

// Parse blok tubuh kelas (field + method) dari buffer yang udah dibaca.
bool detail(const ClassEntry& e, ClassDetail& out);

// Tebak Il2CppTypeEnum dari tipe tertulis di dump (dipakai buat pilih tipe
// baca di menu; 0xFF = nggak dikenal).
uint8_t guessTypeEnum(const std::string& type);

}  // namespace dump

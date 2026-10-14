// ============================================================================
//  mem.h - layer akses memori proses lain (butuh root)
//  Semua pembacaan/penulisan ke proses game lewat /proc/<pid>/mem.
// ============================================================================
#pragma once

#include <cstdint>
#include <cstddef>
#include <string>
#include <vector>

namespace mem {

// Pointer yang disimpan di memori game bisa bawa MTE tag di byte paling atas
// (contoh nyata MLBB: Il2CppClass* disimpan sebagai 0xb400007a967549d0).
// Alamat aslinya = tag-nya dibuang. Semua fungsi di bawah nge-normalisasi
// alamat sendiri, jadi pemanggil nggak perlu mikirin tag.
inline uintptr_t untag(uintptr_t p) {
    return (p >> 56) ? (p & 0x00FFFFFFFFFFFFFFull) : p;
}

struct Region {
    uintptr_t   start = 0;
    uintptr_t   end   = 0;
    uint64_t    fileOff = 0;
    char        perms[5] = {0, 0, 0, 0, 0};
    std::string name;

    bool   readable() const { return perms[0] == 'r'; }
    bool   writable() const { return perms[1] == 'w'; }
    bool   exec()     const { return perms[2] == 'x'; }
    size_t size()     const { return (size_t)(end - start); }
};

// ---- lifecycle -------------------------------------------------------------
bool attach(const char* package);   // cari pid + parse maps + buka /proc/pid/mem
bool attachPid(int pid, const char* label = nullptr);  // langsung ke pid tertentu
void detach();
bool attached();
int  pid();
// nama proses yang lagi di-attach (dari /proc/<pid>/cmdline) -- MLBB misalnya
// engine-nya ada di "com.mobile.legends:UnityKillsMe", bukan proses utamanya.
std::string procName();
bool refresh();                     // parse ulang maps

// Skor "seberapa mirip proses ini punya engine il2cpp", dibaca dari
// /proc/<pid>/maps. Dipakai buat milih proses kalau satu package punya
// beberapa pid (Unity sering naruh engine-nya di proses anak, contoh MLBB:
// com.mobile.legends:UnityKillsMe). <0 = maps nggak kebaca.
int  il2cppScore(int pid);

// ---- info ------------------------------------------------------------------
const std::vector<Region>& regions();
uintptr_t base(const char* moduleName);        // contoh: "libil2cpp.so"
std::string path(const char* moduleName);
const Region* regionOf(uintptr_t addr);
bool canRead(uintptr_t addr, size_t n);
bool canWrite(uintptr_t addr, size_t n);

// ---- RVA yang STABIL antar run ---------------------------------------------
// RVA = file offset segmen + jarak di dalam segmen.
//
// KENAPA bukan (alamat - mem::base(modul)): buat modul yang segmennya dimap
// TERPISAH (nyata di Free Fire -- libil2cpp.so-nya ~1,6 GB dan kepecah jadi
// beberapa blok yang masing-masing dapat alamat acak sendiri), "base" jadi
// nggak karuan: di satu run rva method bisa 0xE7946260, di run lain 0x1B79FED0
// untuk method yang SAMA. Akibatnya dump-nya nggak bisa dipakai buat nge-patch
// lagi (rva-nya cuma valid di run itu). Kalau mapping-nya nyambung (MLBB), hasil
// rvaOf() sama persis dengan cara lama -- jadi ini kompatibel.
uint64_t  rvaOf(uintptr_t addr);        // alamat runtime -> rva (0 = nggak ketemu)
// Kebalikannya: rva -> alamat runtime (0 = region-nya nggak ada di proses ini).
uintptr_t addrOfRva(uint64_t rva);
// SEMUA tafsir alamat buat satu rva. Modul dengan segmen terpisah bisa punya
// lebih dari satu region yang mencakup file offset itu (nyata di FF: satu
// region r--p nutup 177 MB pertama, jadi rva kode di rentang itu kena dua
// region sekaligus). Urutannya: segmen KODE dulu, terus writable, baru sisanya --
// jadi addrOfRva() (yang ambil yang pertama) kepakai buat alamat kode.
void addrOfRvaAll(uint64_t rva, std::vector<uintptr_t>& out, size_t maxOut = 8);

// Tafsir alamat DIBATASI ke satu modul (nama-nya dicek sebagai akhiran path,
// sama kayak mem::base()). addrOfRva() nyari di SEMUA region, jadi nomor rva
// yang sama bisa nyangkut ke segmen kode lib LAIN yang file offset-nya kebetulan
// nutup rentang itu -- kejadian nyata: peek libanogs.so:0x50A9E8 dibalas alamat
// punya lib lain.
uintptr_t addrOfRvaIn(const char* moduleName, uint64_t rva);
// Tafsir VADDR (nilai relatif modul yang belum ditambah bias). Dipakai buat lib
// yang di-load loader SENDIRI ala packer: nilai di PT_DYNAMIC dan r_offset
// relokasinya masih vaddr mentah (nyata di libanogs/libanort Free Fire).
uintptr_t addrOfVaddrIn(const char* moduleName, uint64_t vaddr);

// ---- akses -----------------------------------------------------------------
// Blok besar (>=64 KB) dibaca secara LONGGAR: kalau kernel nolak blok besar,
// permintaannya dipecah sampai 64 KB dan bagian yang nggak kebaca di-nol-kan
// (jadi satu halaman nakal nggak bikin satu chunk pindai kelewat). Baca kecil
// tetap KETAT: gagal = false, nggak ada data nol yang nyamar jadi nilai sah.
bool read (uintptr_t addr, void* out, size_t n);
bool write(uintptr_t addr, const void* in, size_t n);

// berapa kali baca memori gagal (buat diagnosa -- dipakai [check] di log)
size_t readFailures();
void   resetReadFailures();
// Halaman-jelek yang udah ketahuan (guard page / lubang). readFailures()
// ngitungnya SEKALI per halaman -- pindaian ulang halaman yang sama nggak
// nambah angka lagi (dulu tiap pindaian ngitung ulang -> puluhan ribu).
size_t badPageCount();

// Self-check lingkungan: root/izin, mode /proc/pid/mem, jumlah region,
// apakah baca blok 4 MB jalan atau harus dipecah. Semua dicetak ke log dengan
// VERDICT -- dipakai kalau dump gagal di device lain (tanpa perlu nebak).
bool diag(const char* tag = nullptr);

template <class T> bool rd(uintptr_t addr, T& v)        { return read(addr, &v, sizeof(T)); }
template <class T> bool wr(uintptr_t addr, const T& v)  { return write(addr, &v, sizeof(T)); }
template <class T> T    rdp(uintptr_t addr, T def = T()) {
    T v = def;
    return read(addr, &v, sizeof(T)) ? v : def;
}

// ---- pencarian -------------------------------------------------------------
struct PtrHit { uintptr_t addr; uint64_t value; };

// cari string persis (NUL-terminated) di memori proses
uintptr_t findString(const char* s);
// semua kemunculan string (region writable dipindai lebih dulu)
void findStringsAll(const char* s, std::vector<uintptr_t>& out,
                    size_t maxHits = 64, bool writableOnly = false);
// cari string hanya di dalam rentang alamat tertentu (buat cache region metadata)
void findStringsInRange(uintptr_t start, uintptr_t end, const char* s,
                        std::vector<uintptr_t>& out, size_t maxHits = 64);
// cari string hanya di region yang namanya mengandung substring (mis. "libil2cpp.so")
void findStringsInModule(const char* nameSubstr, const char* s,
                         std::vector<uintptr_t>& out, size_t maxHits = 64);
// cari pointer ke salah satu nilai dalam SATU pass (hemat waktu)
void scanQwordSet(const uint64_t* vals, size_t n, std::vector<PtrHit>& out,
                  size_t maxHits = 16384, bool writableOnly = true);
// cari pola hex, wildcard "??" / "?" per-byte: "48 8B ?? E0"
uintptr_t findPattern(uintptr_t start, uintptr_t end, const char* sig);
uintptr_t findPatternIn(const Region& r, const char* sig);
// cari 8 byte value (dipakai buat nyari instance lewat pointer kelas)
void scanQword(uint64_t value, std::vector<uintptr_t>& out,
               size_t maxHits, bool writableOnly = true);
// versi yang byte-nya boleh ditutup mask. Dipakai buat nyari pointer balik ke
// string metadata di device ber-MTE (pointer-nya bawa tag di byte paling atas).
void scanQwordMasked(uint64_t value, uint64_t mask, std::vector<uintptr_t>& out,
                     size_t maxHits, bool writableOnly = true);
// cari 4 byte value
void scanDword(uint32_t value, std::vector<uintptr_t>& out, size_t maxHits);

// total byte yang readable / writable
// (untuk diagnosa)
size_t totalBytes(bool writableOnly);
void   setScanBudget(size_t bytes);
size_t scanBudget();

}  // namespace mem

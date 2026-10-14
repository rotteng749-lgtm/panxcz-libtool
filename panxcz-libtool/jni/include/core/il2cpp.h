// ============================================================================
//  il2cpp.h - resolver metadata runtime IL2CPP
//
//  Strategi:
//    1. nama class dicari sebagai string di memori (global-metadata / rodata)
//    2. semua pointer ke string itu discan -> kandidat Il2CppClass
//    3. tiap kandidat divalidasi (name / namespaze / image masuk akal)
//    4. instance dicari dengan scan pointer ke Il2CppClass (header object)
// ============================================================================
#pragma once

#include <cstdint>
#include <cstddef>
#include <string>
#include <vector>

#include "core/mem.h"

namespace il2 {

// layout Il2CppClass (stabil di il2cpp 24 - 31)
constexpr uint32_t kKlass_Image     = 0x00;
constexpr uint32_t kKlass_Name      = 0x10;
constexpr uint32_t kKlass_Namespace = 0x18;

bool      init();
// nama lib il2cpp yang lagi dipakai (di-set dari target::detectLib())
const std::string& libName();
uintptr_t base();
uintptr_t addr(uint32_t rva);
bool      ready();

bool        readStr(uintptr_t p, char* out, size_t n);
bool        isValidKlass(uintptr_t k);
const char* klassName(uintptr_t k);        // buffer statis (aman 1 thread)
const char* klassNamespace(uintptr_t k);   // buffer statis
// nama class dari sebuah object instance (baca pointer class di field +0x0)
const char* objClassName(uintptr_t obj);

void        findAllKlass(const char* name, std::vector<uintptr_t>& out, size_t maxHits = 32);
uintptr_t   findKlass(const char* name, const char* ns = nullptr);

// Jalur cadangan: resolusi kelas lewat tabel class-slot dari DUMP OFFLINE
// (libbase + slot -> Il2CppClass*). Ini yang bikin game yang metadata-nya
// dipecah (MLBB: liblogic.so/libfirst.so/libcsharp.so) tetap kebaca, karena di
// sana nama kelas nggak punya pointer balik -- jadi scan string mentok.
// Balikin 0 kalau dump belum dimuat atau slot-nya belum di-resolve runtime.
uintptr_t   findKlassDump(const char* name, const char* ns = nullptr, bool quiet = false);
// Dipakai dumper waktu nge-resolve ribuan kelas dari katalog dump sekaligus:
// sukses per-kelas jangan ditulis ke log (logcat kebanjiran). Balikin ke false
// setelah selesai.
void        setDumpBulkQuiet(bool q);
// "scan" (nama class ketemu di memori) atau "dump" (tabel class-slot) --
// di-set oleh findKlass/findKlassDump, buat ditampilin di menu.
const char* lastKlassSource();

void        findInstances(uintptr_t klass, std::vector<uintptr_t>& out, size_t maxHits = 64);
uintptr_t   findInstance(uintptr_t klass, size_t maxHits = 64);

// ---- versi batch: satu pass untuk banyak class sekaligus ----
struct BatchTarget { const char* name; const char* ns; };

// hasil disimpan di cache, jadi panggilan berikutnya instan
void findKlassBatch(const BatchTarget* targets, size_t n, uintptr_t* klassOut);
void findInstancesBatch(const uintptr_t* klasses, size_t n,
                        std::vector<std::vector<uintptr_t>>& out, size_t perMax = 32);
void cacheClear();

// ---- offset field dari metadata (bikin tool tahan game update) ----
//
// Il2CppClass menyimpan pointer ke array FieldInfo. Tiap FieldInfo memuat nama
// field dan offset-nya. Jadi offset TIDAK perlu dari dump -- bisa dibaca dari
// memori game yang sedang jalan, artinya walau game update (dan dump lama),
// offset tetap bisa ditemukan sendiri.
//
// Offset 'fields' di dalam Il2CppClass dan ukuran FieldInfo beda-beda antar
// versi Unity, jadi keduanya di-kalibrasi dulu pakai class RunSessionData yang
// nama fieldnya sudah kita ketahui dari dump (Points=0x50, Coins=0x58).
//
// Balikin true kalau kalibrasi berhasil (hasil ditulis ke kalOff/kalStride).
bool  fieldReaderInit(uintptr_t klassRunSessionData, uint32_t* kalOff = nullptr,
                      uint32_t* kalStride = nullptr);

// offset sebuah field (0 kalau tidak ketemu). Butuh fieldReaderInit() sukses.
uint32_t fieldOffset(uintptr_t klass, const char* fieldName);

// tulis semua nama+offset field sebuah class ke log (buat diagnosa)
void  logClassFields(uintptr_t klass, const char* tag);

// ---- field access ----
uintptr_t getPtr(uintptr_t inst, uint32_t off);

template <class T>
bool get(uintptr_t inst, uint32_t off, T& out) {
    return inst && mem::read(inst + off, &out, sizeof(T));
}

template <class T>
bool set(uintptr_t inst, uint32_t off, const T& v) {
    return inst && mem::write(inst + off, &v, sizeof(T));
}

}  // namespace il2

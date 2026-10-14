// ============================================================================
//  explore.h - explorer class il2cpp yang UMUM (tanpa dump / offsets hardcode)
//
//  Cara kerja: nama class dicari di memori (persis kayak yang dipakai tool-nya
//  kirain: string -> pointer balik -> kandidat Il2CppClass), terus layout
//  metadata-nya DIKALIBRASI SENDIRI:
//
//    * array FieldInfo di dalam Il2CppClass  -> offset field
//    * array MethodInfo** di dalam Il2CppClass -> RVA method
//
//  Jadi nggak ada offsets.h hasil dump: game update pun tetap kebaca, selama
//  layout il2cpp-nya masih versi yang sama (24 - 31).
//
//  Ini yang bikin tool-nya universal: lu tinggal ketik nama class-nya
//  (mis. RunSessionData / PlayerData / Hero), nggak peduli game-nya apa.
// ============================================================================
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace explore {

enum class FType { Ptr, Int32, UInt32, Int64, Float, Double, Bool, String };

// Il2CppTypeEnum (il2cpp-class.h): 0x02 bool, 0x08 i4, 0x09 u4, 0x0A i8,
// 0x0C r4, 0x0D r8, 0x0E string, 0x0F ptr, 0x11 class

const char* typeName(FType t);

struct Field {
    std::string name;
    uint32_t    offset = 0;
    uint32_t    token  = 0;
    // Il2CppTypeEnum dari metadata (0 kalau nggak kebaca) -- dipakai buat
    // nebak tipe field-nya (int/float/ptr/string) di menu.
    uint8_t     typeEnum = 0;
    // true = offset relatif ke static_fields (bukan ke instance object).
    // Cuma bisa diisi dari dump offline -- metadata runtime nggak nyimpen
    // penanda ini di tempat yang gampang dibaca.
    bool        isStatic = false;
    std::string typeName;      // tipe tertulis di dump (buat ditampilin)
};

// Il2CppTypeEnum -> tipe yang dipakai menu
FType guessType(uint8_t il2cppTypeEnum);

struct Method {
    std::string name;
    uintptr_t   ptr = 0;      // alamat absolut di proses game
    uint64_t    rva = 0;      // ptr - base lib il2cpp
    // module pemilik kode RVA itu (MLBB: RVA-nya bisa di liblogic.so ATAU
    // libcsharp.so -- RVA-nya relatif module masing-masing, bukan satu base).
    std::string module;
};

struct KlassInfo {
    bool        ok   = false;
    uintptr_t   addr = 0;
    std::string name;
    std::string ns;
    std::string image;               // assembly (*.dll)
    std::vector<Field>  fields;
    std::vector<Method> methods;

    // hasil kalibrasi (buat diagnosa kalau layout-nya beda)
    uint32_t fieldsPtrOff  = 0;
    uint32_t fieldStride   = 0;
    uint32_t fieldOffSlot  = 0;
    uint32_t methodsPtrOff = 0;
    uint32_t methodNameOff = 0;

    // "scan"  = ketemu dari nama class di memori
    // "dump"  = ketemu dari tabel class-slot dump offline
    std::string source = "scan";
    // field/method-nya dari dump (runtime-nya nggak bisa dibaca)
    bool fromDump = false;
    // static_fields Il2CppClass (0 = belum/nggak ketemu). Field isStatic
    // offset-nya dihitung dari sini.
    uintptr_t statics = 0;
    uint32_t  staticsPtrOff = 0;
    std::string dumpModule;          // module dump si kelas (libcsharp.so, ...)
};

KlassInfo& info();

// ---- layout runtime (dipakai buat DUMP SENDIRI, lihat core/dumpout.h) ------
//
// Kalibrasi array FieldInfo / MethodInfo di dalam Il2CppClass. Ketemu sekali,
// kepakai buat SEMUA kelas (layout-nya seragam), jadi cukup dikalibrasi di
// 1 kelas yang field/method-nya banyak.
// Layout lengkap: selain "di mana array-nya", disimpan juga posisi tiap
// bagian DI DALAM entry -- game seperti MLBB nulis FieldInfo-nya beda dari
// il2cpp standar (token SETELAH offset, plus pointer balik ke kelasnya),
// jadi offset-nya nggak bisa di-hardcode.
constexpr uint32_t kNoOff = 0xFFFFFFFFu;

struct FieldLayout {
    bool ok = false;
    uint32_t ptrOff = 0, stride = 0;
    uint32_t nameOff = 0;        // const char* nama field
    uint32_t typeOff = 8;        // Il2CppType* (kNoOff = nggak ada)
    uint32_t offSlot = 0;        // uint32 offset instance
    uint32_t tokenSlot = 0;      // uint32 token 0x04xxxxxx (0 = nggak ada)
    uint32_t ownerOff = kNoOff;  // pointer balik ke Il2CppClass
    int      score = 0;
    bool     structural = false; // ketemu dari bentuk struktur, bukan tebakan

    // ---- array offset terpisah (il2cpp baru: Free Fire, CODM, Unity 2020+) ----
    // Di il2cpp baru, `FieldInfo.offset` NGGAK selalu keisi: sebagian kelas
    // nyimpen offset field-nya di ARRAY terpisah per kelas yang ditunjuk dari
    // dalam Il2CppClass (dulu kejadian di FF: 19% field offset-nya kebaca 0x0).
    // Kalau array ini ketemu (dikalibrasi + divoting dari banyak kelas), offset
    // yang 0 diisi dari situ -- dan yang udah keisi tetap dicocokin.
    uint32_t offArrayPtrOff = kNoOff;   // offset di Il2CppClass -> uint32[]/uint64[]
    uint32_t offArrayElem   = 4;        // lebar satu elemen (4 atau 8 byte)
    int      offArrayClasses = 0;       // berapa kelas yang setuju (skor)
};

struct MethodLayout {
    bool ok = false;
    uint32_t ptrOff = 0, stride = 8;
    uint32_t nameOff = 0;
    uint32_t klassOff = kNoOff;  // pointer balik ke Il2CppClass
    uint32_t codeOff = kNoOff;   // methodPointer di dalam entry (kNoOff = pakai methodCode())
    int      score = 0;
    bool     structural = false;
};

bool calibrateFieldLayout(uintptr_t klass, FieldLayout& out);
bool calibrateMethodLayout(uintptr_t klass, MethodLayout& out);

// Kalibrasi array offset field TERPISAH (lihat catatan FieldLayout). Dipanggil
// setelah calibrateFieldLayout, pakai daftar kelas sampel: tiap kelas yang
// `FieldInfo.offset`-nya keisi dipakai buat nge-tes kandidat -- array yang benar
// harganya HARUS sama dengan offset yang udah kebaca itu. Balikin true kalau
// ketemu (hasilnya ditulis ke `io`).
bool calibrateFieldOffsetArray(const std::vector<uintptr_t>& klasses, FieldLayout& io,
                               size_t maxSamples = 60);
// Diagnosa buat kasus array-nya nggak ketemu/ragu: cetak qword Il2CppClass +
// kandidat array offset beserta buktinya ke log. Dipakai dari `--foffprobe`.
void diagFieldOffsets(uintptr_t klass, int topCand = 8);

// Diagnosa: cari array FieldInfo dengan syarat longgar (buat game yang
// struct-nya dimodif). Hasilnya ditulis ke log (topN kandidat + contoh entry).
void huntFieldArray(uintptr_t klass, int topN = 3);
// Diagnosa lengkap: layout tiap array di dalam Il2CppClass (nama/kelas/kode/token)
void huntClass(uintptr_t klass, int topN = 6);
// Diagnosa: struktur yang ditunjuk qword-qword di dalam Il2CppClass
void dumpTargets(uintptr_t klass, int topN = 14);
// Diagnosa: ringkasan tiap array (nama/kelas/kode per stride)
void dumpArrays(uintptr_t klass, int topN = 16);
// Diagnosa: cari tabel method (MethodInfo* / MethodInfo inline) di dalam class
void huntMethodTable(uintptr_t klass, int topN = 6);
// Diagnosa: lacak MethodInfo satu method (RVA-nya diambil dari dump yang
// dimuat) + cari tabel yang nunjuk ke situ. Buat nentuin layout MethodInfo di
// game yang beda (mis. MLBB).
void huntMethodInfo(const char* klassName, const char* methodName);
// Diagnosa: isi MethodInfo yang ditunjuk tabel accessor kelas (klass+0x88)
void dumpPropMethodInfos(uintptr_t klass, int topN = 4);
// Diagnosa SATU method: hexdump MethodInfo-nya (0x80 byte) + tafsir tiap field,
// plus di mana string nama method-nya dipegang. Dipakai buat nentuin layout
// MethodInfo game yang metadata runtime-nya beda (MLBB).
void probeMethodInfo(const char* klassName, const char* methodName);

// Baca field/method pakai layout di atas (nggak nge-kalibrasi ulang).
size_t readFields(uintptr_t klass, const FieldLayout& L, std::vector<Field>& out,
                  size_t max = 512);
// withSignature = true -> nama method ditulis lengkap ("void Update()"),
// dipakai dumper; false -> nama polos (tampilan menu).
size_t readMethods(uintptr_t klass, const MethodLayout& L, std::vector<Method>& out,
                   size_t max = 2048, bool withSignature = false);

// Alamat kode sebuah MethodInfo: pointer-nya ada di +0x00 atau +0x08 (beda
// antar versi il2cpp). Yang dipakai yang jatuh di segmen 'x' sebuah module,
// terus RVA + nama module-nya dihitung dari situ.
bool methodCode(uintptr_t methodInfo, uintptr_t& ptrOut, std::string& moduleOut,
                uint64_t& rvaOut);

// Nama tipe runtime dari pointer Il2CppType (dipakai buat nulis signature).
std::string typeNameOf(uintptr_t il2cppType, int depth = 0);

// Cari class + kalibrasi fields/methods. Panggil dari thread (berat).
bool resolve(const char* name, const char* ns = nullptr);

// Instance hidup dari class yang lagi kepilih.
std::vector<uintptr_t> instances(size_t max = 64);

// field value
bool readValue(uintptr_t inst, uint32_t off, FType t, double& num, char* str, size_t strCap);
bool writeValue(uintptr_t inst, uint32_t off, FType t, double num, const char* str);

// dump class yang lagi kepilih ke <dir>/<Class>.txt (+ .cs kalau minta)
bool dumpTo(const char* dir, std::string* pathOut = nullptr);

}  // namespace explore

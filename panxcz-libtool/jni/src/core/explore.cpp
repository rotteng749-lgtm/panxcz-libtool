#include "core/explore.h"

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <sys/stat.h>

#include "core/abort.h"
#include "core/il2cpp.h"
#include "core/log.h"
#include "core/mem.h"
#include "core/offlinedump.h"
#include "core/metapool.h"

namespace explore {

static KlassInfo g_info;

KlassInfo& info() { return g_info; }

FType guessType(uint8_t e) {
    switch (e) {
        case 0x02: return FType::Bool;
        case 0x03: return FType::Int32;   // char
        case 0x04: return FType::Int32;   // i1
        case 0x05: return FType::UInt32;  // u1
        case 0x06: return FType::Int32;   // i2
        case 0x07: return FType::UInt32;  // u2
        case 0x08: return FType::Int32;   // i4
        case 0x09: return FType::UInt32;  // u4
        case 0x0A: return FType::Int64;   // i8
        case 0x0B: return FType::Int64;   // u8
        case 0x0C: return FType::Float;   // r4
        case 0x0D: return FType::Double;  // r8
        case 0x0E: return FType::String;
        case 0x0F: return FType::Ptr;     // ptr
        case 0x11: return FType::Ptr;     // class
        case 0x18: return FType::Int64;   // System.IntPtr
        case 0x19: return FType::Int64;   // System.UIntPtr
        case 0x1Du: return FType::Ptr;    // array -> pointer data
        default:   return FType::Int32;
    }
}

const char* typeName(FType t) {
    switch (t) {
        case FType::Ptr:    return "ptr";
        case FType::Int32:  return "int";
        case FType::UInt32: return "uint";
        case FType::Int64:  return "long";
        case FType::Float:  return "float";
        case FType::Double: return "double";
        case FType::Bool:   return "bool";
        case FType::String: return "string";
    }
    return "?";
}

// ---------------------------------------------------------------------------
// validator nama
// ---------------------------------------------------------------------------
static bool validName(uintptr_t p, size_t maxLen = 80) {
    if (!p || !mem::canRead(p, 2)) return false;
    char buf[96];
    size_t n = maxLen < sizeof(buf) - 1 ? maxLen : sizeof(buf) - 1;
    if (!mem::read(p, buf, n)) return false;
    size_t len = 0;
    while (len < n && buf[len]) {
        unsigned char c = (unsigned char)buf[len];
        if (c < 0x20 || c > 0x7e) return false;
        ++len;
    }
    if (len == 0 || len >= n) return false;       // harus NUL-terminated & nggak kosong
    // nama C# wajar: huruf/angka/_ , diawali huruf/_ , ada minimal 1 huruf
    bool hasAlpha = false;
    for (size_t i = 0; i < len; ++i) {
        char c = buf[i];
        if (isalpha((unsigned char)c)) hasAlpha = true;
        else if (!isdigit((unsigned char)c) && c != '_' && c != '<' && c != '>' && c != '`' && c != '.' && c != '$')
            return false;
    }
    return hasAlpha;
}

// ---------------------------------------------------------------------------
// kalibrasi array FieldInfo
//
// Il2CppClass punya pointer ke FieldInfo[]. Layout FieldInfo (64-bit):
//     +0x00 const char* name
//     +0x08 const Il2CppType* type
//     +0x10 uint32 token          <- token field selalu 0x04xxxxxx
//     +0x14 uint32 offset
// Versi Unity yang lebih baru menggeser stride-nya (0x20), jadi semua kombinasi
// (offset pointer, stride, posisi slot) dicoba, terus dipilih yang skornya
// paling tinggi (jumlah entry berurutan yang lolos validasi).
// ---------------------------------------------------------------------------
struct Calib {
    uint32_t ptrOff = 0, stride = 0, offSlot = 0;
    int score = 0;
};

static int scoreFieldArray(uintptr_t klass, uintptr_t arr, uint32_t stride, uint32_t offSlot) {
    int ok = 0;
    for (int i = 0; i < 24; ++i) {
        uintptr_t base = arr + (uintptr_t)i * stride;
        if (!mem::canRead(base, 0x18)) break;

        uintptr_t namePtr = 0;
        if (!mem::rd(base, namePtr)) break;
        if (!validName(namePtr)) break;

        uint32_t token = 0;
        if (offSlot >= 4) {
            if (!mem::rd(base + (offSlot - 4), token)) break;
            if ((token >> 24) != 0x04) break;      // FIELD token
        }
        uint32_t off = 0;
        if (!mem::rd(base + offSlot, off)) break;
        if (off > 0x40000) break;                  // offset instance wajar
        ++ok;
    }
    (void)klass;
    return ok;
}

static bool calibrateFields(uintptr_t klass, Calib& best) {
    static const uint32_t strides[] = {0x18, 0x20, 0x10, 0x28, 0x30};
    static const uint32_t slots[]   = {0x14, 0x18, 0x1C, 0x10};

    best = Calib{};
    for (uint32_t off = 0x20; off <= 0x140; off += 8) {
        uintptr_t arr = 0;
        if (!mem::rd(klass + off, arr)) continue;
        if (!arr || !mem::canRead(arr, 0x18)) continue;
        for (uint32_t st : strides) {
            for (uint32_t sl : slots) {
                if (sl >= st) continue;             // slot harus di dalam entry
                int s = scoreFieldArray(klass, arr, st, sl);
                if (s > best.score) {
                    best.ptrOff = off;
                    best.stride = st;
                    best.offSlot = sl;
                    best.score = s;
                }
            }
        }
    }
    return best.score >= 2;
}

// ---------------------------------------------------------------------------
// kalibrasi array MethodInfo**
//   MethodInfo (64-bit, il2cpp 24+):
//      +0x00 methodPointer
//      +0x08 (virtualMethodPointer / invoker, tergantung versi)
//      +0x10 const char* name        <- versi lama
//      +0x18 const char* name        <- versi baru
//   'klass' pointer ada tepat 8 byte sesudah nama, dan itu yang dipakai buat
//   mastiin entry-nya beneran punya class ini.
// ---------------------------------------------------------------------------
static int scoreMethodArray(uintptr_t klass, uintptr_t arr, uint32_t nameOff) {
    int ok = 0;
    for (int i = 0; i < 24; ++i) {
        uintptr_t m = 0;
        if (!mem::rd(arr + (uintptr_t)i * 8, m)) break;
        if (!m || !mem::canRead(m, nameOff + 0x18)) break;

        uintptr_t namePtr = 0;
        if (!mem::rd(m + nameOff, namePtr)) break;
        if (!validName(namePtr, 96)) break;

        uintptr_t klassPtr = 0;
        if (!mem::rd(m + nameOff + 8, klassPtr)) break;
        if (klassPtr != klass) break;
        ++ok;
    }
    return ok;
}

static bool calibrateMethods(uintptr_t klass, uint32_t& ptrOffOut, uint32_t& nameOffOut, int& scoreOut) {
    static const uint32_t nameOffs[] = {0x10, 0x18, 0x8};

    int bestScore = 0;
    uint32_t bestPtr = 0, bestName = 0;

    for (uint32_t off = 0x20; off <= 0x160; off += 8) {
        uintptr_t arr = 0;
        if (!mem::rd(klass + off, arr)) continue;
        if (!arr || !mem::canRead(arr, 8)) continue;
        for (uint32_t no : nameOffs) {
            int s = scoreMethodArray(klass, arr, no);
            if (s > bestScore) {
                bestScore = s;
                bestPtr = off;
                bestName = no;
            }
        }
    }
    ptrOffOut = bestPtr;
    nameOffOut = bestName;
    scoreOut = bestScore;
    return bestScore >= 2;
}

// ---------------------------------------------------------------------------
// layout runtime buat DUMP SENDIRI (lihat core/dumpout.h)
// ---------------------------------------------------------------------------
static std::string safeStr(uintptr_t p);          // definisinya di bawah

// Alamat runtime dari (module, rva). Dua tafsir dicoba: (1) rva = file offset
// segmen (dump BARU, stabil antar run -- lihat mem::rvaOf), (2) base + rva
// (dump lama / modul yang mapping-nya nyambung). Di modul yang nyambung
// dua-duanya sama, jadi nggak ada perubahan buat MLBB.
static uintptr_t addrOfRva(const std::string& mod, uint64_t rva) {
    const uintptr_t a = mem::addrOfRva(rva);
    if (a) return a;
    const uintptr_t mb = (mod.empty() || mod == il2::libName()) ? il2::base()
                                                               : mem::base(mod.c_str());
    return mb ? mb + (uintptr_t)rva : 0;
}

static std::string baseName(const std::string& path) {
    const size_t s = path.find_last_of('/');
    return s == std::string::npos ? path : path.substr(s + 1);
}

// ---------------------------------------------------------------------------
// PEMBURU layout FieldInfo (buat game yang struct-nya dimodif, mis. MLBB)
//
// Kalibrasi normal butuh token 0x04xxxxxx di dalam entry. Di MLBB syarat itu
// nggak ketemu, jadi di sini dipakai syarat yang lebih longgar: beberapa entry
// BERURUTAN yang nama-nya string identifier wajar + "offset"-nya masuk akal.
// Kombinasi yang menang dicetak beserta contoh entry-nya, biar layout aslinya
// bisa dilihat (bukan ditebak).
// ---------------------------------------------------------------------------
static int scoreWide(uintptr_t arr, uint32_t stride, uint32_t nameOff, uint32_t offSlot,
                     uint32_t* firstOffOut) {
    int ok = 0;
    uint32_t prev = 0;
    for (int i = 0; i < 12; ++i) {
        const uintptr_t base = arr + (uintptr_t)i * stride;
        if (!mem::canRead(base, stride)) break;
        uintptr_t np = 0;
        if (!mem::rd(base + nameOff, np) || !validName(np, 48)) break;
        uint32_t off = 0;
        if (!mem::rd(base + offSlot, off)) break;
        if (off > 0x20000) break;
        if (i && off < prev) break;            // il2cpp nulis offset menaik
        prev = off;
        ++ok;
    }
    if (firstOffOut && ok) {
        uint32_t off = 0;
        mem::rd(arr + offSlot, off);
        *firstOffOut = off;
    }
    return ok;
}

void huntFieldArray(uintptr_t klass, int topN) {
    struct Cand { size_t score; uint32_t off, stride, nameOff, offSlot; };
    std::vector<Cand> best;

    static const uint32_t strides[] = {0x18, 0x20, 0x10, 0x28, 0x30, 0x38, 0x8};
    static const uint32_t nameOffs[] = {0x0, 0x8};
    static const uint32_t offSlots[] = {0x14, 0x18, 0x1C, 0x10, 0x20, 0x8};

    for (uint32_t off = 0x20; off <= 0x1C0; off += 8) {
        uintptr_t arr = 0;
        if (!mem::rd(klass + off, arr) || !arr) continue;
        if (!mem::canRead(arr, 0x20)) continue;
        for (uint32_t st : strides)
            for (uint32_t no : nameOffs)
                for (uint32_t sl : offSlots) {
                    if (sl + 4 > st) continue;
                    int s = scoreWide(arr, st, no, sl, nullptr);
                    if (s >= 3) best.push_back({(size_t)s, off, st, no, sl});
                }
    }
    std::sort(best.begin(), best.end(), [](const Cand& a, const Cand& b) {
        return a.score > b.score;
    });
    if (best.empty()) {
        logx::add("[exp/hunt] nggak ada array field yang masuk akal di klass+0x20..0x1c0");
        return;
    }
    for (int i = 0; i < topN && i < (int)best.size(); ++i) {
        const Cand& c = best[i];
        uintptr_t arr = 0;
        mem::rd(klass + c.off, arr);
        logx::add("[exp/hunt] kandidat #%d: klass+0x%x -> arr stride 0x%x nameOfs 0x%x "
                  "offSlot 0x%x | %zu entry pertama lolos",
                  i + 1, c.off, c.stride, c.nameOff, c.offSlot, c.score);
        for (int j = 0; j < 4; ++j) {
            uintptr_t base = arr + (uintptr_t)j * c.stride;
            uint64_t q[4] = {0};
            mem::read(base, q, sizeof(q));
            char nm[64] = {0};
            uintptr_t np = 0;
            mem::rd(base + c.nameOff, np);
            if (np) il2::readStr(np, nm, sizeof(nm));
            logx::add("[exp/hunt]     [%d] %llx %llx %llx %llx  name='%s'", j,
                      (unsigned long long)q[0], (unsigned long long)q[1],
                      (unsigned long long)q[2], (unsigned long long)q[3], nm);
        }
    }
}

// ---------------------------------------------------------------------------
// huntClass - diagnosa LENGKAP sebuah Il2CppClass
//
// Buat tiap array yang ditunjuk klass+0x20..0x1c0, dihitung di offset mana
// (di dalam entry) yang berisi:
//   * nama (string identifier)
//   * pointer balik ke kelas itu sendiri      <- tanda FieldInfo/MethodInfo
//   * pointer ke segmen kode (r-x)           <- tanda MethodInfo (methodPointer)
//   * dword token 0x04xxxxxx / 0x06xxxxxx
// Yang muncul di SEMUA entry pertama = layout-nya. Ini yang dipakai buat
// nentuin layout di game yang struct-nya dimodif (MLBB) tanpa hardcode offset.
// ---------------------------------------------------------------------------
// Diagnosa: qword isi Il2CppClass yang menunjuk ke struktur lain -> cetak 8
// qword di struktur itu + tafsirnya (nama? alamat kode? pointer kelas?).
void dumpTargets(uintptr_t klass, int topN) {
    int shown = 0;
    for (uint32_t off = 0x20; off <= 0x1c0 && shown < topN; off += 8) {
        uintptr_t v = mem::untag(mem::rdp<uintptr_t>(klass + off));
        if (!v || !mem::canRead(v, 0x40)) continue;
        const mem::Region* r = mem::regionOf(v);
        char where[96];
        snprintf(where, sizeof(where), "%s", r ? (r->name.empty() ? "anon" : r->name.c_str()) : "?");
        const char* baseName2 = strrchr(where, '/');
        if (baseName2) memmove(where, baseName2 + 1, strlen(baseName2) + 1);

        uint64_t q[8] = {0};
        mem::read(v, q, sizeof(q));
        char nm[64] = {0};
        if (validName((uintptr_t)q[0], 48)) il2::readStr((uintptr_t)q[0], nm, sizeof(nm));
        char nm1[64] = {0};
        if (validName((uintptr_t)q[1], 48)) il2::readStr((uintptr_t)q[1], nm1, sizeof(nm1));
        int nExec = 0;
        char ex[128] = "-";
        for (int j = 0; j < 8; ++j) {
            const mem::Region* rr = mem::regionOf((uintptr_t)q[j]);
            if (rr && rr->exec()) {
                ++nExec;
                if (nExec == 1) {
                    const char* bn = strrchr(rr->name.c_str(), '/');
                    snprintf(ex, sizeof(ex), "q[%d]=%s", j, bn ? bn + 1 : "exec");
                }
            }
        }
        const mem::Region* r0 = mem::regionOf((uintptr_t)q[0]);
        logx::add("[exp/target] klass+0x%x -> 0x%lx [%s] q0..3=%llx %llx %llx %llx | nama0='%s' nama1='%s' | kode %d/8 %s (q0: %s)",
                  off, (unsigned long)v, where, (unsigned long long)q[0],
                  (unsigned long long)q[1], (unsigned long long)q[2],
                  (unsigned long long)q[3], nm, nm1, nExec, ex,
                  r0 ? (r0->exec() ? "exec" : "data") : "-");
        ++shown;
    }
}

// Diagnosa: ringkasan tiap array yang ditunjuk Il2CppClass -- di offset berapa
// ada nama / pointer kelas / alamat kode, buat tiap stride kandidat.
static int cntQword(uintptr_t arr, uint32_t stride, uint32_t o, int n, int kind,
                    uintptr_t klass);   // definisinya di bawah

// Satu struktur dianggap "MethodInfo" kalau di dalamnya ada: alamat KODE
// (segmen r-x), pointer BALIK ke kelasnya, dan nama method (string). Ini
// tanda-tanda yang nggak mungkin kebetulan ketemu bareng.
static bool methodLikeStruct(uintptr_t m, uintptr_t klass, uint32_t* codeOff,
                             uint32_t* nameOff, uint32_t* klassOff) {
    if (!m || !mem::canRead(m, 0x40)) return false;
    uint32_t c = kNoOff, n = kNoOff, k = kNoOff;
    for (uint32_t o = 0; o < 0x40; o += 8) {
        const uintptr_t v = mem::untag(mem::rdp<uintptr_t>(m + o));
        if (!v) continue;
        if (v == klass && k == kNoOff) { k = o; continue; }
        const mem::Region* r = mem::regionOf(v);
        if (r && r->exec() && c == kNoOff) { c = o; continue; }
        if (n == kNoOff && validName(v, 64)) n = o;
    }
    if (codeOff)  *codeOff  = c;
    if (nameOff)  *nameOff  = n;
    if (klassOff) *klassOff = k;
    return c != kNoOff && k != kNoOff;
}

// Diagnosa: lacak MethodInfo SATU method yang RVA-nya udah kita tahu (dari
// dump yang lagi dimuat). Dari alamat kode method itu, dicari qword yang
// isinya alamat itu -> di situ MethodInfo-nya. Terus dicari SIAPA yang nunjuk
// ke MethodInfo itu (tabel method-nya di mana). Ini yang nentuin layout
// MethodInfo di game yang beda dari il2cpp standar (MLBB).
void huntMethodInfo(const char* klassName, const char* methodName) {
    if (!klassName || !methodName) return;

    const uintptr_t klass = il2::findKlass(klassName, nullptr);
    if (!klass) { logx::add("[exp/mi] kelas '%s' nggak ketemu", klassName); return; }

    // RVA method dari dump offline (kalau nggak ada dump, nggak bisa dilacak)
    uint64_t rva = 0;
    std::string mod;
    if (dump::loaded()) {
        std::vector<const dump::ClassEntry*> hits;
        dump::ClassDetail det;
        if (dump::find(klassName, nullptr, hits) && !hits.empty() && dump::detail(*hits[0], det)) {
            for (const auto& m : det.methods) {
                if (!m.hasRva) continue;
                if (m.name.find(methodName) == std::string::npos) continue;
                rva = m.rva;
                mod = m.module.empty() ? hits[0]->module : m.module;
                logx::add("[exp/mi] dari dump: %s -> rva 0x%llx (%s)", m.name.c_str(),
                          (unsigned long long)rva, mod.c_str());
                break;
            }
        }
    }
    if (!rva) { logx::add("[exp/mi] RVA method '%s' nggak ada di dump", methodName); return; }

    const uintptr_t abs = addrOfRva(mod, rva);
    if (!abs) { logx::add("[exp/mi] module '%s' nggak ke-load", mod.c_str()); return; }
    logx::add("[exp/mi] cari qword = 0x%lx (kode method)", (unsigned long)abs);

    std::vector<uintptr_t> hits;
    mem::setScanBudget(12ull * 1024 * 1024 * 1024);
    mem::scanQword((uint64_t)abs, hits, 32, true);
    std::vector<uintptr_t> tagged;
    mem::scanQwordMasked((uint64_t)abs, 0x00FFFFFFFFFFFFFFull, tagged, 32, true);
    for (uintptr_t h : tagged) hits.push_back(h);
    logx::add("[exp/mi] %zu tempat mengandung alamat kode itu", hits.size());

    uintptr_t miFound = 0;
    for (uintptr_t h : hits) {
        for (int k = 0; k <= 4; ++k) {
            const uintptr_t base = h - (uintptr_t)k * 8;
            if (!mem::canRead(base, 0x40)) continue;
            const uintptr_t klassAt = mem::untag(mem::rdp<uintptr_t>(base + 0x10));
            if (klassAt != klass) continue;
            uint64_t q[8] = {0};
            mem::read(base, q, sizeof(q));
            // nama? tiap qword yang nunjuk ke string identifier dicetak
            char nm[4][64] = {{0}};
            int ni = 0;
            for (int j = 0; j < 8 && ni < 4; ++j) {
                const uintptr_t v = mem::untag((uintptr_t)q[j]);
                if (!v || !mem::canRead(v, 8)) continue;
                const mem::Region* r = mem::regionOf(v);
                if (r && r->exec()) continue;
                if (!validName(v, 48)) continue;
                il2::readStr(v, nm[ni], sizeof(nm[0]));
                ++ni;
            }
            logx::add("[exp/mi] MethodInfo @0x%lx (kode+0x%x) q=%llx %llx %llx %llx %llx %llx | "
                      "string: '%s' '%s' '%s' '%s'",
                      (unsigned long)base, k * 8, (unsigned long long)q[0],
                      (unsigned long long)q[1], (unsigned long long)q[2],
                      (unsigned long long)q[3], (unsigned long long)q[4],
                      (unsigned long long)q[5], nm[0], nm[1], nm[2], nm[3]);
            if (!miFound) miFound = base;
            break;
        }
        if (miFound) break;
    }
    if (!miFound) { logx::add("[exp/mi] MethodInfo-nya nggak ketemu (alamat kode = data?)"); return; }

    // siapa yang nunjuk ke MethodInfo itu? itu tabel method-nya.
    std::vector<uintptr_t> refs;
    mem::scanQword((uint64_t)miFound, refs, 24, true);
    std::vector<uintptr_t> refsTag;
    mem::scanQwordMasked((uint64_t)miFound, 0x00FFFFFFFFFFFFFFull, refsTag, 24, true);
    for (uintptr_t r : refsTag) refs.push_back(r);
    logx::add("[exp/mi] %zu tempat nunjuk ke MethodInfo itu:", refs.size());
    for (size_t i = 0; i < refs.size() && i < 12; ++i) {
        const mem::Region* reg = mem::regionOf(refs[i]);
        std::string mn = reg ? reg->name : std::string("?");
        const size_t sl = mn.find_last_of('/');
        if (sl != std::string::npos) mn = mn.substr(sl + 1);
        const uintptr_t rb = mem::base(mn.c_str());
        logx::add("[exp/mi]   #%zu @0x%lx [%s%s%s] dalam kelas: %s", i, (unsigned long)refs[i],
                  mn.c_str(), rb ? " + 0x" : "", rb ? std::to_string(refs[i] - rb).c_str() : "",
                  (refs[i] >= klass && refs[i] < klass + 0x220) ? "YA (langsung di Il2CppClass)"
                                                               : "bukan (tabel lain)");
    }
}

// Diagnosa SATU MethodInfo secara detail: hexdump 0x80 byte pertama + tafsir
// tiap qword, terus cari di mana STRING nama method-nya dipegang. Ini yang
// dipakai buat nentuin "nama method disimpan di field mana" di game yang
// MethodInfo-nya beda dari il2cpp standar (MLBB).
void probeMethodInfo(const char* klassName, const char* methodName) {
    const uintptr_t klass = il2::findKlass(klassName, nullptr);
    if (!klass) { logx::add("[exp/probe] kelas '%s' nggak ketemu", klassName); return; }

    // nama + RVA method dari dump yang lagi dimuat
    std::string full;
    uint64_t rva = 0;
    std::string mod;
    if (dump::loaded()) {
        std::vector<const dump::ClassEntry*> hits;
        dump::ClassDetail det;
        if (dump::find(klassName, nullptr, hits) && !hits.empty() && dump::detail(*hits[0], det)) {
            for (const auto& m : det.methods) {
                if (!m.hasRva || m.name.find(methodName) == std::string::npos) continue;
                full = m.name; rva = m.rva;
                mod = m.module.empty() ? hits[0]->module : m.module;
                break;
            }
        }
    }
    if (!rva) { logx::add("[exp/probe] method '%s' nggak ada di dump", methodName); return; }
    const uintptr_t code = addrOfRva(mod, rva);
    logx::add("[exp/probe] %s -> rva 0x%llx kode 0x%lx (%s)", full.c_str(),
              (unsigned long long)rva, (unsigned long)code, mod.c_str());

    // cari struktur yang isinya {kode, ..., klass}: MethodInfo-nya
    std::vector<uintptr_t> hits;
    mem::scanQword((uint64_t)code, hits, 64, true);
    std::vector<uintptr_t> mh;
    mem::scanQwordMasked((uint64_t)code, 0x00FFFFFFFFFFFFFFull, mh, 64, true);
    for (uintptr_t h : mh) hits.push_back(h);

    int shown = 0;
    for (uintptr_t h : hits) {
        if (shown >= 3) break;
        uintptr_t mi = 0;
        for (int k = -2; k <= 2 && !mi; ++k) {
            const uintptr_t cand = h + (uintptr_t)(k * 8);
            if (!mem::canRead(cand, 0x80)) continue;
            for (uint32_t o = 0x8; o <= 0x40; o += 8)
                if (mem::untag(mem::rdp<uintptr_t>(cand + o)) == klass) { mi = cand; break; }
        }
        if (!mi) { logx::add("[exp/probe] @0x%lx: kode bukan punya current class", (unsigned long)h); continue; }

        logx::add("[exp/probe] MethodInfo @0x%lx (kode di +0x%x)", (unsigned long)mi,
                  (unsigned)(h - mi));
        for (uint32_t o = 0; o < 0x80; o += 8) {
            const uint64_t q = mem::rdp<uint64_t>(mi + o);
            if (!q) continue;
            const uintptr_t v = mem::untag((uintptr_t)q);
            const mem::Region* r = mem::regionOf(v);
            std::string rn = r ? r->name : std::string("?");
            const size_t sl = rn.find_last_of('/');
            if (sl != std::string::npos) rn = rn.substr(sl + 1);
            char tag[64] = "";
            if (v == (uintptr_t)code) snprintf(tag, sizeof(tag), "KODE");
            else if (v == klass)       snprintf(tag, sizeof(tag), "KLASS");
            else if (r && r->exec())   snprintf(tag, sizeof(tag), "exec");
            else if (validName(v, 64)) {
                char s[64] = {0};
                il2::readStr(v, s, sizeof(s));
                snprintf(tag, sizeof(tag), "string '%s'", s);
            } else if (mem::canRead(v, 16)) {
                char hx[40] = {0};
                for (int j = 0; j < 8; ++j)
                    snprintf(hx + j * 3, 4, "%02x ", mem::rdp<uint8_t>(v + j));
                snprintf(tag, sizeof(tag), "data [%s]", hx);
            }
            logx::add("[exp/probe]   +0x%-3x = 0x%-14llx tag=%-16s region=%s%s", o,
                      (unsigned long long)q, tag, rn.c_str(),
                      (r && !r->name.empty() && v >= r->start) ? "" : "");
            if (validName(v, 64)) {
                for (uint32_t lay = 8; lay < 64; lay += 8) {
                    if (!mem::canRead(v + lay, 8)) break;
                    const uintptr_t nv = mem::untag(mem::rdp<uintptr_t>(v + lay));
                    if (!nv || !validName(nv, 64)) continue;
                    char s2[64] = {0};
                    il2::readStr(nv, s2, sizeof(s2));
                    logx::add("[exp/probe]     (+0x%x dari string itu) -> '%s'", lay, s2);
                }
            }
        }
        ++shown;
    }
    if (!shown) logx::add("[exp/probe] MethodInfo-nya nggak ketemu (kode = data?) ");

    // di mana STRING nama method-nya disimpan?
    std::vector<uintptr_t> strs;
    mem::findStringsAll(methodName, strs, 8, false);
    logx::add("[exp/probe] string '%s': %zu lokasi", methodName, strs.size());
    for (size_t i = 0; i < strs.size() && i < 4; ++i) {
        std::vector<uintptr_t> refs;
        mem::scanQword((uint64_t)strs[i], refs, 24, true);
        std::vector<uintptr_t> refsT;
        mem::scanQwordMasked((uint64_t)strs[i], 0x00FFFFFFFFFFFFFFull, refsT, 24, true);
        for (uintptr_t x : refsT) refs.push_back(x);
        char buf[256];
        int n = snprintf(buf, sizeof(buf), "[exp/probe]   str @0x%lx <- %zu pointer:",
                         (unsigned long)strs[i], refs.size());
        for (size_t j = 0; j < refs.size() && j < 6; ++j)
            n += snprintf(buf + n, sizeof(buf) - n, " 0x%lx", (unsigned long)refs[j]);
        logx::add("%s", buf);
    }
}

// Diagnosa: isi MethodInfo yang ditunjuk tabel PROPERTI/ACCESSOR kelas
// (klass+0x88: {kelas, nama, MethodInfo*, MethodInfo*}). Ini MethodInfo yang
// identitasnya pasti -- dipakai buat lihat isi struct-nya, mis. nama method
// ada nggak di runtime.
void dumpPropMethodInfos(uintptr_t klass, int topN) {
    int shown = 0;
    for (uint32_t off = 0x20; off <= 0x220 && shown < topN; off += 8) {
        uintptr_t arr = mem::untag(mem::rdp<uintptr_t>(klass + off));
        if (!arr || !mem::canRead(arr, 0x80)) continue;

        // cari stride yang entry-nya: pointer kelas di +0, lalu nama di +8
        for (uint32_t st : {0x28u, 0x20u, 0x30u, 0x18u, 0x10u}) {
            int good = 0;
            for (int i = 0; i < 6; ++i) {
                const uintptr_t e = arr + (uintptr_t)i * st;
                if (!mem::canRead(e, st)) break;
                if (mem::untag(mem::rdp<uintptr_t>(e)) != klass) break;
                if (!validName(mem::rdp<uintptr_t>(e + 8), 64)) break;
                ++good;
            }
            if (good < 4 || shown >= topN) continue;

            char pn[80] = {0};
            il2::readStr(mem::rdp<uintptr_t>(arr + 8), pn, sizeof(pn));
            logx::add("[exp/prop] klass+0x%x stride 0x%x -> accessor '%s'", off, st, pn);
            // dua pointer sesudah nama = MethodInfo (getter/setter)
            for (uint32_t po = 0x10; po + 8 <= st && po <= 0x18; po += 8) {
                const uintptr_t mi = mem::untag(mem::rdp<uintptr_t>(arr + po));
                if (!mi || !mem::canRead(mi, 0x40)) continue;
                uint64_t q[16] = {0};
                mem::read(mi, q, sizeof(q));
                char strs[6][64] = {{0}};
                int ni = 0;
                char cc[96] = "-";
                for (int j = 0; j < 16; ++j) {
                    const uintptr_t v = mem::untag((uintptr_t)q[j]);
                    if (!v) continue;
                    const mem::Region* r = mem::regionOf(v);
                    if (r && r->exec() && cc[0] == '-') {
                        const char* bn = strrchr(r->name.c_str(), '/');
                        snprintf(cc, sizeof(cc), "q[%d]->%s", j, bn ? bn + 1 : "exec");
                        continue;
                    }
                    if (ni < 6 && mem::canRead(v, 12) && validName(v, 48))
                        il2::readStr(v, strs[ni++], sizeof(strs[0]));
                }
                logx::add("[exp/prop]   MethodInfo @0x%lx (%s) q=", (unsigned long)mi, cc);
                for (int j = 0; j < 16; j += 6)
                    logx::add("[exp/prop]     q[%d..%d] = %llx %llx %llx %llx %llx %llx", j, j + 5,
                              (unsigned long long)q[j], (unsigned long long)q[j + 1],
                              (unsigned long long)q[j + 2], (unsigned long long)q[j + 3],
                              (unsigned long long)q[j + 4], (unsigned long long)q[j + 5]);
                logx::add("[exp/prop]     str: '%s' '%s' '%s' '%s' '%s' '%s'",
                          strs[0], strs[1], strs[2], strs[3], strs[4], strs[5]);
            }
            ++shown;
        }
    }
    if (!shown) logx::add("[exp/prop] nggak ada tabel accessor yang bentuknya masuk akal");
}

// Diagnosa: cari TABEL METHOD sebuah kelas -- tiap qword di klass+0x20..0x220
// dicoba sebagai: array of MethodInfo* (stride 8) atau MethodInfo inline
// (stride lebih besar). Yang lolos: minimal 8 entry berurutan yang bentuknya
// MethodInfo (kode + pointer kelas + nama).
void huntMethodTable(uintptr_t klass, int topN) {
    static const uint32_t strides[] = {0x8, 0x18, 0x20, 0x28, 0x30, 0x38, 0x40};
    int shown = 0;

    for (uint32_t off = 0x20; off <= 0x220 && shown < topN; off += 8) {
        uintptr_t arr = mem::untag(mem::rdp<uintptr_t>(klass + off));
        if (!arr || !mem::canRead(arr, 0x80)) continue;
        for (uint32_t st : strides) {
            if (shown >= topN) break;
            int good = 0;
            uint32_t cOff = kNoOff, nOff = kNoOff, kOff = kNoOff;
            char nm0[80] = {0};
            for (int i = 0; i < 24; ++i) {
                const uintptr_t e = arr + (uintptr_t)i * st;
                if (!mem::canRead(e, st)) break;
                const uintptr_t m = (st == 0x8) ? mem::untag(mem::rdp<uintptr_t>(e)) : e;
                uint32_t c = kNoOff, n = kNoOff, k = kNoOff;
                if (!methodLikeStruct(m, klass, &c, &n, &k)) break;
                if (i == 0) {
                    cOff = c; nOff = n; kOff = k;
                    const uintptr_t np = mem::rdp<uintptr_t>(m + n);
                    if (np) il2::readStr(np, nm0, sizeof(nm0));
                }
                ++good;
            }
            if (good < 8) continue;
            logx::add("[exp/meth] klass+0x%x -> arr 0x%lx stride 0x%x: %d MethodInfo | "
                      "kode+0x%x nama+0x%x kelas+0x%x | contoh='%s'",
                      off, (unsigned long)arr, st, good, cOff, nOff, kOff, nm0);
            ++shown;
        }
    }
    if (!shown) logx::add("[exp/meth] nggak ada tabel method yang bentuknya masuk akal");
}

void dumpArrays(uintptr_t klass, int topN) {
    static const uint32_t strides[] = {0x8, 0x10, 0x18, 0x20, 0x28, 0x30, 0x38, 0x40};
    static const char* stName[] = {"8", "10", "18", "20", "28", "30", "38", "40"};
    int shown = 0;

    for (uint32_t off = 0x20; off <= 0x220 && shown < topN; off += 8) {
        uintptr_t arr = mem::untag(mem::rdp<uintptr_t>(klass + off));
        if (!arr || !mem::canRead(arr, 0x100)) continue;

        char line[512];
        int n = 0;
        bool any = false;
        for (int si = 0; si < 8; ++si) {
            const uint32_t st = strides[si];
            uint32_t bName = kNoOff, bOwn = kNoOff, bExec = kNoOff;
            int cName = 0, cOwn = 0, cExec = 0;
            for (uint32_t o = 0; o + 8 <= st; o += 8) {
                const int cn = cntQword(arr, st, o, 6, 0, klass);
                const int co = cntQword(arr, st, o, 6, 1, klass);
                const int ce = cntQword(arr, st, o, 6, 2, klass);
                if (cn >= 5 && cn > cName) { cName = cn; bName = o; }
                if (co >= 5 && co > cOwn)  { cOwn = co;  bOwn = o; }
                if (ce >= 5 && ce > cExec) { cExec = ce; bExec = o; }
            }
            if (cName < 5 && cOwn < 5 && cExec < 5) continue;
            any = true;
            n += snprintf(line + n, sizeof(line) - n,
                          " [st%sn%s(%d)/o%s(%d)/x%s(%d)]", stName[si],
                          bName == kNoOff ? "-" : "@", cName,
                          bOwn == kNoOff ? "-" : "@", cOwn,
                          bExec == kNoOff ? "-" : "@", cExec);
        }
        if (!any) continue;
        logx::add("[exp/arrays] klass+0x%x -> arr 0x%lx:%s", off, (unsigned long)arr, line);
        // entry mentah: biar layout aslinya bisa dibaca langsung dari log
        for (int i = 0; i < 3; ++i) {
            uint64_t q[8] = {0};
            const uintptr_t b = arr + (uintptr_t)i * 0x20;
            if (!mem::read(b, q, sizeof(q))) break;
            char l2[400];
            int m = 0;
            for (int j = 0; j < 8; ++j)
                m += snprintf(l2 + m, sizeof(l2) - m, " %llx", (unsigned long long)q[j]);
            logx::add("[exp/arrays]     [%d]%s", i, l2);
        }
        ++shown;
    }
    if (!shown) logx::add("[exp/arrays] nggak ada array kandidat");
}

void huntClass(uintptr_t klass, int topN) {
    static const uint32_t strides[] = {0x20, 0x18, 0x28, 0x30, 0x38, 0x10, 0x40};
    int shown = 0;

    for (uint32_t off = 0x20; off <= 0x2c0 && shown < topN; off += 8) {
        uintptr_t arr = 0;
        if (!mem::rd(klass + off, arr) || !arr) continue;
        if (!mem::canRead(arr, 0x40)) continue;
        for (uint32_t st : strides) {
            if (shown >= topN) break;

            // hitung per-offset: berapa dari 8 entry yang isinya nama / kelas /
            // alamat kode. (Bukan "intersection" -- sebagian entry boleh kosong,
            // mis. method abstract tanpa methodPointer.)
            int nName = 0, nKlass = 0, nExec = 0;
            uint32_t nameOff = 0, klassOff = 0, execOff = 0;
            int nameBest = 0, klassBest = 0, execBest = 0;
            for (uint32_t o = 0; o + 8 <= st; o += 8) {
                int cn = 0, ck = 0, ce = 0;
                for (int i = 0; i < 8; ++i) {
                    uintptr_t base = arr + (uintptr_t)i * st;
                    if (!mem::canRead(base, st)) break;
                    uintptr_t v = mem::rdp<uintptr_t>(base + o);
                    if (!v) continue;
                    if (mem::untag(v) == klass) ++ck;
                    const mem::Region* r = mem::regionOf(v);
                    if (r && r->exec()) { ++ce; continue; }
                    if (mem::rdp<uintptr_t>(v + 8) && validName(v, 48)) ++cn;
                }
                if (cn > nameBest)  { nameBest = cn;  nameOff = o; }
                if (ck > klassBest) { klassBest = ck; klassOff = o; }
                if (ce > execBest)  { execBest = ce;  execOff = o; }
                nName = cn; nKlass = ck; nExec = ce;
            }
            if (nameBest < 6) continue;
            if (klassBest < 5 && execBest < 5) continue;

            char nm0[64] = {0};
            uintptr_t np = mem::rdp<uintptr_t>(arr + nameOff);
            if (np) il2::readStr(np, nm0, sizeof(nm0));
            logx::add("[exp/hunt] klass+0x%x stride 0x%x | nama 0x%x (%d/8) kelas 0x%x (%d/8) "
                      "kode 0x%x (%d/8) | contoh='%s'",
                      off, st, nameOff, nameBest, klassOff, klassBest, execOff, execBest, nm0);
            // kalau ada alamat kode, cetak 2 entry mentah -- layout MethodInfo-nya
            // kelihatan langsung dari situ.
            if (execBest >= 5) {
                for (int i = 0; i < 2; ++i) {
                    uint64_t q[8] = {0};
                    mem::read(arr + (uintptr_t)i * st, q, sizeof(q));
                    char line[400];
                    int n = 0;
                    for (int j = 0; j < 8; ++j)
                        n += snprintf(line + n, sizeof(line) - n, " %llx",
                                      (unsigned long long)q[j]);
                    logx::add("[exp/hunt]     [%d]%s", i, line);
                }
            }
            ++shown;
        }
    }
    if (!shown) logx::add("[exp/hunt] nggak ada array yang punya tanda FieldInfo/MethodInfo");
}

// ---------------------------------------------------------------------------
// kalibrasi BERBASIS STRUKTUR
//
// Cara lama nebak stride+slot lalu nyari token 0x04xxxxxx di "slot-4". Itu
// mentok di game yang struktur metadata-nya dimodif -- nyata di MLBB:
//
//     klass+0x78 -> FieldInfo[ stride 0x20 ]
//         +0x00 nama    +0x08 Il2CppType*    +0x10 Il2CppClass* (balik ke kelas)
//         +0x18 uint64 = (token << 32) | offset     <- token SETELAH offset
//
// Jadi yang dicari di sini: array yang entry-nya punya STRING nama + pointer
// balik ke kelas itu sendiri, terus dicari di offset berapa ada pasangan
// {dword offset kecil, dword token 0x04xxxxxx} (urutan bebas) buat FieldInfo,
// atau pointer ke segmen kode buat MethodInfo. Nggak ada offset yang di-bake.
// ---------------------------------------------------------------------------
static int cntQword(uintptr_t arr, uint32_t stride, uint32_t o, int n, int kind,
                    uintptr_t klass) {
    int c = 0;
    // Kalibrasi layout = puluhan ribu baca memori per kelas. Kalau user nge-Stop
    // (atau UI minta batal), loop-nya wajib berhenti SEKARANG, bukan nunggu
    // kelas ini selesai. Cek-nya di sini karena fungsi ini yang paling sering
    // dipanggil di jalur itu.
    if (abortx::requested()) return c;
    for (int i = 0; i < n; ++i) {
        const uintptr_t base = arr + (uintptr_t)i * stride;
        if (!mem::canRead(base, stride)) break;
        const uintptr_t v = mem::rdp<uintptr_t>(base + o);
        if (!v) continue;
        if (kind == 1) { if (mem::untag(v) == klass) ++c; continue; }
        const mem::Region* r = mem::regionOf(v);
        if (kind == 2) { if (r && r->exec()) ++c; continue; }
        if (r && r->exec()) continue;                       // kode bukan nama
        if (validName(v, 48)) ++c;
    }
    return c;
}

// wantToken=false: hitung dword yang nilainya masuk akal sebagai OFFSET field.
// Offset 0 itu sah (field pertama sering di 0), jadi nggak boleh ditolak --
// tapi kalau SEMUA nol berarti itu padding, bukan offset. Makanya nilai
// maksimumnya dikembalikan lewat maxOut biar pemanggil bisa nolak.
static int cntDword(uintptr_t arr, uint32_t stride, uint32_t o, int n, bool wantToken,
                    uint32_t* maxOut = nullptr) {
    int c = 0;
    if (abortx::requested()) { if (maxOut) *maxOut = 0; return c; }
    uint32_t mx = 0;
    for (int i = 0; i < n; ++i) {
        const uintptr_t base = arr + (uintptr_t)i * stride;
        if (!mem::canRead(base, stride)) break;
        const uint32_t v = mem::rdp<uint32_t>(base + o);
        if (v > mx) mx = v;
        if (wantToken ? ((v >> 24) == 0x04) : (v <= 0x40000)) ++c;
    }
    if (maxOut) *maxOut = mx;
    return c;
}

// berapa entry BERURUTAN yang lolos syarat layout-nya
static int streakEntries(uintptr_t arr, uint32_t stride, const FieldLayout& L, bool fields,
                         const MethodLayout& M) {
    const uint32_t nameOff  = fields ? L.nameOff  : M.nameOff;
    const uint32_t ownerOff = fields ? L.ownerOff : M.klassOff;
    int ok = 0;
    for (int i = 0; i < 24; ++i) {
        const uintptr_t base = arr + (uintptr_t)i * stride;
        if (!mem::canRead(base, stride)) break;
        uintptr_t np = 0;
        if (!mem::rd(base + nameOff, np) || !validName(np, 64)) break;
        if (ownerOff != kNoOff) {
            uintptr_t ow = 0;
            if (!mem::rd(base + ownerOff, ow)) break;
        }
        if (fields) {
            uint32_t off = 0;
            if (!mem::rd(base + L.offSlot, off)) break;
            if (off > 0x40000) break;
            if (L.tokenSlot) {
                uint32_t tk = 0;
                if (!mem::rd(base + L.tokenSlot, tk)) break;
                if ((tk >> 24) != 0x04) break;
            }
        } else {
            uintptr_t code = 0;
            if (M.codeOff != kNoOff) code = mem::rdp<uintptr_t>(base + M.codeOff);
            if (!code) break;
            const mem::Region* r = mem::regionOf(code);
            if (!r || !r->exec()) break;
        }
        ++ok;
    }
    return ok;
}

static bool probeArray(uintptr_t klass, uintptr_t arr, uint32_t stride, bool fields,
                       FieldLayout& outF, MethodLayout& outM, int& scoreOut) {
    const int N = 6;
    uint32_t nameOff = kNoOff, ownerOff = kNoOff, codeOff = kNoOff;
    int nameBest = 0, ownerBest = 0, codeBest = 0;

    for (uint32_t o = 0; o + 8 <= stride; o += 8) {
        if (abortx::requested()) return false;
        const int cn = cntQword(arr, stride, o, N, 0, klass);
        if (cn >= N - 1 && cn > nameBest) { nameBest = cn; nameOff = o; }
        const int co = cntQword(arr, stride, o, N, 1, klass);
        if (co >= N - 1 && co > ownerBest) { ownerBest = co; ownerOff = o; }
        const int cc = cntQword(arr, stride, o, N, 2, klass);
        if (cc >= N - 1 && cc > codeBest) { codeBest = cc; codeOff = o; }
    }
    if (nameOff == kNoOff) return false;
    if (ownerOff == nameOff) ownerOff = kNoOff;
    if (codeOff == nameOff) codeOff = kNoOff;
    // FieldInfo/MethodInfo di layout non-standar SELALU punya pointer balik ke
    // kelasnya. Syarat ini yang bikin layout salah (mis. stride kelewat besar
    // sehingga nama field ke-i ketemu tapi offset-nya field lain) langsung
    // ketolak.
    if (ownerOff == kNoOff) return false;

    if (fields) {
        // pasangan {offset kecil, token 0x04xxxxxx}: urutannya bisa dua-duanya
        uint32_t offSlot = kNoOff, tokenSlot = 0;
        for (uint32_t o = 0; o + 4 <= stride; o += 4) {
            uint32_t maxOff = 0;
            if (cntDword(arr, stride, o, N, false, &maxOff) < N) continue;
            if (maxOff == 0) continue;                       // semua nol = padding
            if (o + 4 <= stride && cntDword(arr, stride, o + 4, N, true) == N) {
                offSlot = o; tokenSlot = o + 4; break;
            }
            if (o >= 4 && cntDword(arr, stride, o - 4, N, true) == N) {
                offSlot = o; tokenSlot = o - 4; break;
            }
        }
        if (offSlot == kNoOff) return false;

        outF = FieldLayout{};
        outF.nameOff = nameOff;
        outF.ownerOff = ownerOff;
        outF.offSlot = offSlot;
        outF.tokenSlot = tokenSlot;
        // Il2CppType* biasanya di antara nama dan owner
        for (uint32_t o = 0; o + 8 <= stride; o += 8) {
            if (o == nameOff || o == ownerOff) continue;
            if (cntQword(arr, stride, o, N, 0, klass) >= N - 1) { outF.typeOff = o; break; }
        }
        outF.stride = stride;
        outF.structural = true;
        scoreOut = streakEntries(arr, stride, outF, true, outM);
        outF.score = scoreOut;
        return scoreOut >= 3;
    }

    if (codeOff == kNoOff) return false;
    outM = MethodLayout{};
    outM.nameOff = nameOff;
    outM.klassOff = ownerOff;
    outM.codeOff = codeOff;
    outM.stride = stride;
    outM.structural = true;
    scoreOut = streakEntries(arr, stride, outF, false, outM);
    outM.score = scoreOut;
    return scoreOut >= 3;
}

// cari array FieldInfo/MethodInfo di dalam klass+0x20..0x220 (semua stride
// kandidat), terus pilih skor terbaik.
static bool calibrateStructural(uintptr_t klass, bool fields, FieldLayout& fOut, MethodLayout& mOut,
                                int& scoreOut) {
    static const uint32_t strides[] = {0x20, 0x18, 0x28, 0x30, 0x40, 0x38, 0x10, 0x8};
    bool found = false;
    int best = 0;

    for (uint32_t off = 0x20; off <= 0x220; off += 8) {
        if (abortx::requested()) break;
        uintptr_t arr = 0;
        if (!mem::rd(klass + off, arr) || !arr) continue;
        if (!mem::canRead(arr, 0x30)) continue;
        for (uint32_t st : strides) {
            if (abortx::requested()) break;
            FieldLayout f;
            MethodLayout m;
            int score = 0;
            if (!probeArray(klass, arr, st, fields, f, m, score)) continue;
            if (score <= best) continue;
            best = score;
            found = true;
            if (fields) { f.ptrOff = off; fOut = f; }
            else        { m.ptrOff = off; mOut = m; }
        }
    }
    scoreOut = best;
    return found;
}

bool calibrateFieldLayout(uintptr_t klass, FieldLayout& out) {
    // 1. struktur (game yang metadata-nya dimodif: MLBB)
    FieldLayout f;
    MethodLayout m;
    int score = 0;
    if (calibrateStructural(klass, true, f, m, score) && score >= 3) {
        out = f;
        out.ok = true;
        return true;
    }
    // 2. kalibrasi lama (il2cpp standar, FieldInfo = {nama, tipe, token, offset})
    Calib c;
    const bool ok = calibrateFields(klass, c);
    out = FieldLayout{};
    out.ok       = ok;
    out.ptrOff   = c.ptrOff;
    out.stride   = c.stride;
    out.nameOff  = 0;
    out.typeOff  = 8;
    out.offSlot  = c.offSlot;
    out.tokenSlot = (c.offSlot >= 4) ? (c.offSlot - 4) : 0;
    out.ownerOff = kNoOff;
    out.score    = c.score;
    return ok;
}

bool calibrateMethodLayout(uintptr_t klass, MethodLayout& out) {
    FieldLayout f;
    MethodLayout m;
    int score = 0;
    if (calibrateStructural(klass, false, f, m, score) && score >= 3) {
        out = m;
        out.ok = true;
        return true;
    }
    uint32_t ptrOff = 0, nameOff = 0;
    int s = 0;
    const bool ok = calibrateMethods(klass, ptrOff, nameOff, s);
    out = MethodLayout{};
    out.ok      = ok;
    out.ptrOff  = ptrOff;
    out.stride  = 8;
    out.nameOff = nameOff;
    out.klassOff = nameOff + 8;
    out.codeOff = kNoOff;
    out.score   = s;
    return ok;
}

// Il2CppTypeEnum -> nama tipe tertulis (kebalikannya dump::guessTypeEnum).
// Dipakai buat nulis signature method waktu dump sendiri, biar file-nya bisa
// dibaca balik sama tool ini maupun Il2CppDumper.
std::string typeNameOf(uintptr_t t, int depth) {
    if (!t || depth > 2 || !mem::canRead(t, 0x10)) return "Il2CppObject";
    uint32_t bits = 0;
    if (!mem::rd(t + 8, bits)) return "Il2CppObject";
    const uint8_t e    = (uint8_t)((bits >> 16) & 0xFF);
    const bool    byref = ((bits >> 29) & 1) != 0;
    uintptr_t data = 0;
    mem::rd(t, data);

    std::string out;
    switch (e) {
        case 0x01: out = "void";  break;
        case 0x02: out = "bool";   break;
        case 0x03: out = "char";   break;
        case 0x04: out = "sbyte";  break;
        case 0x05: out = "byte";   break;
        case 0x06: out = "short";  break;
        case 0x07: out = "ushort"; break;
        case 0x08: out = "int";    break;
        case 0x09: out = "uint";   break;
        case 0x0A: out = "long";   break;
        case 0x0B: out = "ulong";  break;
        case 0x0C: out = "float";  break;
        case 0x0D: out = "double"; break;
        case 0x0E: out = "string"; break;
        case 0x18: out = "System.IntPtr";  break;
        case 0x19: out = "System.UIntPtr"; break;
        case 0x1C: out = "object"; break;
        case 0x1D: out = typeNameOf(data, depth + 1) + "[]"; break;   // SZARRAY
        case 0x0F: out = typeNameOf(data, depth + 1) + "*";  break;   // PTR
        case 0x11:                                                      // CLASS
        case 0x12: {                                                    // VALUETYPE
            char nm[160] = {0}, ns[160] = {0};
            snprintf(nm, sizeof(nm), "%s", il2::klassName(data));
            snprintf(ns, sizeof(ns), "%s", il2::klassNamespace(data));
            if (!nm[0]) { out = "Il2CppObject"; break; }
            out = (ns[0] ? std::string(ns) + "." : std::string()) + nm;
            break;
        }
        default: out = "Il2CppObject"; break;
    }
    if (byref) out += "&";
    return out;
}

size_t readFields(uintptr_t klass, const FieldLayout& L, std::vector<Field>& out, size_t max) {
    out.clear();
    if (!klass || !L.ok || !L.stride || L.offSlot == kNoOff) return 0;
    uintptr_t arr = 0;
    if (!mem::rd(klass + L.ptrOff, arr) || !arr) return 0;

    for (size_t i = 0; i < max; ++i) {
        if ((i & 63) == 0 && abortx::requested()) break;
        const uintptr_t base = arr + (uintptr_t)i * L.stride;
        if (!mem::canRead(base, L.stride)) break;
        uintptr_t namePtr = 0;
        if (!mem::rd(base + L.nameOff, namePtr) || !validName(namePtr)) break;
        // Batas array: entry-nya harus menyebut kelas ini sebagai pemiliknya.
        // Tanpa cek ini, pembacaan lewat batas array nyerempet array kelas lain
        // (nama-nya tetap valid) dan field-nya jadi kelebihan.
        if (L.ownerOff != kNoOff) {
            uintptr_t ow = 0;
            if (!mem::rd(base + L.ownerOff, ow)) break;
            if (mem::untag(ow) != klass) break;
        }
        uint32_t off = 0;
        if (!mem::rd(base + L.offSlot, off)) break;
        if (off > 0x40000) break;
        uint32_t token = 0;
        if (L.tokenSlot) mem::rd(base + L.tokenSlot, token);

        Field f;
        f.name   = safeStr(namePtr);
        f.offset = off;
        f.token  = token;
        uintptr_t typePtr = 0;
        if (L.typeOff != kNoOff && mem::rd(base + L.typeOff, typePtr) && typePtr) {
            f.typeName = typeNameOf(typePtr);
            uint32_t bits = 0;
            if (mem::rd(typePtr + 8, bits)) f.typeEnum = (uint8_t)((bits >> 16) & 0xFF);
        }
        out.push_back(std::move(f));
    }

    // ---- offset dari ARRAY TERPISAH (il2cpp baru) ----
    // Di Free Fire/CODM (Unity 2020+) sebagian kelas nggak nyimpen offset di
    // FieldInfo-nya; offset aslinya ada di array per-kelas (Lihat FieldLayout.
    // offArrayPtrOff). Array ini yang dipakai buat ngisi yang masih 0 -- dan
    // kalau harganya beda dengan FieldInfo, yang dipakai nilai array-nya
    // (array itu yang dibaca runtime-nya).
    if (L.offArrayPtrOff != kNoOff && !out.empty()) {
        uintptr_t oarr = 0;
        if (mem::rd(klass + L.offArrayPtrOff, oarr) && oarr) {
            for (size_t i = 0; i < out.size(); ++i) {
                const uintptr_t a = oarr + (uintptr_t)i * L.offArrayElem;
                const uint32_t v = (L.offArrayElem == 8)
                                       ? (uint32_t)mem::rdp<uint64_t>(a)
                                       : mem::rdp<uint32_t>(a);
                if (v && v <= 0x40000) out[i].offset = v;
            }
        }
    }
    return out.size();
}

// ---------------------------------------------------------------------------
// array offset field terpisah (il2cpp baru)
// ---------------------------------------------------------------------------
// Aturan kandidat (sengaja ketat -- salah pilih = offset field ngawur di dump):
//   * tiap elemen <= 0x40000 (offset instance wajar)
//   * urutannya nggak turun kalau nggak nol (layout objek naik)
//   * HARUS ada minimal 3 field yang offset-nya udah kebaca dari FieldInfo, dan
//     nilai array di indeks itu harus SAMA SEMUA (nggak boleh ada yang beda)
static bool plausibleOffsetArray(uintptr_t arr, size_t n, uint32_t elem,
                                 const std::vector<uint32_t>& known,
                                 int& match, int& mismatch) {
    match = mismatch = 0;
    if (!arr || !n) return false;
    if (!mem::canRead(arr, n * elem)) return false;
    uint64_t prev = 0;
    bool nonZero = false;
    for (size_t i = 0; i < n; ++i) {
        uint64_t v = (elem == 8) ? mem::rdp<uint64_t>(arr + i * 8)
                                 : (uint64_t)mem::rdp<uint32_t>(arr + i * 4);
        if (v > 0x40000) return false;
        if (v && v < prev) return false;
        if (v) { prev = v; nonZero = true; }
        if (i < known.size() && known[i]) {
            if (v == known[i]) ++match;
            else               ++mismatch;
        }
    }
    return nonZero;
}

bool calibrateFieldOffsetArray(const std::vector<uintptr_t>& klasses, FieldLayout& io,
                               size_t maxSamples) {
    if (!io.ok) return false;
    std::map<uint64_t, int> votes;          // key = (ptrOff << 8) | (elem==8)
    std::map<uint64_t, int> matches;
    size_t checked = 0;

    FieldLayout base = io;
    base.offArrayPtrOff = kNoOff;           // jangan dipakai buat validasi ini

    for (uintptr_t k : klasses) {
        if (checked >= maxSamples) break;
        if (abortx::requested()) break;

        std::vector<Field> fs;
        const size_t n = readFields(k, base, fs, 128);
        if (n < 3) continue;

        std::vector<uint32_t> known;
        known.reserve(n);
        int nz = 0;
        for (const auto& f : fs) { known.push_back(f.offset); if (f.offset) ++nz; }
        if (nz < 3) continue;               // nggak ada referensi buat nge-tes
        ++checked;

        for (uint32_t off = 0; off + 8 <= 0x200; off += 8) {
            uintptr_t arr = 0;
            if (!mem::rd(k + off, arr) || !arr) continue;
            arr = mem::untag(arr);
            if (!mem::canRead(arr, 4)) continue;
            for (uint32_t elem : {4u, 8u}) {
                int m = 0, mm = 0;
                if (!plausibleOffsetArray(arr, n, elem, known, m, mm)) continue;
                if (m < 3 || mm) continue;
                const uint64_t key = ((uint64_t)off << 8) | (elem == 8 ? 1u : 0u);
                ++votes[key];
                matches[key] += m;
            }
        }
    }
    if (checked < 3) {
        logx::add("[foff] sampel kurang (%zu kelas punya offset pembanding) -- "
                  "array offset nggak dikalibrasi", checked);
        return false;
    }

    uint64_t bestKey = 0;
    int bestVotes = 0, bestMatch = 0;
    for (const auto& kv : votes) {
        const int mv = matches[kv.first];
        if (kv.second > bestVotes || (kv.second == bestVotes && mv > bestMatch)) {
            bestKey = kv.first; bestVotes = kv.second; bestMatch = mv;
        }
    }
    // minimal 3 kelas setuju, dan rata-rata >= 3 field cocok per kelas
    if (bestVotes < 3 || bestMatch < bestVotes * 3) {
        logx::add("[foff] kandidat terbaik cuma %d kelas (cocok %d) -- dianggap nggak ada",
                  bestVotes, bestMatch);
        return false;
    }

    io.offArrayPtrOff  = (uint32_t)(bestKey >> 8);
    io.offArrayElem    = (bestKey & 1) ? 8u : 4u;
    io.offArrayClasses = bestVotes;
    logx::add("[foff] array offset field: Il2CppClass+0x%x -> %s[] (%d kelas setuju, %d field cocok)",
              io.offArrayPtrOff, io.offArrayElem == 8 ? "uint64" : "uint32",
              bestVotes, bestMatch);
    return true;
}

// Diagnosa: cetak qword Il2CppClass + kandidat array offset-nya.
void diagFieldOffsets(uintptr_t klass, int topCand) {
    if (!klass) { logx::add("[foff] klass 0x0"); return; }
    FieldLayout L;
    if (!calibrateFieldLayout(klass, L)) {
        logx::add("[foff] layout FieldInfo nggak kekalibrasi buat kelas ini");
        return;
    }
    logx::add("[foff] layout: ptr+0x%x stride 0x%x name+0x%x off+0x%x token+0x%x",
              L.ptrOff, L.stride, L.nameOff, L.offSlot, L.tokenSlot);

    std::vector<Field> fs;
    const size_t n = readFields(klass, L, fs, 128);
    int nz = 0;
    for (const auto& f : fs) if (f.offset) ++nz;
    logx::add("[foff] %zu field (offset keisi dari FieldInfo: %d)", n, nz);
    if (!n) return;

    std::vector<uint32_t> known;
    known.reserve(n);
    for (const auto& f : fs) known.push_back(f.offset);

    for (uint32_t off = 0; off + 8 <= 0x200; off += 8) {
        uintptr_t v = mem::rdp<uintptr_t>(klass + off);
        const mem::Region* r = v ? mem::regionOf(mem::untag(v)) : nullptr;
        logx::add("[foff]  +0x%03x = 0x%016llx%s", off, (unsigned long long)v,
                  r ? "  <- readable" : "");
    }
    int shown = 0;
    for (uint32_t off = 0; off + 8 <= 0x200 && shown < topCand; off += 8) {
        uintptr_t arr = mem::rdp<uintptr_t>(klass + off);
        if (!arr) continue;
        arr = mem::untag(arr);
        if (!mem::canRead(arr, 4)) continue;
        for (uint32_t elem : {4u, 8u}) {
            int m = 0, mm = 0;
            if (!plausibleOffsetArray(arr, n, elem, known, m, mm)) continue;
            logx::add("[foff]  KANDIDAT +0x%x -> %s[%zu] cocok %d / beda %d%s",
                      off, elem == 8 ? "u64" : "u32", n, m, mm,
                      (m >= 3 && mm == 0) ? "   <== masuk syarat" : "");
            ++shown;
        }
    }
    if (!shown) logx::add("[foff] nggak ada kandidat array offset yang lolos syarat");
}

// MethodInfo (layout sama di il2cpp 24 - 31, cuma 'name'-nya yang geser):
//     name+0x00  nama method          name+0x08  Il2CppClass* (harus class ini)
//     name+0x10  tipe balik (Il2CppType*)
//     name+0x18  parameters (Il2CppType**)
//     name+0x20  parameters_count (uint16)
//
// Di layout non-standar (MLBB) MethodInfo-nya nempel di dalam array (stride
// > 8) dan posisi nama/kelas/kode-nya diambil dari kalibrasi struktur.
size_t readMethods(uintptr_t klass, const MethodLayout& L, std::vector<Method>& out,
                   size_t max, bool withSignature) {
    out.clear();
    if (!klass || !L.ok || !L.stride) return 0;
    uintptr_t arr = 0;
    if (!mem::rd(klass + L.ptrOff, arr) || !arr) return 0;

    for (size_t i = 0; i < max; ++i) {
        if ((i & 63) == 0 && abortx::requested()) break;
        // dua bentuk: array MethodInfo* (stride 8) atau MethodInfo inline
        // (stride > 8, kayak MLBB) -- dua-duanya dibaca dengan cara yang sama.
        const uintptr_t entry = arr + (uintptr_t)i * L.stride;
        if (!mem::canRead(entry, L.stride)) break;
        uintptr_t m = entry;
        if (L.stride == 8) {
            if (!mem::rd(entry, m) || !m) break;
            if (!mem::canRead(m, L.nameOff + 0x28)) break;
        } else if (!mem::canRead(entry, L.nameOff + 0x28)) {
            break;
        }
        uintptr_t namePtr = 0, klassPtr = 0;
        if (!mem::rd(m + L.nameOff, namePtr) || !validName(namePtr, 96)) break;
        if (L.klassOff != kNoOff) {
            if (!mem::rd(m + L.klassOff, klassPtr)) break;
            if (mem::untag(klassPtr) != klass) break;
        }

        const std::string nm = safeStr(namePtr);
        if (nm.empty()) break;

        // Signature (tipe balik + parameter) cuma bisa dibaca di layout
        // standar: offset 'name' -> return/params/count itu il2cpp normal.
        // Di layout non-standar (MethodInfo inline, mis. MLBB) offset-nya beda,
        // jadi nama method ditulis apa adanya daripada nulis sampah.
        const bool stdLayout = (L.codeOff == kNoOff);
        std::string ret = "void";
        if (stdLayout) {
            ret = typeNameOf(mem::rdp<uintptr_t>(m + L.nameOff + 0x10));
            if (ret.empty()) ret = "void";
        }

        // Parameter: kalau count-nya nggak masuk akal, lebih baik nggak
        // ditulis sama sekali daripada nulis sampah ke file dump.
        std::string params;
        uint16_t pcount = 0;
        mem::rd(m + L.nameOff + 0x20, pcount);
        uintptr_t parr = mem::rdp<uintptr_t>(m + L.nameOff + 0x18);
        if (withSignature && stdLayout && pcount <= 32 && parr) {
            for (uint16_t p = 0; p < pcount; ++p) {
                uintptr_t pt = 0;
                if (!mem::rd(parr + (uintptr_t)p * 8, pt) || !pt) { params.clear(); break; }
                if (!params.empty()) params += ", ";
                params += typeNameOf(pt);
            }
        }

        Method mm;
        if (!stdLayout)                  mm.name = nm;
        else if (withSignature)          mm.name = ret + " " + nm + "(" + params + ")";
        else                             mm.name = nm;
        if (L.codeOff != kNoOff) {
            // layout non-standar: alamat kode ada di dalam entry-nya sendiri
            mm.ptr = mem::untag(mem::rdp<uintptr_t>(m + L.codeOff));
            const mem::Region* r = mem::regionOf(mm.ptr);
            if (r && r->exec() && !r->name.empty()) {
                const size_t sl = r->name.find_last_of('/');
                mm.module = sl == std::string::npos ? r->name : r->name.substr(sl + 1);
                mm.rva = mem::rvaOf(mm.ptr);      // stabil antar run (lihat mem.h)
            }
        }
        if (!mm.ptr) methodCode(m, mm.ptr, mm.module, mm.rva);
        out.push_back(std::move(mm));
    }
    return out.size();
}

bool methodCode(uintptr_t m, uintptr_t& ptrOut, std::string& moduleOut, uint64_t& rvaOut) {
    ptrOut = 0;
    moduleOut.clear();
    rvaOut = 0;
    if (!m || !mem::canRead(m, 0x10)) return false;

    for (uint32_t off : {0x00u, 0x08u}) {
        uintptr_t v = 0;
        if (!mem::rd(m + off, v) || !v) continue;
        v = mem::untag(v);
        const mem::Region* r = mem::regionOf(v);
        if (!r || !r->exec() || r->name.empty()) continue;   // bukan alamat kode
        const std::string mod = baseName(r->name);
        const uint64_t rv = mem::rvaOf(v);                   // stabil antar run
        if (!rv) continue;
        ptrOut = v;
        moduleOut = mod;
        rvaOut = rv;
        return true;
    }
    return false;
}

// ---------------------------------------------------------------------------
// resolve
// ---------------------------------------------------------------------------
static std::string safeStr(uintptr_t p) {
    char buf[160] = {0};
    if (p) il2::readStr(p, buf, sizeof(buf));
    return std::string(buf);
}

// isi field/method/static_fields dari dump offline (definisinya di bawah)
static void loadDumpInfo(const char* name, const char* ns);

bool resolve(const char* name, const char* ns) {
    g_info = KlassInfo{};
    if (!mem::attached() || !il2::ready()) {
        logx::add("[exp] belum attach / lib il2cpp belum ketemu");
        return false;
    }
    if (!name || !*name) return false;

    const uintptr_t k = il2::findKlass(name, ns);
    if (!k) {
        logx::add("[exp] class '%s' nggak ketemu di memori", name);
        return false;
    }

    g_info.addr = k;
    g_info.source = il2::lastKlassSource();
    // salin dulu: klassName/klassNamespace pakai buffer statis yang sama
    char nb[160];
    snprintf(nb, sizeof(nb), "%s", il2::klassName(k));
    g_info.name = nb;
    snprintf(nb, sizeof(nb), "%s", il2::klassNamespace(k));
    g_info.ns = nb;
    {
        uintptr_t img = 0;
        if (mem::rd(k + il2::kKlass_Image, img) && img) {
            uintptr_t imgName = 0;
            if (mem::rd(img, imgName) && imgName) g_info.image = safeStr(imgName);
        }
    }

    // ---- fields ----
    // calibrateFieldLayout(): coba kalibrasi STRUKTUR dulu (buat game yang
    // metadata-nya dimodif -- FieldInfo-nya punya pointer balik ke kelas dan
    // token-nya di belakang offset, kaya MLBB), baru kalibrasi lama il2cpp.
    FieldLayout fl;
    if (calibrateFieldLayout(k, fl)) {
        g_info.fieldsPtrOff = fl.ptrOff;
        g_info.fieldStride  = fl.stride;
        g_info.fieldOffSlot = fl.offSlot;

        readFields(k, fl, g_info.fields, 512);
        logx::add("[exp] fields: ptr+0x%x stride 0x%x slot 0x%x (%s) -> %zu field",
                  fl.ptrOff, fl.stride, fl.offSlot,
                  fl.structural ? "layout struktur" : "layout lama", g_info.fields.size());
    } else {
        logx::add("[exp] fields: kalibrasi gagal (class tanpa field?)");
        // Layout-nya nggak ketemu: qword mentah di dalam Il2CppClass dicetak
        // (2 baris, 0x20..0x1A0) -- ini yang dipakai buat nemuin di mana array
        // FieldInfo/MethodInfo sebenarnya di game yang struct-nya dimodif.
        for (int row = 0; row < 3; ++row) {
            uint64_t q[16] = {0};
            const uint32_t base = 0x20 + (uint32_t)row * 0x80;
            mem::read(k + base, q, sizeof(q));
            char line[400];
            int n = 0;
            for (int j = 0; j < 16; ++j)
                n += snprintf(line + n, sizeof(line) - n, " %llx",
                              (unsigned long long)q[j]);
            logx::add("[exp]   klass+0x%03x:%s", base, line);
        }
    }

    // ---- methods ----
    //
    // Urutan prioritas:
    //   1. POOL MethodInfo global (metapool) -- ini yang LENGKAP di game yang
    //      array method kelasnya cuma terisi sebagian (MLBB: 20 dari 121)
    //   2. kalibrasi array MethodInfo di dalam Il2CppClass (il2cpp biasa)
    //   3. dump offline
    MethodLayout ml;
    if (metapool::ready() && metapool::forKlass(k, g_info.methods)) {
        logx::add("[exp] methods: dari POOL global -> %zu method", g_info.methods.size());
    } else if (calibrateMethodLayout(k, ml)) {
        g_info.methodsPtrOff = ml.ptrOff;
        g_info.methodNameOff = ml.nameOff;

        readMethods(k, ml, g_info.methods, 2048);
        logx::add("[exp] methods: ptr+0x%x stride 0x%x name+0x%x (%s) -> %zu method",
                  ml.ptrOff, ml.stride, ml.nameOff,
                  ml.structural ? "layout struktur" : "layout lama",
                  g_info.methods.size());
    } else {
        logx::add("[exp] methods: kalibrasi gagal");
    }

    // ---- dump offline: tipe field, penanda static, + cadangan kalau runtime
    //      nggak bisa di-kalibrasi (MLBB: metadata dipecah 3 lib) ----
    loadDumpInfo(name, ns);

    g_info.ok = true;
    logx::add("[exp] %s%s%s @0x%lx | %zu field, %zu method | %s | sumber=%s%s",
              g_info.ns.c_str(), g_info.ns.empty() ? "" : ".", g_info.name.c_str(),
              (unsigned long)k, g_info.fields.size(), g_info.methods.size(),
              g_info.image.c_str(), g_info.source.c_str(),
              g_info.fromDump ? " (field/method dari dump)" : "");
    return true;
}

// ---------------------------------------------------------------------------
// dump offline: field + method + static_fields
//
// Dipakai DUA cara:
//   1. runtime berhasil kalibrasi -> cuma dipakai buat nempel nama tipe dan
//      nandain mana field statis (metadata runtime nggak nyimpen itu)
//   2. runtime gagal (klasik MLBB) -> field/method-nya diambil dari dump
// ---------------------------------------------------------------------------
static void loadDumpInfo(const char* name, const char* ns) {
    if (!dump::loaded()) return;

    std::vector<const dump::ClassEntry*> hits;
    if (!dump::find(name, ns, hits) || hits.empty()) return;
    const dump::ClassEntry* e = hits[0];

    dump::ClassDetail det;
    if (!dump::detail(*e, det)) return;
    g_info.dumpModule = e->module;
    if (g_info.image.empty()) g_info.image = e->assembly;

    // kumpulkan offset instance dari dump (buat fallback + penanda static).
    //
    // Syaratnya "field ATAU method dari runtime kosong", bukan cuma field: kelas
    // tanpa field instance itu banyak (System.Array, interface, enum kosong) dan
    // dulu kelas begitu nggak dapet apa-apa dari dump (0 field, 0 method) walau
    // method-nya ada di dump.
    const bool wantFields  = g_info.fields.empty()  && !det.fields.empty();
    const bool wantMethods = g_info.methods.empty() && !det.methods.empty();
    // Method dari pool runtime bisa aja DAPET tapi namanya kosong (space nama
    // yang delta-nya belum kekalibrasi) -- itu juga perlu diisi dari dump.
    bool missingNames = false;
    for (const auto& m : g_info.methods)
        if (m.name.empty()) { missingNames = true; break; }
    if (wantFields || wantMethods || missingNames) {
        if (wantFields) g_info.fromDump = true;
        for (const auto& df : det.fields) {
            Field f;
            f.name = df.name;
            f.offset = df.offset;
            f.token = df.token;
            f.isStatic = df.isStatic;
            f.typeName = df.type;
            const uint8_t te = dump::guessTypeEnum(df.type);
            f.typeEnum = (te == 0xFF) ? 0 : te;
            g_info.fields.push_back(f);
        }
        if (g_info.methods.empty()) {
            g_info.methodsPtrOff = 0;
            for (const auto& dm : det.methods) {
                if (!dm.hasRva) continue;
                Method m;
                m.name = dm.name;
                m.rva = dm.rva;
                // module pemilik kode: tag per-method (dump v3.4) kalau ada,
                // kalau nggak ya module kelasnya (dump lama).
                m.module = dm.module.empty() ? e->module : dm.module;
                // Alamat absolut = base module itu + RVA. Base-nya bisa dibaca
                // selama module-nya ke-load di proses (MLBB: liblogic/libfirst/
                // libcsharp semuanya ke-load, jadi method lintas-module pun
                // alamatnya ketemu -- dulu ini 0 semua).
                uintptr_t mbase = (m.module.empty() || m.module == il2::libName())
                                      ? il2::base()
                                      : mem::base(m.module.c_str());
                m.ptr = (mbase && dm.rva) ? mbase + (uintptr_t)dm.rva : 0;
                g_info.methods.push_back(m);
            }
        }
        // Method yang METADATA-nya berhasil dibaca runtime tapi NAMANYA nggak
        // bisa di-resolve (space nama yang delta-nya belum kekalibrasi): namanya
        // dilengkapi dari dump lewat cocok-cocokan RVA. Jadi RVA tetap dari
        // runtime (asli), cuma labelnya yang dipinjam -- nggak ada "unknown".
        size_t filled = 0, filledRva = 0;
        for (auto& m : g_info.methods) {
            // nama kosong -> ambil dari dump lewat RVA yang sama
            if (m.name.empty() && m.rva) {
                for (const auto& dm : det.methods) {
                    if (!dm.hasRva || dm.rva != m.rva) continue;
                    m.name = dm.name;
                    ++filled;
                    break;
                }
            }
            // RVA kosong (MethodInfo-nya belum dibikin engine) -> ambil dari dump
            // lewat nama yang sama. Ini yang bikin hasil dump NOL "unknown":
            // daftar method + nama tetap 100% dari runtime, RVA-nya dilengkapi
            // dari dump offline kalau ada (kalau nggak ada, RVA-nya emang belum
            // ke-resolve engine -- makanya dump sambil in-match lebih lengkap).
            if (!m.rva && !m.name.empty()) {
                for (const auto& dm : det.methods) {
                    if (!dm.hasRva) continue;
                    if (dm.name.find(m.name) == std::string::npos) continue;
                    m.rva = dm.rva;
                    if (m.module.empty()) m.module = dm.module.empty() ? e->module : dm.module;
                    ++filledRva;
                    break;
                }
            }
        }
        if (filled || filledRva)
            logx::add("[exp] dilengkapi dari dump: %zu nama, %zu RVA", filled, filledRva);

        if (wantFields || wantMethods)
            logx::add("[exp] runtime nggak bisa baca %s -- diambil dari dump "
                      "(%zu field, %zu method, module %s)",
                      wantFields ? "field" : "method",
                      det.fields.size(), g_info.methods.size(), e->module.c_str());

        // Diagnosa layout: kalau kalibrasi FieldInfo gagal padahal slotnya benar,
        // biasanya strukturnya dimodif game (MLBB begitu). Ini yang dipakai buat
        // ngecek: image-nya kebaca nggak, dan qword depan bentuknya apa.
        // (cuma pas field yang diambil dari dump -- kelas tanpa field nggak perlu)
        if (wantFields) {
        uint64_t q[4] = {0};
        mem::read(g_info.addr, q, sizeof(q));
        char imgName[96] = {0};
        { uintptr_t np = 0;
          if (q[0] && mem::rd((uintptr_t)q[0], np) && np)
              il2::readStr(np, imgName, sizeof(imgName)); }
        logx::add("[exp] klass 0x%lx image=0x%lx '%s' q0..3=%llx %llx %llx %llx",
                  (unsigned long)g_info.addr, (unsigned long)q[0], imgName,
                  (unsigned long long)q[0], (unsigned long long)q[1],
                  (unsigned long long)q[2], (unsigned long long)q[3]);
        }
    } else {
        // nempelin nama tipe ke field hasil runtime (cocokin nama)
        for (auto& f : g_info.fields) {
            for (const auto& df : det.fields) {
                if (df.name != f.name) continue;
                f.typeName = df.type;
                f.isStatic = df.isStatic;
                break;
            }
        }
    }

    // static_fields: field statis offset-nya relatif ke pointer ini, bukan ke
    // objek. Offsetnya beda-beda antar versi il2cpp, jadi dicoba dari yang
    // paling umum (0xa8) terus diverifikasi: alamat field statis TERAKHIR
    // harus masih kebaca.
    uint32_t maxStatic = 0;
    for (const auto& f : g_info.fields)
        if (f.isStatic && f.offset + 8 > maxStatic) maxStatic = f.offset + 8;
    if (!maxStatic) return;

    static const uint32_t cands[] = {0xa8, 0xb0, 0xa0, 0x98, 0x90, 0x88,
                                     0xb8, 0xc0, 0xc8, 0xd0, 0x80};
    for (uint32_t off : cands) {
        uintptr_t sf = 0;
        if (!mem::rd(g_info.addr + off, sf) || !sf) continue;
        if (!mem::canRead(sf, maxStatic)) continue;
        g_info.statics = sf;
        g_info.staticsPtrOff = off;
        logx::add("[exp] static_fields +0x%x -> 0x%lx (%u byte field statis)",
                  off, (unsigned long)sf, maxStatic);
        return;
    }
    logx::add("[exp] static_fields nggak ketemu di +0x80..+0xd0 (field statis tetap "
              "ditampilin, nilainya nggak kebaca)");
}

std::vector<uintptr_t> instances(size_t max) {
    std::vector<uintptr_t> out;
    if (g_info.addr) il2::findInstances(g_info.addr, out, max);
    return out;
}

// ---------------------------------------------------------------------------
// baca / tulis nilai
// ---------------------------------------------------------------------------
bool readValue(uintptr_t inst, uint32_t off, FType t, double& num, char* str, size_t strCap) {
    if (!inst) return false;
    num = 0;
    if (str && strCap) str[0] = 0;
    switch (t) {
        case FType::Ptr: {
            uint64_t v = 0;
            if (!mem::rd(inst + off, v)) return false;
            num = (double)v;
            return true;
        }
        case FType::Int32:  { int32_t  v = 0; if (!mem::rd(inst + off, v)) return false; num = v; return true; }
        case FType::UInt32: { uint32_t v = 0; if (!mem::rd(inst + off, v)) return false; num = v; return true; }
        case FType::Int64:  { int64_t  v = 0; if (!mem::rd(inst + off, v)) return false; num = (double)v; return true; }
        case FType::Float:  { float    v = 0; if (!mem::rd(inst + off, v)) return false; num = v; return true; }
        case FType::Double: { double   v = 0; if (!mem::rd(inst + off, v)) return false; num = v; return true; }
        case FType::Bool:   { uint8_t  v = 0; if (!mem::rd(inst + off, v)) return false; num = v ? 1 : 0; return true; }
        case FType::String: {
            uintptr_t p = 0;
            if (!mem::rd(inst + off, p)) return false;
            num = (double)p;
            if (str && strCap && p) {
                // Il2CppString: +0x10 = length (int32), +0x14 = char16 data
                int32_t len = 0;
                if (mem::rd(p + 0x10, len) && len > 0 && len < 128) {
                    size_t n = (size_t)len;
                    if (n >= strCap) n = strCap - 1;
                    std::vector<uint16_t> w(n + 1, 0);
                    if (mem::read(p + 0x14, w.data(), n * 2)) {
                        for (size_t i = 0; i < n; ++i) str[i] = (char)(w[i] < 0x80 ? w[i] : '?');
                        str[n] = 0;
                    }
                }
            }
            return true;
        }
    }
    return false;
}

bool writeValue(uintptr_t inst, uint32_t off, FType t, double num, const char* str) {
    if (!inst) return false;
    switch (t) {
        case FType::Ptr: {
            uint64_t v = str ? strtoull(str, nullptr, 0) : (uint64_t)num;
            return mem::wr(inst + off, v);
        }
        case FType::Int32:  { int32_t  v = str ? (int32_t)strtol(str, nullptr, 0)  : (int32_t)num;  return mem::wr(inst + off, v); }
        case FType::UInt32: { uint32_t v = str ? (uint32_t)strtoul(str, nullptr, 0) : (uint32_t)num; return mem::wr(inst + off, v); }
        case FType::Int64:  { int64_t  v = str ? (int64_t)strtoll(str, nullptr, 0) : (int64_t)num;  return mem::wr(inst + off, v); }
        case FType::Float:  { float    v = (float)num; return mem::wr(inst + off, v); }
        case FType::Double: { double   v = num;        return mem::wr(inst + off, v); }
        case FType::Bool:   { uint8_t  v = num ? 1 : 0; return mem::wr(inst + off, v); }
        case FType::String: {
            if (!str) return false;
            // tulis ASCII ke char16 data (byte-length di-set kalau ada)
            uintptr_t p = 0;
            if (!mem::rd(inst + off, p) || !p) return false;
            size_t n = strlen(str);
            if (n > 120) n = 120;
            std::vector<uint16_t> w(n + 1, 0);
            for (size_t i = 0; i < n; ++i) w[i] = (uint16_t)(unsigned char)str[i];
            if (!mem::write(p + 0x14, w.data(), n * 2)) return false;
            int32_t len = (int32_t)n;
            return mem::wr(p + 0x10, len);
        }
    }
    return false;
}

// ---------------------------------------------------------------------------
// dump
// ---------------------------------------------------------------------------
static void mkdirP(const char* path) {
    char tmp[256];
    snprintf(tmp, sizeof(tmp), "%s", path);
    for (char* p = tmp + 1; *p; ++p) {
        if (*p == '/') { *p = 0; ::mkdir(tmp, 0777); *p = '/'; }
    }
    ::mkdir(tmp, 0777);
}

bool dumpTo(const char* dir, std::string* pathOut) {
    if (!g_info.ok || !g_info.addr) return false;
    mkdirP(dir);

    char path[512];
    snprintf(path, sizeof(path), "%s/%s.txt", dir, g_info.name.c_str());
    std::ofstream f(path, std::ios::trunc);
    if (!f.is_open()) {
        logx::add("[exp] gagal nulis %s", path);
        return false;
    }

    f << "// panxcz_libtool dump\n";
    f << "// package : " << g_info.ns << "::" << g_info.name << "\n";
    f << "// assembly: " << g_info.image << "\n";
    f << "// klass   : 0x" << std::hex << g_info.addr << std::dec << "\n";
    f << "// il2cpp  : " << il2::libName() << " base=0x" << std::hex << il2::base() << std::dec << "\n";
    f << "\nclass " << (g_info.ns.empty() ? "" : g_info.ns + ".") << g_info.name << " // fields="
      << g_info.fields.size() << " methods=" << g_info.methods.size() << "\n{\n";
    for (const auto& fl : g_info.fields) {
        char line[256];
        snprintf(line, sizeof(line), "    // 0x%04X token 0x%08X\n    void* %s; // 0x%04X\n",
                 fl.offset, fl.token, fl.name.c_str(), fl.offset);
        f << line;
    }
    f << "\n";
    for (const auto& m : g_info.methods) {
        char line[320];
        snprintf(line, sizeof(line), "    // RVA: 0x%llX\n    void %s(); // 0x%llX\n",
                 (unsigned long long)m.rva, m.name.c_str(), (unsigned long long)m.ptr);
        f << line;
    }
    f << "}\n";
    f.close();

    if (pathOut) *pathOut = path;
    logx::add("[exp] dump ditulis: %s (%zu field, %zu method)", path,
              g_info.fields.size(), g_info.methods.size());
    return true;
}

}  // namespace explore

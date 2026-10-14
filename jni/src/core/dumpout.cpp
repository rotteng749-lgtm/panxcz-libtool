// ============================================================================
//  dumpout.cpp -- lihat Includes/core/dumpout.h buat penjelasan lengkapnya.
//
//  Ringkas: pindai region data module il2cpp, cari cell (qword) yang isinya
//  Il2CppClass* -- itu tabel class-slot yang sama kayak yang nulis dump
//  Il2CppDumper. Tiap kelas dibaca (nama/namespace/image/field/method + RVA)
//  terus ditulis sebagai .cs (+ opsional .json / .h / .ptrs.tsv).
//
//  Nggak ada nama lib yang di-bake: module yang dipindai diambil dari maps
//  proses yang lagi di-attach. Tanpa root nggak jalan (butuh /proc/pid/mem).
// ============================================================================
#include "core/dumpout.h"

#include "core/metapool.h"

#include <sys/stat.h>
#include <sys/types.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <set>
#include <unordered_map>
#include <string>
#include <vector>

#include "core/abort.h"
#include "core/explore.h"
#include "core/offlinedump.h"
#include "core/il2cpp.h"
#include "core/log.h"
#include "core/mem.h"
#include "core/target.h"

namespace dumpout {
namespace {

// di-set dari menu (tombol Stop) -- dicek di loop pindai & loop per-kelas
std::atomic<bool> g_abort{false};

// ---------------------------------------------------------------------------
// struktur kerja
// ---------------------------------------------------------------------------
struct Rec {
    uintptr_t klass = 0;
    uintptr_t cell  = 0;          // alamat cell di dalam module (=> slot RVA)
    std::string module;           // module pemilik cell
    uint64_t slotRva = 0;
    std::string name, ns, image;
    uintptr_t   parent = 0;
    // Identitas KATALOG (dari dump offline). Kelas yang Il2CppClass*-nya belum
    // ada di runtime (slotnya belum ke-resolve / memang nggak punya slot) tetap
    // bisa ditulis lengkap pakai ini -- itulah yang bikin dump nggak "kepotong"
    // (referensi Il2CppDumper: SEMUA kelas, bukan cuma yang kepakai match).
    uint32_t    typeDefIndex = 0;
    uint64_t    token = 0;
    dump::Kind  kind = dump::Kind::Class;
    bool        catalogOnly = false;   // nggak ada Il2CppClass* runtime
    std::vector<explore::Field>  fields;
    std::vector<explore::Method> methods;
    // Entri katalog asal class ini. Dipakai step 6 buat menyatu daftar method
    // katalog (signature penuh) dengan hasil runtime (RVA valid) SATU KELAS
    // SEKALI JALAN -- bukan semua kelas sekaligus (itu bikin OOM: 28 rb kelas
    // x ~200 rb method = ratusan MB string).
    const dump::ClassEntry* catEntry = nullptr;
};

// Nama polos method: buang tipe return + parameter, sisain nama.
// "static void SetPreInvite(ulong a, bool b)" -> "SetPreInvite"
static std::string trimStr(const std::string& s) {
    size_t b = 0;
    while (b < s.size() && (s[b] == ' ' || s[b] == '\t')) ++b;
    size_t e = s.size();
    while (e > b && (s[e - 1] == ' ' || s[e - 1] == '\t')) --e;
    return s.substr(b, e - b);
}

static std::string bareMethod(const std::string& s) {
    const size_t par = s.find('(');
    std::string head = par == std::string::npos ? s : s.substr(0, par);
    const size_t sp = head.find_last_of(" \t");
    if (sp != std::string::npos) head = head.substr(sp + 1);
    return trimStr(head);
}

// ---------------------------------------------------------------------------
// util
// ---------------------------------------------------------------------------
std::string baseName(const std::string& path) {
    const size_t s = path.find_last_of('/');
    return s == std::string::npos ? path : path.substr(s + 1);
}

std::string stripSub(const std::string& pkg) {
    const size_t c = pkg.find(':');
    return c == std::string::npos ? pkg : pkg.substr(0, c);
}

void mkdirP(const char* path) {
    char tmp[512];
    snprintf(tmp, sizeof(tmp), "%s", path);
    for (char* p = tmp + 1; *p; ++p) {
        if (*p == '/') { *p = 0; ::mkdir(tmp, 0777); *p = '/'; }
    }
    ::mkdir(tmp, 0777);
}



bool colsWanted(const Options& o, const char* k) {
    if (o.cols.empty()) return true;
    for (const auto& c : o.cols) if (c == k) return true;
    return false;
}

std::string esc(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 8);
    for (char c : s) {
        if (c == '"' || c == '\\') { out += '\\'; out += c; }
        else if ((unsigned char)c < 0x20) out += ' ';
        else out += c;
    }
    return out;
}

std::string hex(uint64_t v) {
    char b[32];
    snprintf(b, sizeof(b), "0x%llX", (unsigned long long)v);
    return b;
}

// ---------------------------------------------------------------------------
// kandidat Il2CppClass
//
// Dua tingkat, dan yang dipakai buat MENERIMA cuma tingkat dua:
//   1. nama di +0x10 kelihatan nama C# (huruf/angka/_/`/<>/./$)  -> cek murah,
//      dipakai buat nyaring duplikat scan
//   2. isValidKlass(): image-nya Il2CppImage yang namanya *.dll + namespace
//      printable. Ini yang bikin sampah kebuang -- tabel data module penuh
//      pointer acak yang qword-nya kebetulan nunjuk ke string printable
//      (percobaan pertama: 62 rb "kelas" palsu dengan nama kaya " 5?|z").
// ---------------------------------------------------------------------------
bool csNameOk(const char* nm) {
    if (!nm || !nm[0]) return false;
    bool alpha = false;
    for (const char* q = nm; *q; ++q) {
        const unsigned char c = (unsigned char)*q;
        if (isalpha(c)) { alpha = true; continue; }
        if (isdigit(c)) continue;
        if (c == '_' || c == '`' || c == '<' || c == '>' || c == '.' || c == '$' || c == '-') continue;
        return false;
    }
    return alpha;
}

bool nameOk(uintptr_t k, char* nameOut, size_t cap) {
    if (!k || k < 0x10000 || (k & 7)) return false;
    if (!mem::canRead(k, 0x20)) return false;
    uintptr_t np = 0;
    if (!mem::rd(k + il2::kKlass_Name, np) || !np) return false;
    if (!mem::canRead(np, 4)) return false;

    char nm[128];
    if (!il2::readStr(np, nm, sizeof(nm))) return false;
    if (!csNameOk(nm)) return false;
    if (nameOut && cap) snprintf(nameOut, cap, "%s", nm);
    return true;
}

// ---------------------------------------------------------------------------
// pindai satu region: cari cell Il2CppClass*
// ---------------------------------------------------------------------------
struct Hit { uintptr_t cell; uintptr_t klass; };

// Progres pindai (dipakai progress bar di UI): byte yang udah dipindai dibagi
// total byte yang bakal dipindai SEMUA module. Di-set sebelum pindai mulai.
size_t g_scanBytes = 0;
size_t g_scanTotalBytes = 1;

void scanRegion(const mem::Region& r, std::vector<Hit>& hits, size_t* mb,
                std::vector<uint8_t>& buf, size_t* looseOut) {
    static const size_t kChunk = 4u << 20;
    if (buf.size() != kChunk) buf.resize(kChunk);

    int chunks = 0;
    for (uintptr_t a = r.start; a + 16 <= r.end; ) {
        if (g_abort.load()) return;
        // progres + denyut: UI bisa lihat kerjanya masih hidup & bisa nge-Stop.
        // Tanpa denyut, device lambat kelihatan seperti "macet" dan tombol Stop
        // nggak ngefek (dua-duanya masalah lama).
        if ((++chunks & 15) == 0) {
            abortx::beat("pindai class-slot",
                         (float)((double)g_scanBytes / (double)g_scanTotalBytes));
        }
        size_t n = (size_t)(r.end - a);
        if (n > kChunk) n = kChunk;
        n &= ~(size_t)7;
        if (!n) break;
        if (!mem::read(a, buf.data(), n)) { a += n; g_scanBytes += n; continue; }
        if (mb) *mb += n >> 20;
        g_scanBytes += n;

        for (size_t i = 0; i + 8 <= n; i += 8) {
            uint64_t q = 0;
            memcpy(&q, buf.data() + i, 8);
            // ---- saringan MURAH (tanpa syscall) dulu ----
            // Il2CppClass* selalu: nilai besar, sejajar 8, dan nunjuk ke region
            // DATA (kelas nggak pernah di segmen kode). Cek ini in-memory saja,
            // jadi qword yang jelas bukan pointer nggak ngebuang satu pread.
            // Ini yang bikin pindai tetap masuk akal di device lambat.
            if (q < 0x10000u) continue;
            const uintptr_t k = mem::untag((uintptr_t)q);
            if (k & 7) continue;
            const mem::Region* kr = mem::regionOf(k);
            if (!kr || !kr->readable() || kr->exec()) continue;
            if (!nameOk(k, nullptr, 0)) continue;
            if (looseOut) ++*looseOut;
            hits.push_back({a + i, k});
        }
        a += n;
    }
}

// ---------------------------------------------------------------------------
// daftar method DARI DUMP OFFLINE
//
// Dipakai buat kelas yang MethodInfo-nya belum ada di pool runtime -- di MLBB
// itu kejadian nyata (BattleManager: 0 dari 121 di pool waktu game baru jalan,
// tapi 121 lengkap di dump offline versi yang sama). Syaratnya cuma: kelasnya
// ada di dump offline. Nama + RVA di sana asli hasil Il2CppDumper/frida, jadi
// tetap sah buat patch -- dan formatnya ("// RVA: 0x.. (lib.so)" + baris nama)
// sama persis dengan yang ditulis tool ini, jadi bisa dibaca balik.
// ---------------------------------------------------------------------------
size_t methodsFromDump(const std::string& name, const std::string& ns,
                       const std::string& module, std::vector<explore::Method>& out) {
    out.clear();
    if (!dump::loaded() || name.empty()) return 0;
    std::vector<const dump::ClassEntry*> hits;
    if (!dump::find(name.c_str(), ns.empty() ? nullptr : ns.c_str(), hits) || hits.empty())
        return 0;
    dump::ClassDetail det;
    if (!dump::detail(*hits[0], det)) return 0;

    for (const auto& dm : det.methods) {
        if (!dm.hasRva || !dm.rva) continue;
        explore::Method m;
        m.name   = dm.name;                 // udah termasuk parameter
        m.rva    = dm.rva;
        m.module = dm.module.empty()
                       ? (hits[0]->module.empty() ? module : hits[0]->module)
                       : dm.module;
        m.ptr = 0;                          // metode belum dibikin engine -> nggak ada alamat runtime
        out.push_back(std::move(m));
    }
    return out.size();
}

// Isi field + method dari blok tubuh kelas di dump offline (buat kelas yang
// Il2CppClass*-nya nggak ada di runtime -- supaya isinya tetap kekatalog).
// `dump::detail` mem-parse satu tubuh kelas; panggil SEKALI buat dua-duanya.
//
// CATATAN method: method TANPA RVA tetap ikut (signature-nya masih valid dan
// itu informasi yang mahal -- dulu dibuang, sehingga dump native kehilangan
// ribuan method). RVA-nya nol, nanti dilengkapi dari runtime kalau ada.
size_t fillFromDump(const dump::ClassEntry& e, std::vector<explore::Field>& fo,
                    std::vector<explore::Method>& mo) {
    fo.clear();
    mo.clear();
    dump::ClassDetail det;
    if (!dump::detail(e, det)) return 0;
    for (const auto& df : det.fields) {
        if (df.name.empty()) continue;
        explore::Field f;
        f.name     = df.name;
        f.typeName = df.type;
        f.offset   = df.offset;
        f.token    = df.token;
        f.isStatic = df.isStatic;
        f.typeEnum = dump::guessTypeEnum(df.type);
        fo.push_back(std::move(f));
    }
    for (const auto& dm : det.methods) {
        if (dm.name.empty()) continue;
        explore::Method m;
        m.name   = dm.name;                 // udah termasuk parameter
        m.rva    = dm.hasRva ? dm.rva : 0;  // 0 = belum ketauan, dilengkapi runtime
        m.module = dm.module.empty() ? e.module : dm.module;
        m.ptr    = 0;                       // belum dibikin engine -> nggak ada alamat runtime
        mo.push_back(std::move(m));
    }
    return mo.size();
}

// Isi identitas katalog (namespace/token/kind) dari entri dump -- dipakai buat
// SEMUA kelas yang ikut katalog, biar header `// Namespace: ...; Token: 0x...`
// dan keyword (class/struct/enum/interface) yang ditulis nggak palsu.
void setCatalogIdentity(Rec& r, const dump::ClassEntry& e) {
    if (r.ns.empty() && !e.ns.empty()) r.ns = e.ns;
    r.typeDefIndex = e.typeDefIndex;
    r.token        = e.token;
    r.kind         = e.kind;
    if (r.image.empty() && !e.assembly.empty()) r.image = e.assembly;
}

// ---------------------------------------------------------------------------
// module il2cpp yang dipindai
// ---------------------------------------------------------------------------
bool knownIl2cppModule(const std::string& b) {
    static const char* kNames[] = {
        "libil2cpp.so", "libcsharp.so", "liblogic.so", "libfirst.so",
        "libunity.so", "libgame.so", "libmonobdwgc-2.0.so",
    };
    for (const char* n : kNames) if (b == n) return true;
    return b.find("il2cpp") != std::string::npos;
}

std::vector<std::string> moduleNames(const Options& o, bool* allFound) {
    std::vector<std::string> out;
    if (!o.libs.empty()) { if (allFound) *allFound = true; return o.libs; }

    std::set<std::string> seen;
    for (const auto& r : mem::regions()) {
        if (r.name.empty() || !r.readable()) continue;
        const std::string b = baseName(r.name);
        if (b.size() < 4 || b.compare(b.size() - 3, 3, ".so") != 0) continue;
        if (!knownIl2cppModule(b) || seen.count(b)) continue;
        seen.insert(b);
        out.push_back(b);
    }
    if (allFound) *allFound = !out.empty();

    if (!o.allLibs) {
        const std::string cur = il2::libName();
        if (!cur.empty()) {
            // lib yang lagi dipakai duluan, sisanya dibuang
            out.clear();
            out.push_back(cur);
        } else if (out.size() > 1) {
            out.resize(1);
        }
    }
    return out;
}

// ---------------------------------------------------------------------------
// baca metadata tiap kelas
// ---------------------------------------------------------------------------
void readMeta(Rec& r) {
    char buf[200];
    uintptr_t p = 0;
    if (mem::rd(r.klass + il2::kKlass_Name, p) && p) {
        il2::readStr(p, buf, sizeof(buf));
        r.name = buf;
    }
    if (mem::rd(r.klass + il2::kKlass_Namespace, p) && p) {
        il2::readStr(p, buf, sizeof(buf));
        r.ns = buf;
    }
    if (mem::rd(r.klass + il2::kKlass_Image, p) && p) {
        uintptr_t np = 0;
        if (mem::rd(p, np) && np) {
            il2::readStr(np, buf, sizeof(buf));
            r.image = buf;
        }
    }
}

// Offset 'parent' di dalam Il2CppClass beda antar versi il2cpp. Dicari dengan
// statistik: offset mana yang isinya TERBANYAK menunjuk ke kelas lain yang
// juga ada di tabel (kalau nunjuk ke kelas di tabel, hampir pasti parent).
uint32_t parentOffset(const std::vector<Rec>& recs) {
    std::set<uintptr_t> S;
    for (const auto& r : recs) S.insert(r.klass);

    const size_t sample = recs.size() < 4000 ? recs.size() : 4000;
    uint32_t bestOff = 0;
    size_t   bestN = 0;
    for (uint32_t off = 0x28; off <= 0x90; off += 8) {
        if (abortx::requested()) return 0;
        abortx::beat("cari offset parent");
        size_t n = 0;
        for (size_t i = 0; i < sample; ++i) {
            uintptr_t v = mem::untag(mem::rdp<uintptr_t>(recs[i].klass + off));
            if (!v || v == recs[i].klass) continue;      // self-ref bukan parent
            if (S.count(v)) ++n;
        }
        if (n > bestN) { bestN = n; bestOff = off; }
    }
    // harus "mayoritas" kelas punya parent di tabel, biar bukan kebetulan
    if (bestN < sample / 4) return 0;
    logx::add("[dumpout] offset parent = +0x%x (%zu/%zu kelas nunjuk ke kelas lain)",
              bestOff, bestN, sample);
    return bestOff;
}

}  // namespace

// versi app dari dumpsys (tool jalan sebagai root, jadi bisa manggil dumpsys).
// Dipakai buat nama folder <pkg>/<versi>/ -- sama kaya konvensi dumper lain.
// (Definisinya di LUAR anonymous namespace: dipakai juga sama core/libdump.)
std::string pkgVersion(const std::string& pkg) {
    if (pkg.empty()) return "unknown";
    char cmd[256];
    snprintf(cmd, sizeof(cmd), "dumpsys package %s 2>/dev/null | grep -m1 versionName", pkg.c_str());
    FILE* f = popen(cmd, "r");
    if (!f) return "unknown";
    char line[256] = {0};
    if (!fgets(line, sizeof(line), f)) { pclose(f); return "unknown"; }
    pclose(f);

    char* eq = strchr(line, '=');
    if (!eq) return "unknown";
    std::string v = eq + 1;
    while (!v.empty() && (v.back() == '\n' || v.back() == '\r' || v.back() == ' ')) v.pop_back();
    bool sane = !v.empty() && isdigit((unsigned char)v[0]) && v.find('.') != std::string::npos;
    return sane ? v : "unknown";
}

// Stop itu SATU tombol buat semua kerja berat: flag di sini dipakai dumper,
// tapi pool MethodInfo (metapool), kalibrasi layout (explore), dan memory
// scanner nanya ke abortx yang sama -- jadi nggak ada lagi "Stop diklik tapi
// kerjanya jalan terus".
void requestAbort()    { g_abort.store(true); abortx::request("tombol Stop"); }
void clearAbort()      { g_abort.store(false); abortx::clear(); }
bool abortRequested()  { return g_abort.load() || abortx::requested(); }

// ---------------------------------------------------------------------------
// run
// ---------------------------------------------------------------------------
bool run(const Options& o, Result& res) {
    res = Result{};
    // Satu dump = satu kerja: mulai dari keadaan bersih. (Dulu flag batal
    // di-reset di TENGAH run -- kalau user nge-Stop pas fase awal, permintaan
    // itu jadi kehapus sendiri dan kerjanya lanjut terus.)
    g_abort.store(false);
    abortx::clear();
    mem::resetReadFailures();
    abortx::beat("dump: mulai");

    // Timer per fase: tiap fase dicetak berapa lama. Kalau di device lain ada
    // fase yang lambat, log-nya langsung nunjukin fase mana (dulu cuma ada
    // "mulai"/"selesai", jadi kelihatannya macet tanpa jejak). Sekalian
    // nge-denyut ke UI biar tombol Stop tetap masuk akal.
    double phT0 = abortx::nowSec();
    auto phase = [&phT0](const char* nm) {
        logx::add("[dumpout] fase '%s': %.1f s", nm, abortx::nowSec() - phT0);
        phT0 = abortx::nowSec();
        abortx::beat(nm);
    };

    if (!mem::attached() || !il2::ready()) {
        logx::add("[dumpout] belum attach / lib il2cpp belum ketemu -- nggak bisa dump");
        return false;
    }

    bool ok = false;
    std::vector<std::string> mods = moduleNames(o, &ok);
    if (mods.empty()) {
        logx::add("[dumpout] nggak ada module il2cpp yang ke-load di proses ini");
        return false;
    }

    logx::add("[dumpout] module dipindai: %zu (%s%s)", mods.size(), mods[0].c_str(),
              o.allLibs ? ", ..." : " -- pakai --all-libs buat semua");
    for (const auto& m : mods)
        logx::add("[dumpout]   %s base=0x%lx", m.c_str(),
                  (unsigned long)mem::base(m.c_str()));

    // ---- 1. pindai cell class-slot -----------------------------------------
    abortx::beat("pindai class-slot", 0.0f);
    abortx::span(0.02f, 0.28f);           // 2%..30% = pindai class-slot
    std::vector<Hit> hits;
    std::vector<std::string> hitModule;      // sejajar dengan hits
    std::vector<uint8_t> scanBuf;
    size_t loose = 0;
    // total byte yang bakal dipindai (buat progress bar) -- dihitung dulu biar
    // bar-nya nggak nunggu selesai baru gerak
    g_scanBytes = 0;
    g_scanTotalBytes = 0;
    for (const auto& mod : mods)
        for (const auto& r : mem::regions()) {
            if (r.name.empty() || !r.readable()) continue;
            if (baseName(r.name) != mod) continue;
            if (r.exec() && !r.writable()) continue;
            g_scanTotalBytes += r.size();
        }
    if (!g_scanTotalBytes) g_scanTotalBytes = 1;

    for (const auto& mod : mods) {
        const size_t before = hits.size();
        for (const auto& r : mem::regions()) {
            if (r.name.empty() || !r.readable()) continue;
            if (baseName(r.name) != mod) continue;
            // tabel usage ada di DATA module (r--/rw-); segmen kode (r-x)
            // nggak mungkin isinya tabel, jadi dilewat biar cepet.
            if (r.exec() && !r.writable()) continue;
            scanRegion(r, hits, &res.scannedMB, scanBuf, &loose);
        }
        for (size_t i = before; i < hits.size(); ++i) hitModule.push_back(mod);
        logx::add("[dumpout] %s: %zu cell kandidat (total %zu) [gagal baca=%zu]",
                  mod.c_str(), hits.size() - before, hits.size(), mem::readFailures());
        if (g_abort.load()) break;
    }
    res.candCells = hits.size();
    phase("pindai class-slot");
    if (g_abort.load()) {
        logx::add("[dumpout] DIHENTIKAN waktu pindai (%s) -- nggak ada file ditulis",
                  abortx::reason());
        res.aborted = true;
        return false;
    }

    // ---- 2. saring: validasi kuat (image *.dll + namespace printable) ------
    abortx::span(0.30f, 0.07f);        // 30%..37% = validasi kelas
    std::vector<Hit> keep;
    std::vector<std::string> keepModule;
    for (size_t i = 0; i < hits.size(); ++i) {
        if ((i & 1023) == 0) {
            if (abortx::requested()) { res.aborted = true; return false; }
            abortx::beat("validasi kelas",
                         hits.empty() ? -1.0f : (float)((double)i / (double)hits.size()));
        }
        if (!il2::isValidKlass(hits[i].klass)) continue;
        keep.push_back(hits[i]);
        keepModule.push_back(hitModule[i]);
    }
    logx::add("[dumpout] %zu cell nama-wajar -> %zu lolos validasi penuh (image *.dll)",
              loose, keep.size());

    // Kalau validasi ketat nol (ada game yang Il2CppImage-nya nggak standar),
    // jatuh ke jalur longgar: nama wajar + punya tetangga 8-byte di tabel.
    if (keep.size() < 5 && hits.size() > 8) {
        logx::add("[dumpout] validasi ketat nyaris nol -- pakai jalur longgar "
                  "(nama wajar + tetangga di tabel)");
        keep.clear();
        keepModule.clear();
        for (size_t i = 0; i < hits.size(); ++i) {
            const bool neighbour =
                (i + 1 < hits.size() && hits[i + 1].cell - hits[i].cell == 8) ||
                (i > 0 && hits[i].cell - hits[i - 1].cell == 8);
            if (!neighbour) continue;
            keep.push_back(hits[i]);
            keepModule.push_back(hitModule[i]);
        }
        logx::add("[dumpout] jalur longgar: %zu cell", keep.size());
    }

    // ---- 3. susun kelas (dedupe per alamat class) --------------------------
    abortx::span(0.37f, 0.03f);        // 37%..40% = susun kelas
    std::vector<Rec> recs;
    {
        std::set<uintptr_t> seen;
        for (size_t i = 0; i < keep.size(); ++i) {
            if ((i & 511) == 0)
                abortx::beat("susun kelas",
                             keep.empty() ? -1.0f : (float)((double)i / (double)keep.size()));
            if (!seen.insert(keep[i].klass).second) continue;
            Rec r;
            r.klass   = keep[i].klass;
            r.cell    = keep[i].cell;
            r.module  = keepModule[i];
            readMeta(r);
            if (r.name.empty()) continue;
            r.slotRva = mem::rvaOf(r.cell);   // stabil antar run (lihat mem.h)
            recs.push_back(std::move(r));
        }
        // CATATAN: dulu di sini ada dedupe nama+ns ("kadang satu kelas ke-pakai
        // dua cell"). Sekarang SENGAJA dihapus: dua Il2CppClass* yang beda tapi
        // namanya sama itu kelas yang BENERAN beda (instansiasi generic kaya
        // HashSet`1, tipe kembar antar-assembly). Il2CppDumper nulis semuanya
        // (dump referensi MLBB: 28141 kelas, cuma 22618 nama unik), jadi kita
        // juga harus nulis semuanya -- dedupe-nya cukup per ALAMAT kelas (seen
        // di atas), yang memang udah jadi identitas objek kelas il2cpp.
    }
    logx::add("[dumpout] %zu kelas dari tabel class-slot", recs.size());
    const size_t scanSeen = recs.size();   // buat log ringkas di step 4b

    // ---- 4. filter kelas --------------------------------------------------
    if (!o.nsFilter.empty() || !o.likeFilter.empty()) {
        std::vector<Rec> f;
        for (auto& r : recs) {
            if (!o.nsFilter.empty() && r.ns != o.nsFilter) continue;
            if (!o.likeFilter.empty() && r.name.find(o.likeFilter) == std::string::npos &&
                r.ns.find(o.likeFilter) == std::string::npos) continue;
            f.push_back(std::move(r));
        }
        logx::add("[dumpout] filter -> %zu kelas", f.size());
        recs.swap(f);
    }

    // ---- 4b. kelas tambahan dari KATALOG dump offline ---------------------
    //
    // Pindai class-slot cuma nemu kelas yang cell-nya SUDAH di-resolve runtime
    // (MLBB: ~5 rb dari ~28 rb kelas). Katalog dump offline nyimpen SEMUA kelas
    // beserta identitasnya (namespace/token/jenis/field/method). Tiap entri
    // dicoba di-resolve runtime lewat il2::findKlassDump (base + slot ->
    // Il2CppClass*, WAJIB divalidasi -- slot basi ditolak, jadi aman kalau game
    // update). Yang NGGAK ke-resolve tetap ikut ditulis sebagai entri
    // "katalog-saja" -- tanpa ini dump kita cuma ~20% dari referensi (kelas
    // yang nggak kepakai di match jadi hilang).
    bool catLoaded = false;
    if (dump::loaded() && !o.noDumpClasses) {
        abortx::span(0.37f, 0.02f);    // 37%..39% = kelas dari katalog dump offline
        il2::setDumpBulkQuiet(true);   // jangan banjirin logcat: bisa ribuan kelas
        catLoaded = true;
        const std::vector<dump::ClassEntry>& cat = dump::classes();

        // KATALOG JADI SUMBER KEBENARAN DAFTAR KELAS.
        //
        // Pindai class-slot cuma nemu kelas yang cell-nya udah di-resolve runtime
        // (MLBB: ~5 rb dari ~28 rb kelas) PLUS ribuan INSTANSIASI GENERIC yang
        // TypeDef-nya cuma satu. Kalau daftar scan dipakai apa adanya lalu
        // ditambahi katalog, satu kelas fisik bisa nongol DUA kali (sekali record
        // scan token 0x0, sekali record katalog) -- dump jadi 37.959, bukan
        // 28.139 seperti Il2CppDumper.
        //
        // Jadi di sini daftar kelas dibangun LANGSUNG dari katalog: SATU record
        // per entri (assembly, TypeDef token). Tiap entri dicoba di-resolve ke
        // Il2CppClass* runtime lewat il2::findKlassDump (base + slot, WAJIB
        // divalidasi -- slot basi ditolak, jadi aman kalau game update): kalau
        // ke-resolve, isi kelas dibaca dari memori; kalau nggak, isinya dari teks
        // dump (record "katalog-saja"). Identitasnya pakai (assembly, token) --
        // BUKAN (namespace, nama): di referensi ada 896 pasang nama kembar, mis.
        // `<>c` (kelas closure compiler) 378 biji cuma di Assembly-CSharp.
        std::vector<Rec> catRecs;
        catRecs.reserve(cat.size());
        std::set<uintptr_t> usedAddr;   // Il2CppClass* yang udah dipakai entri katalog lain
        size_t catSeen = 0, resolved = 0, unresolved = 0;
        for (const auto& e : cat) {
            if ((++catSeen & 255) == 0) {
                if (abortx::requested()) { res.aborted = true; return false; }
                abortx::beat("kelas dari dump offline",
                             cat.empty() ? -1.0f : (float)((double)catSeen / (double)cat.size()));
            }
            if (e.name.empty()) continue;
            if (!o.nsFilter.empty() && e.ns != o.nsFilter) continue;
            if (!o.likeFilter.empty() && e.name.find(o.likeFilter) == std::string::npos &&
                e.ns.find(o.likeFilter) == std::string::npos) continue;

            Rec r;
            r.module       = e.module;
            r.name         = e.name;
            r.ns           = e.ns;
            r.image        = e.assembly;
            r.kind         = e.kind;
            r.typeDefIndex = e.typeDefIndex;
            r.token        = e.token;
            if (!e.slots.empty()) r.slotRva = e.slots[0].rva;   // slot kandidat dari dump

            const uintptr_t k = il2::findKlassDump(e.name.c_str(),
                                                   e.ns.empty() ? nullptr : e.ns.c_str(), true);
            // findKlassDump nge-cache per (ns,nama): entri katalog yang namanya
            // kembar (mis. `<>c` 378 biji) semua balik ke Il2CppClass* yang SAMA.
            // Kalau alamat itu udah dipakai entri lain, entri ini nggak boleh
            // ikut di-resolve -- kalau tidak, ratusan kelas kembar ketiban jadi
            // satu. Isinya diambil dari teks dump (record katalog-saja).
            const uintptr_t ua = k ? mem::untag(k) : 0;
            r.catEntry = &e;
            if (k && usedAddr.insert(ua).second) {
                r.klass = k;                 // Il2CppClass* runtime -> isi dibaca nanti (step 6)
                ++resolved;
            } else {
                r.klass       = 0;
                r.catalogOnly = true;        // nggak ada Il2CppClass* runtime
                ++unresolved;
                fillFromDump(e, r.fields, r.methods);   // isi dari teks dump
            }
            catRecs.push_back(std::move(r));
        }

        // Daftar scan (tabel class-slot) SENGAJA nggak ditempel lagi: isinya
        // mayoritas INSTANSIASI GENERIC dari TypeDef yang katalognya udah ada
        // (List<int>, List<string>, ... punya Il2CppClass* sendiri walau
        // TypeDef-nya satu), jadi kalau ditempel jumlahnya balik jadi puluhan
        // ribu. Katalog = jumlah kelas yang benar (persis referensi Il2CppDumper).

        recs.swap(catRecs);
        il2::setDumpBulkQuiet(false);
        abortx::span(0.0f, 1.0f);
        logx::add("[dumpout] katalog offline: %zu kelas di-resolve runtime, "
                  "%zu kelas cuma-katalog -> total %zu (scan class-slot %zu dipakai "
                  "cuma buat validasi, daftar dari katalog)",
                  resolved, unresolved, recs.size(), scanSeen);
    }
    if (recs.empty()) return false;

    // ---- 5. kalibrasi layout field/method (mayoritas dari sampel) ---------
    explore::FieldLayout  fl;
    explore::MethodLayout ml;
    {
        // Kunci vote = seluruh layout (posisi array, stride, posisi tiap bagian
        // di dalam entry), bukan cuma offset array -- karena game seperti MLBB
        // nulis FieldInfo-nya dengan urutan yang beda.
        auto keyF = [](const explore::FieldLayout& f) {
            char b[128];
            snprintf(b, sizeof(b), "%x.%x.%x.%x.%x.%x", f.ptrOff, f.stride, f.nameOff,
                     f.typeOff, f.offSlot, f.tokenSlot);
            return std::string(b) + (f.structural ? "S" : "L");
        };
        auto keyM = [](const explore::MethodLayout& m) {
            char b[128];
            snprintf(b, sizeof(b), "%x.%x.%x.%x.%x", m.ptrOff, m.stride, m.nameOff,
                     m.klassOff, m.codeOff);
            return std::string(b) + (m.structural ? "S" : "L");
        };
        std::map<std::string, std::pair<int, explore::FieldLayout>> votes;
        std::map<std::string, std::pair<int, explore::MethodLayout>> mvotes;

        // Sampelnya banyak (kelas pertama di tabel sering kelas kecil tanpa
        // field), tapi BERHENTI begitu udah cukup bukti suara -- kalibrasi
        // struktur itu mahal (puluhan ribu baca memori per kelas).
        //
        // DUA pengaman, dua-duanya penting di device/game yang layout-nya beda:
        //   1. batas WAKTU (kCalibBudgetSec) -- dulu kalau layout-nya nggak
        //      kekalibrasi, loop-nya jalan sampai 2000 kelas = belasan menit
        //      ("resolve method nggak pernah selesai").
        //   2. cek ABORT tiap kelas -- biar tombol Stop beneran ngefek.
        abortx::span(0.40f, 0.22f);    // 40%..62% = kalibrasi layout
        const size_t sample = recs.size() < 2000 ? recs.size() : 2000;
        constexpr double kCalibBudgetSec = 45.0;
        const double calibT0 = abortx::nowSec();
        size_t okF = 0, okM = 0;
        bool calibTimedOut = false, calibAborted = false;
        constexpr int kEnoughVotes = 6;
        if (o.diag) {
            // bukti mentah: qword di dalam Il2CppClass buat beberapa kelas
            // pertama -- dipakai kalau kalibrasi gagal dan perlu lihat layout
            // aslinya (MLBB misalnya).
            for (size_t i = 0; i < recs.size() && i < o.diag; ++i) {
                uint64_t q[26] = {0};
                mem::read(recs[i].klass + 0x20, q, sizeof(q));
                char line[512];
                int n = 0;
                for (int j = 0; j < 26; ++j)
                    n += snprintf(line + n, sizeof(line) - n, " %llx",
                                  (unsigned long long)q[j]);
                logx::add("[dumpout/diag] %-24s +0x20..: %s",
                          recs[i].name.c_str(), line);
                // di mana array FieldInfo/MethodInfo-nya (kalau ada)?
                explore::dumpArrays(recs[i].klass, 8);
                explore::huntMethodTable(recs[i].klass, 6);
            }
        }
        int bestVotesF = 0, bestVotesM = 0;
        size_t calibDone = 0;
        for (size_t i = 0; i < sample; ++i) {
            calibDone = i + 1;
            if (abortx::requested()) { calibAborted = true; break; }
            if ((i & 15) == 0) {
                abortx::beat("kalibrasi layout", (float)((double)i / (double)sample));
                if (abortx::nowSec() - calibT0 > kCalibBudgetSec) { calibTimedOut = true; break; }
            }
            if (okF < 40) {
                explore::FieldLayout f;
                if (explore::calibrateFieldLayout(recs[i].klass, f) && f.score >= 2) {
                    auto& v = votes[keyF(f)];
                    ++v.first;
                    if (v.first == 1) v.second = f;
                    if ((int)v.first > bestVotesF) bestVotesF = (int)v.first;
                    ++okF;
                }
            }
            if (okM < 40) {
                explore::MethodLayout m;
                if (explore::calibrateMethodLayout(recs[i].klass, m) && m.score >= 2) {
                    auto& v = mvotes[keyM(m)];
                    ++v.first;
                    if (v.first == 1) v.second = m;
                    if ((int)v.first > bestVotesM) bestVotesM = (int)v.first;
                    ++okM;
                }
            }
            if (okF >= 40 && okM >= 40) break;
            // layout yang menang udah jelas -> nggak usah ngitung sisanya
            if (bestVotesF >= kEnoughVotes && bestVotesM >= kEnoughVotes) break;
            if (i > 150 && bestVotesF >= kEnoughVotes && !res.methodLayoutOk && bestVotesM == 0) break;
        }
        if (calibAborted || calibTimedOut)
            logx::add("[dumpout] kalibrasi %s setelah %zu dari %zu kelas (budget %.0f s) "
                      "-- lanjut pakai layout yang paling banyak suaranya",
                      calibAborted ? "DIBATALKAN" : "waktu habis", calibDone, sample,
                      kCalibBudgetSec);
        logx::add("[dumpout] sampel kalibrasi: %zu kelas dari %zu (sukses field %zu, method %zu)",
                  sample, recs.size(), okF, okM);
        int best = 0;
        for (auto& kv : votes) if (kv.second.first > best) { best = kv.second.first; fl = kv.second.second; }
        best = 0;
        for (auto& kv : mvotes) if (kv.second.first > best) { best = kv.second.first; ml = kv.second.second; }

        res.fieldLayoutOk  = fl.ok && fl.score >= 2;
        res.methodLayoutOk = ml.ok && ml.score >= 2;
        logx::add("[dumpout] layout field : %s ptraddr+0x%x stride 0x%x name+0x%x type+0x%x "
                  "offset+0x%x token+0x%x [%s, skor %d]",
                  res.fieldLayoutOk ? "OK" : "GAGAL", fl.ptrOff, fl.stride, fl.nameOff,
                  fl.typeOff, fl.offSlot, fl.tokenSlot,
                  fl.structural ? "struktur" : "kalibrasi lama", fl.score);
        logx::add("[dumpout] layout method: %s ptr+0x%x stride 0x%x name+0x%x klass+0x%x kode+0x%x "
                  "[%s, skor %d]", res.methodLayoutOk ? "OK" : "GAGAL", ml.ptrOff,
                  ml.stride, ml.nameOff, ml.klassOff, ml.codeOff,
                  ml.structural ? "struktur" : "kalibrasi lama", ml.score);

        // ---- 5a-bis. array OFFSET FIELD terpisah (il2cpp baru: FF/CODM) ----
        // Di Unity 2020+ sebagian kelas nggak nyimpen offset di FieldInfo-nya
        // (nyata di FF: 19% field offset-nya 0x0). Offset aslinya ada di array
        // per-kelas yang ditunjuk dari dalam Il2CppClass -> dikalibrasi dari
        // sampel kelas yang offset-nya KEBACA (itu yang jadi pembanding).
        if (res.fieldLayoutOk) {
            std::vector<uintptr_t> sampleK;
            const size_t cap = recs.size() < 120 ? recs.size() : 120;
            sampleK.reserve(cap);
            for (size_t i = 0; i < cap; ++i) sampleK.push_back(recs[i].klass);
            abortx::beat("cari array offset field", -1.0f);
            if (!explore::calibrateFieldOffsetArray(sampleK, fl, 60))
                logx::add("[dumpout] offset field dari FieldInfo aja (array terpisah "
                          "nggak ketemu -- lihat --foffprobe buat diagnosanya)");
        }
    }
    phase("kalibrasi layout");

    // ---- 5b. POOL MethodInfo global (kalau game-nya pakai) ------------------    //
    // Di game seperti MLBB, array method di dalam kelas cuma terisi sebagian
    // (nyata: BattleManager 20 dari 121). Daftar method LENGKAP-nya cuma ada di
    // pool global, jadi kalau pool-nya kebaca dia yang dipakai: RVA + nama semua
    // dari runtime -> nggak ada lagi "RVA: unknown".
    if (!metapool::ready() && !o.noPool) {
        abortx::span(0.62f, 0.26f);    // 62%..88% = pool method (metapool yang ngisi)
        abortx::beat("pool method", 0.0f);
        if (!metapool::init())
            logx::add("[dumpout] pool method global nggak kepakai: %s", metapool::lastError());
        abortx::span(0.0f, 1.0f);      // balik ke skala penuh
        phase("pool method");
    }
    const bool usePool = metapool::ready() && !o.noPool;
    if (abortx::requested()) {
        logx::add("[dumpout] DIHENTIKAN (%s) -- nggak ada file ditulis", abortx::reason());
        res.aborted = true;
        return false;
    }

    // ---- 5c. kelas tambahan dari POOL METHOD ------------------------------
    //
    // Sebagian kelas ada di game (punya MethodInfo di pool) tapi TIDAK punya
    // cell di tabel class-slot module -- jadi dulu nggak ikut ke-dump walau
    // method-nya kebaca. Sekarang kelas itu ikut ditulis (slot-nya kosong).
    //
    // Sumbernya metapool::allClasses(): GABUNGAN kelas yang punya MethodInfo
    // runtime + kelas yang cuma kepetakan lewat typeId/token/tabel tipe. Di
    // MLBB nyata: g_entries cuma nyentuh ~8 rb kelas, tapi peta typeId pool
    // kenal ~24 rb -- selisihnya inilah kelas yang selama ini "nggak ke-dump".
    // Pointer kelasnya SUDAH ke-resolve (valid, dari runtime), jadi aman
    // di-dump: nama/field dari runtime, method dari dump offline kalau kosong.
    //
    // Catatan jujur: typeId record method-def = (nomor file metadata << 24) |
    // index tipe, dan index itu TIDAK disimpan di dalam Il2CppClass (dicek:
    // 400 sampel x 0x800 byte, nol kecocokan) -- jadi typeId nggak bisa dipakai
    // buat nemuin kelas yang belum kelihatan dari dua sumber di atas. Yang
    // bikin peta typeId->kelas lengkap justru token TypeDef + tabel tipe.
    // Kalau katalog offline dipakai, daftar kelasnya SUDAH lengkap & persis
    // referensi -- nambahin kelas dari pool cuma bikin kelebihan (kelas itu
    // katalognya udah nulis). Jadi langkah ini cuma jalan tanpa katalog.
    if (usePool && !catLoaded) {
        size_t extra = 0;
        std::set<uintptr_t> haveAddr2;
        for (const auto& r : recs) haveAddr2.insert(mem::untag(r.klass));
        abortx::span(0.88f, 0.04f);    // 88%..92% = daftar kelas dari pool
        const std::vector<uintptr_t>& poolKlass = metapool::allClasses();
        const size_t poolTotal = poolKlass.empty() ? 1 : poolKlass.size();
        size_t scanned = 0, dropName = 0, dropDup = 0;
        for (uintptr_t k : poolKlass) {
            if (!k) continue;
            if ((++scanned & 255) == 0) {
                if (abortx::requested()) { res.aborted = true; return false; }
                abortx::beat("daftar kelas dari pool",
                             (float)((double)scanned / (double)poolTotal));
            }
            if (!haveAddr2.insert(mem::untag(k)).second) continue;   // udah ada dari scan/4b
            Rec r;
            r.klass = k;
            readMeta(r);
            if (r.name.empty()) { ++dropName; continue; }
            if (!o.nsFilter.empty() && r.ns != o.nsFilter) continue;
            if (!o.likeFilter.empty() && r.name.find(o.likeFilter) == std::string::npos &&
                r.ns.find(o.likeFilter) == std::string::npos) continue;
            // Dulu di sini ada dedupe nama (uniq2) yang buang ~8 rb kelas: itu
            // kelas yang beneran beda tapi namanya sama (instansiasi generic).
            // Sekarang dipertahankan -- identitas kelas = alamatnya (haveAddr2),
            // bukan namanya. dropDup tinggal buat kompat log aja (selalu 0).
            recs.push_back(std::move(r));
            ++extra;
        }
        logx::add("[dumpout] +%zu kelas dari pool (peta typeId/token/tabel tipe) -> total %zu "
                  "[pool kenal %zu, nama kosong %zu, duplikat %zu]",
                  extra, recs.size(), poolKlass.size(), dropName, dropDup);
    }
    abortx::span(0.0f, 1.0f);
    phase("daftar kelas dari pool");

    // ---- 5d. satukan per (assembly, token TypeDef) -------------------------
    //
    // Il2CppDumper nulis SATU entri per TypeDef metadata -- dump referensi MLBB
    // punya 28.139 baris "// Namespace:" (persis jumlah TypeDef, lihat
    // analisa: distinct(module,Token) == total == 28139). Runtime kita nemu
    // Il2CppClass* JAUH lebih banyak (~68 rb) karena INSTANSIASI GENERIC:
    // List<int>, List<string>, List<Foo> ... masing-masing punya objek kelas
    // sendiri walau TypeDef-nya cuma satu. Sebaliknya kalau di-dedup per NAMA,
    // tipe KEMBAR antar-assembly (nama sama, TypeDef beda -- 177 struct
    // __StaticArrayInitTypeSize misalnya) malah ketiban dan hasilnya kurang.
    //
    // Kunci yang bener persis kayak referensi: (image assembly, token TypeDef).
    // Token TypeDef itu dword 0x02xxxxxx di dalam Il2CppClass, offset-nya udah
    // DIKALIBRASI pool (metapool::typeTokenOffset, di MLBB +0xfc). Kalau offset
    // itu nggak kedeteksi, lewatin -- dedup per alamat aja (perilaku lama).
    // Dengan katalog, daftar kelasnya SUDAH satu entri per TypeDef (identitas
    // dari katalog), jadi dedup runtime di sini nggak perlu -- malah berisiko
    // (token runtime yang kebaca bisa beda dari token katalog). Lewati.
    {
        const uint32_t tokOff = catLoaded ? 0 : metapool::typeTokenOffset();
        if (tokOff) {
            std::set<std::pair<uintptr_t, uint32_t>> seenTok;
            std::set<uintptr_t> imgPtrs;
            std::set<uint32_t> toks;
            std::vector<Rec> clean;
            clean.reserve(recs.size());
            size_t merged = 0, noTok = 0, imgZero = 0;
            for (auto& r : recs) {
                if (r.catalogOnly) { clean.push_back(std::move(r)); continue; }  // nggak ada Il2CppClass*
                const uint32_t tok = mem::rdp<uint32_t>(r.klass + tokOff);
                if ((tok >> 24) != 0x02) { clean.push_back(std::move(r)); ++noTok; continue; }
                const uintptr_t img = mem::untag(mem::rdp<uintptr_t>(r.klass + il2::kKlass_Image));
                if (!img) ++imgZero;
                imgPtrs.insert(img);
                toks.insert(tok & 0x00FFFFFFu);
                const std::pair<uintptr_t, uint32_t> key{img, tok & 0x00FFFFFFu};
                if (!seenTok.insert(key).second) { ++merged; continue; }
                clean.push_back(std::move(r));
            }
            logx::add("[dumpout] satukan per TypeDef: %zu -> %zu kelas "
                      "(token +0x%x; %zu image, %zu token unik, %zu image=0; "
                      "%zu instansiasi generic digabung, %zu tanpa token)",
                      recs.size(), clean.size(), tokOff, imgPtrs.size(), toks.size(),
                      imgZero, merged, noTok);
            recs.swap(clean);
        }
    }

    abortx::span(0.92f, 0.02f);        // 92%..94% = cari offset parent
    const uint32_t pOff = parentOffset(recs);
    res.parentOk = pOff != 0;
    abortx::span(0.0f, 1.0f);
    phase("cari offset parent");

    // --classes N: batasi HASIL (bukan sampel kalibrasi) biar tes cepat tetap
    // dapet layout yang bener.
    if (o.maxClasses && recs.size() > o.maxClasses) recs.resize(o.maxClasses);

    // ---- 6. baca isi tiap kelas -------------------------------------------
    // (JANGAN reset flag batal di sini -- kalau user udah nge-Stop, permintaan
    //  itu harus tetap dihormati sampai run() selesai.)
    abortx::span(0.94f, 0.05f);        // 94%..99% = baca isi kelas
    abortx::beat("baca isi kelas", 0.0f);
    const size_t classTotal = recs.empty() ? 1 : recs.size();
    size_t classDone = 0, fromDumpMethods = 0, mergedClasses = 0;
    size_t catMethodTotal = 0, runtimeOnlyTotal = 0;
    size_t staticMarkedTotal = 0, fieldFromCatalog = 0;
    for (auto& r : recs) {
        if (g_abort.load()) { res.aborted = true; break; }
        if ((++classDone & 63) == 0)
            abortx::beat("baca isi kelas", (float)((double)classDone / (double)classTotal));
        // Kelas katalog-saja: Il2CppClass*-nya nggak ada di runtime, isinya
        // udah diambil dari dump offline pas step 4b -- jangan dibaca dari memori.
        if (r.catalogOnly || !r.klass) continue;
        if (pOff) r.parent = mem::untag(mem::rdp<uintptr_t>(r.klass + pOff));
        if (res.fieldLayoutOk)  explore::readFields(r.klass, fl, r.fields, 256);
        if (usePool) {
            // RVA method yang belum dibikin engine + nama yang index-nya nggak
            // kebaca dilengkapi dari dump offline (biar hasilnya nol "unknown").
            if (!o.noEnrich) metapool::enrichFromDump(r.klass, r.name);
            metapool::forKlass(r.klass, r.methods, 4096);
        }
        if (r.methods.empty() && res.methodLayoutOk)
            explore::readMethods(r.klass, ml, r.methods, 1024, true);
        // Masih kosong? Ambil dari dump offline (kalau ada). Tanpa ini, kelas
        // kaya BattleManager nongol di dump tanpa satu method pun.
        if (r.methods.empty() && !o.noDumpMethods && dump::loaded() &&
            methodsFromDump(r.name, r.ns, r.module, r.methods))
            ++fromDumpMethods;

        // ---- GABUNG katalog + runtime ------------------------------------
        // Katalog: signature lengkap ("static bool IsDead()") + RVA dari dump
        // metadata. Runtime: RVA yang PASTI valid (dipetakan ulang tiap run).
        //
        // Aturannya:
        //   1. daftar katalog jadi DASAR (itu yang punya tipe + parameter)
        //   2. method runtime yang namanya belum ada di katalog -> DITAMBAHKAN
        //   3. entri katalog yang RVA-nya 0 -> diisi dari runtime bila nama sama
        //
        // Katalog DI-PARSE DI SINI, satu kelas sekali jalan lalu dibuang --
        // bukan sekali buat semua kelas: 28 rb kelas x 200 rb method = ratusan
        // MB string, dan prosesnya dibunuh kernel (OOM) sebelum sempat nulis.
        if (!r.catalogOnly && !o.noDumpMethods && r.catEntry) {
            dump::ClassDetail det;
            if (dump::detail(*r.catEntry, det) && !det.methods.empty()) {
                std::unordered_map<std::string, size_t> byName;
                byName.reserve(det.methods.size() * 2);
                std::vector<explore::Method> merged;
                merged.reserve(det.methods.size() + r.methods.size());
                for (const auto& dm : det.methods) {
                    if (dm.name.empty()) continue;
                    explore::Method m;
                    m.name   = dm.name;
                    m.rva    = dm.hasRva ? dm.rva : 0;
                    m.module = dm.module.empty() ? r.catEntry->module : dm.module;
                    byName.emplace(bareMethod(m.name), merged.size());
                    merged.push_back(std::move(m));
                }
                size_t added = 0, filled = 0;
                for (auto& m : r.methods) {
                    if (m.name.empty()) continue;
                    auto it = byName.find(bareMethod(m.name));
                    if (it == byName.end()) {
                        merged.push_back(std::move(m));   // cuma ada di runtime
                        ++added;
                        continue;
                    }
                    explore::Method& cm = merged[it->second];
                    if (!m.rva && cm.rva) cm.rva = m.rva;
                    if (cm.module.empty()) cm.module = m.module;
                    if (m.ptr) cm.ptr = m.ptr;
                    ++filled;
                }
                r.methods.swap(merged);
                catMethodTotal   += filled;
                runtimeOnlyTotal += added;
                // ---- field:_static + field yang runtime nggak punya --------
                // FieldInfo runtime nggak nyimpen penanda static, jadi kelas
                // yang ter-resolve runtime nulis field statis seolah-olah
                // instance. Katalog punya penandanya (dan field statis yang
                // nggak muncul di array FieldInfo runtime) -> pakai itu.
                if (!det.fields.empty()) {
                    std::unordered_map<std::string, size_t> byField;
                    byField.reserve(r.fields.size() * 2);
                    for (size_t i = 0; i < r.fields.size(); ++i)
                        byField.emplace(r.fields[i].name, i);
                    size_t marked = 0, addedF = 0;
                    for (const auto& df : det.fields) {
                        if (df.name.empty()) continue;
                        auto it = byField.find(df.name);
                        if (it != byField.end()) {
                            explore::Field& f = r.fields[it->second];
                            if (df.isStatic && !f.isStatic) { f.isStatic = true; ++marked; }
                            if (!f.offset && df.offset) f.offset = df.offset;
                            if (f.typeName.empty()) f.typeName = df.type;
                            if (!f.typeEnum) f.typeEnum = dump::guessTypeEnum(df.type);
                            continue;
                        }
                        // field ada di katalog tapi nggak di runtime -> ikut
                        // ditulis (banyak di antaranya statis).
                        explore::Field f;
                        f.name     = df.name;
                        f.typeName = df.type;
                        f.offset   = df.offset;
                        f.token    = df.token;
                        f.isStatic = df.isStatic;
                        f.typeEnum = dump::guessTypeEnum(df.type);
                        r.fields.push_back(std::move(f));
                        ++addedF;
                    }
                    staticMarkedTotal += marked;
                    fieldFromCatalog  += addedF;
                }
            }
            ++mergedClasses;
        }
    }
    if (mergedClasses)
        logx::add("[dumpout] digabung katalog+runtime: %zu kelas | method: %zu baris "
                  "dapat RVA dari runtime, %zu method hanya ada di runtime | "
                  "field: %zu ditandai static, %zu diambil dari katalog",
                  mergedClasses, catMethodTotal, runtimeOnlyTotal,
                  staticMarkedTotal, fieldFromCatalog);
    if (fromDumpMethods)
        logx::add("[dumpout] +%zu kelas yang daftar method-nya diambil dari dump offline",
                  fromDumpMethods);
    abortx::span(0.0f, 1.0f);
    phase("baca isi kelas");
    if (res.aborted) {
        logx::add("[dumpout] DIHENTIKAN (%s) setelah %zu kelas -- nggak ada file ditulis",
                  abortx::reason(), classDone);
        return false;
    }

    // ---- 7. urut + tulis ---------------------------------------------------
    abortx::span(0.99f, 0.01f);        // 99%..100% = urut + tulis file
    abortx::beat("tulis file", 0.0f);
    std::sort(recs.begin(), recs.end(), [](const Rec& a, const Rec& b) {
        if (a.module != b.module) return a.module < b.module;
        if (a.image != b.image)   return a.image < b.image;
        if (a.name != b.name)     return a.name < b.name;
        return a.ns < b.ns;
    });

    const std::string pkg = target::package().empty() ? "unknown" : stripSub(target::package());
    const std::string ver = pkgVersion(pkg);

    // SCAN SAJA (tombol "Start scan"): hasilnya dihitung & dilaporin, tapi
    // nggak ada file yang ditulis -- jadi bisa dipakai buat ngukur kelas/method
    // yang kepakai di match berjalan tanpa numpuk file.
    if (o.dryRun) {
        size_t withMethod = 0;
        res.classes = recs.size();
        for (const auto& r : recs) {
            if (r.slotRva) ++res.withSlot;
            if (!r.methods.empty()) ++withMethod;
            res.fields  += r.fields.size();
            res.methods += r.methods.size();
        }
        res.dir = o.outDir.empty() ? (target::dumpDir() + "/" + pkg + "/" + ver) : o.outDir;
        logx::add("[dumpout] SCAN SAJA: %zu kelas, %zu slot, %zu field, %zu method "
                  "(%zu kelas punya method) | folder kalau di-dump: %s",
                  res.classes, res.withSlot, res.fields, res.methods, withMethod,
                  res.dir.c_str());
        return true;
    }

    // Root default: <dumpdir>/<pkg>/<versi>/  -- dumpdir bisa diganti di menu
    // (Setting) atau CLI (--outdir), defaultnya /sdcard/panxcz-tools/dumper.
    // Stop ditekan waktu baca kelas -> jangan tulis file setengah jadi.
    if (res.aborted) {
        logx::add("[dumpout] DIHENTIKAN (tombol Stop) -- nggak ada file yang ditulis");
        return false;
    }

    std::string dir = o.outDir;
    if (dir.empty()) dir = target::dumpDir() + "/" + pkg + "/" + ver;
    mkdirP(dir.c_str());

    // Nama file: "native" (atau native_<lib>) -- SENGAJA beda dari dump hasil
    // frida/Il2CppDumper (`*_full.cs` / `*_libcsharp.so.cs`) supaya nggak
    // nimpa/nutupin. Tool milih dump dari jumlah slot, dan dump metadata itu
    // slotnya lebih banyak (metode-nya lengkap), jadi dia yang tetap kepakai.
    std::string tag = o.fileTag;
    if (tag.empty()) {
        if (o.allLibs) {
            tag = "native_full";
        } else {
            std::string lib = baseName(mods[0]);           // libcsharp.so
            if (lib.compare(lib.size() - 3, 3, ".so") == 0) lib = lib.substr(0, lib.size() - 3);
            tag = "native_" + lib;
        }
    }
    if (tag.empty()) tag = "native";
    const std::string base = dir + "/" + pkg + "_" + tag;

    // nama kelas buat referensi parent
    std::set<uintptr_t> addresses;
    for (const auto& r : recs) addresses.insert(r.klass);

    std::ofstream cs(base + ".cs", std::ios::trunc | std::ios::binary);
    if (!cs.is_open()) {
        logx::add("[dumpout] gagal nulis %s.cs (izin folder?)", base.c_str());
        return false;
    }

    std::ofstream js, hd, pt;
    if (o.json)   js.open(base + ".json", std::ios::trunc | std::ios::binary);
    if (o.header) hd.open(base + ".h", std::ios::trunc | std::ios::binary);
    if (o.ptr)    pt.open(base + ".ptrs.tsv", std::ios::trunc | std::ios::binary);
    if (js.is_open()) js << "{\n  \"package\": \"" << esc(pkg) << "\",\n  \"version\": \""
                         << esc(ver) << "\",\n  \"tool\": \"panxcz_libtool (native root dump)\",\n"
                         << "  \"classes\": [\n";
    if (hd.is_open()) hd << "// panxcz_libtool native dump -- tabel siap-patch\n"
                            "// { \"Namespace::Class::Method\", \"module\", rva }\n"
                            "static const panxcz_sym kPanxczSym[] = {\n";

    {
        char head[1024];
        size_t slotCount = 0;
        for (const auto& r : recs) if (r.slotRva) ++slotCount;
        snprintf(head, sizeof(head),
                 "// ============================================================================\n"
                 "// panxcz_libtool native dump (root, tanpa frida)\n"
                 "// package : %s\n"
                 "// proses  : %s (pid %d)\n"
                 "// versi   : %s\n"
                 "// module  : %s\n"
                 "// kelas   : %zu (%zu punya class-slot)\n"
                 "// dibuat  : dari memori proses yang lagi jalan (%zu cell dipindai, %zu MB)\n"
                 "// CATATAN : field statis TIDAK ditandai (metadata runtime nggak nyimpen\n"
                 "//           penandanya). Offset instance & RVA method asli dari runtime.\n"
                 "// ============================================================================\n",
                 pkg.c_str(),
                 mem::procName().empty() ? "?" : mem::procName().c_str(), mem::pid(),
                 ver.c_str(), mods[0].c_str(), recs.size(), slotCount,
                 res.candCells, res.scannedMB);
        cs << head;
    }

    const bool wantField  = colsWanted(o, "field");
    const bool wantMethod = colsWanted(o, "method");
    const bool wantSlot   = colsWanted(o, "slot");
    const bool wantToken  = colsWanted(o, "token");
    const bool wantParent = colsWanted(o, "parent");

    std::string curModule, curImage;
    int moduleNo = 0;
    bool firstJson = true;
    size_t writeDone = 0;
    const size_t writeTotal = recs.empty() ? 1 : recs.size();
    // baris tabel class-slot buat file .h (ditulis di bagian bawah)
    std::string slotRows;

    for (const auto& r : recs) {
        if ((++writeDone & 127) == 0)
            abortx::beat("tulis file", (float)((double)writeDone / (double)writeTotal));
        if (r.module != curModule || r.image != curImage) {
            curModule = r.module;
            curImage  = r.image;
            ++moduleNo;
            char b[512];
            snprintf(b, sizeof(b), "// Module %d: %s / %s\n", moduleNo, curModule.c_str(),
                     curImage.empty() ? "<unknown>" : curImage.c_str());
            cs << b;
        }

        char b[512];
        snprintf(b, sizeof(b), "// Namespace: %s; TypeDefIndex: %u; Token: 0x%llX\n",
                 r.ns.c_str(), r.typeDefIndex, (unsigned long long)r.token);
        cs << b;
        if (wantSlot && r.slotRva) {
            snprintf(b, sizeof(b), "// Class-slot candidates: %s 0x%llX\n", r.module.c_str(),
                     (unsigned long long)r.slotRva);
            cs << b;
            if (hd.is_open()) {
                slotRows += "    { \"";
                slotRows += esc(r.ns);
                if (!r.ns.empty()) slotRows += "::";
                slotRows += esc(r.name);
                slotRows += "\", \"" + esc(r.module) + "\", " + hex(r.slotRva) + "u },\n";
            }
            ++res.withSlot;
        }
        if (wantParent && r.parent && addresses.count(r.parent)) {
            char pn[220] = {0};
            uintptr_t np = mem::rdp<uintptr_t>(r.parent + il2::kKlass_Name);
            if (np) il2::readStr(np, pn, sizeof(pn));
            if (pn[0]) { snprintf(b, sizeof(b), "// Parent: %s\n", pn); cs << b; }
        }
        // Keyword sesuai jenis di katalog (Il2CppDumper nulis class/struct/enum/
        // interface; dulu kita selalu nulis "class").
        const char* kw = r.kind == dump::Kind::Struct    ? "struct"
                       : r.kind == dump::Kind::Enum      ? "enum"
                       : r.kind == dump::Kind::Interface ? "interface"
                                                         : "class";
        snprintf(b, sizeof(b), "%s %s\n{\n", kw, r.name.c_str());
        cs << b;

        if (js.is_open()) {
            js << (firstJson ? "    " : ",\n    ") << "{ \"name\": \"" << esc(r.name)
               << "\", \"namespace\": \"" << esc(r.ns) << "\", \"image\": \"" << esc(r.image)
               << "\", \"module\": \"" << esc(r.module) << "\", \"slot\": \"" << hex(r.slotRva)
               << "\"";
            firstJson = false;
        }

        if (wantField) {
            if (o.diag && fl.ok) {
                // bukti mentah: entry FieldInfo yang dibaca + hasil tafsirnya
                const uintptr_t arr = mem::untag(mem::rdp<uintptr_t>(r.klass + fl.ptrOff));
                for (int i = 0; i < 3 && i < (int)r.fields.size(); ++i) {
                    uint64_t q[4] = {0};
                    mem::read(arr + (uintptr_t)i * fl.stride, q, sizeof(q));
                    logx::add("[dumpout/diag] %s entry%d q0..3=%llx %llx %llx %llx -> nama='%s' "
                              "off=0x%X tok=0x%X (stride 0x%x name+0x%x off+0x%x tok+0x%x)",
                              r.name.c_str(), i, (unsigned long long)q[0],
                              (unsigned long long)q[1], (unsigned long long)q[2],
                              (unsigned long long)q[3], r.fields[i].name.c_str(),
                              r.fields[i].offset, r.fields[i].token, fl.stride, fl.nameOff,
                              fl.offSlot, fl.tokenSlot);
                }
            }
            if (js.is_open() && !r.fields.empty()) js << ",\n      \"fields\": [";
            bool firstF = true;
            for (const auto& f : r.fields) {
                const std::string t = f.typeName.empty() ? "Il2CppObject" : f.typeName;
                if (wantToken)
                    snprintf(b, sizeof(b), "    %s %s; // offset: 0x%X; token: 0x%X\n",
                             t.c_str(), f.name.c_str(), f.offset, f.token);
                else
                    snprintf(b, sizeof(b), "    %s %s; // offset: 0x%X\n",
                             t.c_str(), f.name.c_str(), f.offset);
                cs << b;
                if (js.is_open()) {
                    js << (firstF ? "\n" : ",\n") << "        { \"name\": \"" << esc(f.name)
                       << "\", \"type\": \"" << esc(t) << "\", \"offset\": \"" << hex(f.offset)
                       << "\", \"token\": \"" << hex(f.token) << "\" }";
                    firstF = false;
                }
                ++res.fields;
            }
            if (js.is_open() && !r.fields.empty()) js << "\n      ]";
        }

        cs << "\n";
        if (wantMethod) {
            if (js.is_open() && !r.methods.empty()) js << ",\n      \"methods\": [";
            bool firstM = true;
            for (const auto& m : r.methods) {
                if (m.rva) {
                    if (!m.module.empty() && m.module != curModule)
                        snprintf(b, sizeof(b), "    // RVA: 0x%llX (%s)\n",
                                 (unsigned long long)m.rva, m.module.c_str());
                    else
                        snprintf(b, sizeof(b), "    // RVA: 0x%llX\n",
                                 (unsigned long long)m.rva);
                    cs << b;
                }
                snprintf(b, sizeof(b), "    %s;\n", m.name.c_str());
                cs << b;
                if (o.ptr && pt.is_open())
                    pt << r.ns << (r.ns.empty() ? "" : ".") << r.name << "\t" << m.name << "\t"
                       << (m.module.empty() ? r.module : m.module) << "\t0x"
                       << std::hex << m.rva << std::dec << "\t0x" << std::hex
                       << (unsigned long long)m.ptr << std::dec << "\n";
                if (js.is_open()) {
                    js << (firstM ? "\n" : ",\n") << "        { \"name\": \"" << esc(m.name)
                       << "\", \"module\": \"" << esc(m.module.empty() ? r.module : m.module)
                       << "\", \"rva\": \"" << hex(m.rva) << "\", \"ptr\": \"" << hex(m.ptr)
                       << "\" }";
                    firstM = false;
                }
                ++res.methods;

                if (hd.is_open() && m.rva)
                    hd << "    { \"" << esc(r.ns) << (r.ns.empty() ? "" : "::") << esc(r.name)
                       << "::" << esc(m.name) << "\", \""
                       << esc(m.module.empty() ? r.module : m.module) << "\", " << hex(m.rva)
                       << "u },\n";
            }
            if (js.is_open() && !r.methods.empty()) js << "\n      ]";
        }

        cs << "}\n";
        if (js.is_open()) js << " }";
    }

    if (js.is_open()) js << "\n  ]\n}\n";
    if (hd.is_open()) {
        hd << "};\n\n";
        hd << "// class-slot: RVA cell yang ISINYA Il2CppClass* (bukan alamat kode).\n"
              "//   libbase + rva -> Il2CppClass*  ->  +0xa8 static_fields  ->  +offset\n"
              "static const panxcz_sym kPanxczClassSlot[] = {\n";
        hd << slotRows;
        hd << "};\n";
    }
    cs.close();
    abortx::span(0.0f, 1.0f);
    abortx::beat("dump selesai", 1.0f);

    res.aborted = g_abort.load();
    res.classes = recs.size();
    res.modules = (size_t)moduleNo;
    res.dir = dir;
    res.path = base + ".cs";
    if (js.is_open()) { js.close(); res.jsonPath   = base + ".json"; }
    if (hd.is_open()) { hd.close(); res.headerPath = base + ".h"; }
    if (pt.is_open()) { pt.close(); res.ptrsPath   = base + ".ptrs.tsv"; }

    logx::add("[dumpout] selesai: %zu kelas (%zu slot, %zu field, %zu method, %zu module) "
              "-> %s", res.classes, res.withSlot, res.fields, res.methods, res.modules,
              res.path.c_str());
    return true;
}

}  // namespace dumpout

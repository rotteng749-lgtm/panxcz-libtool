// ============================================================================
//  metapool.cpp - lihat include/core/metapool.h buat penjelasan lengkapnya.
//
//  Ringkas alurnya:
//    1. cari mapping file metadata (global-metadata.dat dkk) dari /proc/maps
//    2. pindai heap game buat nemu POOL MethodInfo (stride 0x50)
//    3. kalibrasi delta nama per "space" (byte atas index nama) -> nama method
//    4. kelompokkan entry pool per Il2CppClass -> daftar method LENGKAP per kelas
//
//  Langkah 3 sengaja TIDAK pakai konstanta hardcode: anchor-nya diambil dari
//  (a) dump offline kalau ada (nama + RVA) dan (b) tabel properti kelas
//  (nama properti = pointer langsung ke string, accessor-nya "get_"/"set_").
//  Hasil tiap anchor divote, jadi sampel yang meleset nggak ngerusak hasil.
// ============================================================================
#include "core/metapool.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <vector>

#include <pthread.h>

#include "core/abort.h"
#include "core/il2cpp.h"
#include "core/log.h"
#include "core/mem.h"
#include "core/offlinedump.h"

namespace metapool {
namespace {

// ---------------------------------------------------------------------------
// LAYOUT MethodInfo -- BEDA antar game, jadi DICARI saat jalan (bukan hardcode)
// ---------------------------------------------------------------------------
//  Dua gaya nyata yang ketemu di lapangan:
//
//  (a) MLBB (il2cpp yang dimodif): Il2CppClass* di +0x10, dan nama method TIDAK
//      ada di struct -- yang ada pointer ke record metadata di +0x28 (dword0 =
//      indeks nama, +0x14 = token 0x06xxxxxx). Nama di-resolve lewat kalibrasi
//      delta/anchor (lihat calibrate()).
//
//  (b) il2cpp standar Unity 2020+ (nyata di Free Fire, CODM): nama method ADA di
//      struct sebagai `const char*` di +0x18, klass di +0x20. Jadi nama bisa
//      dibaca LANGSUNG -- nggak butuh anchor sama sekali. Dulu layout ini nggak
//      didukung: pool FF kedeteksi pakai layout MLBB (kelas "valid" kebetulan),
//      tapi indeks nama yang dibaca = isi string, makanya muncul
//      "space 101/109/116 -> nggak kekalibrasi" dan nama method 0.
//
//  Karena itu SEMUA layout dicoba dalam satu kali pindai, lalu yang paling
//  banyak entry + paling banyak nama KEBACA yang dipakai.
struct MiLayout {
    const char* tag;
    uint32_t    klassOff;    // Il2CppClass* di dalam MethodInfo
    uint32_t    stride;      // jarak antar MethodInfo di pool
    uint32_t    nameOff;     // const char* nama method (kNoOff = nggak ada)
    uint32_t    metaOff;     // pointer ke record metadata (kNoOff = nggak ada)
};
constexpr uint32_t kNoOff = 0xFFFFFFFFu;

const MiLayout kLayouts[] = {
    // MLBB: klass +0x10, nama lewat record metadata +0x28, stride 0x50
    {"MLBB (klass+0x10, record+0x28, stride 0x50)", 0x10, 0x50, kNoOff, 0x28},
    // MLBB versi lain: nama lewat record +0x20
    {"MLBB-alt (klass+0x10, record+0x20, stride 0x50)", 0x10, 0x50, kNoOff, 0x20},
    // il2cpp standar 2020+ (Free Fire, CODM): klass +0x20, nama LANGSUNG +0x18
    {"il2cpp-2021 (klass+0x20, nama+0x18, stride 0x50)", 0x20, 0x50, 0x18, kNoOff},
    {"il2cpp-2021 (klass+0x20, nama+0x18, stride 0x48)", 0x20, 0x48, 0x18, kNoOff},
    // il2cpp 2018/2019: klass +0x18, nama LANGSUNG +0x10
    {"il2cpp-2019 (klass+0x18, nama+0x10, stride 0x50)", 0x18, 0x50, 0x10, kNoOff},
    {"il2cpp-2019 (klass+0x18, nama+0x10, stride 0x40)", 0x18, 0x40, 0x10, kNoOff},
};
constexpr size_t kLayoutCount = sizeof(kLayouts) / sizeof(kLayouts[0]);

// Layout yang lagi dipakai (diisi waktu init() milih pemenangnya).
MiLayout g_mi = kLayouts[0];
// Paksa satu layout dari luar (--pool-layout=N, -1 = otomatis).
int g_forceLayout = -1;

// Layout "nama langsung" butuh bukti nama kebaca sebanyak ini biar dianggap
// menang (biar satu-dua kebetulan nggak ngalahin layout yang beneran jalan).
constexpr size_t kMinDirectNamed = 50;

constexpr uint32_t kMiCode = 0x00;     // methodPointer (sama di semua versi)

constexpr uint32_t kKlassName = 0x10;
constexpr uint32_t kKlassProps = 0x88; // PropertyInfo[] (MLBB)

struct MapFile {
    std::string path;       // nama file (basename)
    uintptr_t   start = 0, end = 0;
    std::vector<uint8_t> data;   // isinya (dimuat kalau kepakai)
    bool        loaded = false;
};

struct SpaceFix {
    int      space = -1;
    int      mapIdx = -1;
    uint64_t delta = 0;
    int      votes = 0;
    bool     ok = false;
};

std::vector<MapFile>  g_maps;
std::vector<MethodEnt> g_entries;
std::map<uintptr_t, std::vector<size_t>> g_byKlass;
std::map<uintptr_t, std::vector<MethodEnt>> g_methodsByKlass;   // daftar LENGKAP per kelas
// cache daftar SEMUA Il2CppClass* yang pool kenal (lihat allClasses()).
std::vector<uintptr_t> g_allKlass;
bool g_allKlassBuilt = false;
std::vector<SpaceFix> g_fix;             // index = byte space (0..255 dipakai sebagian)
Stats        g_stats;
std::string  g_err;
bool         g_ready = false;
size_t       g_capMB = 2048;             // batas pindai heap (MB)
std::vector<std::pair<uintptr_t, uintptr_t>> g_exec;   // segmen kode

size_t g_mapBytes = 0;
constexpr size_t kMaxMapBytes = 64u << 20;

// Batas waktu buat kalibrasi nama (anchor). Anchor itu nyari string di file
// metadata yang bisa puluhan MB, jadi biayanya naik kalau mapping-nya besar /
// beda versi. Tanpa batas, di device lain ini bisa jalan belasan menit
// ("resolve method nggak pernah selesai"). Kalau habis, kalibrasi berhenti dan
// yang udah kedapet tetap dipakai.
constexpr double kCalibBudgetSec = 60.0;
double g_calibDeadline = 0.0;

inline bool anchorOut() {
    return abortx::requested() || (g_calibDeadline > 0.0 && abortx::nowSec() > g_calibDeadline);
}

// Region heap tempat pool MethodInfo dialokasikan. Nama region BEDA antar
// ROM/versi Android: libc_malloc (Android lama / linker lama), scudo (default
// Android 11+), jemalloc, tcmalloc. Dulu cuma "libc_malloc" yang dikenali --
// di HP ber-scudo tahap pertama nggak nemu apa-apa, jadi tahap dua nge-scan
// SEMUA region writable: lambat banget dan hasilnya bisa beda.
inline bool heapLike(const std::string& nm) {
    static const char* k[] = {"libc_malloc", "scudo", "jemalloc", "tcmalloc",
                              "gwp", "anon:libc", "alloc"};
    for (const char* s : k)
        if (nm.find(s) != std::string::npos) return true;
    return false;
}

// ---------------------------------------------------------------------------
// util
// ---------------------------------------------------------------------------
inline bool identChar(unsigned char c) {
    if (isalnum(c)) return true;
    switch (c) {
        case '_': case '<': case '>': case '.': case '`': case '$':
        case '-': case '+': case '@': case '[': case ']': case '*': case '&':
            return true;
        default: return false;
    }
}

bool looksIdent(const char* s, size_t n) {
    if (n == 0 || n > 200) return false;
    int letters = 0;
    for (size_t i = 0; i < n; ++i) {
        const unsigned char c = (unsigned char)s[i];
        if (!identChar(c)) return false;
        if (isalpha(c) || c == '_') ++letters;
    }
    // murni angka / tanda baca doang = bukan nama method
    if (!letters) return false;
    bool alpha = false;
    for (size_t i = 0; i < n; ++i) if (isalpha((unsigned char)s[i])) alpha = true;
    return alpha;
}

// baca string identifier dari memori game (di-terminasi NUL)
bool readIdent(uintptr_t p, char* out, size_t n) {
    if (!p || n < 8 || !mem::canRead(p, 4)) return false;
    if (!mem::read(p, out, n - 1)) return false;
    out[n - 1] = 0;
    size_t len = strnlen(out, n - 1);
    if (!len) return false;
    return looksIdent(out, len);
}

void buildExec() {
    g_exec.clear();
    for (const auto& r : mem::regions())
        if (r.readable() && r.exec()) g_exec.push_back({r.start, r.end});
}

bool inExec(uintptr_t a) {
    for (const auto& e : g_exec) if (a >= e.first && a < e.second) return true;
    return false;
}

const char* moduleOf(uintptr_t addr) {
    const mem::Region* r = mem::regionOf(addr);
    if (!r || r->name.empty()) return nullptr;
    const size_t sl = r->name.find_last_of('/');
    return r->name.c_str() + (sl == std::string::npos ? 0 : sl + 1);
}

bool inMap(uintptr_t a, int* which = nullptr) {
    for (size_t i = 0; i < g_maps.size(); ++i) {
        if (a >= g_maps[i].start && a < g_maps[i].end) {
            if (which) *which = (int)i;
            return true;
        }
    }
    return false;
}

// cari substring di buffer (buat nyari nama di file metadata yang udah dimuat)
size_t findIn(const std::vector<uint8_t>& hay, const char* needle, size_t start = 0) {
    const size_t n = strlen(needle);
    if (!n || hay.size() < n) return std::string::npos;
    const uint8_t* h = hay.data();
    for (size_t i = start; i + n <= hay.size(); ++i) {
        if (h[i] != (uint8_t)needle[0]) continue;
        if (memcmp(h + i, needle, n) == 0) return i;
    }
    return std::string::npos;
}

// nama method dari signature dump: "static void Foo(int a)" -> "Foo"
std::string baseName(const std::string& sig) {
    size_t p = sig.find('(');
    std::string head = (p == std::string::npos) ? sig : sig.substr(0, p);
    // buang spasi kiri-kanan
    size_t b = 0, e = head.size();
    while (b < e && isspace((unsigned char)head[b])) ++b;
    while (e > b && isspace((unsigned char)head[e - 1])) --e;
    head = head.substr(b, e - b);
    // buang "static"/modifier lain + tipe balikan -> ambil kata terakhir
    size_t sp = head.find_last_of(" \t");
    std::string name = (sp == std::string::npos) ? head : head.substr(sp + 1);
    if (name.size() > 1 && name[0] == '~') name = name.substr(1);
    return name;
}

// Indeks nama dari record metadata (cuma layout gaya MLBB yang punya ini).
// 0 = nggak ada / pointer record-nya nggak masuk akal.
uint32_t nameIndexAt(uintptr_t mi) {
    if (g_mi.metaOff == kNoOff) return 0;
    const uintptr_t rec = mem::rdp<uintptr_t>(mi + g_mi.metaOff);
    if (!rec || !mem::canRead(rec, 0x8)) return 0;
    return mem::rdp<uint32_t>(rec);
}

// Nama method yang disimpan LANGSUNG di dalam struct MethodInfo (il2cpp standar,
// `const char* name`). Ini jalur "nama pasti" -- nggak butuh kalibrasi/anchor.
bool nameInStruct(const MiLayout& ly, uintptr_t mi, std::string& out) {
    out.clear();
    if (ly.nameOff == kNoOff) return false;
    const uintptr_t sp = mem::untag(mem::rdp<uintptr_t>(mi + ly.nameOff));
    if (!sp) return false;
    char nm[200];
    if (!readIdent(sp, nm, sizeof(nm))) return false;
    out = nm;
    return true;
}

// ---------------------------------------------------------------------------
// 1. mapping file metadata
// ---------------------------------------------------------------------------
void findMaps() {
    g_maps.clear();
    for (const auto& r : mem::regions()) {
        if (!r.readable() || r.name.empty()) continue;
        const std::string full = r.name;
        if (full.find("metadata") == std::string::npos) continue;
        if (full.size() < 4 || full.compare(full.size() - 4, 4, ".dat") != 0) continue;
        if (r.size() < 0x1000) continue;

        const size_t sl = full.find_last_of('/');
        const std::string base = (sl == std::string::npos) ? full : full.substr(sl + 1);

        // gabung segmen yang nyambung (file besar bisa kepecah beberapa baris)
        bool merged = false;
        for (auto& m : g_maps)
            if (m.path == base && (r.start == m.end || m.start == r.end)) {
                m.start = std::min(m.start, r.start);
                m.end   = std::max(m.end, r.end);
                merged  = true;
                break;
            }
        if (merged) continue;

        MapFile m;
        m.path = base;
        m.start = r.start;
        m.end = r.end;
        g_maps.push_back(std::move(m));
    }
    std::sort(g_maps.begin(), g_maps.end(),
              [](const MapFile& a, const MapFile& b) { return a.start < b.start; });
}

// muat isi mapping ke RAM (biar nama bisa dicari/dibaca cepat tanpa syscall)
bool loadMap(int i) {
    MapFile& m = g_maps[i];
    if (m.loaded) return true;
    size_t n = (size_t)(m.end - m.start);
    if (n > kMaxMapBytes) n = kMaxMapBytes;
    if (g_mapBytes + n > (256u << 20)) return false;
    m.data.resize(n);
    if (!mem::read(m.start, m.data.data(), n)) { m.data.clear(); return false; }
    m.loaded = true;
    g_mapBytes += n;
    return true;
}

// ---------------------------------------------------------------------------
// 2. pindai pool MethodInfo
// ---------------------------------------------------------------------------
bool regionWorth(const mem::Region& r) {
    if (!r.readable() || !r.writable() || r.exec()) return false;
    const size_t sz = r.size();
    if (sz < 0x2000 || sz > (1u << 30)) return false;
    const std::string& nm = r.name;
    // heap dalvik / stack / jit nggak mungkin nyimpen MethodInfo il2cpp
    if (nm.find("dalvik") != std::string::npos) return false;
    if (nm.find("stack") != std::string::npos) return false;
    if (nm.find("jit") != std::string::npos) return false;
    return true;
}

// hasil pindai buat SATU layout
struct PoolCand {
    std::vector<uintptr_t> addr;        // kandidat MethodInfo (alamat runtime)
    std::vector<size_t>    phaseCount;  // sebaran (alamat % stride)
    std::vector<MethodEnt> entries;     // yang lolos validasi
    size_t phase = 0;                   // fase terpilih
    size_t named = 0;                   // entry yang namanya kebaca LANGSUNG
};

bool anyCand(const std::vector<PoolCand>& c) {
    for (const auto& pc : c) if (!pc.addr.empty()) return true;
    return false;
}

size_t countCand(const std::vector<PoolCand>& c) {
    size_t n = 0;
    for (const auto& pc : c) n += pc.addr.size();
    return n;
}

// Validasi kandidat satu layout:
//   * kelasnya harus punya nama yang masuk akal (sama kayak dulu)
//   * kalau layout-nya nyimpen nama method DI DALAM struct (il2cpp standar),
//     nama itu WAJIB kebaca -- itu bukti layout-nya benar, dan sekaligus jadi
//     nama method (nggak butuh anchor/delta).
// Kalau fase (alamat % stride) nggak nyisain satu entry pun, coba lagi tanpa
// filter fase (game bisa nge-malloc pool-nya nggak sejajar stride).
void validateCandPass(size_t L, PoolCand& pc, bool usePhase) {
    const MiLayout& ly = kLayouts[L];
    char nm[160];
    size_t checked = 0;
    const size_t total = pc.addr.size() ? pc.addr.size() : 1;
    for (uintptr_t a : pc.addr) {
        if (abortx::requested()) return;
        if (usePhase && (a % ly.stride) != pc.phase) continue;
        if ((++checked & 2047) == 0)
            abortx::beat("validasi pool method", (float)((double)checked / (double)total));
        const uintptr_t k = mem::untag(mem::rdp<uintptr_t>(a + ly.klassOff));
        if (!k) continue;
        const uintptr_t np = mem::untag(mem::rdp<uintptr_t>(k + kKlassName));
        if (!readIdent(np, nm, sizeof(nm))) continue;

        MethodEnt e;
        e.mi = a;
        e.klass = k;
        e.code = mem::untag(mem::rdp<uintptr_t>(a + kMiCode));
        const char* mod = moduleOf(e.code);
        if (mod) {
            e.module = mod;
            e.rva = mem::rvaOf(e.code);      // stabil antar run (lihat mem.h)
        }
        if (ly.nameOff != kNoOff && !nameInStruct(ly, a, e.name)) continue;
        if (!e.name.empty()) ++pc.named;
        pc.entries.push_back(std::move(e));
    }
}

void validateCand(size_t L, PoolCand& pc) {
    pc.entries.clear();
    pc.named = 0;
    validateCandPass(L, pc, true);
    if (pc.entries.empty()) validateCandPass(L, pc, false);
}

// Nama paling ujung (buang kualifikasi interface/generic):
// "System.Collections.ICollection.get_IsReadOnly" -> "get_IsReadOnly"
std::string tailName(const std::string& s) {
    const size_t d = s.find_last_of('.');
    return (d == std::string::npos) ? s : s.substr(d + 1);
}

// Cocokkan hasil pool satu layout dengan DUMP OFFLINE (sumber independen):
// kelas + RVA + nama method harus sama. Balikin persen kecocokan, -1 kalau
// nggak bisa diuji (dump nggak dimuat / layout-nya nggak punya nama langsung).
int dumpAgree(const std::vector<MethodEnt>& ents, int want, int* checkedOut = nullptr) {
    if (checkedOut) *checkedOut = 0;
    if (!dump::loaded()) return -1;
    int checked = 0, ok = 0;
    for (const MethodEnt& e : ents) {
        if (e.name.empty() || !e.rva) continue;
        char kn[160] = {0};
        if (!readIdent(mem::untag(mem::rdp<uintptr_t>(e.klass + kKlassName)), kn, sizeof(kn))) continue;
        if (++checked > want) break;
        std::vector<const dump::ClassEntry*> hits;
        dump::ClassDetail det;
        if (!dump::find(kn, nullptr, hits) || hits.empty() || !dump::detail(*hits[0], det)) continue;
        for (const auto& dm : det.methods) {
            if (!dm.hasRva || dm.rva != e.rva) continue;
            if (tailName(baseName(dm.name)) == tailName(e.name)) { ++ok; break; }
        }
    }
    if (checkedOut) *checkedOut = checked;
    return checked ? (ok * 100 / checked) : -1;
}

// Bukti MANDIRI (nggak butuh dump): tiap entry yang beneran MethodInfo harus
// ADA di array method kelasnya (Il2CppClass + <offset> -> MethodInfo*, stride 8).
// Layout yang bener = hampir semua entry-nya nongol di situ; layout yang cuma
// "kebetulan mirip" hampir nggak ada. Balikin persen kecocokan, -1 kalau nggak
// bisa diuji.
int arrayAgree(const PoolCand& pc, int want) {
    static const uint32_t kArrOffs[] = {0x98, 0x90, 0xa0, 0x88, 0x80, 0xa8, 0xb0};
    const size_t kEnts = 512;                 // entry yang dicek per offset
    std::vector<uintptr_t> buf(kEnts);
    int checked = 0, ok = 0;
    for (const MethodEnt& e : pc.entries) {
        if (!e.klass || !e.mi) continue;
        if (++checked > want) break;
        bool found = false;
        for (uint32_t off : kArrOffs) {
            uintptr_t arr = 0;
            if (!mem::rd(mem::untag(e.klass) + off, arr) || !arr) continue;
            const mem::Region* r = mem::regionOf(arr);
            if (!r || !r->readable() || r->exec()) continue;
            if (!mem::read(arr, buf.data(), kEnts * sizeof(uintptr_t))) continue;
            for (size_t i = 0; i < kEnts; ++i) {
                const uintptr_t m = mem::untag(buf[i]);
                if (!m) break;
                if (m == e.mi) { found = true; break; }
            }
            if (found) break;
        }
        if (found) ++ok;
    }
    return checked ? (ok * 100 / checked) : -1;
}

bool findPool() {
    const size_t chunk = 1u << 20;
    std::vector<uint64_t> q(chunk / 8 + 8);
    std::vector<PoolCand> c(kLayoutCount);
    for (size_t i = 0; i < kLayoutCount; ++i) c[i].phaseCount.assign(kLayouts[i].stride, 0);
    size_t scanned = 0;
    const size_t capBytes = g_capMB << 20;
    const size_t margin  = 0x60;      // byte ekor region yang nggak dipakai

    buildExec();

    // tahap 1: heap game dulu (di MLBB pool-nya ada di situ). Nama region heap
    // beda-beda antar ROM (libc_malloc / scudo / jemalloc) -- lihat heapLike().
    size_t nextLog = 0;
    for (int pass = 0; pass < 2 && !anyCand(c); ++pass) {
        for (const auto& r : mem::regions()) {
            if (abortx::requested()) { g_err = "pindai pool dibatalkan (Stop)"; return false; }
            if (!regionWorth(r)) continue;
            const bool isMalloc = heapLike(r.name);
            if (pass == 0 && !isMalloc) continue;          // tahap 1: cuma heap
            if (pass == 1 && isMalloc && anyCand(c)) continue;
            if (scanned > capBytes) break;

            for (uintptr_t a = r.start; a + margin < r.end; ) {
                if (abortx::requested()) { g_err = "pindai pool dibatalkan (Stop)"; return false; }
                // progres tiap 256 MB: kelihatan di log kalau device-nya lambat,
                // dan UI tetap tahu kerjanya hidup (tombol Stop tetap masuk akal).
                if (scanned >= nextLog) {
                    nextLog = scanned + (256u << 20);
                    abortx::beat("pindai pool method",
                                 capBytes ? (float)((double)scanned / (double)capBytes) : -1.0f);
                    logx::add("[pool] pindai tahap %d: %zu MB, %zu kandidat (semua layout)",
                              pass + 1, scanned >> 20, countCand(c));
                }
                size_t want = (size_t)std::min<uintptr_t>(chunk, r.end - a - margin);
                want &= ~(size_t)7;
                if (want < 0x40) break;
                if (!mem::read(a, q.data(), want)) break;        // region berubah/hilang
                const size_t nq = want / 8;
                // SEMUA layout dicek dalam SATU lintasan: yang dibaca cuma array
                // qword yang udah ada di buffer (nggak ada syscall tambahan),
                // jadi nyobain 6 layout harganya hampir sama kayak 1.
                for (size_t i = 0; i + margin / 8 < nq; ++i) {
                    const uint64_t q0 = q[i];
                    if (!q0) continue;
                    const uintptr_t code = mem::untag((uintptr_t)q0);
                    if (!inExec(code)) continue;
                    const uintptr_t addr = a + i * 8;
                    for (size_t L = 0; L < kLayoutCount; ++L) {
                        const MiLayout& ly = kLayouts[L];
                        const uintptr_t k = mem::untag(q[i + ly.klassOff / 8]);
                        if (!k) continue;
                        const mem::Region* kr = mem::regionOf(k);
                        if (!kr || !kr->readable() || kr->exec()) continue;
                        if (ly.nameOff != kNoOff) {
                            // nama langsung di struct -> harus nunjuk region kebaca
                            const uintptr_t npx = mem::untag(q[i + ly.nameOff / 8]);
                            if (!npx) continue;
                            const mem::Region* nr = mem::regionOf(npx);
                            if (!nr || !nr->readable() || nr->exec()) continue;
                        } else {
                            // record metadata -> harus nunjuk mapping .dat
                            const uintptr_t md = (uintptr_t)q[i + ly.metaOff / 8];
                            if (!md || !inMap(md)) continue;
                        }
                        c[L].addr.push_back(addr);
                        ++c[L].phaseCount[addr % ly.stride];
                    }
                }
                a += want;
                scanned += want;
                if (scanned > capBytes) break;
            }
            if (scanned > capBytes) break;
        }
    }
    g_stats.scannedMB = scanned >> 20;
    g_stats.cand = countCand(c);
    if (!g_stats.cand) { g_err = "pool MethodInfo nggak ketemu"; return false; }

    // fase pool per layout = (alamat % stride) yang paling banyak kena
    for (size_t L = 0; L < kLayoutCount; ++L) {
        PoolCand& pc = c[L];
        if (pc.addr.empty()) continue;
        pc.phase = 0;
        for (size_t i = 1; i < pc.phaseCount.size(); ++i)
            if (pc.phaseCount[i] > pc.phaseCount[pc.phase]) pc.phase = i;
    }

    // ---- validasi tiap layout, terus pilih pemenangnya ---------------------
    for (size_t L = 0; L < kLayoutCount; ++L) {
        if (c[L].addr.empty()) {
            logx::add("[pool] layout '%s': 0 kandidat", kLayouts[L].tag);
            continue;
        }
        if (abortx::requested()) { g_err = "pindai pool dibatalkan (Stop)"; return false; }
        validateCand(L, c[L]);
        logx::add("[pool] layout '%s': %zu kandidat, fase 0x%zx, %zu entry valid, %zu nama kebaca",
                  kLayouts[L].tag, c[L].addr.size(), c[L].phase, c[L].entries.size(), c[L].named);
    }

    int win = -1;
    if (g_forceLayout >= 0 && g_forceLayout < (int)kLayoutCount) {
        win = g_forceLayout;
        logx::add("[pool] layout DIPAKSA dari CLI: %s", kLayouts[win].tag);
    } else {
        // (0) Bukti MANDIRI: entry-nya ada nggak di array method kelasnya sendiri?
        //     Ini yang paling kuat dan nggak butuh apa-apa dari luar -- struktur
        //     lain yang "kebetulan mirip" nggak mungkin nongol di array itu.
        int bestAgree = -1, agreeEntries = 0;
        for (size_t L = 0; L < kLayoutCount; ++L) {
            if (c[L].entries.empty()) continue;
            const int ag = arrayAgree(c[L], 60);
            if (ag < 0) continue;
            logx::add("[pool] layout '%s': %d%% entry-nya ada di array method kelasnya",
                      kLayouts[L].tag, ag);
            if (ag > bestAgree || (ag == bestAgree && (int)c[L].entries.size() > agreeEntries)) {
                bestAgree = ag;
                agreeEntries = (int)c[L].entries.size();
                win = (int)L;
            }
        }
        if (win >= 0 && bestAgree < 50) {
            // (0b) kalau array kelas nggak bisa dipakai buat mastiin, coba
            //      cocokkan sama dump offline (kalau dimuat).
            win = -1;
            int bestDump = -1;
            agreeEntries = 0;
            for (size_t L = 0; L < kLayoutCount; ++L) {
                if (c[L].entries.empty() || c[L].named == 0) continue;
                const int ag = dumpAgree(c[L].entries, 120);
                if (ag < 0) continue;
                logx::add("[pool] layout '%s': cocok sama dump %d%% (%zu entry)",
                          kLayouts[L].tag, ag, c[L].entries.size());
                if (ag > bestDump || (ag == bestDump && (int)c[L].entries.size() > agreeEntries)) {
                    bestDump = ag;
                    agreeEntries = (int)c[L].entries.size();
                    win = (int)L;
                }
            }
            bestAgree = bestDump;
            if (win < 0 || bestDump < 30) bestAgree = -1;
        }
        if (win >= 0 && bestAgree >= 0) {
            logx::add("[pool] layout '%s' dipilih dari kecocokan (%d%%)",
                      kLayouts[win].tag, bestAgree);
        } else {
            // (1) nggak ada dump -> layout yang nama method-nya kebaca LANGSUNG
            //     menang (nama asli dari struct, nggak butuh anchor).
            win = -1;
            size_t bestNamed = 0;
            for (size_t L = 0; L < kLayoutCount; ++L)
                if (c[L].named > bestNamed) { bestNamed = c[L].named; win = (int)L; }
            if (bestNamed < kMinDirectNamed) {
                // (2) nggak ada yang nyimpen nama di struct (MLBB) -> pakai layout
                //     dengan entry valid TERBANYAK; seri = paling awal di tabel.
                if (win >= 0)
                    logx::add("[pool] layout '%s' cuma %zu nama kebaca -- pilih entry terbanyak",
                              kLayouts[win].tag, bestNamed);
                int be = -1;
                win = -1;
                for (size_t L = 0; L < kLayoutCount; ++L) {
                    const int n = (int)c[L].entries.size();
                    if (n > be) { be = n; win = (int)L; }
                }
            }
        }
    }
    if (win < 0 || c[(size_t)win].entries.empty()) {
        g_err = "pool ketemu tapi isinya nggak valid";
        return false;
    }

    g_mi      = kLayouts[win];
    g_entries = std::move(c[(size_t)win].entries);
    g_stats.phase   = c[(size_t)win].phase;
    g_stats.entries = g_entries.size();
    logx::add("[pool] layout DIPAKAI: %s -- %zu entry, %zu nama kebaca",
              g_mi.tag, g_entries.size(), c[(size_t)win].named);
    return true;
}

// ---------------------------------------------------------------------------
// 3. kalibrasi delta nama
// ---------------------------------------------------------------------------
void voteAnchor(std::map<std::pair<int, int>, std::map<uint64_t, int>>& votes,
                uint32_t idx, const char* name) {
    const int space = (int)(idx >> 24);
    if (space <= 0 || !name || !*name) return;
    std::string pat(name);
    pat += '\0';
    for (size_t mi = 0; mi < g_maps.size(); ++mi) {
        if (anchorOut()) return;                 // Stop / waktu habis
        if (!loadMap((int)mi)) continue;
        const size_t pos = findIn(g_maps[mi].data, pat.c_str());
        if (pos == std::string::npos) continue;
        const uint64_t delta = (uint64_t)idx - (uint64_t)pos;
        votes[{space, (int)mi}][delta] += 1;
    }
}

// ---------------------------------------------------------------------------
// tabel METHOD DEFINITION di file metadata
//
// Tiap MethodInfo nunjuk record 0x20 byte di +0x28. Isinya:
//
//     +0x00 dword index nama (space<<24 | offset)
//     +0x04 dword ID TIPE PEMILIK -- semua method satu kelas punya nilai ini
//     +0x08 dword tipe balikan/parameter
//     +0x14 dword token 0x06xxxxxx (MethodDef)
//
// PENTING: tabel ini LENGKAP. MethodInfo cuma dibuat buat method yang udah
// dipakai game (nyata: BattleManager 20 dari 121), tapi record-nya ada semua.
// Jadi dari sini daftar method per kelas bisa dilengkapi -- nama + token dari
// metadata, RVA-nya nyusul kalau MethodInfo-nya udah kebentuk.
// ---------------------------------------------------------------------------
struct RecordEnt {
    uintptr_t ptr = 0;
    uint32_t  idx = 0;
    uint32_t  typeId = 0;
    uint32_t  token = 0;
    size_t    miEnt = (size_t)-1;   // index di g_entries (kalau MethodInfo-nya ada)
};

std::vector<RecordEnt> g_records;
std::map<uint32_t, std::vector<size_t>> g_recByType;   // typeId -> record
std::map<uint32_t, uintptr_t> g_typeKlass;             // typeId -> Il2CppClass*

bool recordLooks(uintptr_t p) {
    if (!p || !mem::canRead(p, 0x20)) return false;
    const uint32_t idx = mem::rdp<uint32_t>(p);
    const uint32_t type = mem::rdp<uint32_t>(p + 4);
    const uint32_t tok = mem::rdp<uint32_t>(p + 0x14);
    if (!idx || !type) return false;
    if ((tok >> 24) != 0x06) return false;          // MethodDef token
    return true;
}

void buildRecords() {
    if (g_entries.empty()) return;
    if (g_mi.metaOff == kNoOff) return;      // layout ini nggak punya record metadata

    // Batas tabel method-def.
    //
    // CATATAN PENTING (ini penyebab dump "jalan >10 menit nggak selesai"):
    // dulu batasnya cuma min/max alamat record yang ketemu, terus jarak itu
    // disisir 0x20 per langkah. Kalau ada SATU pointer 'outlier' (record yang
    // ketemu di tempat lain -- cukup satu biar min..max-nya jadi ratusan MB /
    // GB), loop-nya nyisir ujung ke ujung dan praktis nggak pernah selesai.
    // Nyata kejadian di MLBB in-lobby: prosesnya nggantung di fase ini
    // sementara dump sebelumnya (di layar login) cepat.
    //
    // Sekarang: alamat record DIKLASTER dulu (tabelnya satu blok berdekatan),
    // blok terpadat yang dipakai, sisanya diabaikan. Plus batas lebar 64 MB
    // biar apa pun isi memorinya, loop-nya tetap terbatas.
    uintptr_t lo = (uintptr_t)-1, hi = 0;
    std::map<uintptr_t, size_t> recOfMi;
    for (size_t i = 0; i < g_entries.size(); ++i) {
        const uintptr_t r = mem::rdp<uintptr_t>(g_entries[i].mi + g_mi.metaOff);
        if (!r) continue;
        recOfMi[r] = i;
        if (r < lo) lo = r;
        if (r > hi) hi = r;
    }
    if (lo == (uintptr_t)-1) return;

    {
        std::vector<uintptr_t> addrs;
        addrs.reserve(recOfMi.size());
        for (const auto& kv : recOfMi) addrs.push_back(kv.first);
        std::sort(addrs.begin(), addrs.end());
        size_t i0 = 0, bestI = 0, bestN = 0;
        for (size_t i = 0; i < addrs.size(); ++i) {
            if (i && addrs[i] - addrs[i - 1] > (1u << 20)) i0 = i;   // jeda >1MB = blok baru
            if (i - i0 + 1 > bestN) { bestN = i - i0 + 1; bestI = i0; }
        }
        const uintptr_t cLo = addrs[bestI];
        const uintptr_t cHi = addrs[bestI + bestN - 1];
        if (cLo != lo || cHi != hi)
            logx::add("[pool] tabel record diklaster: 0x%lx..0x%lx (%zu record, %zu MB) "
                      "-- outlier 0x%lx..0x%lx dibuang",
                      (unsigned long)cLo, (unsigned long)cHi, bestN,
                      (unsigned long)((cHi - cLo + 0x20) >> 20),
                      (unsigned long)lo, (unsigned long)hi);
        lo = cLo;
        hi = cHi;
    }

    // batas lebar: apa pun yang terjadi, jangan nyisir lebih dari 64 MB
    constexpr uintptr_t kMaxSpan = 64u << 20;
    if (hi - lo > kMaxSpan) {
        logx::add("[pool] tabel record kelebaran (%lu MB) -- dipotong ke %lu MB",
                  (unsigned long)((hi - lo) >> 20), (unsigned long)(kMaxSpan >> 20));
        hi = lo + kMaxSpan;
    }

    uintptr_t a = lo;
    for (int i = 0; i < 100000 && a >= 0x20 && recordLooks(a - 0x20); ++i) {
        if ((i & 8191) == 0 && abortx::requested()) return;
        a -= 0x20;
    }
    lo = a;
    for (int i = 0; i < 100000 && recordLooks(hi + 0x20); ++i) {
        if ((i & 8191) == 0 && abortx::requested()) return;
        hi += 0x20;
    }

    logx::add("[pool] baca tabel record 0x%lx..0x%lx (%lu MB)",
              (unsigned long)lo, (unsigned long)hi, (unsigned long)((hi - lo) >> 20));
    size_t steps = 0;
    const uintptr_t span = (hi - lo) ? (hi - lo) : 1;
    for (uintptr_t p = lo; p <= hi; p += 0x20) {
        if ((++steps & 8191) == 0) {
            if (abortx::requested()) return;
            abortx::beat("baca tabel method-def", (float)((double)(p - lo) / (double)span));
        }
        if (!recordLooks(p)) continue;
        RecordEnt r;
        r.ptr = p;
        r.idx = mem::rdp<uint32_t>(p);
        r.typeId = mem::rdp<uint32_t>(p + 4);
        r.token = mem::rdp<uint32_t>(p + 0x14);
        auto it = recOfMi.find(p);
        r.miEnt = (it == recOfMi.end()) ? (size_t)-1 : it->second;
        g_recByType[r.typeId].push_back(g_records.size());
        g_records.push_back(r);
    }

    // typeId -> kelas (dari MethodInfo yang udah kebentuk)
    for (const auto& e : g_entries) {
        const uintptr_t r = mem::rdp<uintptr_t>(e.mi + g_mi.metaOff);
        if (!r) continue;
        const uint32_t type = mem::rdp<uint32_t>(r + 4);
        if (type) g_typeKlass[type] = e.klass;
    }
    logx::add("[pool] record method-def: %zu record, %zu tipe pemilik, %zu kepetakan ke kelas",
              g_records.size(), g_recByType.size(), g_typeKlass.size());
}

// ---------------------------------------------------------------------------
// typeId -> kelas (peta LENGKAP)
//
// buildRecords() cuma bisa memetakan typeId yang MethodInfo-nya udah dibikin
// engine. Di MLBB nyata: 2.464 tipe dari puluhan ribu -- jadi ribuan kelas
// daftar method-nya bolong. Padahal Il2CppClass menyimpan typeId-nya sendiri,
// jadi kalau offsetnya ketemu, SEMUA record bisa dipetakan.
//
// Offsetnya dicari saat jalan (nggak di-hardcode): kelas yang typeId-nya udah
// diketahui dipakai jadi sampel, lalu dihitung di offset berapa dword-nya
// cocok dengan typeId kelas itu. Offset dengan kecocokan terbanyak menang --
// dan harus menang JELAS, biar kebetulan tidak ikut kepakai.
// ---------------------------------------------------------------------------
std::map<uint32_t, uintptr_t> g_typeKlassAll;
uint32_t g_typeOff = 0;
uint32_t g_typeDiff = 0;    // nilai-di-kelas = indeks + geseran ini
int      g_typeWidth = 4;   // 4 byte (dword) atau 2 byte (halfword)
static size_t g_typeCollide = 0;   // berapa kelas yang indeksnya bentrok

// indeks tipe yang disimpan di kelas: 24 bit bawah dari typeId record
// (byte paling atas = nomor file metadata / image). Kalau field-nya cuma 2 byte,
// yang kelihatan 16 bit bawah.
static inline uint32_t typeIdxMask() { return g_typeWidth == 2 ? 0x0000FFFFu : 0x00FFFFFFu; }
static inline uint32_t typeIdxOf(uint32_t typeId) { return typeId & typeIdxMask(); }

// ---------------------------------------------------------------------------
// Cari FIELD indeks tipe di dalam Il2CppClass.
//
// Pencarian "nilai sama persis" gagal di MLBB, dan alasannya masuk akal:
//   1. indeks yang disimpan kelas bisa DIGESER konstanta (registry global vs
//      indeks per-image) -> nilainya nggak sama walau maksudnya sama;
//   2. lebarnya bisa 2 byte (indeks tipe < 65536), bukan 4 byte.
// Jadi yang dicari bukan "nilainya sama", tapi "SELISIHNYA (nilai - indeks)
// selalu sama di offset yang sama". Itu tanda kuat: cuma field indeks yang
// selisihnya konsisten di ratusan kelas.
// ---------------------------------------------------------------------------
struct TypeFieldCand {
    uint32_t off = 0;
    int      width = 4;
    uint32_t mask = 0xFFFFFFFFu;   // tafsir nilainya (lihat findTypeField)
    uint32_t diff = 0;             // nilai = (indeks & mask) + diff
    int      hits = 0;
};

// balikin true kalau ada kandidat yang layak dipakai; `all` diisi semua kandidat
// (buat diagnostik --typeprobe)
static bool findTypeField(int samples, TypeFieldCand& out,
                          std::vector<TypeFieldCand>& all, int* usedOut,
                          std::vector<std::vector<uint8_t>>* bufOut = nullptr,
                          std::vector<uint32_t>* idOut = nullptr) {
    const uint32_t kScan = 0x800;
    std::vector<std::vector<uint8_t>> bufs;
    std::vector<uint32_t> ids;
    bufs.reserve(samples);
    ids.reserve(samples);
    for (const auto& kv : g_typeKlass) {
        if ((int)bufs.size() >= samples) break;
        const uintptr_t k = mem::untag(kv.second);
        if (!k || !mem::canRead(k, kScan)) continue;
        std::vector<uint8_t> b(kScan);
        if (!mem::read(k, b.data(), kScan)) continue;
        if (b[0] == 0 && b[4] == 0 && b[8] == 0) continue;   // kelas kosong/garbage
        bufs.push_back(std::move(b));
        ids.push_back(kv.first);
    }
    if (usedOut) *usedOut = (int)bufs.size();
    if (bufs.size() < 6) return false;
    if (bufOut) *bufOut = bufs;
    if (idOut)  *idOut  = ids;

    all.clear();
    TypeFieldCand best;
    int second = 0;
    // Tiga tafsir nilai yang disimpan kelas:
    //   dword penuh      -> typeId apa adanya (image << 24 | indeks)
    //   dword 24 bit     -> indeksnya aja (byte image-nya dibuang/ketimpa flag)
    //   halfword 16 bit  -> indeksnya aja, lebarnya 2 byte
    struct Enc { int width; uint32_t mask; };
    for (const Enc e : {Enc{4, 0xFFFFFFFFu}, Enc{4, 0x00FFFFFFu}, Enc{2, 0x0000FFFFu}}) {
        const uint32_t mask = e.mask;
        for (uint32_t off = 0; off + (uint32_t)e.width <= kScan; off += (uint32_t)e.width) {
            std::map<uint32_t, int> hist;                 // selisih -> jumlah
            for (size_t i = 0; i < bufs.size(); ++i) {
                uint32_t v = 0;
                memcpy(&v, bufs[i].data() + off, (size_t)e.width);
                const uint32_t d = (v - (ids[i] & mask)) & mask;
                ++hist[d];
            }
            auto top = hist.begin();
            for (auto it = hist.begin(); it != hist.end(); ++it)
                if (it->second > top->second) top = it;
            TypeFieldCand c;
            c.off = off;
            c.width = e.width;
            c.mask = mask;
            c.diff = top->first;
            c.hits = top->second;
            all.push_back(c);
            if (c.hits > best.hits) { second = best.hits; best = c; }
            else if (c.hits > second) second = c.hits;
        }
    }
    std::sort(all.begin(), all.end(),
              [](const TypeFieldCand& a, const TypeFieldCand& b) {
                  if (a.hits != b.hits) return a.hits > b.hits;
                  if (a.width != b.width) return a.width > b.width;
                  return a.off < b.off;
              });

    const int used = (int)bufs.size();
    // syarat: mayoritas kelas cocok, dan menang JELAS atas kandidat lain
    if (best.hits < 8 || best.hits * 3 < used * 2) return false;
    if (second * 2 > best.hits) return false;
    out = best;
    return true;
}

uint32_t detectTypeOffset(int samples) {
    if (g_typeKlass.size() < 6) return 0;
    TypeFieldCand best;
    std::vector<TypeFieldCand> all;
    int used = 0;
    if (!findTypeField(samples, best, all, &used)) {
        logx::add("[pool] field indeks tipe kelas NGGAK ketemu (%d sampel; kandidat terbaik "
                  "+0x%x/%dbyte %d cocok) -- cek --typeprobe",
                  used, all.empty() ? 0 : all[0].off, all.empty() ? 0 : all[0].width,
                  all.empty() ? 0 : all[0].hits);
        return 0;
    }
    g_typeWidth = best.width;
    g_typeDiff  = best.diff;
    logx::add("[pool] field indeks tipe kelas: +0x%x (%d byte, geseran 0x%x) -- %d/%d sampel"
              " cocok", best.off, best.width, best.diff, best.hits, used);
    return best.off;
}

void buildTypeMap() {
    if (!g_typeOff) return;
    const uint32_t mask = typeIdxMask();
    std::map<uint32_t, std::pair<uintptr_t, int>> tmp;   // indeks -> kelas (pilih 1)
    size_t skipped = 0;
    for (const auto& e : g_entries) {
        const uintptr_t k = mem::untag(e.klass);
        if (!k) continue;
        const uint32_t v = g_typeWidth == 2
                               ? (uint32_t)mem::rdp<uint16_t>(k + g_typeOff)
                               : mem::rdp<uint32_t>(k + g_typeOff);
        const uint32_t idx = (v - g_typeDiff) & mask;
        if (!idx) { ++skipped; continue; }
        auto it = tmp.find(idx);
        if (it == tmp.end()) {
            tmp.emplace(idx, std::make_pair(k, 1));
        } else {
            ++it->second.second;
            if (it->second.first != k) ++g_typeCollide;
        }
    }
    for (const auto& kv : tmp) g_typeKlassAll.emplace(kv.first, kv.second.first);
    logx::add("[pool] peta indeks-tipe dari kelas: %zu indeks (%zu bentrok, %zu kelas dilewat)",
              g_typeKlassAll.size(), g_typeCollide, skipped);
}

// ---------------------------------------------------------------------------
// TOKEN TypeDef di dalam Il2CppClass (jalur yang NYATA jalan di MLBB)
//
// Il2CppClass punya dword token metadata-nya sendiri: byte paling atas 0x02
// (tabel TypeDef), 3 byte bawah = nomor baris di tabel TypeDef. Indeks tipe yang
// dipakai RECORD beda tipis dari token itu, dan selisihnya KONSTAN PER FILE
// METADATA (bukan per kelas):
//
//     indeks record = (token & 0xFFFFFF) + K[space]        space = byte atas typeId
//
// Contoh nyata MLBB 22.1.97.12061 (dicek satu-satu vs dump offline):
//   SafeFileHandle (space 1 / liblogic.so): token 0x2000067, indeks 0x66   -> K=-1
//   kelas space 3 (libcsharp.so)          : token 0x2000e4d, indeks 0xfc0  -> K=+371
//
// K TIDAK di-hardcode: diukur dari kelas yang typeId-nya udah diketahui (dari
// record MethodInfo), hasilnya divote per space.
// ---------------------------------------------------------------------------
constexpr uint32_t kKlassImage  = 0x00;    // Il2CppImage*
constexpr uint32_t kTokScan     = 0x800;   // lebar pemindaian Il2CppClass

uint32_t g_tokOff = 0;                     // offset token TypeDef (0 = nggak ada)
std::map<uint32_t, int32_t> g_tokK;         // space -> geseran (indeks = baris + K)
std::map<uint32_t, uintptr_t> g_tokMap;     // typeId LENGKAP -> Il2CppClass*

struct TokCand {
    uint32_t off = 0;
    int      hit = 0;                        // kelas yang dword-nya bertoken TypeDef
    int      score = 0;                      // total suara pemenang tiap space
    std::map<uint32_t, int32_t> k;           // space -> geseran
    std::map<uint32_t, int>     perSpace;    // space -> berapa yang cocok
    std::map<uint32_t, int>     perSpaceTok; // space -> berapa yang punya token
};

// Sampel: kelas yang typeId-nya udah diketahui, dibaca sekali per kelas.
//
// JATAHNYA DIBAGI PER SPACE -- ini penting dan dulu kelewat: kalau sampelnya
// cuma diambil urut dari daftar, SPACE TERBESAR bisa nggak kebagian sama sekali
// (nyata di MLBB: 400 sampel pertama isinya space 1+2 doang, padahal space 3
// punya 1.670 tipe -- jadi K space 3 nggak keukur dan seluruh peta token kosong).
static void tokSamples(int samples, std::vector<std::vector<uint8_t>>& bufs,
                       std::vector<uint32_t>& ids) {
    std::map<uint32_t, int> nSpace;
    for (const auto& kv : g_typeKlass) nSpace[kv.first >> 24] += 1;
    if (nSpace.empty()) return;
    const int perSpace = std::max(8, samples / (int)nSpace.size());

    bufs.reserve((size_t)samples);
    ids.reserve((size_t)samples);
    std::map<uint32_t, int> taken;
    for (int round = 0; round < 2; ++round) {
        for (const auto& kv : g_typeKlass) {
            if ((int)bufs.size() >= samples) break;
            const uint32_t sp = kv.first >> 24;
            if (round == 0 && taken[sp] >= perSpace) continue;
            const uintptr_t k = mem::untag(kv.second);
            if (!k || !mem::canRead(k, kTokScan)) continue;
            std::vector<uint8_t> b(kTokScan);
            if (!mem::read(k, b.data(), kTokScan)) continue;
            if (b[0] == 0 && b[4] == 0 && b[8] == 0) continue;   // kelas kosong/garbage
            bufs.push_back(std::move(b));
            ids.push_back(kv.first);
            ++taken[sp];
        }
    }
}

static bool findTokenField(int samples, TokCand& best, std::vector<TokCand>& all,
                           int* usedOut) {
    std::vector<std::vector<uint8_t>> bufs;
    std::vector<uint32_t> ids;
    tokSamples(samples, bufs, ids);
    if (usedOut) *usedOut = (int)bufs.size();
    if (bufs.size() < 6) return false;

    // berapa sampel per space (buat ngukur "berapa persen yang berkode di space itu")
    std::map<uint32_t, int> spaceTot;
    for (uint32_t id : ids) spaceTot[id >> 24] += 1;

    all.clear();
    for (uint32_t off = 0; off + 4 <= kTokScan; off += 4) {
        std::map<uint32_t, std::map<int64_t, int>> v;      // space -> selisih -> jumlah
        std::map<uint32_t, int> spaceTok;                  // space -> punya token
        int hit = 0;
        for (size_t i = 0; i < bufs.size(); ++i) {
            uint32_t t = 0;
            memcpy(&t, bufs[i].data() + off, 4);
            if ((t >> 24) != 0x02) continue;               // bukan token tabel TypeDef
            const uint32_t sp = ids[i] >> 24;
            if (!sp) continue;
            ++hit;
            ++spaceTok[sp];
            ++v[sp][(int64_t)(ids[i] & 0x00FFFFFFu) - (int64_t)(t & 0x00FFFFFFu)];
        }
        if (hit < 8) continue;
        TokCand c;
        c.off = off;
        c.hit = hit;
        for (auto& sp : v) {
            int top = 0;
            int64_t topD = 0;
            for (auto& d : sp.second)
                if (d.second > top) { top = d.second; topD = d.first; }
            c.perSpaceTok[sp.first] = spaceTok.count(sp.first) ? spaceTok[sp.first] : 0;
            // K cuma dipakai kalau MAYORITAS sampel space itu sepakat -- kalau
            // nggak, space-nya dibiarin nggak punya K (nanti dicari lewat jalur
            // "jawaban tunggal") daripada dipakai buat nebak-nebak.
            if (top * 2 < c.perSpaceTok[sp.first]) continue;
            c.k[sp.first] = (int32_t)topD;
            c.perSpace[sp.first] = top;
            c.score += top;
        }
        all.push_back(std::move(c));
    }
    if (all.empty()) return false;
    std::sort(all.begin(), all.end(), [](const TokCand& a, const TokCand& b) {
        if (a.score != b.score) return a.score > b.score;
        return a.off < b.off;
    });
    best = all[0];
    const int used = (int)bufs.size();
    // syarat: mayoritas kelas punya token di offset ini, dan menang JELAS
    if (best.score < 12 || best.score * 5 < used * 3) return false;
    if (all.size() > 1 && all[1].score * 2 > best.score) return false;
    return true;
}

uint32_t detectTypeTokenOffset(int samples) {
    if (g_typeKlass.size() < 6) return 0;
    TokCand best;
    std::vector<TokCand> all;
    int used = 0;
    if (!findTokenField(samples, best, all, &used)) {
        logx::add("[pool] token TypeDef di kelas NGGAK ketemu (%d sampel; kandidat terbaik "
                  "+0x%x skor %d/%d) -- peta tipe cuma dari record MethodInfo",
                  used, all.empty() ? 0 : all[0].off,
                  all.empty() ? 0 : all[0].score, used);
        return 0;
    }
    std::string b;
    for (const auto& sp : best.perSpaceTok) {
        char t[80];
        const bool ok = best.k.count(sp.first) != 0;
        snprintf(t, sizeof(t), " space %u: %s%s (%d/%d)", sp.first,
                 ok ? "K=" : "K=?", ok ? std::to_string(best.k.at(sp.first)).c_str() : "-",
                 ok ? best.perSpace.at(sp.first) : 0, sp.second);
        b += t;
    }
    logx::add("[pool] token TypeDef di kelas: +0x%x -- %d/%d kelas dword-nya bertoken,%s",
              best.off, best.hit, used, b.c_str());
    g_tokK = best.k;
    return best.off;
}

// rentang indeks tipe per space (dari tabel record -- SEMUA tipe ada di situ)
std::map<uint32_t, std::pair<uint32_t, uint32_t>> g_spaceRange;
std::map<uintptr_t, uint32_t> g_spaceByImage;
int g_imgPtrCount = 0;

void computeSpaceRanges() {
    g_spaceRange.clear();
    for (const auto& r : g_records) {
        const uint32_t sp = r.typeId >> 24;
        if (!sp) continue;
        const uint32_t idx = r.typeId & 0x00FFFFFFu;
        auto it = g_spaceRange.find(sp);
        if (it == g_spaceRange.end()) g_spaceRange[sp] = std::make_pair(idx, idx);
        else {
            if (idx < it->second.first)  it->second.first  = idx;
            if (idx > it->second.second) it->second.second = idx;
        }
    }
}

// space dari sebuah kelas lewat pointer image (+0x00). Kelas satu image selalu
// punya pointer image yang sama -- jadi ini yang bikin token bisa dipetakan
// walau indeksnya bentrok antar file metadata (indeks itu per FILE, bukan
// global: space 1 punya indeks 0x66..0x1791, space 3 punya 0x6..0x4f54).
void buildSpaceByImage() {
    g_spaceByImage.clear();
    std::map<uintptr_t, std::map<uint32_t, int>> v;
    for (const auto& kv : g_typeKlass) {
        const uintptr_t k = mem::untag(kv.second);
        if (!k) continue;
        const uintptr_t img = mem::untag(mem::rdp<uintptr_t>(k + kKlassImage));
        if (!img) continue;
        ++v[img][kv.first >> 24];
    }
    g_imgPtrCount = (int)v.size();
    for (auto& e : v) {
        uint32_t best = 0;
        int top = 0, tot = 0;
        for (auto& s : e.second) { tot += s.second; if (s.second > top) { top = s.second; best = s.first; } }
        if (best && top * 2 >= tot) g_spaceByImage[e.first] = best;
    }
    logx::add("[pool] pointer image kelas: %zu beda buat %zu kelas -- %s",
              v.size(), g_typeKlass.size(),
              v.size() <= 64 ? "dipakai buat nentuin space dari token"
                             : "nggak dipakai (+0x00 bukan pointer image)");
    if (v.size() > 64) g_spaceByImage.clear();
}

int spaceOfKlass(uintptr_t k) {
    if (g_spaceByImage.empty()) return 0;
    const uintptr_t img = mem::untag(mem::rdp<uintptr_t>(k + kKlassImage));
    auto it = g_spaceByImage.find(img);
    return it == g_spaceByImage.end() ? 0 : (int)it->second;
}

// token -> typeId, jalur cadangan (kalau pointer image nggak nunjuk space yang
// jelas): coba semua space yang punya K, terima CUMA kalau jawabannya tunggal,
// indeksnya masuk rentang space itu, dan tipenya emang ada record-nya.
static bool typeFromTokenUnique(uint32_t rowid, uint32_t& out) {
    int hits = 0;
    for (const auto& kv : g_tokK) {
        const int64_t idx = (int64_t)rowid + kv.second;
        if (idx < 0 || idx > 0x00FFFFFF) continue;
        auto r = g_spaceRange.find(kv.first);
        if (r == g_spaceRange.end()) continue;
        if ((uint32_t)idx < r->second.first || (uint32_t)idx > r->second.second) continue;
        const uint32_t tid = (kv.first << 24) | (uint32_t)idx;
        if (!g_recByType.count(tid)) continue;
        out = tid;
        ++hits;
    }
    return hits == 1;
}

// Bangun peta typeId(LENGKAP)->kelas dari token. Ini yang bikin kelas yang
// daftar method-nya bolong (karena MethodInfo-nya belum dibikin engine) tetap
// kepetakan selama kelasnya ada di pool.
struct TokStats { size_t ok = 0, byImg = 0, uniq = 0, noTok = 0, dup = 0, clash = 0; };
TokStats g_tokStat;

size_t buildTypeMapFromToken() {
    if (!g_tokOff) return 0;
    computeSpaceRanges();
    buildSpaceByImage();

    // kelas yang udah kita tahu PASTI (dari record MethodInfo) ditaruh duluan --
    // ini yang paling dipercaya, token cuma nambah sisanya.
    std::map<uint32_t, uintptr_t> out;
    for (const auto& kv : g_typeKlass) {
        const uintptr_t k = mem::untag(kv.second);
        if (k) out[kv.first] = k;
    }

    TokStats st;
    for (const auto& kv : g_byKlass) {
        const uintptr_t k = mem::untag(kv.first);
        if (!k || !mem::canRead(k + g_tokOff, 4)) continue;
        const uint32_t tok = mem::rdp<uint32_t>(k + g_tokOff);
        if ((tok >> 24) != 0x02) { ++st.noTok; continue; }
        const uint32_t rowid = tok & 0x00FFFFFFu;

        uint32_t tid = 0;
        const int sp = spaceOfKlass(k);
        if (sp && g_tokK.count((uint32_t)sp)) {
            const int64_t idx = (int64_t)rowid + g_tokK[(uint32_t)sp];
            if (idx >= 0 && idx <= 0x00FFFFFF) {
                const uint32_t t = ((uint32_t)sp << 24) | (uint32_t)idx;
                if (g_recByType.count(t)) { tid = t; ++st.byImg; }
            }
        }
        if (!tid) {
            if (!typeFromTokenUnique(rowid, tid)) { ++st.noTok; continue; }
            ++st.uniq;
        }
        auto it = out.find(tid);
        if (it == out.end()) { out[tid] = k; ++st.ok; }
        else if (it->second != k) { ++st.clash; if (it->second == 0) it->second = k; }
        else ++st.dup;
    }
    const size_t total = out.size();
    g_tokMap.swap(out);
    g_tokStat = st;
    logx::add("[pool] peta tipe dari TOKEN: %zu tipe (MethodInfo %zu + token image %zu + tunggal "
              "%zu; kelas tanpa token %zu, tipe kembar %zu, kelas bentrok %zu)",
              total, g_typeKlass.size(), st.byImg, st.uniq, st.noTok, st.dup, st.clash);
    return total;
}

// ---------------------------------------------------------------------------
// TABEL TIPE: Il2CppClass*[] yang diindeks indeks tipe
//
// Kelas yang MethodInfo-nya belum dibikin engine nggak bakal nongol di pool.
// Tapi engine sendiri nyimpen TABEL tipe buat nyari kelas dari type id. Isinya
// array pointer kelas, indeksnya indeks tipe. Tabelnya nggak diekspor/di-export
// jadi dicari dengan VOTE: tiap kelas yang udah dikenal dicari di memori, dan
// kalau pointer-nya nongol di alamat A:
//
//     base[space] = A - indeks * 8
//
// Base yang dipilih mayoritas (dan menang jelas). Nggak ada alamat yang di-
// hardcode, jadi tahan ASLR + game update.
// ---------------------------------------------------------------------------
struct TypeTable { uintptr_t base = 0; int votes = 0; };
std::map<uint32_t, TypeTable> g_typeTable;
std::map<uintptr_t, uint32_t> g_typeTableRegion;   // base -> space (buat log)
size_t g_tableMapped = 0;

void findTypeTables() {
    g_typeTable.clear();
    g_typeTableRegion.clear();
    if (g_typeKlass.size() < 24) return;

    // daftar kelas yang udah dikenal (buat dicari di memori), diurut biar bisa
    // di-binary-search -- nggak ada alokasi per qword.
    std::vector<std::pair<uintptr_t, std::pair<uint32_t, uint32_t>>> known;
    known.reserve(g_typeKlass.size());
    for (const auto& kv : g_typeKlass) {
        const uintptr_t k = mem::untag(kv.second);
        if (!k) continue;
        known.push_back({k, {kv.first >> 24, kv.first & 0x00FFFFFFu}});
    }
    if (known.size() < 24) return;
    std::sort(known.begin(), known.end(),
              [](const std::pair<uintptr_t, std::pair<uint32_t, uint32_t>>& a,
                 const std::pair<uintptr_t, std::pair<uint32_t, uint32_t>>& b) {
                  return a.first < b.first;
              });

    std::map<uint32_t, std::map<uintptr_t, int>> votes;
    const size_t chunk = 1u << 20;
    std::vector<uint64_t> q(chunk / 8 + 8);
    size_t scanned = 0, slotHit = 0;
    const size_t capBytes = g_capMB << 20;
    int wantSpace = 0;
    for (const auto& kv : g_typeKlass) if (kv.first >> 24) { if (!votes.count(kv.first >> 24)) ++wantSpace; }

    for (int pass = 0; pass < 2; ++pass) {
        for (const auto& r : mem::regions()) {
            if (abortx::requested() || scanned > capBytes) break;
            if (!regionWorth(r)) continue;
            if (pass == 0 && !heapLike(r.name)) continue;      // heap dulu
            if (pass == 1 && heapLike(r.name)) continue;
            for (uintptr_t a = r.start; a + 0x40 < r.end; ) {
                if (abortx::requested()) return;
                size_t want = (size_t)std::min<uintptr_t>(chunk, r.end - a);
                want &= ~(size_t)7;
                if (want < 0x40) break;
                if (!mem::read(a, q.data(), want)) break;
                const size_t n = want / 8;
                for (size_t i = 0; i < n; ++i) {
                    const uintptr_t v = mem::untag((uintptr_t)q[i]);
                    if (v < known.front().first || v > known.back().first) continue;
                    size_t lo = 0, hi = known.size();
                    while (lo < hi) {
                        const size_t m = (lo + hi) / 2;
                        if (known[m].first < v) lo = m + 1; else hi = m;
                    }
                    if (lo >= known.size() || known[lo].first != v) continue;
                    const uintptr_t slot = a + i * 8;
                    const uintptr_t base = slot - (uintptr_t)known[lo].second.second * 8;
                    ++votes[known[lo].second.first][base];
                    ++slotHit;
                }
                a += want;
                scanned += want;
                if (scanned > capBytes) break;
            }
            if (scanned > capBytes) break;
        }
        // berhenti kalau tiap space yang punya cukup kelas udah punya base jelas
        int settled = 0;
        for (auto& sp : votes) {
            int top = 0;
            for (auto& b : sp.second) if (b.second > top) top = b.second;
            if (top >= 16) ++settled;
        }
        if (settled >= wantSpace && settled > 0) break;
    }
    logx::add("[pool] cari tabel tipe: %zu MB dipindai, %zu pointer kelas nongol di %zu space",
              scanned >> 20, slotHit, votes.size());

    for (auto& sp : votes) {
        uintptr_t best = 0;
        int top = 0, second = 0;
        for (auto& b : sp.second) {
            if (b.second > top) { second = top; top = b.second; best = b.first; }
            else if (b.second > second) second = b.second;
        }
        if (top < 16 || second * 2 > top) {
            logx::add("[pool]   space %u: base nggak jelas (%d suara, kedua %d) -- dilewat",
                      sp.first, top, second);
            continue;
        }
        g_typeTable[sp.first] = {best, top};
        const mem::Region* br = mem::regionOf(best);
        logx::add("[pool]   space %u: tabel tipe di 0x%lx (%d suara) di region %s",
                  sp.first, (unsigned long)best, top,
                  br && !br->name.empty() ? br->name.c_str() : "?");
    }
}

// Isi peta dari tabel tipe -- tiap entry DIVALIDASI: pointernya harus nunjuk
// region kebaca non-eksekusi DAN token kelas itu harus nyambung (indeks = baris+K).
// Kalau yang lolos cuma sedikit, berarti tebakan tabelnya salah -> dibuang.
struct TblStat { size_t checked = 0, ok = 0, empty = 0, badPtr = 0, badTok = 0, dup = 0, dropped = 0; };
TblStat g_tblStat;

// ---------------------------------------------------------------------------
// Verifikasi peta tipe vs DUMP OFFLINE (sumber independen)
//
// Tabel tipe itu array pointer, jadi "tabelnya bener" harus dibuktiin. Dua bukti:
//   (1) token kelas harus nyambung (indeks = baris + K) -- udah dicek per entry;
//   (2) nama kelas runtime ada di dump, DAN nomor TypeDefIndex-nya sama dengan
//       indeks yang kita pilih. Bukti (2) ini yang mastiin tabelnya nggak GESER
//       (kalau base-nya salah kelipatan 8, token masih bisa kelihatan "konsisten"
//       tapi TypeDefIndex-nya nggak bakal cocok sama dump).
// ---------------------------------------------------------------------------
void verifyTypeMap() {
    if (!dump::loaded() || g_tokMap.empty()) return;
    int okMi = 0, chkMi = 0, okTbl = 0, chkTbl = 0, noDump = 0;
    for (const auto& kv : g_tokMap) {
        const bool fromMi = g_typeKlass.count(kv.first) != 0;
        if (chkMi >= 200 && chkTbl >= 200) break;
        if (fromMi && chkMi >= 200) continue;
        if (!fromMi && chkTbl >= 200) continue;
        const uintptr_t k = mem::untag(kv.second);
        if (!k) continue;
        char nm[160] = {0};
        if (!readIdent(mem::untag(mem::rdp<uintptr_t>(k + kKlassName)), nm, sizeof(nm))) continue;
        std::vector<const dump::ClassEntry*> hits;
        if (!dump::find(nm, nullptr, hits) || hits.empty()) { ++noDump; continue; }
        const uint32_t idx = kv.first & 0x00FFFFFFu;
        bool sameIdx = false;
        for (const auto* h : hits)
            if (h->typeDefIndex == idx) { sameIdx = true; break; }
        if (fromMi) { ++chkMi; okMi += sameIdx ? 1 : 0; }
        else        { ++chkTbl; okTbl += sameIdx ? 1 : 0; }
    }
    logx::add("[pool] cek peta tipe vs dump (TypeDefIndex di dump harus sama): "
              "dari MethodInfo %d/%d, dari TABEL %d/%d (kelas nggak ada di dump %d)",
              okMi, chkMi, okTbl, chkTbl, noDump);
}

size_t buildTypeMapFromTable() {
    g_tableMapped = 0;
    g_tblStat = TblStat{};
    if (g_typeTable.empty() || !g_tokOff) return 0;

    for (const auto& t : g_typeTable) {
        const uint32_t sp = t.first;
        auto rg = g_spaceRange.find(sp);
        if (rg == g_spaceRange.end()) continue;
        // K WAJIB udah keukur dari kelas yang typeId-nya diketahui. Tanpa K,
        // tabel bisa "kelihatan cocok" padahal entries-nya geser (base salah
        // kelipatan 8 tetap ngasih selisih konstan) -- jadi lebih baik dilewat
        // daripada salah peta.
        if (!g_tokK.count(sp)) {
            logx::add("[pool] tabel tipe space %u: K belum keukur -- dilewat", sp);
            continue;
        }
        const int32_t K = g_tokK[sp];
        const uint32_t lo = rg->second.first, hi = rg->second.second;
        const size_t n = (size_t)(hi - lo + 1);
        if (!n || n > (1u << 22)) continue;
        const uintptr_t first = t.second.base + (uintptr_t)lo * 8;
        if (!mem::canRead(first, n * 8)) {
            logx::add("[pool] tabel tipe space %u: 0x%lx nggak kebaca -- dilewat",
                      sp, (unsigned long)first);
            continue;
        }
        std::vector<uintptr_t> buf(n);
        if (!mem::read(first, buf.data(), n * 8)) continue;

        // hitung dulu berapa yang lolos -- keputusan "tabel ini bener" dibuat
        // sebelum nyampur ke peta utama.
        size_t good = 0;
        for (size_t i = 0; i < n; ++i) {
            const uintptr_t k = mem::untag(buf[i]);
            if (!k) continue;
            const mem::Region* kr = mem::regionOf(k);
            if (!kr || !kr->readable() || kr->exec()) continue;
            const uint32_t tok = mem::rdp<uint32_t>(k + g_tokOff);
            if ((tok >> 24) != 0x02) continue;
            if ((int64_t)(tok & 0x00FFFFFFu) + K != (int64_t)(lo + i)) continue;
            ++good;
        }
        g_tblStat.checked += n;
        if (good < 16 || good * 5 < n) {
            logx::add("[pool] tabel tipe space %u: cuma %zu/%zu entry nyambung -- dibuang",
                      sp, good, n);
            g_tblStat.dropped += n;
            continue;
        }

        for (size_t i = 0; i < n; ++i) {
            const uint32_t idx = lo + (uint32_t)i;
            const uintptr_t k = mem::untag(buf[i]);
            if (!k) { ++g_tblStat.empty; continue; }
            const mem::Region* kr = mem::regionOf(k);
            if (!kr || !kr->readable() || kr->exec()) { ++g_tblStat.badPtr; continue; }
            const uint32_t tok = mem::rdp<uint32_t>(k + g_tokOff);
            if ((tok >> 24) != 0x02) { ++g_tblStat.badTok; continue; }
            if ((int64_t)(tok & 0x00FFFFFFu) + K != (int64_t)idx) { ++g_tblStat.badTok; continue; }
            const uint32_t tid = (sp << 24) | idx;
            if (g_tokMap.count(tid)) { ++g_tblStat.dup; continue; }
            g_tokMap[tid] = k;
            ++g_tblStat.ok;
            ++g_tableMapped;
        }
        logx::add("[pool] tabel tipe space %u: %zu entry, %zu nyambung (kosong %zu, pointer rusak %zu, "
                  "token nggak nyambung %zu, udah kepetakan %zu)",
                  sp, n, g_tblStat.ok, g_tblStat.empty, g_tblStat.badPtr, g_tblStat.badTok,
                  g_tblStat.dup);
    }
    return g_tableMapped;
}


// Semua space yang udah kelihatan punya minimal `want` suara di delta teratasnya?
bool allSpacesHaveVotes(const std::map<std::pair<int, int>, std::map<uint64_t, int>>& votes,
                        int want) {
    if (votes.empty()) return false;
    std::map<int, int> top;
    for (const auto& kv : votes)
        for (const auto& d : kv.second)
            if (d.second > top[kv.first.first]) top[kv.first.first] = d.second;
    for (const auto& t : top)
        if (t.second < want) return false;
    return true;
}

// anchor dari tabel PROPERTI kelas (jalan tanpa dump offline):
// PropertyInfo entry (MLBB): {klass, nama, MethodInfo* get, MethodInfo* set}
void propAnchors(std::map<std::pair<int, int>, std::map<uint64_t, int>>& votes, int tryClasses) {
    int used = 0;
    for (const auto& kv : g_byKlass) {
        if (used >= tryClasses) break;
        if (anchorOut()) break;
        if ((used & 15) == 0)
            abortx::beat("kalibrasi nama (properti)", 0.35f + 0.3f * (float)((double)used / 400.0));
        // Berhenti begitu SEMUA space (byte-atas index nama) udah punya cukup
        // suara. Ini yang bikin kalibrasi nggak makan waktu penuh: sebagian
        // besar kelas nggak nambah bukti apa-apa lagi.
        if ((used & 31) == 0 && used && allSpacesHaveVotes(votes, 8)) break;
        if (kv.second.size() < 2) continue;
        const uintptr_t klass = kv.first;
        uintptr_t arr = mem::untag(mem::rdp<uintptr_t>(klass + kKlassProps));
        if (!arr || !mem::canRead(arr, 0x30)) continue;

        // cari stride yang entry-nya {klass, nama}
        for (uint32_t st : {0x28u, 0x20u, 0x30u, 0x18u}) {
            int good = 0;
            for (int i = 0; i < 4; ++i) {
                const uintptr_t e = arr + (uintptr_t)i * st;
                if (!mem::canRead(e, st)) break;
                if (mem::untag(mem::rdp<uintptr_t>(e)) != klass) break;
                char pn[160];
                if (!readIdent(mem::untag(mem::rdp<uintptr_t>(e + 8)), pn, sizeof(pn))) break;
                ++good;
            }
            if (good < 3) continue;

            for (int i = 0; i < 6; ++i) {
                const uintptr_t e = arr + (uintptr_t)i * st;
                char pn[160];
                if (!readIdent(mem::untag(mem::rdp<uintptr_t>(e + 8)), pn, sizeof(pn))) break;
                for (int which = 0; which < 2; ++which) {
                    const uintptr_t mi = mem::untag(mem::rdp<uintptr_t>(e + 0x10 + (uintptr_t)which * 8));
                    if (!mi || !mem::canRead(mi, 0x30)) continue;
                    const uint32_t idx = nameIndexAt(mi);
                    if (!idx) continue;
                    std::string want = (which == 0 ? "get_" : "set_") + std::string(pn);
                    voteAnchor(votes, idx, want.c_str());
                }
            }
            ++used;
            break;
        }
    }
}

// anchor dari dump offline: nama method + RVA-nya udah kita tahu
void dumpAnchors(std::map<std::pair<int, int>, std::map<uint64_t, int>>& votes, int tryClasses) {
    if (!dump::loaded()) return;
    int used = 0;
    for (const auto& kv : g_byKlass) {
        if (used >= tryClasses) break;
        if (anchorOut()) break;
        if (kv.second.size() < 3) continue;
        char kname[160];
        const uintptr_t np = mem::untag(mem::rdp<uintptr_t>(kv.first + kKlassName));
        if (!readIdent(np, kname, sizeof(kname))) continue;

        std::vector<const dump::ClassEntry*> hits;
        dump::ClassDetail det;
        if (!dump::find(kname, nullptr, hits) || hits.empty()) continue;
        if (!dump::detail(*hits[0], det)) continue;
        ++used;

        int found = 0;
        for (size_t si : kv.second) {
            const MethodEnt& e = g_entries[si];
            for (const auto& dm : det.methods) {
                if (!dm.hasRva || dm.rva != e.rva) continue;
                const std::string nm = baseName(dm.name);
                if (nm.empty()) break;
                const uint32_t idx = nameIndexAt(e.mi);
                if (idx) voteAnchor(votes, idx, nm.c_str());
                ++found;
                break;
            }
            if (found >= 8) break;
        }
    }
}

// ---------------------------------------------------------------------------
// Anchor TANPA label: "rangkaian panjang nama"
//
// Method bertetangga di dalam satu kelas punya index nama yang BERTAMBAH, dan
// nama-namanya disimpan berurutan di file metadata: [nama1][NUL][nama2][NUL]...
// Jadi selisih index antar method = panjang nama masing-masing (+1). Yang kita
// punya cuma index-nya -- tapi dari situ bisa dihitung PANJANG nama yang
// diharapkan, terus dicocokin ke file metadata: cari posisi yang punya rentetan
// nama dengan panjang persis seperti itu. Kalau ketemu:
//
//      delta[space] = index_pertama - posisi
//
// Ini yang bikin kalibrasi jalan walau dump offline belum dimuat dan kelasnya
// nggak punya properti (contoh nyata: Assembly-CSharp.dll = space 3).
void runAnchors(std::map<std::pair<int, int>, std::map<uint64_t, int>>& votes) {
    int tried = 0;
    for (const auto& kv : g_byKlass) {
        if (tried >= 40) break;
        if (anchorOut()) break;
        if ((tried & 3) == 0) abortx::beat("kalibrasi nama", 0.1f * (float)((double)tried / 40.0));
        if (kv.second.size() < 6) continue;

        // urutkan entry kelas ini menurut alamat MethodInfo-nya, ambil index nama
        std::vector<std::pair<uint32_t, uint32_t>> seq;   // (idx, mi)
        for (size_t i : kv.second) {
            const uint32_t idx = nameIndexAt(g_entries[i].mi);
            if (idx) seq.push_back({idx, (uint32_t)g_entries[i].mi});
        }
        if (seq.size() < 5) continue;
        std::sort(seq.begin(), seq.end());
        // buang duplikat + index yang nggak nyambung (kelas lain / beda image)
        std::vector<uint32_t> idxs;
        for (const auto& s : seq) {
            if (!idxs.empty() && s.first == idxs.back()) continue;
            idxs.push_back(s.first);
        }
        if (idxs.size() < 5) continue;
        ++tried;

        const int space = (int)(idxs[0] >> 24);
        if (space <= 0) continue;
        const size_t pairs = std::min<size_t>(idxs.size(), 8);
        std::vector<uint32_t> len;
        for (size_t k = 1; k < pairs; ++k) {
            const uint32_t gap = idxs[k] - idxs[k - 1];
            if (gap == 0 || gap > 160) { len.clear(); break; }
            len.push_back(gap - 1);          // = panjang nama ke-(k-1)
        }
        if (len.size() < 4) continue;

        for (size_t mi = 0; mi < g_maps.size(); ++mi) {
            if (anchorOut()) break;
            if (!loadMap((int)mi)) continue;
            abortx::beat("kalibrasi nama (rangkaian)");
            const std::vector<uint8_t>& d = g_maps[mi].data;
            const uint32_t first = idxs[0];
            for (size_t p = 0; p < d.size(); ++p) {
                if (!(p & 0xFFFFF) && anchorOut()) break;      // cek tiap ~1 MB
                if (p + (size_t)len[0] >= d.size()) break;
                if (d[p] == 0 || !identChar(d[p])) continue;
                if (p > 0 && identChar(d[p - 1])) continue;      // harus awal string
                bool okAll = true;
                for (size_t k = 0; k < len.size(); ++k) {
                    const size_t q = p + (size_t)(idxs[k] - first);
                    if (q + len[k] >= d.size() || d[q + len[k]] != 0) { okAll = false; break; }
                    for (uint32_t c = 0; c < len[k]; ++c)
                        if (!identChar(d[q + c])) { okAll = false; break; }
                    if (!okAll) break;
                    if (!looksIdent((const char*)d.data() + q, len[k])) { okAll = false; break; }
                }
                if (!okAll) continue;
                const uint64_t delta = (uint64_t)first - (uint64_t)p;
                votes[{space, (int)mi}][delta] += 2;             // bobot lebih
                break;                                           // 1 posisi per kelas
            }
        }
    }
    if (tried) logx::add("[pool] anchor panjang-nama: %d kelas dicoba", tried);
}

bool calibrate() {
    std::map<std::pair<int, int>, std::map<uint64_t, int>> votes;
    if (abortx::requested()) { g_err = "kalibrasi dibatalkan (Stop)"; return false; }
    g_calibDeadline = abortx::nowSec() + kCalibBudgetSec;
    abortx::beat("kalibrasi nama");
    // Urutannya dari yang paling murah & paling pasti:
    //   1. dump offline (kalau dimuat) -- nama + RVA udah diketahui, instan
    //   2. rangkaian panjang nama -- tanpa label, tapi nyisir file metadata
    //   3. tabel properti -- paling banyak kelas, paling mahal (dibatasi waktu)
    dumpAnchors(votes, 400);
    const size_t afterDump = votes.size();
    runAnchors(votes);
    propAnchors(votes, 400);
    const size_t afterProp = votes.size();
    const bool cut = anchorOut();
    g_calibDeadline = 0.0;
    if (votes.empty()) { g_err = "anchor nama nggak ada (dump nggak dimuat?)"; return false; }
    if (cut)
        logx::add("[pool] kalibrasi nama %s setelah %.0f s -- pakai yang udah kedapet",
                  abortx::requested() ? "DIBATALKAN" : "waktu habis",
                  (double)kCalibBudgetSec);

    // Diagnosa: pasangan (space -> map) yang dapet suara + delta teratasnya.
    // Ini yang dipakai kalau ada space yang nggak kekalibrasi (contoh nyata:
    // space 3 alias Assembly-CSharp.dll).
    logx::add("[pool] anchor: %zu pasangan (dump %zu, properti nambah %zu)",
              votes.size(), afterDump, afterProp - afterDump);
    for (auto& kv : votes) {
        int top = 0;
        uint64_t topD = 0;
        for (auto& d : kv.second)
            if (d.second > top) { top = d.second; topD = d.first; }
        logx::add("[pool]   space %d map %s: %zu delta beda, delta teratas 0x%llx (%d suara)",
                  kv.first.first, kv.first.second < (int)g_maps.size()
                                      ? g_maps[kv.first.second].path.c_str() : "?",
                  kv.second.size(), (unsigned long long)topD, top);
    }

    g_fix.assign(256, SpaceFix{});
    int okSpaces = 0;
    // votes[(space, map)][delta] = jumlah anchor yang cocok.
    // Yang dipilih: pasangan (space -> map, delta) dengan suara terbanyak.
    for (auto& kv : votes) {
        const int space = kv.first.first;
        const int mi    = kv.first.second;
        if (space <= 0 || space >= 256) continue;
        for (auto& dd : kv.second) {              // delta -> jumlah suara
            if (dd.second <= g_fix[space].votes) continue;
            SpaceFix f;
            f.space = space;
            f.mapIdx = mi;
            f.delta = dd.first;
            f.votes = dd.second;
            f.ok = false;
            g_fix[space] = f;
        }
    }
    for (int sp = 0; sp < 256; ++sp) {
        SpaceFix& f = g_fix[sp];
        if (f.votes < 3) { f.ok = false; continue; }
        f.ok = (f.mapIdx >= 0 && f.mapIdx < (int)g_maps.size() && loadMap(f.mapIdx));
        if (f.ok) ++okSpaces;
    }
    if (!okSpaces) { g_err = "delta nama nggak kekalibrasi"; return false; }
    return true;
}

// nama dari index (pakai delta hasil kalibrasi + mapping yang udah dimuat)
bool resolveName(uint32_t idx, std::string& out) {
    out.clear();
    if (!idx) return false;
    const int space = (int)(idx >> 24);
    if (space <= 0 || space >= 256) return false;
    const SpaceFix& f = g_fix[space];
    if (!f.ok) return false;
    const uint64_t off = (uint64_t)idx - f.delta;
    const std::vector<uint8_t>& d = g_maps[f.mapIdx].data;
    if (off >= d.size()) return false;
    size_t e = (size_t)off;
    while (e < d.size() && e - off < 200 && d[e]) ++e;
    const size_t len = e - (size_t)off;
    if (!looksIdent((const char*)d.data() + off, len)) return false;
    out.assign((const char*)d.data() + off, len);
    return true;
}

// ---------------------------------------------------------------------------
}  // namespace

// Pool ini bisa diminta dari dua tempat sekaligus (tombol di menu + dumper
// yang jalan di thread sendiri). Tanpa guard, dua pindai barengan = boros RAM
// dan hasilnya bisa setengah-setengah.
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;

bool init() {
    pthread_mutex_lock(&g_lock);
    struct Unlock {
        ~Unlock() { pthread_mutex_unlock(&g_lock); }
    } unlock;
    if (g_ready) return true;
    g_entries.clear();
    g_byKlass.clear();
    g_maps.clear();
    g_allKlassBuilt = false;
    g_allKlass.clear();
    g_fix.assign(256, SpaceFix{});
    g_stats = Stats{};
    g_mapBytes = 0;
    g_err.clear();

    if (!mem::attached()) { g_err = "belum attach ke proses game"; return false; }

    findMaps();
    if (g_maps.empty()) { g_err = "mapping metadata nggak ketemu"; return false; }

    if (!findPool()) return false;

    // Kelompokkan per kelas DULU: kalibrasi nama butuh daftar method per kelas
    // (anchor tabel properti + anchor dump offline dicari lewat situ).
    if (abortx::requested()) { g_err = "dibatalkan (Stop)"; return false; }
    for (size_t i = 0; i < g_entries.size(); ++i) {
        if ((i & 8191) == 0 && abortx::requested()) { g_err = "dibatalkan (Stop)"; return false; }
        g_byKlass[g_entries[i].klass].push_back(i);
    }

    // Layout "nama langsung" (il2cpp standar) NGGAK butuh kalibrasi anchor:
    // namanya udah kebaca dari struct MethodInfo-nya sendiri waktu validasi.
    const bool directName = (g_mi.nameOff != kNoOff);
    if (directName) {
        logx::add("[pool] nama method dibaca LANGSUNG dari struct MethodInfo "
                  "(nama+0x%x, klass+0x%x) -- nggak butuh anchor/delta",
                  g_mi.nameOff, g_mi.klassOff);
    } else if (!calibrate()) {
        logx::add("[pool] %s -- method tetap kebaca, nama dilewat", g_err.c_str());
    }

    // resolusi nama + kelompokkan per kelas
    {
        std::vector<int> spaceNamed(256, 0), spaceAll(256, 0);
        g_entries.erase(std::remove_if(g_entries.begin(), g_entries.end(),
                                       [](const MethodEnt& e) { return e.klass == 0 || e.code == 0; }),
                        g_entries.end());
        size_t nres = 0;
        for (auto& e : g_entries) {
            if ((++nres & 8191) == 0) {
                if (abortx::requested()) { g_err = "dibatalkan (Stop)"; return false; }
                abortx::beat("resolve nama method");
            }
            if (directName) {
                if (e.name.empty()) nameInStruct(g_mi, e.mi, e.name);   // jaga-jaga
            } else {
                const uint32_t idx = nameIndexAt(e.mi);
                resolveName(idx, e.name);
                const int sp = (int)(idx >> 24);
                if (sp > 0 && sp < 256) { ++spaceAll[sp]; if (!e.name.empty()) ++spaceNamed[sp]; }
            }
            if (!e.name.empty()) ++g_stats.named;
        }
        for (int sp = 0; sp < 256; ++sp) {
            if (!spaceAll[sp]) continue;
            char b[192];
            const SpaceFix& f = g_fix[sp];
            if (f.ok)
                snprintf(b, sizeof(b), "space %d -> %s (delta 0x%llx, %d suara, nama %d/%d)",
                         sp, g_maps[f.mapIdx].path.c_str(),
                         (unsigned long long)f.delta, f.votes, spaceNamed[sp], spaceAll[sp]);
            else
                snprintf(b, sizeof(b), "space %d -> nggak kekalibrasi (nama %d/%d)",
                         sp, spaceNamed[sp], spaceAll[sp]);
            g_stats.spaceInfo.push_back(b);
            if (f.ok) ++g_stats.spaces;
        }
        // ---- gabungkan dengan tabel method-definition (daftar LENGKAP) ----
        // Record metadata ada buat SEMUA method; MethodInfo cuma buat yang udah
        // dibikin game. Jadi nama + token diambil dari record, RVA-nya dari
        // MethodInfo kalau ada (kalau belum: RVA 0 alias belum ke-resolve).
        //
        // Ini cuma buat layout yang nunjuk record metadata (MLBB). Layout "nama
        // langsung" (il2cpp standar) nggak punya record: daftar method per kelas
        // diambil apa adanya dari pool -- nama + RVA-nya udah lengkap.
        size_t recOnly = 0, recTotal = 0, recSkip = 0, typeOk = 0, typeNo = 0;
        if (!directName) {
        buildRecords();

        // ---- lengkapi peta typeId -> kelas ---------------------------------
        // Tanpa ini, kelas yang MethodInfo-nya belum dibikin engine kehilangan
        // daftar method-nya (recordnya ada, tapi nggak ketahuan punya kelas mana).
        // Field indeks tipe di kelas dideteksi dari kelas yang udah keketahui.
        std::map<uint32_t, uintptr_t> byMi;               // indeks (tanpa image) -> kelas
        for (const auto& kv : g_typeKlass) byMi.emplace(typeIdxOf(kv.first), kv.second);
        const size_t miMapped = byMi.size();
        g_typeOff = detectTypeOffset(400);
        if (g_typeOff) {
            buildTypeMap();
            for (const auto& kv : g_typeKlassAll) byMi.emplace(kv.first, kv.second);
        } else {
            logx::add("[pool] field indeks tipe kelas nggak ketemu -- pakai jalur token");
        }

        // jalur token TypeDef (yang nyata jalan di MLBB) + tabel tipe
        g_tokOff = detectTypeTokenOffset(1200);
        size_t tokMapped = 0, tableMapped = 0;
        if (g_tokOff) {
            tokMapped = buildTypeMapFromToken();   // sekaligus isi g_spaceRange
            findTypeTables();
            tableMapped = buildTypeMapFromTable();
            if (tableMapped) verifyTypeMap();
        } else {
            g_tokMap.clear();
            g_tokMap.insert(g_typeKlass.begin(), g_typeKlass.end());
        }
        logx::add("[pool] typeId -> kelas: %zu dari MethodInfo, %zu dari token, %zu dari tabel "
                  "= %zu tipe (record punya %zu tipe)",
                  miMapped, tokMapped, tableMapped, g_tokMap.size(), g_recByType.size());
        g_stats.typeMapped   = g_tokMap.size();
        g_stats.typeFromTok  = tokMapped;
        g_stats.typeFromTable = tableMapped;
        g_stats.typeRecords  = g_recByType.size();

        size_t nrec = 0;
        for (auto& kv : g_recByType) {
            if ((++nrec & 1023) == 0 && abortx::requested()) { g_err = "dibatalkan (Stop)"; return false; }
            // typeId LENGKAP dulu (indeks tipe itu PER FILE metadata, jadi yang
            // cuma pake indeks bisa nyangkut ke kelas file lain); indeks-only
            // dipakai sebagai cadangan biar game tanpa token tetap jalan.
            auto tok = g_tokMap.find(kv.first);
            uintptr_t klass = (tok != g_tokMap.end()) ? tok->second : 0;
            if (!klass) {
                auto it = byMi.find(typeIdxOf(kv.first));
                if (it != byMi.end()) klass = it->second;
            }
            if (!klass) { recSkip += kv.second.size(); ++typeNo; continue; }
            ++typeOk;
            std::vector<MethodEnt> list;
            list.reserve(kv.second.size());
            for (size_t ri : kv.second) {
                const RecordEnt& r = g_records[ri];
                MethodEnt m;
                m.klass = klass;
                m.token = r.token;
                if (r.miEnt != (size_t)-1) {
                    const MethodEnt& src = g_entries[r.miEnt];
                    m.mi = src.mi;
                    m.code = src.code;
                    m.rva = src.rva;
                    m.module = src.module;
                } else {
                    ++recOnly;
                }
                resolveName(r.idx, m.name);
                list.push_back(std::move(m));
            }
            std::sort(list.begin(), list.end(),
                      [](const MethodEnt& a, const MethodEnt& b) { return a.token < b.token; });
            recTotal += list.size();
            g_methodsByKlass[klass] = std::move(list);
        }
        }   // if (!directName)
        // kelas yang tipe-pemiliknya belum kepetakan: pakai entry pool langsung
        for (auto& kv : g_byKlass) {
            if (g_methodsByKlass.count(kv.first)) continue;
            std::vector<MethodEnt> list;
            for (size_t i : kv.second) list.push_back(g_entries[i]);
            g_methodsByKlass[kv.first] = std::move(list);
        }
        if (directName)
            logx::add("[pool] gabungan: %zu kelas, %zu method (nama+RVA langsung dari "
                      "MethodInfo, tanpa record metadata)",
                      g_methodsByKlass.size(), g_entries.size());
        else {
            logx::add("[pool] gabungan: %zu kelas, %zu method (dari record %zu, belum ada RVA %zu, "
                      "record nggak kepetakan %zu)",
                      g_methodsByKlass.size(), recTotal, g_records.size(), recOnly, recSkip);
            logx::add("[pool] tipe di tabel record: %zu kepetakan ke kelas + %zu nggak = %zu tipe",
                      typeOk, typeNo, typeOk + typeNo);
        }

        g_stats.classes = g_methodsByKlass.size();
        g_stats.entries = recTotal ? recTotal : g_entries.size();

        // sebaran per module (buat lihat pool-nya lengkap atau nggak)
        std::map<std::string, size_t> perMod;
        for (const auto& e : g_entries) ++perMod[e.module.empty() ? "?" : e.module];
        for (const auto& m : perMod)
            logx::add("[pool]   %s: %zu method", m.first.c_str(), m.second);
    }

    g_stats.layout = g_mi.tag;
    g_ready = !g_entries.empty();
    if (g_stats.typeMapped)
        logx::add("[pool] typeId: %zu tipe kepetakan dari %zu tipe (token +0x%x, tabel tipe %zu tipe)",
                  g_stats.typeMapped, g_stats.typeRecords, g_tokOff, g_tableMapped);
    logx::add("[pool] %zu method, %zu kelas, %zu punya nama | %zu MB dipindai, %zu kandidat, fase 0x%zx"
              " | layout %s",
              g_stats.entries, g_stats.classes, g_stats.named, g_stats.scannedMB,
              g_stats.cand, g_stats.phase, g_mi.tag);
    for (const auto& s : g_stats.spaceInfo) logx::add("[pool]   %s", s.c_str());

    // Contoh nyata: bukti paling cepat buat ngecek layout-nya bener -- kelas,
    // nama method, RVA. Kalau nama kelas + nama method + RVA-nya masuk akal
    // (cocok sama dump offline), berarti pool-nya kebaca dengan benar.
    {
        int shown = 0;
        const uintptr_t mb = mem::base(il2::libName().c_str());
        for (const auto& e : g_entries) {
            if (e.name.empty() || !e.rva) continue;
            char kn[160] = "?";
            readIdent(mem::untag(mem::rdp<uintptr_t>(e.klass + kKlassName)), kn, sizeof(kn));
            (void)mb;
            logx::add("[pool] contoh: %s::%s  rva=0x%llx (%s)  kode=0x%lx",
                      kn, e.name.c_str(), (unsigned long long)e.rva,
                      e.module.empty() ? "?" : e.module.c_str(), (unsigned long)e.code);
            if (++shown >= 5) break;
        }
    }
    // Verifikasi akhir (kalau dump offline dimuat): cocokkan sampel entry pool
    // sama dump -- kelas + RVA + nama method. Ini bukti independen bahwa pool
    // beneran kebaca (bukan cuma "ada nama").
    // Verifikasi ini BUKTI INDEPENDEN, jadi hasilnya dibedah apa adanya:
    //   - "RVA+nama cocok"      = dump dan runtime nunjuk method yang sama
    //   - "nama cocok doang"    = dump punya method dengan nama itu, tapi nggak
    //                             nulis RVA-nya (dump hasil tool lain sering
    //                             nulis "// RVA: unknown (null_entry)")
    //   - "dump nggak punya RVA"= seluruh tubuh kelas itu kosong RVA-nya, jadi
    //                             buat ngecek RVA memang nggak ada bahannya
    //   - "beda beneran"        = dump punya RVA, tapi RVA / namanya nggak ada
    //                             yang nyambung -- INI yang bahaya
    if (dump::loaded() && g_ready) {
        int checked = 0, ok = 0, noClass = 0, noMethod = 0, nameOnly = 0, noSrc = 0, diff = 0;
        for (const auto& e : g_entries) {
            if (e.name.empty() || !e.rva) continue;
            char kn[160] = {0};
            if (!readIdent(mem::untag(mem::rdp<uintptr_t>(e.klass + kKlassName)), kn, sizeof(kn))) continue;
            if (++checked > 2000) break;
            std::vector<const dump::ClassEntry*> hits;
            if (!dump::find(kn, nullptr, hits) || hits.empty()) {
                ++noClass;
                continue;
            }
            // Nama kelas doang nggak cukup buat milih entry: di dump bisa ada
            // beberapa kelas ber-nama sama (namespace beda / module beda). Jadi
            // SEMUA kandidat dicek, hasil terbaik yang dipakai -- kalau ada satu
            // yang nyambung, itu bukti sahnya.
            const std::string me = tailName(e.name);
            bool found = false, rvaOnly = false, nameHit = false, anyRva = false;
            for (const auto* h : hits) {
                dump::ClassDetail det;
                if (!dump::detail(*h, det)) continue;
                for (const auto& dm : det.methods) {
                    if (dm.hasRva) anyRva = true;
                    const bool same = tailName(baseName(dm.name)) == me;
                    if (same) {
                        nameHit = true;
                        if (dm.hasRva && dm.rva == e.rva) { found = true; break; }
                    } else if (dm.hasRva && dm.rva == e.rva) {
                        rvaOnly = true;      // RVA-nya sama, tulisan namanya beda
                    }
                }
                if (found) break;
            }
            if (found) ++ok;
            else if (rvaOnly) ++noMethod;
            else if (nameHit) ++nameOnly;              // nama ada, RVA-nya nggak dicatat
            else if (anyRva) {                         // beda beneran -- contohnya dicetak
                ++diff;
                static std::string lastMis;          // satu contoh per kelas, biar nggak spam
                if (diff <= 3 && lastMis != kn) {
                    lastMis = kn;
                    char ex[200] = "-";
                    dump::ClassDetail d0;
                    if (dump::detail(*hits[0], d0)) {
                        for (const auto& dm : d0.methods) {
                            if (!dm.hasRva) continue;
                            snprintf(ex, sizeof(ex), "%s rva=0x%llx %s",
                                     baseName(dm.name).c_str(),
                                     (unsigned long long)dm.rva,
                                     dm.module.empty() ? "" : dm.module.c_str());
                            break;
                        }
                    }
                    logx::add("[pool] verifikasi MISMATCH: %s::%s pool rva=0x%llx (%s) -- "
                              "dump `%s` di %s, contoh %s",
                              kn, e.name.c_str(), (unsigned long long)e.rva,
                              e.module.empty() ? "?" : e.module.c_str(),
                              hits[0]->name.c_str(),
                              hits[0]->module.empty() ? "?" : hits[0]->module.c_str(), ex);
                }
            }
            else ++noSrc;                              // dump-nya nggak punya RVA
        }
        logx::add("[pool] verifikasi vs dump (independen): %d/%d RVA+nama cocok, %d nama cocok "
                  "doang (dump nggak nulis RVA-nya); kelas nggak ada di dump %d, "
                  "dump nggak punya RVA %d, beda beneran %d",
                  ok, checked, nameOnly, noClass, noSrc, diff);
    }
    if (!g_ready) g_err = "pool kosong";
    return g_ready;
}

bool ready() { return g_ready; }
const Stats& stats() { return g_stats; }
const char* lastError() { return g_err.c_str(); }
const std::vector<MethodEnt>& all() { return g_entries; }

// Daftar SEMUA Il2CppClass* yang pool kenal: g_byKlass (punya MethodInfo
// runtime) + g_methodsByKlass (kepetakan lewat typeId/token/tabel tipe).
// Di-cache: dumper manggil ini sekali per run, tapi daftarnya bisa puluhan ribu.
const std::vector<uintptr_t>& allClasses() {
    if (g_allKlassBuilt) return g_allKlass;
    g_allKlass.clear();
    g_allKlass.reserve(g_byKlass.size() + g_methodsByKlass.size());
    std::set<uintptr_t> seen;
    auto add = [&](uintptr_t k) {
        k = mem::untag(k);
        if (k && seen.insert(k).second) g_allKlass.push_back(k);
    };
    for (const auto& kv : g_byKlass)        add(kv.first);
    for (const auto& kv : g_methodsByKlass) add(kv.first);
    g_allKlassBuilt = true;
    return g_allKlass;
}

void setScanCapMB(size_t mb) { g_capMB = mb ? mb : 2048; }

void setForcedLayout(int idx) { g_forceLayout = idx; }

size_t layoutCount() { return kLayoutCount; }

const char* layoutTag(size_t idx) { return idx < kLayoutCount ? kLayouts[idx].tag : "?"; }

bool klassName(uintptr_t klass, std::string& out) {
    out.clear();
    const uintptr_t k = mem::untag(klass);
    if (!k) return false;
    char nm[160] = {0};
    if (!readIdent(mem::untag(mem::rdp<uintptr_t>(k + kKlassName)), nm, sizeof(nm))) return false;
    out = nm;
    return !out.empty();
}

bool forKlass(uintptr_t klass, std::vector<explore::Method>& out, size_t max) {
    out.clear();
    auto it = g_methodsByKlass.find(klass);
    if (it == g_methodsByKlass.end()) {
        // kelas yang tipe-pemiliknya nggak kekalibrasi: pakai entry pool apa adanya
        auto e = g_byKlass.find(klass);
        if (e == g_byKlass.end()) return false;
        for (size_t i : e->second) {
            if (out.size() >= max) break;
            const MethodEnt& m = g_entries[i];
            if (m.name.empty()) continue;
            explore::Method o;
            o.name = m.name;
            o.ptr = m.code;
            o.rva = m.rva;
            o.module = m.module;
            out.push_back(std::move(o));
        }
        return !out.empty();
    }
    for (const auto& m : it->second) {
        if (out.size() >= max) break;
        if (m.name.empty()) continue;      // nama belum kekalibrasi -> jangan ditulis
        explore::Method o;
        o.name = m.name;
        o.ptr = m.code;
        o.rva = m.rva;
        o.module = m.module;
        out.push_back(std::move(o));
    }
    return !out.empty();
}

bool nameOfIndex(uint32_t idx, std::string& out) { return resolveName(idx, out); }

// --- typeId -> kelas (akses publik) -----------------------------------------
// Definisinya di luar anonymous namespace, tapi pakai data yang diisi di
// detectTypeOffset()/buildTypeMap() (satu TU, jadi tetap kelihatan).
size_t   typeMapSize() { return g_stats.typeMapped ? g_stats.typeMapped : g_typeKlass.size(); }
uint32_t typeOffset()  { return g_typeOff; }
uint32_t typeTokenOffset() { return g_tokOff; }
size_t   typeTableSize()   { return g_tableMapped; }

// ---------------------------------------------------------------------------
// Probe: apakah RECORD metadata nyimpen POINTER ke kelasnya?
//
// Kalau iya, peta typeId->kelas nggak perlu ditebak dari field indeks: tiap
// record langsung nunjuk kelasnya. Diuji pakai record yang kelasnya UDAH
// diketahui (record-nya kepakai MethodInfo yang kebaca pool): field mana di
// record yang isinya sama dengan kelas MethodInfo itu -- yang mayoritas = itu
// pointer kelasnya.
// ---------------------------------------------------------------------------
void probeRecordClassPtr(int samples) {
    std::map<uint32_t, int> hit;
    int used = 0, ptrLike = 0;
    std::map<uint32_t, int> anyPtr;
    for (const auto& r : g_records) {
        if (r.miEnt == (size_t)-1) continue;
        const uintptr_t klass = mem::untag(g_entries[r.miEnt].klass);
        if (!klass) continue;
        ++used;
        for (uint32_t off = 0; off + 8 <= 0x60; off += 8) {
            const uintptr_t v = mem::rdp<uintptr_t>(r.ptr + off);
            if (v > 0x1000 && (v & 0xff) == 0) ++anyPtr[off];
            if (mem::untag(v) == klass) ++hit[off];
        }
        if (used >= samples) break;
    }
    logx::add("[rprobe] %d record (yang kelasnya udah diketahui) diperiksa", used);
    for (const auto& h : anyPtr)
        logx::add("[rprobe]   field +0x%02x: pointer %d, == kelas %d", h.first, h.second,
                  hit.count(h.first) ? hit[h.first] : 0);
    ptrLike = (int)hit.size();
    (void)ptrLike;

    // Bentuk mentah di sekitar offset yang paling sering "cocok" (+0xf0..+0x110)
    // -- biar kelihatan itu field 4 byte, potongan pointer, atau pasangan field.
    int shown = 0;
    for (const auto& kv : g_typeKlass) {
        if (shown >= 4) break;
        const uintptr_t k = mem::untag(kv.second);
        if (!k) continue;
        char nm[96] = "?";
        readIdent(mem::untag(mem::rdp<uintptr_t>(k + kKlassName)), nm, sizeof(nm));
        logx::add("[rprobe] %s typeId=0x%08x kelas=0x%lx", nm, kv.first, (unsigned long)k);
        for (uint32_t off = 0xe0; off < 0x120; off += 8) {
            const uint64_t q = mem::rdp<uint64_t>(k + off);
            logx::add("[rprobe]   +0x%03x: %016llx   (dword: %08x %08x)", off,
                      (unsigned long long)q, (uint32_t)(q & 0xffffffffu),
                      (uint32_t)(q >> 32));
        }
        ++shown;
    }
}

void dumpTypeProbe(int samples, int topN) {
    probeRecordClassPtr(samples);
    if (g_typeKlass.empty()) { logx::add("[tprobe] peta typeId->kelas masih kosong"); return; }

    // Sebaran nilai typeId-nya -- buat lihat encodingnya (indeks kecil? ada byte
    // image di atas seperti index nama?)
    {
        uint32_t lo = 0xFFFFFFFF, hi = 0;
        std::map<uint32_t, int> highByte;
        for (const auto& kv : g_typeKlass) {
            if (kv.first < lo) lo = kv.first;
            if (kv.first > hi) hi = kv.first;
            ++highByte[kv.first >> 24];
        }
        logx::add("[tprobe] %zu typeId: min 0x%x, max 0x%x", g_typeKlass.size(), lo, hi);
        for (const auto& hb : highByte)
            logx::add("[tprobe]   byte-atas 0x%02x : %d tipe", hb.first, hb.second);
    }

    // ---- pencarian "SELISIH konsisten" (lihat findTypeField) + "sama persis" --
    TypeFieldCand best;
    std::vector<TypeFieldCand> all;
    std::vector<std::vector<uint8_t>> bufs;
    std::vector<uint32_t> ids;
    int used = 0;
    const bool ok = findTypeField(samples, best, all, &used, &bufs, &ids);
    logx::add("[tprobe] %d sampel kelas dipindai 0x800 byte", used);

    if (all.empty()) { logx::add("[tprobe] nggak ada kandidat field"); return; }

    // Sebaran selisih buat kandidat teratas: kalau field-nya bener, harusnya
    // NGUMPUL di satu-dua nilai selisih; kalau nyebar rata = kebetulan.
    const uint32_t idxMax = [&] { uint32_t m = 0;
        for (const auto& kv : g_typeKlass) if ((kv.first & 0x00FFFFFFu) > m) m = kv.first & 0x00FFFFFFu;
        return m; }();
    logx::add("[tprobe] indeks tipe terbesar yang diketahui: 0x%x", idxMax);
    for (int c = 0; c < 3 && c < (int)all.size(); ++c) {
        const TypeFieldCand& cd = all[c];
        if ((int)bufs.size() <= c) break;
        std::map<uint32_t, int> dh;
        std::map<uint32_t, int> perSpace;
        int inRange = 0;
        const uint32_t mask = cd.mask;
        for (size_t i = 0; i < bufs.size(); ++i) {
            uint32_t v = 0;
            memcpy(&v, bufs[i].data() + cd.off, (size_t)cd.width);
            ++dh[(v - (ids[i] & mask)) & mask];
            const uint32_t idx = (v - cd.diff) & mask;
            if (idx && idx <= idxMax) ++inRange;
            if (bufs[i].size() > 8) ++perSpace[bufs[i][0x08]];
        }
        std::vector<std::pair<int, uint32_t>> top;
        for (const auto& d : dh) top.push_back({d.second, d.first});
        std::sort(top.begin(), top.end(),
                  [](const std::pair<int, uint32_t>& a, const std::pair<int, uint32_t>& b) {
                      return a.first > b.first; });
        std::string s;
        for (int i = 0; i < 5 && i < (int)top.size(); ++i) {
            char b[40];
            snprintf(b, sizeof(b), "%s0x%x:%d", i ? "  " : "", top[i].second, top[i].first);
            s += b;
        }
        logx::add("[tprobe] +0x%03x/%dbyte mask 0x%08x selisih: %s | indeks masuk akal: %d/%zu",
                  cd.off, cd.width, cd.mask, s.c_str(), inRange, bufs.size());
    }

    // ---- Apakah indeks di record == TypeDefIndex di dump offline? ----
    // Kalau iya, kelas mana pun bisa dipetakan ke typeId-nya lewat NAMA kelas +
    // nomor modul di dump -- tanpa perlu nebak field di Il2CppClass.
    if (dump::loaded()) {
        int checked = 0, sameIdx = 0, sameAll = 0;
        std::map<std::string, int> perMod;        // modul dump -> berapa kali cocok
        std::map<uint32_t, std::map<uint32_t, int>> idxDelta;   // image -> idx-dump -> n
        for (const auto& kv : g_typeKlass) {
            if (checked >= 300) break;
            const uintptr_t k = mem::untag(kv.second);
            if (!k) continue;
            char nm[128] = {0};
            readIdent(mem::untag(mem::rdp<uintptr_t>(k + kKlassName)), nm, sizeof(nm));
            if (!nm[0]) continue;
            std::vector<const dump::ClassEntry*> hits;
            if (!dump::find(nm, nullptr, hits) || hits.empty()) continue;
            const uint32_t recIdx = kv.first & 0x00FFFFFFu;
            const uint32_t dumpIdx = hits[0]->typeDefIndex;
            ++checked;
            idxDelta[kv.first >> 24][recIdx - dumpIdx]++;
            if (dumpIdx && recIdx == dumpIdx) ++sameIdx;
            if (hits[0]->module.find("libcsharp") != std::string::npos && (kv.first >> 24) == 3)
                ++sameAll;
            if (dumpIdx && recIdx == dumpIdx) ++perMod[hits[0]->module];
        }
        logx::add("[tprobe] cek typeId vs TypeDefIndex dump: %d kelas dibandingkan, %d sama persis",
                  checked, sameIdx);
        for (const auto& img : idxDelta) {
            std::vector<std::pair<int, uint32_t>> top;
            for (const auto& d : img.second) top.push_back({d.second, d.first});
            std::sort(top.begin(), top.end(),
                      [](const std::pair<int, uint32_t>& a, const std::pair<int, uint32_t>& b) {
                          return a.first > b.first; });
            logx::add("[tprobe]   image %u: selisih (idx record - TypeDefIndex) teratas:",
                      img.first);
            for (int i = 0; i < 4 && i < (int)top.size(); ++i)
                logx::add("[tprobe]     selisih %d : %d kelas", (int32_t)top[i].second, top[i].first);
        }
        for (const auto& m : perMod)
            logx::add("[tprobe]   cocok persis di modul %s: %d", m.first.c_str(), m.second);
    } else {
        logx::add("[tprobe] dump offline nggak dimuat -- cek TypeDefIndex dilewat");
    }

    // ---- SEMUA pasangan yang diketahui: hubungan handle-kelas <-> typeId ----
    // Kalau delta-nya konstan PER FILE METADATA (byte atas typeId), peta
    // record->kelas bisa dibangun dari handle kelas: typeId = handle - delta.
    {
        const TypeFieldCand cd = all[0];
        std::map<uint32_t, std::map<uint32_t, int>> perImg;   // image -> delta -> n
        std::map<uint32_t, std::pair<uint32_t, uint32_t>> range;
        int read = 0;
        for (const auto& kv : g_typeKlass) {
            const uintptr_t k = mem::untag(kv.second);
            if (!k) continue;
            const uint32_t v = (cd.width == 2) ? (uint32_t)mem::rdp<uint16_t>(k + cd.off)
                                               : mem::rdp<uint32_t>(k + cd.off);
            const uint32_t img = kv.first >> 24;
            ++perImg[img][v - kv.first];
            auto it = range.find(img);
            const uint32_t idx = kv.first & 0x00FFFFFFu;
            if (it == range.end()) range[img] = {idx, idx};
            else {
                if (idx < it->second.first) it->second.first = idx;
                if (idx > it->second.second) it->second.second = idx;
            }
            ++read;
        }
        for (const auto& img : perImg) {
            std::vector<std::pair<int, uint32_t>> top;
            for (const auto& d : img.second) top.push_back({d.second, d.first});
            std::sort(top.begin(), top.end(),
                      [](const std::pair<int, uint32_t>& a, const std::pair<int, uint32_t>& b) {
                          return a.first > b.first; });
            const auto& r = range[img.first];
            logx::add("[tprobe] image %u: %d tipe, indeks 0x%x..0x%x, delta teratas 0x%x (%d), "
                      "kedua 0x%x (%d)",
                      img.first, read ? (int)img.second.size() : 0, r.first, r.second,
                      top[0].second, top[0].first,
                      top.size() > 1 ? top[1].second : 0,
                      top.size() > 1 ? top[1].first : 0);
        }
    }

    // Pecahan per-file-metadata (byte atas typeId) buat kandidat teratas + contoh
    // nyata (nama kelas, typeId, nilai di offset itu). Ini yang nunjukin kandidat
    // bener atau cuma kebetulan.
    {
        const TypeFieldCand cd = all[0];
        const uint32_t mask = cd.mask;
        std::map<uint32_t, std::map<uint32_t, int>> perSpace;
        for (size_t i = 0; i < bufs.size() && i < ids.size(); ++i) {
            uint32_t v = 0;
            memcpy(&v, bufs[i].data() + cd.off, (size_t)cd.width);
            perSpace[ids[i] >> 24][(v - (ids[i] & mask)) & mask]++;
        }
        for (const auto& sp : perSpace) {
            std::vector<std::pair<int, uint32_t>> top;
            for (const auto& d : sp.second) top.push_back({d.second, d.first});
            std::sort(top.begin(), top.end(),
                      [](const std::pair<int, uint32_t>& a, const std::pair<int, uint32_t>& b) {
                          return a.first > b.first; });
            char b[160] = {0};
            for (int i = 0; i < 4 && i < (int)top.size(); ++i)
                snprintf(b + strlen(b), sizeof(b) - strlen(b), "  selisih 0x%x: %d",
                         top[i].second, top[i].first);
            logx::add("[tprobe] space %u (image %u):%s", sp.first, sp.first, b);
        }
        int shown = 0;
        for (const auto& kv : g_typeKlass) {
            if (shown >= 8) break;
            const uintptr_t k = mem::untag(kv.second);
            if (!k || !mem::canRead(k, 0x800)) continue;
            char nm[128] = "?";
            readIdent(mem::untag(mem::rdp<uintptr_t>(k + kKlassName)), nm, sizeof(nm));
            const uint32_t v = (cd.width == 2) ? (uint32_t)mem::rdp<uint16_t>(k + cd.off)
                                               : mem::rdp<uint32_t>(k + cd.off);
            logx::add("[tprobe]   %-34s typeId=0x%08x  nilai=0x%x  selisih=0x%x",
                      nm, kv.first, v, (v - (kv.first & mask)) & mask);
            ++shown;
        }
    }
    logx::add("[tprobe] -- kandidat field indeks (offset, lebar, geseran, cocok) --");
    for (int i = 0; i < topN && i < (int)all.size(); ++i)
        logx::add("[tprobe]   +0x%03x  %d byte  geseran 0x%-6x  %d/%d",
                  all[i].off, all[i].width, all[i].diff, all[i].hits, used);
    if (ok)
        logx::add("[tprobe] DIPAKAI: +0x%x %d byte geseran 0x%x (%d cocok, kedua terbaik %d)",
                  best.off, best.width, best.diff, best.hits, all[1].hits);
    else
        logx::add("[tprobe] nggak ada kandidat yang layak (mayoritas + menang jelas)");

    // berapa kelas yang bakal saling bentrok kalau field ini dipakai
    {
        uint32_t off = ok ? best.off : all[0].off;
        int width = ok ? best.width : all[0].width;
        uint32_t diff = ok ? best.diff : all[0].diff;
        const uint32_t mask = (width == 2) ? 0x0000FFFFu : 0x00FFFFFFu;
        std::map<uint32_t, int> seen;
        int n = 0, collide = 0;
        size_t dupKey = 0;
        for (const auto& e : g_entries) {
            const uintptr_t k = mem::untag(e.klass);
            if (!k) continue;
            const uint32_t v = (width == 2) ? (uint32_t)mem::rdp<uint16_t>(k + off)
                                            : mem::rdp<uint32_t>(k + off);
            const uint32_t idx = (v - diff) & mask;
            if (!idx) continue;
            ++n;
            auto it = seen.find(idx);
            if (it == seen.end()) seen.emplace(idx, 1);
            else if (++it->second == 2) ++collide;
        }
        for (const auto& kv : g_recByType) ++dupKey;   // (cuma buat catatan)
        (void)dupKey;
        logx::add("[tprobe] kalau dipakai: %d kelas -> %zu indeks unik, %d indeks dipakai >1 kelas",
                  n, seen.size(), collide);
    }
}

// Info yang cuma ada di runtime (nama dari metadata) + yang cuma ada di dump
// offline (RVA buat method yang MethodInfo-nya belum dibikin engine) digabung
// per kelas. Dipanggil dumper biar hasilnya nggak ada "unknown".
size_t enrichFromDump(uintptr_t klass, const std::string& kname) {
    if (!dump::loaded() || kname.empty()) return 0;
    auto it = g_methodsByKlass.find(klass);
    if (it == g_methodsByKlass.end()) return 0;

    bool need = false;
    for (const auto& m : it->second)
        if (!m.rva || m.name.empty()) { need = true; break; }
    if (!need) return 0;

    std::vector<const dump::ClassEntry*> hits;
    dump::ClassDetail det;
    if (!dump::find(kname.c_str(), nullptr, hits) || hits.empty()) return 0;
    if (!dump::detail(*hits[0], det)) return 0;

    size_t n = 0;
    for (auto& m : it->second) {
        if (m.rva && !m.name.empty()) continue;
        for (const auto& dm : det.methods) {
            if (!dm.hasRva) continue;
            const std::string base = baseName(dm.name);
            if (!m.name.empty()) {
                if (base != m.name) continue;
            } else if (m.rva && dm.rva != m.rva) {
                continue;
            }
            if (m.name.empty()) m.name = base;
            m.rva = dm.rva;
            if (m.module.empty()) m.module = dm.module.empty() ? hits[0]->module : dm.module;
            ++n;
            break;
        }
    }
    return n;
}

const MethodEnt* byRva(uint64_t rva, const std::string& module) {
    for (const auto& e : g_entries)
        if (e.rva == rva && (module.empty() || e.module == module)) return &e;
    return nullptr;
}

}  // namespace metapool

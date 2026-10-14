#include "core/il2cpp.h"
#include "core/log.h"
#include "core/offlinedump.h"
#include "core/target.h"

#include <cstring>
#include <cstdio>
#include <string>

namespace il2 {

static uintptr_t   g_base = 0;
static std::string g_libName;

const std::string& libName() { return g_libName; }

bool ready() { return mem::attached() && g_base != 0; }
uintptr_t base() { return g_base; }
uintptr_t addr(uint32_t rva) { return g_base ? g_base + (uintptr_t)rva : 0; }

bool init() {
    if (!mem::attached()) {
        logx::add("[il2] mem belum attach");
        return false;
    }
    // Lib-nya DIPILIH SAAT RUNTIME (target::detectLib): yang diketik user,
    // atau kandidat umum (libil2cpp.so, liblogic.so, ...), atau lib yang
    // di sebelahnya ada global-metadata.dat. Jadi satu biner ini bisa
    // dipakai di game Unity/il2cpp mana pun.
    const std::string libName = target::detectLib();
    g_base = libName.empty() ? 0 : mem::base(libName.c_str());
    g_libName = libName;
    if (!g_base) {
        // Kalau proses ini nggak punya bukti lib engine sama sekali, kemungkinan
        // besar engine-nya jalan di proses lain (Unity sering begini: MLBB di
        // "com.mobile.legends:UnityKillsMe"). Kasih tahu di log biar nggak
        // nyasar nebak nama lib.
        if (mem::il2cppScore(mem::pid()) <= 0) {
            logx::add("[il2] proses pid=%d kelihatannya BUKAN pemilik lib engine",
                      mem::pid());
            logx::add("[il2] pilih pid lain di tab TARGET (yang ada tanda *), atau "
                      "pakai: panxcz_libtool.sh %s --pid <pid>",
                      target::package().c_str());
        }
        logx::add("[il2] lib il2cpp TIDAK ketemu - dump .so di sekitar sini:");
        int shown = 0;
        for (const auto& r : mem::regions()) {
            if (r.name.find(".so") == std::string::npos) continue;
            if (r.fileOff != 0) continue;
            logx::add("      %s", r.name.c_str());
            if (++shown >= 12) break;
        }
        return false;
    }
    logx::add("[il2] %s base=0x%lx", libName.c_str(), (unsigned long)g_base);
    return true;
}

// ----------------------------------------------------------------------------
// util
// ----------------------------------------------------------------------------
bool readStr(uintptr_t p, char* out, size_t n) {
    if (!p || !out || n < 2) return false;
    out[0] = 0;
    if (!mem::canRead(p, 1)) return false;
    if (!mem::read(p, out, n - 1)) {
        // baca pelan-pelan sampai kena batas region
        size_t got = 0;
        while (got < n - 1) {
            if (!mem::canRead(p + got, 1)) break;
            char c = 0;
            if (!mem::read(p + got, &c, 1)) break;
            out[got++] = c;
            if (!c) break;
        }
        out[got] = 0;
        return got > 0;
    }
    out[n - 1] = 0;
    return true;
}

static bool printableStr(const char* s, size_t maxLen) {
    size_t n = strnlen(s, maxLen);
    if (n == 0) return true;             // namespace kosong valid
    if (n >= maxLen) return false;
    for (size_t i = 0; i < n; ++i) {
        unsigned char c = (unsigned char)s[i];
        if (c < 0x20 || c > 0x7E) return false;
    }
    return true;
}

// Validasi Il2CppClass tanpa bergantung offset byval_arg (yang beda antar
// versi il2cpp). Tanda yang dipakai:
//   +0x10 name      -> harus string printable
//   +0x18 namespaze -> string printable, boleh 0 (namespace global)
//   +0x00 image     -> Il2CppImage, dan Il2CppImage::name (di +0x00) adalah
//                      string berakhiran ".dll"
bool isValidKlass(uintptr_t k) {
    if (!k || !mem::canRead(k, 0x40)) return false;

    uintptr_t nameP = 0, nsP = 0, imgP = 0;
    mem::rd(k + kKlass_Name, nameP);
    mem::rd(k + kKlass_Namespace, nsP);
    mem::rd(k + kKlass_Image, imgP);

    if (!nameP || !mem::canRead(nameP, 1)) return false;

    char nm[200], ns[200];
    if (!readStr(nameP, nm, sizeof(nm))) return false;
    if (!printableStr(nm, sizeof(nm))) return false;
    if (nm[0] == 0) return false;

    // namespaze: nullptr = namespace global (umum di il2cpp baru)
    if (nsP) {
        if (nsP == nameP) return false;
        if (!mem::canRead(nsP, 1)) return false;
        if (!readStr(nsP, ns, sizeof(ns))) return false;
        if (!printableStr(ns, sizeof(ns))) return false;
    }

    // image harus Il2CppImage dengan nama *.dll
    if (!imgP || !mem::canRead(imgP, 0x10)) return false;
    uintptr_t imgNameP = 0;
    mem::rd(imgP + 0x00, imgNameP);
    if (!imgNameP || !mem::canRead(imgNameP, 1)) return false;
    char imgName[160];
    if (!readStr(imgNameP, imgName, sizeof(imgName))) return false;
    if (!printableStr(imgName, sizeof(imgName))) return false;
    if (!strstr(imgName, ".dll")) return false;

    return true;
}

const char* klassName(uintptr_t k) {
    static char buf[160];
    buf[0] = 0;
    if (!k) return buf;
    uintptr_t p = 0;
    if (!mem::rd(k + kKlass_Name, p)) return buf;
    readStr(p, buf, sizeof(buf));
    return buf;
}

// Semua fungsi di atas memakai buffer statis yang sama, jadi JANGAN dua panggilan
// dalam satu ekspresi (mis. strcmp(klassName(a), klassName(b))). Pakai salinan.
const char* objClassName(uintptr_t obj) {
    return klassName(obj ? getPtr(obj, 0) : 0);
}

const char* klassNamespace(uintptr_t k) {
    static char buf[160];
    buf[0] = 0;
    if (!k) return buf;
    uintptr_t p = 0;
    if (!mem::rd(k + kKlass_Namespace, p)) return buf;
    readStr(p, buf, sizeof(buf));
    return buf;
}

// ----------------------------------------------------------------------------
// cari Il2CppClass
// ----------------------------------------------------------------------------
void findAllKlass(const char* name, std::vector<uintptr_t>& out, size_t maxHits) {
    out.clear();
    if (!name || !*name) return;

    // Nama class il2cpp ada di blob global-metadata (dan di rodata lib-nya),
    // jadi cari di sana dulu -- jauh lebih cepat dari memindai 6 GB heap.
    // Yang dipakai buat class itu yang dari metadata, jadi itu yang diutamakan.
    std::vector<uintptr_t> strs;
    mem::findStringsInModule("global-metadata", name, strs, 8);
    if (!il2::libName().empty())
        mem::findStringsInModule(il2::libName().c_str(), name, strs, 8);
    if (strs.size() > 8) strs.resize(8);
    if (strs.empty()) {
        const uintptr_t s = mem::findString(name);
        if (s) strs.push_back(s);
    }
    if (strs.empty()) {
        logx::add("[il2] string class '%s' tidak ada di memori", name);
        return;
    }

    // Pointer balik ke string itu dicari DUA kali: 8 byte persis, terus dengan
    // byte paling atas diabaikan (device ber-MTE nulis tag di sana).
    // Memori game bisa lebih gede dari budget default (MLBB ~6,3 GB), jadi
    // budget-nya dinaikin biar pass-nya nggak kepotong di tengah jalan.
    mem::setScanBudget(12ull * 1024 * 1024 * 1024);

    std::vector<uintptr_t> hits;
    for (uintptr_t s : strs) mem::scanQword((uint64_t)s, hits, 512, true);
    std::vector<uintptr_t> tagged;
    for (uintptr_t s : strs)
        mem::scanQwordMasked((uint64_t)s, 0x00FFFFFFFFFFFFFFull, tagged, 512, true);
    // sebagian pointer bisa ada di region read-only juga (Il2CppImage)
    std::vector<uintptr_t> ro;
    for (uintptr_t s : strs) mem::scanQword((uint64_t)s, ro, 256, false);
    logx::add("[il2] '%s': str=0x%lx hits=%zu(+tag %zu, ro %zu)",
              name, (unsigned long)strs[0], hits.size(), tagged.size(), ro.size());

    for (uintptr_t h : hits) out.push_back(h);
    for (uintptr_t h : tagged) {
        bool dup = false;
        for (uintptr_t x : out) if (x == h) { dup = true; break; }
        if (!dup) out.push_back(h);
    }
    for (uintptr_t h : ro) {
        bool dup = false;
        for (uintptr_t x : out) if (x == h) { dup = true; break; }
        if (!dup) out.push_back(h);
    }

    // kandidat klass = (alamat field name) - 0x10
    std::vector<uintptr_t> klass;
    int rejected = 0;
    for (uintptr_t h : out) {
        if (h < kKlass_Name) continue;
        uintptr_t k = h - kKlass_Name;
        if (!mem::canRead(k, 0x40)) { ++rejected; continue; }
        if (!isValidKlass(k)) {
            ++rejected;
            // dump 6 qword pertama kandidat yang ditolak (buat cek layout)
            if (rejected <= 3) {
                uint64_t q[6] = {0};
                mem::read(k, q, sizeof(q));
                char line[320];
                snprintf(line, sizeof(line),
                         "[il2]   tolak 0x%lx: %016lx %016lx %016lx %016lx %016lx %016lx",
                         (unsigned long)k, (unsigned long)q[0], (unsigned long)q[1],
                         (unsigned long)q[2], (unsigned long)q[3], (unsigned long)q[4],
                         (unsigned long)q[5]);
                logx::add("%s", line);
            }
            continue;
        }
        // pastikan name-nya memang persis
        const char* kn = klassName(k);
        if (strcmp(kn, name) != 0) { ++rejected; continue; }
        bool dup = false;
        for (uintptr_t x : klass) if (x == k) { dup = true; break; }
        if (!dup) klass.push_back(k);
        if (klass.size() >= maxHits) break;
    }
    if (klass.empty())
        logx::add("[il2] '%s': %zu kandidat, semua ditolak", name, out.size());
    out.swap(klass);
}

static const char* g_lastSource = "scan";
const char* lastKlassSource() { return g_lastSource; }

uintptr_t findKlass(const char* name, const char* ns) {
    g_lastSource = "scan";

    // Dump offline duluan kalau ada: jalurnya O(1) (baca 1 sel + validasi),
    // sedangkan scan-string harus muterin ~5 GB memori (di MLBB ~10 detik dan
    // biasanya gagal). Kalau dump-nya basi, kandidatnya ditolak terus jatuh ke
    // scan -- jadi nggak ada yang hilang.
    if (dump::loaded()) {
        const uintptr_t kd = findKlassDump(name, ns, true);
        if (kd) { g_lastSource = "dump"; return kd; }
    }

    std::vector<uintptr_t> v;
    findAllKlass(name, v, 32);
    if (!v.empty()) {
        if (!ns || !*ns) return v[0];
        for (uintptr_t k : v) {
            // klassNamespace() pakai buffer statis: bandingin langsung aman di sini
            // karena cuma satu panggilan dalam satu ekspresi.
            if (strcmp(klassNamespace(k), ns) == 0) return k;
        }
        return v[0];
    }
    // Scan string mentok -> coba tabel class-slot dari dump offline.
    // Ini jalur wajib buat MLBB (metadata dipecah, nama kelas nggak punya
    // pointer balik), dan nggak nyakitin game lain karena tetap divalidasi.
    const uintptr_t k = findKlassDump(name, ns);
    if (k) { g_lastSource = "dump"; return k; }
    return 0;
}

// ---------------------------------------------------------------------------
// resolusi lewat tabel class-slot dump offline
// ---------------------------------------------------------------------------
// Di dump, tiap kelas punya baris:
//     // Class-slot candidates: 0x635b290        (atau "libcsharp.so 0x635b290")
// Itu RVA cell di dalam module yang isinya Il2CppClass*. Runtime ngisi cell
// itu saat metadata-usage-nya ke-resolve; kalau belum, isinya masih token
// (0x0603d19d) atau nol -- makanya WAJIB divalidasi, bukan langsung dipakai.
static uintptr_t untag(uintptr_t p) {
    // device ber-MTE nulis tag di byte paling atas (mis. 0xb400007a...)
    if (p >> 56) return p & 0x00FFFFFFFFFFFFFFull;
    return p;
}

// Validasi ringan: cuma nama kelas di +0x10 yang harus sama. Dipakai kalau
// isValidKlass() nolak padahal cell-nya kelihatan bener -- MLBB punya
// Il2CppImage non-standar (metadata-nya dipecah 3 lib), jadi syarat
// "image harus *.dll" bisa kelewat ketat di sana.
static bool weakNameOk(uintptr_t k, const char* name) {
    uintptr_t np = 0;
    if (!mem::rd(k + kKlass_Name, np) || !np) return false;
    char nm[160];
    if (!readStr(np, nm, sizeof(nm))) return false;
    return strcmp(nm, name) == 0;
}

static std::vector<std::pair<std::string, uintptr_t>> g_dumpCache;

// Di-set waktu dumper nge-resolve RIBUAN kelas sekaligus dari katalog dump
// (dumpout 4b). Kalau nggak, tiap kelas sukses nulis satu baris log -> logcat
// kebanjiran 18 rb baris cuma buat "berhasil". Default: tetap verbose (dipakai
// resolver menu per-kelas, di situ jejaknya berguna).
static bool g_dumpBulkQuiet = false;
void setDumpBulkQuiet(bool q) { g_dumpBulkQuiet = q; }

uintptr_t findKlassDump(const char* name, const char* ns, bool quiet) {
    if (!name || !*name || !dump::loaded()) return 0;

    const std::string key = std::string(ns ? ns : "") + "::" + name;
    for (const auto& kv : g_dumpCache)
        if (kv.first == key) return kv.second;

    std::vector<const dump::ClassEntry*> hits;
    if (!dump::find(name, ns, hits)) {
        if (!quiet) logx::add("[il2/dump] '%s' nggak ada di tabel dump", name);
        return 0;
    }

    int tried = 0, alive = 0, weak = 0, missed = 0;
    for (const dump::ClassEntry* e : hits) {
        for (const dump::Slot& s : e->slots) {
            uintptr_t base = 0;
            if (s.module.empty() || s.module == libName()) base = g_base;
            else                                          base = mem::base(s.module.c_str());
            ++tried;

            // Beberapa tafsir rva dicoba: dump baru = file offset segmen (stabil),
            // dump lama = base + rva, plus region lain yang kebetulan mencakup
            // file offset itu (modul yang segmennya dimap terpisah). Yang dipakai:
            // yang isinya kelas SAH; kalau nggak ada yang sah, yang isinya masuk
            // akal. Buat modul yang mapping-nya nyambung semua kandidat sama.
            std::vector<uintptr_t> cands;
            mem::addrOfRvaAll(s.rva, cands, 6);
            if (base) cands.push_back(mem::untag(base) + (uintptr_t)s.rva);

            uintptr_t k = 0, kWeak = 0;
            for (uintptr_t c : cands) {
                uintptr_t v = 0;
                if (!c || !mem::rd(c, v) || !v) continue;
                v = untag(v);
                if (!v || !mem::canRead(v, 0x40)) continue;
                if (isValidKlass(v)) { k = v; break; }
                if (!kWeak) kWeak = v;
            }
            if (!k) k = kWeak;
            if (!k) { ++missed; continue; }

            const bool strong = isValidKlass(k);
            if (strong) ++alive;
            if (!strong && !weakNameOk(k, name)) { ++missed; continue; }
            if (!strong) ++weak;

            if (ns && *ns) {
                char kns[160];
                snprintf(kns, sizeof(kns), "%s", klassNamespace(k));
                if (strcmp(kns, ns) != 0) continue;
            }

            g_dumpCache.push_back({key, k});
            if (!g_dumpBulkQuiet)
                logx::add("[il2/dump] %s -> 0x%lx  (%s + 0x%llx) %s%s", name,
                          (unsigned long)k,
                          s.module.empty() ? libName().c_str() : s.module.c_str(),
                          (unsigned long long)s.rva,
                          e->assembly.empty() ? "" : e->assembly.c_str(),
                          strong ? "" : "  [validasi lemah: image non-standar]");
            return k;
        }
    }

    if (!quiet) {
        if (!tried)
            logx::add("[il2/dump] '%s': %zu entri dump, tapi module sel (libcell) nggak "
                      "ke-load di proses ini", name, hits.size());
        else
            logx::add("[il2/dump] '%s': %d cell dicoba (kuat %d, lemah %d, nggak nyambung %d) "
                      "-- kalau game baru di login, coba lagi setelah masuk lobby",
                      name, tried, alive, weak, missed);
    }
    return 0;
}

// ----------------------------------------------------------------------------
// cari instance
// ----------------------------------------------------------------------------
void findInstances(uintptr_t klass, std::vector<uintptr_t>& out, size_t maxHits) {
    out.clear();
    if (!klass) return;

    std::vector<uintptr_t> hits;
    mem::scanQword((uint64_t)klass, hits, maxHits * 8, true);

    for (uintptr_t h : hits) {
        // validasi bentuk object: [0]=klass, [8]=monitor (0 atau pointer valid)
        uint64_t hdr[2] = {0, 0};
        if (!mem::read(h, hdr, sizeof(hdr))) continue;
        if (hdr[0] != (uint64_t)klass) continue;
        if (hdr[1] != 0 && !mem::canRead((uintptr_t)hdr[1], 1)) continue;
        out.push_back(h);
        if (out.size() >= maxHits) break;
    }
}

uintptr_t findInstance(uintptr_t klass, size_t maxHits) {
    std::vector<uintptr_t> v;
    findInstances(klass, v, maxHits);
    return v.empty() ? 0 : v[0];
}

// ----------------------------------------------------------------------------
// cache klass (nama+namespace -> alamat)
// ----------------------------------------------------------------------------
static std::vector<std::pair<std::string, uintptr_t>> g_cache;

// Region tempat IL2CPP menyimpan salinan heap dari metadata string.
// Semua nama class ada di situ, jadi setelah ketemu sekali, pencarian
// berikutnya cukup memindai region ini (jauh lebih cepat).
static uintptr_t g_metaLo = 0, g_metaHi = 0;

// Cari semua kemunculan nama class.
// PENTING: salinan metadata yang di-rodata .so juga harus ikut dicari --
// di device ini justru salinan itulah yang punya pointer balik ke Il2CppClass.
static uintptr_t findNameOccurrences(const char* name, std::vector<uintptr_t>& occ) {
    occ.clear();
    // 1) jalur tercepat: rodata libil2cpp.so (di sinilah nama class berada)
    mem::findStringsInModule("libil2cpp.so", name, occ, 32);
    if (!occ.empty()) return occ[0];
    // 2) global-metadata kalau metadata dipisah
    mem::findStringsInModule("global-metadata", name, occ, 32);
    if (!occ.empty()) return occ[0];
    // 3) region metadata yang sudah pernah ketemu
    if (g_metaLo && g_metaHi) {
        mem::findStringsInRange(g_metaLo, g_metaHi, name, occ, 32);
        if (!occ.empty()) return occ[0];
    }
    // 4) fallback: seluruh memori
    mem::findStringsAll(name, occ, 32, false);
    return occ.empty() ? 0 : occ[0];
}

static std::string cacheKey(const char* name, const char* ns) {
    std::string k;
    if (ns && *ns) { k = ns; k += '.'; }
    k += name;
    return k;
}

static uintptr_t cacheGet(const char* name, const char* ns) {
    std::string key = cacheKey(name, ns);
    for (const auto& kv : g_cache)
        if (kv.first == key) return kv.second;
    return 0;
}

static void cachePut(const char* name, const char* ns, uintptr_t k) {
    std::string key = cacheKey(name, ns);
    for (auto& kv : g_cache)
        if (kv.first == key) { kv.second = k; return; }
    g_cache.push_back({key, k});
}

void cacheClear() {
    g_cache.clear();
    g_metaLo = g_metaHi = 0;
}

// ----------------------------------------------------------------------------
// batch: cari banyak class dalam satu pass pointer
// ----------------------------------------------------------------------------
void findKlassBatch(const BatchTarget* targets, size_t n, uintptr_t* klassOut) {
    if (!targets || !klassOut || !n) return;

    for (size_t i = 0; i < n; ++i)
        klassOut[i] = cacheGet(targets[i].name, targets[i].ns);

    // 1) kumpulkan semua kemunculan string dari class yang belum ketemu
    std::vector<uint64_t> vals;
    std::vector<int>      owner;
    for (size_t i = 0; i < n; ++i) {
        if (klassOut[i]) continue;
        std::vector<uintptr_t> occ;
        findNameOccurrences(targets[i].name, occ);
        logx::add("[il2] '%s': %zu kemunculan string", targets[i].name, occ.size());
        for (uintptr_t a : occ) { vals.push_back((uint64_t)a); owner.push_back((int)i); }
    }
    if (vals.empty()) {
        // nama class-nya nggak ada di memori sama sekali -> jalur dump doang
        for (size_t i = 0; i < n; ++i)
            if (!klassOut[i]) klassOut[i] = findKlassDump(targets[i].name, targets[i].ns, true);
        return;
    }

    // 2) satu pass cari pointer ke string-string itu
    std::vector<mem::PtrHit> hits;
    mem::scanQwordSet(vals.data(), vals.size(), hits, 131072, true);
    logx::add("[il2] pass pointer: %zu hit dari %zu target string", hits.size(), vals.size());

    // 3) validasi kandidat
    int found = 0;
    for (const auto& h : hits) {
        if (h.addr < kKlass_Name) continue;
        int ti = -1;
        for (size_t j = 0; j < vals.size(); ++j)
            if (vals[j] == h.value) { ti = owner[j]; break; }
        if (ti < 0 || klassOut[ti]) continue;

        uintptr_t k = h.addr - kKlass_Name;
        if (!isValidKlass(k)) continue;
        if (strcmp(klassName(k), targets[ti].name) != 0) continue;
        if (targets[ti].ns && *targets[ti].ns &&
            strcmp(klassNamespace(k), targets[ti].ns) != 0) continue;

        klassOut[ti] = k;
        cachePut(targets[ti].name, targets[ti].ns, k);

        // simpan region metadata string buat mempercepat pencarian berikutnya
        if (!g_metaLo) {
            uintptr_t nameP = 0;
            mem::rd(k + kKlass_Name, nameP);
            const mem::Region* mr = mem::regionOf(nameP);
            if (mr) { g_metaLo = mr->start; g_metaHi = mr->end; }
        }

        logx::add("[il2] '%s' -> klass 0x%lx", targets[ti].name, (unsigned long)k);
        if (++found >= (int)n) break;
    }

    // 4. yang masih kosong: coba tabel class-slot dari dump offline
    for (size_t i = 0; i < n; ++i) {
        if (klassOut[i]) continue;
        klassOut[i] = findKlassDump(targets[i].name, targets[i].ns, true);
        if (klassOut[i]) cachePut(targets[i].name, targets[i].ns, klassOut[i]);
    }
}

void findInstancesBatch(const uintptr_t* klasses, size_t n,
                        std::vector<std::vector<uintptr_t>>& out, size_t perMax) {
    out.assign(n, std::vector<uintptr_t>());
    if (!klasses || !n) return;
    std::vector<std::vector<uintptr_t>> front(n), back(n);

    std::vector<uint64_t> vals;
    for (size_t i = 0; i < n; ++i)
        if (klasses[i]) vals.push_back((uint64_t)klasses[i]);
    if (vals.empty()) return;

    std::vector<mem::PtrHit> hits;
    mem::scanQwordSet(vals.data(), vals.size(), hits, 262144, true);
    logx::add("[il2] pass instance: %zu hit", hits.size());

    size_t skippedSelfRef = 0;
    for (const auto& h : hits) {
        int ti = -1;
        for (size_t j = 0; j < n; ++j)
            if (klasses[j] == h.value) { ti = (int)j; break; }
        if (ti < 0 || front[ti].size() + back[ti].size() >= perMax) continue;

        // Buang "instance" yang sebenarnya ada DI DALAM object Il2CppClass itu
        // sendiri. Il2CppClass punya field yang menunjuk balik ke dirinya
        // (element_class di +0x40 dan sejenisnya), jadi [0]==klass juga cocok
        // di situ -- dan kalau lolos, semua "field"-nya jadi pointer sampah.
        if (h.addr >= klasses[ti] && h.addr < klasses[ti] + 0x400) {
            ++skippedSelfRef;
            continue;
        }

        // validasi bentuk object IL2CPP: [0]=klass, [8]=monitor (0/pointer valid)
        uint64_t hdr[2] = {0, 0};
        if (!mem::read(h.addr, hdr, sizeof(hdr))) continue;
        if (hdr[0] != h.value) continue;
        if (hdr[1] != 0 && !mem::canRead((uintptr_t)hdr[1], 1)) continue;

        // Object yang hidup hampir selalu punya monitor = 0. Struct metadata
        // (Il2CppType dsb.) yang [0]-nya kebetulan sama tidak. Simpan di dua
        // daftar supaya yang monitor-nya nol selalu di depan.
        (hdr[1] == 0 ? front[ti] : back[ti]).push_back(h.addr);
    }
    for (size_t i = 0; i < n; ++i) {
        if (!klasses[i]) continue;
        out[i] = front[i];
        out[i].insert(out[i].end(), back[i].begin(), back[i].end());
        if (out[i].size() > perMax) out[i].resize(perMax);
    }
    if (skippedSelfRef)
        logx::add("[il2] buang %zu hit di dalam object class (self-reference)", skippedSelfRef);
    for (size_t i = 0; i < n; ++i)
        if (klasses[i]) logx::add("[il2] instance klass 0x%lx: %zu",
                                 (unsigned long)klasses[i], out[i].size());
}

// ----------------------------------------------------------------------------
// pembaca offset field dari metadata il2cpp (runtime, tanpa dump)
// ----------------------------------------------------------------------------
static uint32_t g_fieldsOff  = 0;      // offset pointer 'fields' di Il2CppClass
static uint32_t g_fieldStride = 0x20;  // ukuran satu FieldInfo

// Cek satu kandidat (offset 'fields', stride): apakah isi array-nya benar-benar
// FieldInfo milik klass ini? Indikatornya: tiap entri punya pointer parent yang
// sama dengan klass, dan nama-namanya cocok dengan yang kita tunggu.
static bool fieldsLookRight(uintptr_t klass, uint32_t klassOff, uint32_t stride,
                            const char* const* want, size_t nWant) {
    uintptr_t fp = 0;
    if (!mem::rd(klass + klassOff, fp)) return false;
    if (!fp || !mem::canRead(fp, stride * 2)) return false;

    int matched = 0, seen = 0;
    for (uint32_t i = 0; i < 128; ++i) {
        uintptr_t np = 0, pp = 0;
        if (!mem::rd(fp + (uintptr_t)i * stride, np)) break;
        if (!np || !mem::canRead(np, 2)) break;
        mem::rd(fp + (uintptr_t)i * stride + 0x10, pp);
        if (pp != klass) continue;

        char nm[96];
        if (!readStr(np, nm, sizeof(nm))) continue;
        if (!printableStr(nm, sizeof(nm))) continue;
        ++seen;
        for (size_t k = 0; k < nWant; ++k)
            if (strcmp(nm, want[k]) == 0) { ++matched; break; }
    }
    // semua nama yang dicari harus ketemu, dan array-nya kelihatan wajar
    return matched == (int)nWant && seen >= (int)nWant;
}

bool fieldReaderInit(uintptr_t klass, uint32_t* kalOff, uint32_t* kalStride) {
    if (g_fieldsOff) return true;                 // sudah pernah kalibrasi
    if (!klass) return false;

    // Nama field RunSessionData yang sudah kita pastikan dari dump. Dipakai
    // sekaligus bukti: kalau tebakan (fieldsOff, stride) benar, offset yang
    // terbaca harus PERSIS sama (Points 0x50, Coins 0x58, _distance 0x18).
    static const char* want[] = {"Points", "Coins", "_distance"};
    static const uint32_t wantOff[] = {0x50u, 0x58u, 0x18u};
    static const uint32_t strides[] = {0x20u, 0x18u, 0x28u, 0x30u};

    for (uint32_t off = 0x38; off <= 0x180; off += 8) {
        for (uint32_t si = 0; si < 4; ++si) {
            const uint32_t stride = strides[si];
            if (!fieldsLookRight(klass, off, stride, want, 3)) continue;

            // verifikasi silang: offset yang terbaca harus sama dengan dump
            bool ok = true;
            uintptr_t fp = 0;
            mem::rd(klass + off, fp);
            for (size_t k = 0; k < 3 && ok; ++k) {
                uint32_t got = 0;
                for (uint32_t i = 0; i < 128; ++i) {
                    uintptr_t np = 0, pp = 0;
                    if (!mem::rd(fp + (uintptr_t)i * stride, np) || !np) break;
                    mem::rd(fp + (uintptr_t)i * stride + 0x10, pp);
                    if (pp != klass) continue;
                    char nm[96];
                    if (!readStr(np, nm, sizeof(nm))) continue;
                    if (strcmp(nm, want[k]) == 0) { mem::rd(fp + (uintptr_t)i * stride + 0x18, got); break; }
                }
                if (got != wantOff[k]) ok = false;
            }
            if (!ok) continue;

            g_fieldsOff = off;
            g_fieldStride = stride;
            if (kalOff) *kalOff = off;
            if (kalStride) *kalStride = stride;
            logx::add("[il2] field reader siap: fields@+0x%x stride=0x%x (terverifikasi)",
                      off, stride);
            return true;
        }
    }
    logx::add("[il2] field reader GAGAL kalibrasi -- metadata beda dari yang diduga");
    return false;
}

uint32_t fieldOffset(uintptr_t klass, const char* fieldName) {
    if (!g_fieldsOff || !klass || !fieldName || !*fieldName) return 0;
    uintptr_t fp = 0;
    if (!mem::rd(klass + g_fieldsOff, fp) || !fp) return 0;
    for (uint32_t i = 0; i < 128; ++i) {
        uintptr_t np = 0, pp = 0;
        if (!mem::rd(fp + (uintptr_t)i * g_fieldStride, np) || !np) break;
        if (!mem::canRead(np, 2)) break;
        mem::rd(fp + (uintptr_t)i * g_fieldStride + 0x10, pp);
        if (pp != klass) continue;
        char nm[96];
        if (!readStr(np, nm, sizeof(nm))) continue;
        if (strcmp(nm, fieldName) != 0) continue;
        uint32_t off = 0;
        mem::rd(fp + (uintptr_t)i * g_fieldStride + 0x18, off);
        return off;
    }
    return 0;
}

void logClassFields(uintptr_t klass, const char* tag) {
    if (!g_fieldsOff) { logx::add("[il2] %s: field reader belum siap", tag); return; }
    const char* nm = klassName(klass);
    const size_t len = strlen(nm);
    char copy[160];
    if (len >= sizeof(copy)) return;
    memcpy(copy, nm, len + 1);
    logx::add("[il2] fields %s (class %s):", tag, copy);

    uintptr_t fp = 0;
    if (!mem::rd(klass + g_fieldsOff, fp) || !fp) return;
    for (uint32_t i = 0; i < 128; ++i) {
        uintptr_t np = 0, pp = 0;
        if (!mem::rd(fp + (uintptr_t)i * g_fieldStride, np) || !np) break;
        mem::rd(fp + (uintptr_t)i * g_fieldStride + 0x10, pp);
        if (pp != klass) continue;
        char fn[96];
        if (!readStr(np, fn, sizeof(fn))) continue;
        uint32_t off = 0;
        mem::rd(fp + (uintptr_t)i * g_fieldStride + 0x18, off);
        logx::add("[il2]   %s = 0x%x", fn, off);
    }
}

uintptr_t getPtr(uintptr_t inst, uint32_t off) {
    uintptr_t p = 0;
    if (!inst) return 0;
    if (!mem::canRead(inst + off, sizeof(p))) return 0;
    mem::rd(inst + off, p);
    return p;
}

}  // namespace il2

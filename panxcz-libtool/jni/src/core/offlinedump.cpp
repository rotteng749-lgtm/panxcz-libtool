#include "core/offlinedump.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>

#include "core/log.h"
#include "core/target.h"

namespace dump {

// ---------------------------------------------------------------------------
// state
// ---------------------------------------------------------------------------
static std::string             g_buf;        // isi file dump (36 MB-an)
static std::string             g_path;
static std::vector<ClassEntry> g_classes;
// "<lib>\t<dll>" -> jumlah kelas. Dipakai buat ringkasan module: di dump
// gabungan (multi-module) satu lib bisa muncul dengan beberapa dll, jadi yang
// ditulis itu dll dengan kelas paling banyak -- dulu cuma yang pertama ketemu,
// jadi bisa salah (liblogic.so -> Assembly-CSharp-firstpass.dll).
static std::vector<std::pair<std::string, size_t>> g_modules;
static size_t                  g_withSlot = 0;
static std::vector<std::string> g_files;     // file yang ikut kebaca (bisa >1)

bool loaded() { return !g_classes.empty(); }
const std::string& path() { return g_path; }
const std::vector<std::string>& files() { return g_files; }
size_t classCount() { return g_classes.size(); }
size_t slotCount() { return g_withSlot; }
size_t moduleCount() { return g_modules.size(); }
const std::vector<ClassEntry>& classes() { return g_classes; }

std::string moduleSummary() {
    // kelompokkan per lib, ambil dll yang kelasnya paling banyak
    std::vector<std::pair<std::string, std::pair<std::string, size_t>>> perLib;
    for (const auto& m : g_modules) {
        const size_t tab = m.first.find('\t');
        if (tab == std::string::npos) continue;
        const std::string lib = m.first.substr(0, tab);
        const std::string dll = m.first.substr(tab + 1);
        bool found = false;
        for (auto& e : perLib) {
            if (e.first != lib) continue;
            if (m.second > e.second.second) e.second = {dll, m.second};
            found = true;
            break;
        }
        if (!found) perLib.push_back({lib, {dll, m.second}});
    }

    std::string s;
    for (const auto& e : perLib) {
        if (!s.empty()) s += " | ";
        s += e.first + " -> " + e.second.first + " (" + std::to_string(e.second.second) + ")";
    }
    return s;
}

// ---------------------------------------------------------------------------
// util
// ---------------------------------------------------------------------------
static std::string trim(const std::string& s) {
    size_t b = 0, e = s.size();
    while (b < e && isspace((unsigned char)s[b])) ++b;
    while (e > b && isspace((unsigned char)s[e - 1])) --e;
    return s.substr(b, e - b);
}

// "1 2 3" -> "3 2 1" (kata terakhir jadi depan), dipakai buat misah "tipe nama"
static bool splitLastWord(const std::string& s, std::string& head, std::string& last) {
    size_t e = s.size();
    while (e > 0 && isspace((unsigned char)s[e - 1])) --e;
    size_t b = e;
    while (b > 0 && !isspace((unsigned char)s[b - 1])) --b;
    if (b == 0 || b >= e) return false;
    last = s.substr(b, e - b);
    head = trim(s.substr(0, b));
    return true;
}

// baris demi baris tanpa alokasi: panggil callback(pos_awal, pos_akhir)
template <class F>
static void forEachLine(const std::string& buf, F f) {
    size_t i = 0, n = buf.size();
    while (i < n) {
        size_t j = buf.find('\n', i);
        if (j == std::string::npos) j = n;
        size_t e = j;
        if (e > i && buf[e - 1] == '\r') --e;
        f(i, e);
        i = j + 1;
    }
}

// ---------------------------------------------------------------------------
// parse slot: "libcsharp.so 0x62dc5e0" / "0x62dc5e0" / "a 0x1, b 0x2"
// ---------------------------------------------------------------------------
static void parseSlots(const std::string& raw, const std::string& fallbackModule,
                       std::vector<Slot>& out) {
    out.clear();
    size_t i = 0;
    while (i < raw.size()) {
        size_t comma = raw.find(',', i);
        std::string part = trim(raw.substr(i, comma == std::string::npos
                                                 ? std::string::npos : comma - i));
        i = (comma == std::string::npos) ? raw.size() : comma + 1;
        if (part.empty() || part == "unknown") continue;

        size_t hex = part.find("0x");
        if (hex == std::string::npos) continue;
        Slot s;
        s.rva = strtoull(part.c_str() + hex, nullptr, 16);
        std::string mod = trim(part.substr(0, hex));
        s.module = mod.empty() ? fallbackModule : mod;
        if (s.rva) out.push_back(s);
    }
}

// ---------------------------------------------------------------------------
// load
// ---------------------------------------------------------------------------
// ---------------------------------------------------------------------------
// baca file ke g_buf (nambah di belakang -> beberapa file bisa digabung)
// ---------------------------------------------------------------------------
static bool appendFile(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f.is_open()) {
        logx::add("[dump] nggak bisa buka %s", path.c_str());
        return false;
    }
    f.seekg(0, std::ios::end);
    const std::streamoff size = f.tellg();
    f.seekg(0, std::ios::beg);
    if (size <= 0) {
        logx::add("[dump] %s kosong", path.c_str());
        return false;
    }
    const size_t at = g_buf.size();
    g_buf.resize(at + (size_t)size);
    if (!f.read(&g_buf[at], size)) {
        logx::add("[dump] gagal baca %s", path.c_str());
        g_buf.resize(at);
        return false;
    }
    f.close();
    if (g_buf.back() != '\n') g_buf.push_back('\n');   // jangan nempel ke file berikutnya
    return true;
}

// ---------------------------------------------------------------------------
// parse isi g_buf (boleh gabungan beberapa file) jadi katalog kelas
// ---------------------------------------------------------------------------
static void parseClasses() {
    std::string module, assembly, ns;
    uint32_t    typedefIndex = 0;
    uint64_t    token = 0;
    bool        inBody = false;     // lagi di dalam blok kelas yang baru dipush
    bool        havePendingSlots = false;
    std::vector<Slot> pendingSlots;

    g_classes.reserve(32768);

    forEachLine(g_buf, [&](size_t a, size_t b) {
        const char* p = g_buf.data();
        const size_t len = b - a;
        if (len == 0) return;

        // ---- "// Module 3: libcsharp.so / Assembly-CSharp.dll"
        if (len > 10 && strncmp(p + a, "// Module ", 10) == 0) {
            inBody = false;
            std::string line(p + a, len);
            size_t colon = line.find(": ");
            size_t slash = line.find(" / ", colon == std::string::npos ? 0 : colon);
            if (colon != std::string::npos) {
                module = trim(line.substr(colon + 2,
                                          slash == std::string::npos
                                              ? std::string::npos : slash - colon - 2));
            }
            if (slash != std::string::npos)
                assembly = trim(line.substr(slash + 3));
            return;
        }

        // ---- "// Namespace: Mono; TypeDefIndex: 3; Token: 0x2000004"
        if (len > 13 && strncmp(p + a, "// Namespace:", 13) == 0) {
            inBody = false;
            std::string line(p + a, len);
            const char* kNs = "; TypeDefIndex:";
            size_t k = line.find(kNs);
            ns = trim(line.substr(13, k == std::string::npos ? std::string::npos : k - 13));
            typedefIndex = 0;
            token = 0;
            if (k != std::string::npos) {
                size_t t = line.find("Token: ", k);
                if (t != std::string::npos)
                    token = strtoull(line.c_str() + t + 7, nullptr, 16);
                typedefIndex = (uint32_t)strtoul(line.c_str() + k + strlen(kNs), nullptr, 10);
            }
            havePendingSlots = false;
            pendingSlots.clear();
            return;
        }

        // ---- "// RVA: 0x..." (kualitas tubuh kelas -- lihat ClassEntry::rvaReal)
        {
            size_t s = a;
            while (s < b && (p[s] == ' ' || p[s] == '\t')) ++s;
            if (inBody && b - s > 6 && p[s] == '/' && p[s + 1] == '/' &&
                !g_classes.empty()) {
                size_t rv = std::string(p + s, b - s).find("RVA:");
                if (rv != std::string::npos) {
                    const std::string line(p + s, b - s);
                    if (line.find("0x", rv) != std::string::npos) ++g_classes.back().rvaReal;
                    else                                          ++g_classes.back().rvaUnknown;
                    return;
                }
            }
        }

        // ---- "// Class-slot candidates: 0x62dc5e0"
        if (len > 25 && strncmp(p + a, "// Class-slot candidates:", 25) == 0) {
            parseSlots(trim(std::string(p + a + 25, len - 25)), module, pendingSlots);
            havePendingSlots = true;
            return;
        }

        // ---- deklarasi kelas: "class X", "struct X", "enum X", "interface X"
        //      (modifier public/private/... dibuang dulu)
        {
            size_t s = a;
            while (s < b && (p[s] == ' ' || p[s] == '\t')) ++s;
            if (s == a && p[s] != '/' && p[s] != '#') {   // kolom 0 = top-level
                size_t ws = s + 1;
                while (ws < b && !isspace((unsigned char)p[ws])) ++ws;
                if (ws < b) {
                    const char* kw = p + s;
                    size_t kwLen = ws - s;
                    bool isDecl = false;
                    Kind kind = Kind::Class;
                    const char* kws[] = {"class", "struct", "enum", "interface"};
                    const Kind  kinds[] = {Kind::Class, Kind::Struct, Kind::Enum, Kind::Interface};
                    for (int ki = 0; ki < 4; ++ki)
                        if (kwLen == strlen(kws[ki]) && strncmp(kw, kws[ki], kwLen) == 0) {
                            isDecl = true;
                            kind = kinds[ki];
                        }
                    if (isDecl) {
                        size_t ns0 = ws;
                        while (ns0 < b && isspace((unsigned char)p[ns0])) ++ns0;
                        size_t ne = ns0;
                        while (ne < b && p[ne] != ' ' && p[ne] != '{' && p[ne] != ':') ++ne;

                        ClassEntry e;
                        e.name.assign(p + ns0, ne - ns0);
                        e.ns = ns;
                        e.assembly = assembly;
                        e.module = module;
                        e.typeDefIndex = typedefIndex;
                        e.token = token;
                        e.kind = kind;
                        e.slots = havePendingSlots ? pendingSlots : std::vector<Slot>();
                        e.bodyOff = b + 1;
                        inBody = false;      // RVA-nya baru dihitung mulai baris berikutnya

                        if (!e.name.empty()) {
                            if (!e.slots.empty()) ++g_withSlot;
                            g_classes.push_back(std::move(e));
                            inBody = true;

                            // catat module->dll buat ringkasan (dihitung)
                            const std::string key = module + "\t" + assembly;
                            bool counted = false;
                            for (auto& m : g_modules)
                                if (m.first == key) { ++m.second; counted = true; break; }
                            if (!counted) g_modules.push_back({key, 1});
                        }
                        havePendingSlots = false;
                        pendingSlots.clear();
                        return;
                    }
                }
            }
        }
    });

}

// ---------------------------------------------------------------------------
// load
// ---------------------------------------------------------------------------
// Nama file aja (log lebih enak dibaca daripada path panjang) -- tapi kalau ada
// dua file namanya sama dari folder beda, path penuhnya yang dipakai.
static std::string shortName(const std::string& p) {
    const size_t s = p.find_last_of('/');
    return s == std::string::npos ? p : p.substr(s + 1);
}

bool loadMany(const std::vector<std::string>& paths) {
    unload();
    if (paths.empty()) return false;

    for (const auto& p : paths) {
        const size_t before = g_classes.size();
        (void)before;
        if (appendFile(p)) g_files.push_back(p);
    }
    if (g_buf.empty()) {
        unload();
        return false;
    }

    parseClasses();
    if (g_classes.empty()) {
        logx::add("[dump] %zu file dibaca tapi nggak ada kelas yang kebaca (format beda?)",
                  g_files.size());
        unload();
        return false;
    }

    // Buang kelas kembar (dump per-lib sering nulis kelas yang sama dua kali,
    // mis. kelas yang method-nya nyebar ke dua lib). Tanpa ini jumlah kelas + slot
    // jadi kelihatan lebih banyak dari yang beneran ada.
    if (g_files.size() > 1) {
        std::vector<ClassEntry> uniq;
        uniq.reserve(g_classes.size());
        std::map<std::string, size_t> seen;
        size_t dup = 0;
        size_t better = 0;
        for (auto& e : g_classes) {
            const std::string key = e.name + "\t" + e.ns + "\t" + e.module + "\t" + e.assembly;
            auto it = seen.find(key);
            if (it != seen.end()) {
                ++dup;
                ClassEntry& keep = uniq[it->second];
                if (keep.slots.empty() && !e.slots.empty()) keep.slots = e.slots;
                // tubuh yang RVA-nya nyata lebih dipakai (dump lain nulis
                // "RVA: unknown" -- kalau dibiarkan, kelas yang RVA-nya asli
                // ketutup cuma karena filenya lebih dulu urutannya).
                if (e.rvaReal > keep.rvaReal) {
                    const std::vector<Slot> keepSlots = keep.slots;
                    keep = std::move(e);
                    if (keep.slots.empty()) keep.slots = keepSlots;
                    ++better;
                }
                continue;
            }
            seen[key] = uniq.size();
            uniq.push_back(std::move(e));
        }
        if (dup) logx::add("[dump] gabungan: %zu kelas kembar dibuang (%zu di antaranya diganti versi yang RVA method-nya nyata)",
                           dup, better);
        g_classes.swap(uniq);
        // hitung ulang jumlah kelas yang punya slot (yang tadi dihitung per file)
        g_withSlot = 0;
        for (const auto& e : g_classes) if (!e.slots.empty()) ++g_withSlot;
    }

    if (g_files.size() == 1) {
        g_path = g_files[0];
        logx::add("[dump] %s: %zu kelas, %zu punya slot, %zu module (%zu MB)",
                  shortName(g_files[0]).c_str(), g_classes.size(), g_withSlot,
                  g_modules.size(), (size_t)(g_buf.size() / (1024 * 1024)));
    } else {
        g_path = shortName(g_files[0]) + " +" + std::to_string(g_files.size() - 1);
        logx::add("[dump] gabungan %zu file: %zu kelas, %zu punya slot, %zu module (%zu MB)",
                  g_files.size(), g_classes.size(), g_withSlot,
                  g_modules.size(), (size_t)(g_buf.size() / (1024 * 1024)));
        for (const auto& p : g_files) logx::add("[dump]   + %s", shortName(p).c_str());
    }
    {
        const std::string sum = moduleSummary();
        size_t pos = 0;
        while (pos < sum.size()) {   // per lib (dipisah " | ")
            size_t bar = sum.find(" | ", pos);
            const std::string one = sum.substr(pos, bar == std::string::npos
                                                        ? std::string::npos : bar - pos);
            logx::add("[dump]   %s", one.c_str());
            if (bar == std::string::npos) break;
            pos = bar + 3;
        }
    }
    return true;
}

bool load(const char* path) {
    if (!path || !*path) return false;
    std::vector<std::string> one{std::string(path)};
    return loadMany(one);
}

bool loadAuto(std::string* triedPath) {
    const std::vector<std::string> paths = target::findDumpFiles();
    if (triedPath) {
        if (paths.empty()) triedPath->clear();
        else if (paths.size() == 1) *triedPath = paths[0];
        else *triedPath = shortName(paths[0]) + " +" + std::to_string(paths.size() - 1);
    }
    if (paths.empty()) {
        logx::add("[dump] file dump nggak ketemu (taruh di /data/local/tmp/panxcz_dump.cs "
                  "atau /sdcard/panxcz/dump.cs, atau set dump= di cfg)");
        return false;
    }
    return loadMany(paths);
}

void unload() {
    g_buf.clear();
    g_buf.shrink_to_fit();
    g_classes.clear();
    g_classes.shrink_to_fit();
    g_modules.clear();
    g_files.clear();
    g_withSlot = 0;
    g_path.clear();
}

// ---------------------------------------------------------------------------
// find
// ---------------------------------------------------------------------------
// Nama di dump ini "apa adanya" dari metadata: generic pake backtick (List`1).
// Yang pengen dicari orang biasanya "List", jadi dipakai juga versi tanpa
// suffix `N dan tanpa namespace.
// hasil `find` diurut dari yang tubuhnya paling punya RVA nyata -- semua
// pemanggil cuma liat hits[0], jadi urutannya yang nentuin kelas mana yang
// dipakai buat nama method + RVA.
static void sortHits(std::vector<const ClassEntry*>& out) {
    if (out.size() < 2) return;
    std::stable_sort(out.begin(), out.end(),
                     [](const ClassEntry* a, const ClassEntry* b) {
                         if (a->rvaReal != b->rvaReal) return a->rvaReal > b->rvaReal;
                         return a->slots.size() > b->slots.size();
                     });
}

static std::string stripGeneric(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '`') {
            ++i;
            while (i < s.size() && isdigit((unsigned char)s[i])) ++i;
            --i;
            continue;
        }
        out.push_back(s[i]);
    }
    return out;
}

bool find(const char* name, const char* ns, std::vector<const ClassEntry*>& out) {
    out.clear();
    if (!name || !*name || g_classes.empty()) return false;

    const std::string want = name;
    const std::string wantNs = ns ? ns : "";

    // 1) persis + namespace cocok
    for (const auto& e : g_classes)
        if (e.name == want && (wantNs.empty() || e.ns == wantNs))
            out.push_back(&e);
    if (!out.empty()) { sortHits(out); return true; }

    // 2) persis, namespace apa aja
    for (const auto& e : g_classes)
        if (e.name == want) out.push_back(&e);
    if (!out.empty()) { sortHits(out); return true; }

    // 3) versi tanpa arity generic (List`1 -> List)
    const std::string loose = stripGeneric(want);
    if (loose != want) {
        for (const auto& e : g_classes) {
            if (e.name.find('`') == std::string::npos) continue;
            if (stripGeneric(e.name) != loose) continue;
            if (!wantNs.empty() && e.ns != wantNs) continue;
            out.push_back(&e);
        }
        sortHits(out);
    }
    return !out.empty();
}

// ---------------------------------------------------------------------------
// detail (field + method dari blok kelas)
// ---------------------------------------------------------------------------
bool detail(const ClassEntry& e, ClassDetail& out) {
    out = ClassDetail{};
    if (!e.bodyOff || e.bodyOff >= g_buf.size()) return false;

    const char* p = g_buf.data();
    const size_t n = g_buf.size();
    bool pendingRva = false, pendingHasRva = false;
    uint64_t pendingRvaVal = 0;
    std::string pendingMod;   // tag module di belakang RVA: "0x1219718 (liblogic.so)"

    size_t i = e.bodyOff;
    while (i < n) {
        size_t j = g_buf.find('\n', i);
        if (j == std::string::npos) j = n;
        size_t b = j;
        if (b > i && p[b - 1] == '\r') --b;
        const size_t len = b - i;

        // akhir blok: '}' di kolom 0
        if (len > 0 && p[i] == '}') break;
        if (len == 0) { i = j + 1; continue; }

        // baris komentar ("// RVA: 0x..." / "// 0x...") -- di dump komentarnya
        // IKUT TERINDENTASI ("    // RVA: 0x..."), makanya spasi depannya dibuang
        size_t s = i;
        while (s < b && (p[s] == ' ' || p[s] == '\t')) ++s;
        if (b - s > 2 && p[s] == '/' && p[s + 1] == '/') {
            std::string line(p + s, b - s);
            size_t rv = line.find("RVA:");
            if (rv != std::string::npos) {
                size_t hex = line.find("0x", rv);
                if (hex != std::string::npos) {
                    pendingRvaVal = strtoull(line.c_str() + hex, nullptr, 16);
                    pendingHasRva = (pendingRvaVal != 0);
                    // module di belakang RVA (dump v3.4 / "RVA: 0x.. (lib.so)")
                    size_t op = line.find('(', hex);
                    size_t cl = (op == std::string::npos)
                                    ? std::string::npos : line.find(')', op);
                    if (cl != std::string::npos && cl > op + 1)
                        pendingMod = trim(line.substr(op + 1, cl - op - 1));
                    else
                        pendingMod.clear();
                } else {
                    pendingHasRva = false;
                    pendingRvaVal = 0;
                    pendingMod.clear();
                }
                pendingRva = true;
            }
            i = j + 1;
            continue;
        }

        // baris isi: harus indentasi (field/method selalu di dalam blok)
        if (s == i) { i = j + 1; continue; }
        std::string line = trim(std::string(p + s, b - s));

        size_t cm = line.find("; //");
        if (cm != std::string::npos) {
            // ---- FIELD
            std::string decl = trim(line.substr(0, cm));
            std::string meta = line.substr(cm + 3);

            Field f;
            size_t off = meta.find("offset: 0x");
            size_t tok = meta.find("token: 0x");
            if (off == std::string::npos) { i = j + 1; continue; }
            f.offset = (uint32_t)strtoul(meta.c_str() + off + 10, nullptr, 16);
            if (tok != std::string::npos)
                f.token = (uint32_t)strtoul(meta.c_str() + tok + 9, nullptr, 16);
            f.isStatic = meta.find("static_fields") != std::string::npos;

            const char* st = "static ";
            if (decl.compare(0, 7, st) == 0) {
                f.isStatic = true;
                decl = trim(decl.substr(7));
            }
            std::string type, name;
            if (splitLastWord(decl, type, name)) {
                f.type = type;
                f.name = name;
            } else {
                f.type = "";
                f.name = decl;
            }
            if (!f.name.empty()) out.fields.push_back(std::move(f));
            i = j + 1;
            continue;
        }

        if (line.size() > 2 && line.back() == ';') {
            // ---- METHOD
            Method m;
            m.name = trim(line.substr(0, line.size() - 1));
            if (pendingRva) {
                m.rva = pendingRvaVal;
                m.hasRva = pendingHasRva;
                m.module = pendingMod;
            }
            pendingRva = false;
            pendingHasRva = false;
            pendingRvaVal = 0;
            pendingMod.clear();
            if (!m.name.empty()) out.methods.push_back(std::move(m));
        }
        i = j + 1;
    }
    return !out.fields.empty() || !out.methods.empty();
}

// ---------------------------------------------------------------------------
// tipe: teks dump -> Il2CppTypeEnum (0xFF kalau nggak dikenal)
// ---------------------------------------------------------------------------
uint8_t guessTypeEnum(const std::string& type) {
    static const struct { const char* n; uint8_t v; } kMap[] = {
        {"bool", 0x02}, {"System.Boolean", 0x02},
        {"sbyte", 0x04}, {"System.SByte", 0x04},
        {"byte", 0x05},  {"System.Byte", 0x05},
        {"short", 0x06}, {"System.Int16", 0x06},
        {"ushort", 0x07},{"System.UInt16", 0x07},
        {"int", 0x08},   {"System.Int32", 0x08},
        {"uint", 0x09},  {"System.UInt32", 0x09},
        {"long", 0x0A},  {"System.Int64", 0x0A},
        {"ulong", 0x0B}, {"System.UInt64", 0x0B},
        {"float", 0x0C}, {"System.Single", 0x0C},
        {"double", 0x0D},{"System.Double", 0x0D},
        {"string", 0x0E}, {"System.String", 0x0E},
        {"System.IntPtr", 0x18}, {"System.UIntPtr", 0x19},
    };
    if (type.empty()) return 0xFF;
    for (const auto& m : kMap)
        if (type == m.n) return m.v;

    // pointer / array -> dibaca sebagai pointer
    if (type.find('*') != std::string::npos) return 0x0F;
    if (type.size() > 2 && type.compare(type.size() - 2, 2, "[]") == 0) return 0x1D;
    // generic & tipe reference lain: pointer ke objek
    if (type.find('`') != std::string::npos) return 0x11;
    if (type == "object" || type == "System.Object") return 0x11;
    // sisanya (UnityEngine.Vector3, enum, struct, class) nggak bisa ditebak
    // cuma dari nama; menu default-nya Int32, user bisa ganti.
    if (isupper((unsigned char)type[0]) || type.find('.') != std::string::npos) return 0x11;
    return 0xFF;
}

}  // namespace dump

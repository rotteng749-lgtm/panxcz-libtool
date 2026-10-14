// ============================================================================
//  patch.cpp -- lihat include/core/patch.h
// ============================================================================
#include "core/patch.h"

#include "core/abort.h"
#include "core/il2cpp.h"
#include "core/log.h"
#include "core/mem.h"
#include "core/target.h"

#include <cctype>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <algorithm>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include <sys/stat.h>

namespace patch {
namespace {

std::vector<Entry> g_list;
bool               g_enabled = true;
std::string        g_file;

// method yang "dibikin balikin nilai" (force return) -- lihat forceReturn().
// Ditaruh di sini karena tick() (yang ngecek drift) ada di atas blok force.
struct Forced {
    std::string label;
    std::string text;                 // nilai yang dipasang (buat ditampilin)
    std::string module;               // buat catatan di file (rva relatif module)
    uint64_t    rva  = 0;
    std::vector<uint8_t> stub;        // byte stub yang kita tulis
    std::vector<uint8_t> orig;        // byte asli sebelum ditimpa
};
std::map<uintptr_t, Forced> g_forced;
// catatan di file (dari run sebelumnya) -- dibaca buat --unforce all
struct FileForced {
    std::string module, label, text;
    uint64_t    rva = 0;
    std::vector<uint8_t> stub, orig;
};
std::vector<FileForced> g_forcedFile;
char               g_err[256] = {0};
double             g_lastTick = 0.0;
const double       kTickSec = 1.0;

void setErr(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(g_err, sizeof(g_err), fmt, ap);
    va_end(ap);
}

std::string trim(const std::string& s) {
    size_t b = 0, e = s.size();
    const size_t z = s.find('\0');      // nilai yang kebawa byte 0 dipotong
    if (z != std::string::npos) e = z;
    while (b < e && isspace((unsigned char)s[b])) ++b;
    while (e > b && isspace((unsigned char)s[e - 1])) --e;
    return s.substr(b, e - b);
}

// buang komentar: '#' atau "//"
void stripComment(std::string& s) {
    size_t h = s.find('#');
    if (h != std::string::npos) s = s.substr(0, h);
    size_t sl = s.find("//");
    if (sl != std::string::npos) s = s.substr(0, sl);
}

std::string lower(std::string s) {
    for (char& c : s) c = (char)tolower((unsigned char)c);
    return s;
}

// token-token hex: "C0 03 5F D6" / "C0,03,5FD6" -> {C0,03,5F,D6}
// KETAT: token yang bukan hex (mis. "??") = error, karena pola memang cuma
// boleh di sisi signature -- kalau nggak ketat, "??" bakal di-skip diam-diam
// dan byte-nya jadi salah (dulu begitu di UI).
bool parseHexBytes(const std::string& in, std::vector<uint8_t>& out, std::string* err) {
    out.clear();
    std::string cur;
    auto flush = [&](const std::string& t) -> bool {
        if (t.empty()) return true;
        if (t.size() > 2) return false;
        for (char c : t) if (!isxdigit((unsigned char)c)) return false;
        out.push_back((uint8_t)strtoul(t.c_str(), nullptr, 16));
        return true;
    };
    for (char c : in) {
        if (isxdigit((unsigned char)c)) { cur += c; continue; }
        if (c == ' ' || c == ',' || c == ':' || c == '\t' || c == '-') {
            if (!flush(cur)) break;
            cur.clear();
            continue;
        }
        if (err) *err = std::string("byte hex-nya nggak sah (ada '") + c + "')";
        out.clear();
        return false;
    }
    if (!flush(cur)) {
        if (err) *err = "token hex kelewat panjang (maks 2 digit per byte)";
        out.clear();
        return false;
    }
    return !out.empty();
}

uintptr_t moduleBase(const std::string& mod) {
    uintptr_t b = 0;
    if (mod.empty()) b = il2::base();
    else             b = mem::base(mod.c_str());
    return b ? mem::untag(b) : 0;
}

// modul yang dipakai buat ngecari pola: kalau kosong = lib il2cpp yang aktif
const std::string& moduleNameFor(const std::string& mod) {
    return mod.empty() ? il2::libName() : mod;
}

uintptr_t findSig(const std::string& mod, const std::string& sig) {
    const std::string& name = moduleNameFor(mod);
    for (const auto& r : mem::regions()) {
        if (!r.readable() || r.size() < 8) continue;
        if (!name.empty() && r.name.find(name) == std::string::npos) continue;
        uintptr_t hit = mem::findPatternIn(r, sig.c_str());
        if (hit) return hit;
    }
    return 0;
}

// Semua kemunculan pola (dibatasi `maxHits` + `maxMs` biar nggak kelamaan di
// region besar). Dipakai buat milih match yang BELUM ditimpa -- kalau nggak,
// tool jalan kedua kali bakal nempatch match berikutnya (byte yang udah
// ditimpa jadi nggak cocok sama polanya lagi).
void findSigAll(const std::string& mod, const std::string& sig,
                std::vector<uintptr_t>& out, size_t maxHits = 8,
                double maxSec = 3.0) {
    out.clear();
    const std::string& name = moduleNameFor(mod);
    const double t0 = abortx::nowSec();
    for (const auto& r : mem::regions()) {
        if (!r.readable() || r.size() < 8) continue;
        if (!name.empty() && r.name.find(name) == std::string::npos) continue;
        uintptr_t at = r.start;
        while (at < r.end && out.size() < maxHits) {
            uintptr_t hit = mem::findPattern(at, r.end, sig.c_str());
            if (!hit) break;
            out.push_back(hit);
            at = hit + 1;
            if (abortx::nowSec() - t0 > maxSec) break;
        }
    }
    // Urut alamat naik -> "match pertama" SELALU yang paling rendah, jadi
    // pilihannya deterministik antar-jalan (urutan region di maps bisa beda).
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
}

uintptr_t resolveAddr(Entry& e, bool logFail) {
    if (!e.sig.empty()) {
        std::vector<uintptr_t> hits;
        findSigAll(e.module, e.sig, hits);
        if (hits.empty()) {
            e.found = false;
            if (logFail)
                logx::add("[patch] pola nggak ketemu di %s: %s", moduleNameFor(e.module).c_str(),
                          e.sig.c_str());
            return 0;
        }
        e.found = true;
        // Kalau ADA match yang isinya UDAH sama dengan byte pengganti, berarti
        // patch-nya udah kepasang di situ -> pakai itu (idempotent: tool jalan
        // kedua kali nggak nempatch kemunculan lain dari pola yang sama).
        std::vector<uint8_t> cur(e.bytes.size(), 0);
        for (uintptr_t a : hits) {
            if (e.bytes.empty()) return a;
            if (!mem::read(a, cur.data(), cur.size())) continue;
            if (memcmp(cur.data(), e.bytes.data(), cur.size()) == 0) return a;
        }
        return hits.front();
    }
    if (e.absolute) return mem::untag(e.rva);
    // rva = file offset segmen (stabil antar run, benar buat modul yang
    // segmennya dimap terpisah seperti Free Fire). Kalau nggak ketemu,
    // fallback cara lama: base + rva.
    {
        const uintptr_t a = mem::addrOfRva(e.rva);
        if (a) return a;
    }
    const uintptr_t b = moduleBase(e.module);
    if (!b) {
        if (logFail)
            logx::add("[patch] module '%s' nggak ketemu di proses ini",
                      moduleNameFor(e.module).c_str());
        return 0;
    }
    return b + e.rva;
}

std::string hexOf(const std::vector<uint8_t>& v) {
    std::string o;
    char b[4];
    for (size_t i = 0; i < v.size(); ++i) {
        snprintf(b, sizeof(b), "%02X", v[i]);
        if (i) o += ' ';
        o += b;
    }
    return o;
}

// cari alamat entri ini (buat ditampilin di UI)
std::string entryAddrStr(const Entry& e) {
    if (!e.addr) return "-";
    char b[32];
    snprintf(b, sizeof(b), "0x%lx", (unsigned long)e.addr);
    return b;
}

void mkdirP(const char* path) {
    char tmp[512];
    snprintf(tmp, sizeof(tmp), "%s", path);
    for (char* p = tmp + 1; *p; ++p)
        if (*p == '/') { *p = 0; ::mkdir(tmp, 0777); *p = '/'; }
    ::mkdir(tmp, 0777);
}

}  // namespace

// ---------------------------------------------------------------------------
std::vector<Entry>& list() { return g_list; }
bool&               enabled() { return g_enabled; }
void                setEnabled(bool on) { g_enabled = on; }
const char*         lastError() { return g_err; }
const std::string&  loadedFrom() { return g_file; }

std::string summary() {
    size_t on = 0;
    for (const auto& e : g_list) if (e.on) ++on;
    char b[64];
    snprintf(b, sizeof(b), "%zu patch (%zu ON)", g_list.size(), on);
    return b;
}

// ---------------------------------------------------------------------------
// parser satu baris
//
//   libil2cpp.so:0x1234 = C0 03 5F D6     # ret
//   0x1234 = C0 03 5F D6                  (module = lib il2cpp utama)
//   abs:0x7A12A000 = 01 00 00 00          (alamat mutlak / data heap)
//   sig libil2cpp.so = 48 8B ?? E0 -> C0 03 5F D6
//   sig = 48 8B ?? E0 -> C0 03 5F D6      (cari di lib il2cpp utama)
// ---------------------------------------------------------------------------
bool addSpec(const std::string& rawLine, std::string* err) {
    std::string line = trim(rawLine);
    stripComment(line);
    line = trim(line);
    if (line.empty()) return false;

    const std::string low = lower(line);
    const bool isSig = low.rfind("sig", 0) == 0;

    // ---- bentuk signature: sig <module> = <pola> -> <byte> ----
    if (isSig) {
        std::string rest = trim(line.substr(3));
        const size_t arrow = rest.find("->");
        if (arrow == std::string::npos) {
            if (err) *err = "patch pola harus pakai '->' (pola -> byte pengganti)";
            return false;
        }
        std::string left  = rest.substr(0, arrow);
        std::string right = rest.substr(arrow + 2);
        const size_t eq = left.find('=');
        std::string mod, sig;
        if (eq == std::string::npos) {
            sig = trim(left);
        } else {
            mod = trim(left.substr(0, eq));
            sig = trim(left.substr(eq + 1));
        }
        if (mod == "sig") mod.clear();               // "sig sig = ..." (typo) -> lib utama
        return addSig(mod, sig, trim(right));
    }

    // ---- bentuk RVA / absolut: <target> = <byte> ----
    const size_t eq = line.find('=');
    if (eq == std::string::npos) {
        if (err) *err = "formatnya '<module>:<rva> = <byte hex>' (tanda '=' nggak ada)";
        return false;
    }
    const std::string target = trim(line.substr(0, eq));
    const std::string rhs    = trim(line.substr(eq + 1));
    if (target.empty() || rhs.empty()) {
        if (err) *err = "target / byte-nya kosong";
        return false;
    }

    Entry e;
    if (lower(target).rfind("abs:", 0) == 0) {
        e.absolute = true;
        e.rva = strtoull(trim(target.substr(4)).c_str(), nullptr, 0);
    } else {
        const size_t colon = target.rfind(':');
        if (colon == std::string::npos) {
            e.rva = strtoull(target.c_str(), nullptr, 0);
        } else {
            e.module = trim(target.substr(0, colon));
            e.rva    = strtoull(trim(target.substr(colon + 1)).c_str(), nullptr, 0);
        }
    }
    if (!e.rva) {
        if (err) *err = "rva/alamatnya 0 (kosong?)";
        return false;
    }

    std::string perr;
    if (!parseHexBytes(rhs, e.bytes, &perr)) {
        if (err) *err = perr.empty() ? "byte penggantinya kosong" : perr;
        return false;
    }
    if (e.bytes.size() > 256) {
        if (err) *err = "kepanjangan (maks 256 byte per patch)";
        return false;
    }

    char lbl[64];
    if (e.absolute) snprintf(lbl, sizeof(lbl), "abs:0x%llX", (unsigned long long)e.rva);
    else if (e.module.empty()) snprintf(lbl, sizeof(lbl), "0x%llX", (unsigned long long)e.rva);
    else snprintf(lbl, sizeof(lbl), "%s:0x%llX", e.module.c_str(), (unsigned long long)e.rva);

    // label eksplisit? ("... = C0 03 5F D6  nama bebas") -> simpan di belakang
    e.label = lbl;
    g_list.push_back(e);
    return true;
}

bool addSig(const std::string& module, const std::string& sig,
            const std::string& hex, const std::string& label) {
    if (trim(sig).empty()) { setErr("pola kosong"); return false; }
    Entry e;
    e.module = trim(module);
    if (e.module == "sig") e.module.clear();
    e.sig = trim(sig);
    std::string perr;
    if (!parseHexBytes(hex, e.bytes, &perr)) {
        setErr("%s", perr.empty() ? "byte penggantinya kosong" : perr.c_str());
        return false;
    }
    e.label = label.empty() ? (e.sig + "  ->  " + hexOf(e.bytes)) : label;
    g_list.push_back(e);
    return true;
}

uintptr_t findSigAddr(const std::string& module, const std::string& sig) {
    if (trim(sig).empty()) { setErr("pola kosong"); return 0; }
    return findSig(module, trim(sig));
}

// ---------------------------------------------------------------------------
bool applyEntry(size_t idx) {
    if (idx >= g_list.size()) return false;
    Entry& e = g_list[idx];
    if (e.bytes.empty()) { setErr("byte pengganti kosong"); return false; }

    const uintptr_t addr = resolveAddr(e, /*logFail=*/true);
    if (!addr) { setErr("%s", "alamat nggak ketemu (module/pola)"); return false; }
    e.addr = addr;

    std::vector<uint8_t> cur(e.bytes.size(), 0);
    if (!mem::read(addr, cur.data(), cur.size())) {
        setErr("gagal baca 0x%lx", (unsigned long)addr);
        logx::add("[patch] gagal BACA 0x%lx (%zu byte)", (unsigned long)addr, cur.size());
        return false;
    }
    // simpan byte asli SEKALI (sebelum pertama kali ditimpa)
    if (e.orig.size() != e.bytes.size()) e.orig = cur;

    const bool already = (memcmp(cur.data(), e.bytes.data(), cur.size()) == 0);
    if (!already && !mem::write(addr, e.bytes.data(), e.bytes.size())) {
        setErr("gagal tulis 0x%lx (izin?)", (unsigned long)addr);
        logx::add("[patch] gagal TULIS 0x%lx (%zu byte) -- region nggak bisa ditulis?",
                  (unsigned long)addr, e.bytes.size());
        return false;
    }

    // verifikasi: baca ulang, harus sama persis
    std::vector<uint8_t> back(e.bytes.size(), 0);
    const bool ok = mem::read(addr, back.data(), back.size()) &&
                    memcmp(back.data(), e.bytes.data(), back.size()) == 0;
    e.on       = true;
    e.verified = ok;
    if (ok) {
        logx::add("[patch] %s%s @ %s (%zu byte) [%s]", already ? "sudah" : "apply",
                  already ? "" : "", entryAddrStr(e).c_str(), e.bytes.size(),
                  e.label.c_str());
    } else {
        logx::add("[patch] TULISAN DITOLAK/KETIMPA @ %s -- isinya masih: %s",
                  entryAddrStr(e).c_str(), hexOf(back).c_str());
    }
    return ok;
}

bool setOn(size_t idx, bool on) {
    if (idx >= g_list.size()) return false;
    Entry& e = g_list[idx];
    if (on) return applyEntry(idx);

    // restore: balikin byte asli
    if (e.bytes.empty()) return false;
    const uintptr_t addr = resolveAddr(e, true);
    if (!addr) return false;
    if (e.orig.size() != e.bytes.size()) {      // belum pernah apply -> apa-apa
        e.on = false;
        return true;
    }
    if (!mem::write(addr, e.orig.data(), e.orig.size())) {
        setErr("gagal restore 0x%lx", (unsigned long)addr);
        return false;
    }
    std::vector<uint8_t> back(e.orig.size(), 0);
    const bool ok = mem::read(addr, back.data(), back.size()) &&
                    memcmp(back.data(), e.orig.data(), back.size()) == 0;
    e.on = false;
    logx::add("[patch] restore @ %s (%zu byte)%s", entryAddrStr(e).c_str(), e.orig.size(),
              ok ? "" : " -- GAGAL (masih ketimpa)");
    return ok;
}

bool applyAll() {
    size_t ok = 0, fail = 0;
    for (size_t i = 0; i < g_list.size(); ++i) {
        if (g_list[i].bytes.empty()) continue;
        if (applyEntry(i)) ++ok; else ++fail;
    }
    logx::add("[patch] apply semua: %zu sukses, %zu gagal (%zu entri)", ok, fail, g_list.size());
    return ok > 0 || g_list.empty();
}

bool restoreAll() {
    size_t ok = 0;
    for (size_t i = 0; i < g_list.size(); ++i)
        if (g_list[i].on && setOn(i, false)) ++ok;
    logx::add("[patch] restore semua: %zu dibalikin", ok);
    return true;
}

void removeAt(size_t idx) {
    if (idx >= g_list.size()) return;
    if (g_list[idx].on) setOn(idx, false);
    g_list.erase(g_list.begin() + (long)idx);
}

void clear() {
    restoreAll();
    g_list.clear();
}

// Drift: game sering nulis ulang kodenya (page di-COW ulang, atau lib di-reload
// waktu masuk match). Tiap detik cek entri yang ON: kalau isinya udah beda,
// pasang ulang. Ini yang bikin patch "nempel" walau game-nya ngereset memori.
void tick() {
    // method yang "dibikin balikin nilai" juga dicek: kalau halaman kodenya
    // ditulis ulang game (lib di-reload pas masuk match), stub-nya dipasang lagi.
    for (auto& kv : g_forced) {
        if (kv.second.stub.empty()) continue;
        std::vector<uint8_t> cur(kv.second.stub.size(), 0);
        if (!mem::read(kv.first, cur.data(), cur.size())) continue;
        if (memcmp(cur.data(), kv.second.stub.data(), cur.size()) == 0) continue;
        if (mem::write(kv.first, kv.second.stub.data(), kv.second.stub.size()))
            logx::add("[force] dipasang ulang @ 0x%lx (%s) -- kodenya ke-reset game",
                      (unsigned long)kv.first, kv.second.label.c_str());
    }
    if (g_list.empty()) return;
    const double now = abortx::nowSec();
    if (now - g_lastTick < kTickSec) return;
    g_lastTick = now;

    for (size_t i = 0; i < g_list.size(); ++i) {
        Entry& e = g_list[i];
        if (!e.on || e.bytes.empty()) continue;
        std::vector<uint8_t> cur(e.bytes.size(), 0);
        // Baca di alamat terakhir. Kalau nggak kebaca (lib di-reload / alamat
        // geser), baru resolve ulang -- resolusi pola itu mahal, jangan tiap detik.
        bool got = e.addr && mem::read(e.addr, cur.data(), cur.size());
        if (!got) {
            const uintptr_t a = resolveAddr(e, false);
            if (!a) continue;
            e.addr = a;
            got = mem::read(a, cur.data(), cur.size());
            if (!got) continue;
        }
        const uintptr_t addr = e.addr;
        if (memcmp(cur.data(), e.bytes.data(), cur.size()) == 0) { e.verified = true; continue; }
        // Kalau byte di alamat itu justru SAMA dengan byte yang diminta entri
        // lain yang juga ON, berarti keadaannya memang disengaja (dua entri di
        // alamat yang sama) -- jangan pasang ulang, nanti saling nimpuk terus.
        bool ownedByOther = false;
        for (size_t j = 0; j < g_list.size() && !ownedByOther; ++j) {
            if (j == i || !g_list[j].on) continue;
            if (g_list[j].addr != addr) continue;
            if (g_list[j].bytes.size() != cur.size()) continue;
            if (memcmp(g_list[j].bytes.data(), cur.data(), cur.size()) == 0)
                ownedByOther = true;
        }
        if (ownedByOther) { e.verified = true; continue; }
        ++e.reapplies;
        logx::add("[patch] byte-nya ketimpa balik @ 0x%lx -- pasang ulang (#%d)",
                  (unsigned long)addr, e.reapplies);
        applyEntry(i);
    }
}

// ---------------------------------------------------------------------------
std::string defaultPath() {
    std::string root = target::dumpDir();          // .../panxcz-tools/dumper
    while (!root.empty() && root.back() == '/') root.pop_back();
    const size_t slash = root.rfind('/');
    if (slash != std::string::npos) root = root.substr(0, slash);
    std::string pkg = target::package();
    if (pkg.empty()) pkg = "default";
    return root + "/patch/" + pkg + ".txt";
}

bool saveFile(const std::string& pathIn) {
    const std::string path = pathIn.empty() ? defaultPath() : pathIn;
    const size_t slash = path.rfind('/');
    if (slash != std::string::npos) mkdirP(path.substr(0, slash).c_str());

    std::ofstream f(path, std::ios::trunc);
    if (!f.is_open()) { setErr("gagal nulis %s", path.c_str()); return false; }
    f << "# panxcz patch -- " << target::package() << "\n";
    f << "# format:\n";
    f << "#   <module>:<rva> = <byte hex>       contoh: libil2cpp.so:0x1A2B3C = C0 03 5F D6\n";
    f << "#   abs:<alamat>   = <byte hex>       contoh: abs:0x7A12A000 = 01 00 00 00\n";
    f << "#   sig <module>   = <pola> -> <byte> contoh: sig libil2cpp.so = 48 8B ?? E0 -> C0 03 5F D6\n";
    f << "# (baris ini dibaca OTOMATIS tiap tool jalan -- mode otomatis meng-apply-nya)\n";
    for (const auto& e : g_list) {
        if (e.bytes.empty()) continue;
        if (!e.sig.empty()) {
            f << "sig " << e.module << " = " << e.sig << " -> " << hexOf(e.bytes);
        } else if (e.absolute) {
            f << "abs:0x" << std::hex << std::uppercase << e.rva << std::dec;
            f << " = " << hexOf(e.bytes);
        } else {
            if (!e.module.empty()) f << e.module << ":";
            f << "0x" << std::hex << std::uppercase << e.rva << std::dec;
            f << " = " << hexOf(e.bytes);
        }
        f << "   # " << e.label << (e.on ? "  [ON]" : "  [OFF]") << "\n";
    }
    f.close();
    g_file = path;
    logx::add("[patch] daftar disimpan: %s (%zu entri)", path.c_str(), g_list.size());
    return true;
}

bool loadFile(const std::string& pathIn, bool applyNow) {
    const std::string path = pathIn.empty() ? defaultPath() : pathIn;
    std::ifstream f(path);
    if (!f.is_open()) { setErr("nggak ada file %s", path.c_str()); return false; }

    // daftar lama dibalikin dulu biar nggak numpuk
    restoreAll();
    g_list.clear();

    std::string line;
    int ok = 0, bad = 0, lineno = 0;
    while (std::getline(f, line)) {
        ++lineno;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        std::string probe = line;
        stripComment(probe);
        if (trim(probe).empty()) continue;
        std::string err;
        if (addSpec(line, &err)) {
            ++ok;
            // label = isi komentarnya (biar enak dibaca di UI)
            const size_t h = line.find('#');
            if (h != std::string::npos && !g_list.empty()) {
                std::string lbl = trim(line.substr(h + 1));
                // buang penanda [ON]/[OFF] yang ditulis saveFile
                const size_t bar = lbl.find('[');
                if (bar != std::string::npos) lbl = trim(lbl.substr(0, bar));
                if (!lbl.empty()) g_list.back().label = lbl;
            }
        } else {
            ++bad;
            logx::add("[patch] baris %d dilewat: %s", lineno, err.c_str());
        }
    }
    f.close();
    g_file = path;
    logx::add("[patch] muat %s: %d entri%s", path.c_str(), ok,
              bad ? " (ada yang dilewat -- lihat log)" : "");
    if (ok == 0) { setErr("nggak ada entri sah di %s", path.c_str()); return false; }
    if (applyNow) applyAll();
    return true;
}

// ============================================================================
//  FORCE RETURN VALUE -- lihat include/core/patch.h
//
//  Encodings AArch64-nya ditulis manual (nggak butuh assembler pihak ketiga):
//
//    ret                    = 0xD65F03C0
//    movz Xd, #imm16, lsl s = 0xD2800000 | (imm<<5) | ((s/16)<<21)
//    movk Xd, #imm16, lsl s = 0xF2800000 | ...
//    movz Wd, #imm16, lsl s = 0x52800000 | ...
//    movk Wd, #imm16, lsl s = 0x72800000 | ...
//    fmov Sd, Wn            = 0x1E270000 | (n<<5) | d     (s0 <- w0)
//    fmov Dd, Xn            = 0x9E670000 | (n<<5) | d     (d0 <- x0)
//
//  Semua sudah dicek terhadap hasil asli assembler aarch64 (lihat README).
// ============================================================================
namespace {

uint32_t movzX(int reg, uint16_t imm, int shift) {
    return 0xD2800000u | ((uint32_t)imm << 5) | ((uint32_t)(shift / 16) << 21) | (uint32_t)reg;
}
uint32_t movkX(int reg, uint16_t imm, int shift) {
    return 0xF2800000u | ((uint32_t)imm << 5) | ((uint32_t)(shift / 16) << 21) | (uint32_t)reg;
}
uint32_t movzW(int reg, uint16_t imm, int shift) {
    return 0x52800000u | ((uint32_t)imm << 5) | ((uint32_t)(shift / 16) << 21) | (uint32_t)reg;
}
uint32_t movkW(int reg, uint16_t imm, int shift) {
    return 0x72800000u | ((uint32_t)imm << 5) | ((uint32_t)(shift / 16) << 21) | (uint32_t)reg;
}

void put32(std::vector<uint8_t>& v, uint32_t w) {
    v.push_back((uint8_t)(w & 0xFF));
    v.push_back((uint8_t)((w >> 8) & 0xFF));
    v.push_back((uint8_t)((w >> 16) & 0xFF));
    v.push_back((uint8_t)((w >> 24) & 0xFF));
}
inline uint32_t kRet = 0xD65F03C0u;

const char* patchRetName(Ret r) {
    switch (r) {
        case Ret::Void:   return "void";
        case Ret::Bool:   return "bool";
        case Ret::I16:    return "int16";
        case Ret::U16:    return "uint16";
        case Ret::I32:    return "int32";
        case Ret::U32:    return "uint32";
        case Ret::I64:    return "int64";
        case Ret::U64:    return "uint64";
        case Ret::Float:  return "float";
        case Ret::Double: return "double";
        case Ret::Ptr:    return "object/string";
    }
    return "?";
}

}  // namespace

const char* retName(Ret r) { return patchRetName(r); }

bool retFromName(const std::string& in, Ret& out) {
    std::string s = lower(in);
    while (!s.empty() && (s[0] == ' ' || s[0] == '\t')) s.erase(0, 1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) s.pop_back();
    if (s == "void" || s == "nop" || s == "-") { out = Ret::Void; return true; }
    if (s == "bool" || s == "boolean")           { out = Ret::Bool; return true; }
    if (s == "i16" || s == "int16" || s == "short")        { out = Ret::I16; return true; }
    if (s == "u16" || s == "uint16" || s == "ushort")      { out = Ret::U16; return true; }
    if (s == "i32" || s == "int32" || s == "int")          { out = Ret::I32; return true; }
    if (s == "u32" || s == "uint32" || s == "uint")        { out = Ret::U32; return true; }
    if (s == "i64" || s == "int64" || s == "long")         { out = Ret::I64; return true; }
    if (s == "u64" || s == "uint64" || s == "ulong")       { out = Ret::U64; return true; }
    if (s == "f32" || s == "float" || s == "single")       { out = Ret::Float; return true; }
    if (s == "f64" || s == "double")                       { out = Ret::Double; return true; }
    if (s == "ptr" || s == "object" || s == "string")      { out = Ret::Ptr; return true; }
    return false;
}

bool buildReturn(Ret r, double num, uint64_t ptr, std::vector<uint8_t>& out,
                 std::string* err) {
    out.clear();
    const uint64_t iv = (uint64_t)(int64_t)num;
    switch (r) {
        case Ret::Void:
            put32(out, kRet);
            break;
        case Ret::Bool:
            put32(out, movzW(0, (uint16_t)(num != 0.0 ? 1 : 0), 0));
            put32(out, kRet);
            break;
        case Ret::I16:
        case Ret::U16:
        case Ret::I32:
        case Ret::U32:
            put32(out, movzW(0, (uint16_t)(iv & 0xFFFF), 0));
            if (iv >> 16) put32(out, movkW(0, (uint16_t)((iv >> 16) & 0xFFFF), 16));
            put32(out, kRet);
            break;
        case Ret::I64:
        case Ret::U64:
            put32(out, movzX(0, (uint16_t)(iv & 0xFFFF), 0));
            if (iv >> 16) put32(out, movkX(0, (uint16_t)((iv >> 16) & 0xFFFF), 16));
            if (iv >> 32) put32(out, movkX(0, (uint16_t)((iv >> 32) & 0xFFFF), 32));
            if (iv >> 48) put32(out, movkX(0, (uint16_t)((iv >> 48) & 0xFFFF), 48));
            put32(out, kRet);
            break;
        case Ret::Float: {
            float f = (float)num;
            uint32_t bits;
            memcpy(&bits, &f, 4);
            put32(out, movzW(0, (uint16_t)(bits & 0xFFFF), 0));
            if (bits >> 16) put32(out, movkW(0, (uint16_t)((bits >> 16) & 0xFFFF), 16));
            put32(out, 0x1E270000u);          // fmov s0, w0
            put32(out, kRet);
            break;
        }
        case Ret::Double: {
            uint64_t bits;
            memcpy(&bits, &num, 8);
            put32(out, movzX(0, (uint16_t)(bits & 0xFFFF), 0));
            if (bits >> 16) put32(out, movkX(0, (uint16_t)((bits >> 16) & 0xFFFF), 16));
            if (bits >> 32) put32(out, movkX(0, (uint16_t)((bits >> 32) & 0xFFFF), 32));
            if (bits >> 48) put32(out, movkX(0, (uint16_t)((bits >> 48) & 0xFFFF), 48));
            put32(out, 0x9E670000u);          // fmov d0, x0
            put32(out, kRet);
            break;
        }
        case Ret::Ptr:
            if (!ptr) {
                put32(out, movzX(0, 0, 0));   // mov x0, #0
                put32(out, kRet);
                break;
            }
            put32(out, movzX(0, (uint16_t)(ptr & 0xFFFF), 0));
            put32(out, movkX(0, (uint16_t)((ptr >> 16) & 0xFFFF), 16));
            put32(out, movkX(0, (uint16_t)((ptr >> 32) & 0xFFFF), 32));
            put32(out, movkX(0, (uint16_t)((ptr >> 48) & 0xFFFF), 48));
            put32(out, kRet);
            break;
        default:
            if (err) *err = "tipe nggak didukung";
            return false;
    }
    if (out.size() > 32) { out.clear(); if (err) *err = "stub kelamaan"; return false; }
    return true;
}

bool forceReturn(uintptr_t addr, Ret r, double num, uint64_t ptr, const std::string& label,
                 const std::string& module, uint64_t rva, std::string* err) {
    addr = mem::untag(addr);
    if (!addr) { if (err) *err = "alamat kosong"; return false; }
    std::vector<uint8_t> stub;
    if (!buildReturn(r, num, ptr, stub, err)) return false;

    std::vector<uint8_t> orig(stub.size(), 0);
    if (!mem::read(addr, orig.data(), orig.size())) {
        if (err) *err = "kode methodnya nggak kebaca";
        logx::add("[force] nggak bisa baca kode @ 0x%lx", (unsigned long)addr);
        return false;
    }
    auto it = g_forced.find(addr);
    if (it == g_forced.end()) {
        Forced f;
        f.orig = orig;                    // byte asli direkam SEKALI
        g_forced[addr] = std::move(f);
        it = g_forced.find(addr);
    }
    if (!mem::write(addr, stub.data(), stub.size())) {
        if (err) *err = "nggak bisa nulis ke halaman kode (izin?)";
        logx::add("[force] GAGAL tulis 0x%lx (%zu byte) -- halaman kode nggak bisa ditulis",
                  (unsigned long)addr, stub.size());
        return false;
    }
    std::vector<uint8_t> back(stub.size(), 0);
    const bool ok = mem::read(addr, back.data(), back.size()) &&
                    memcmp(back.data(), stub.data(), back.size()) == 0;

    char text[96];
    if (r == Ret::Ptr)
        snprintf(text, sizeof(text), "%s (0x%llx)", patchRetName(r), (unsigned long long)ptr);
    else if (r == Ret::Float || r == Ret::Double)
        snprintf(text, sizeof(text), "%s (%g)", patchRetName(r), num);
    else if (r == Ret::Bool)
        snprintf(text, sizeof(text), "bool (%s)", num != 0.0 ? "true" : "false");
    else
        snprintf(text, sizeof(text), "%s (%lld)", patchRetName(r), (long long)num);

    it->second.stub   = stub;
    it->second.text   = text;
    it->second.label  = label;
    it->second.module = module;
    it->second.rva    = rva;
    saveForced();
    logx::add("[force] %s @ 0x%lx <- %s (%zu byte, %s) -- stub: %s",
              label.empty() ? "method" : label.c_str(), (unsigned long)addr, text, stub.size(),
              ok ? "terpasang" : "TULISAN DITOLAK/KETIMPA", hexOf(back).c_str());
    if (!ok && err) *err = "tulisan ditolak (byte-nya nggak berubah)";
    return ok;
}

bool unforceReturn(uintptr_t addr) {
    addr = mem::untag(addr);
    auto it = g_forced.find(addr);
    if (it == g_forced.end()) return false;
    if (!it->second.orig.empty() &&
        !mem::write(addr, it->second.orig.data(), it->second.orig.size())) {
        logx::add("[force] restore GAGAL @ 0x%lx", (unsigned long)addr);
        return false;
    }
    std::vector<uint8_t> back(it->second.orig.size(), 0);
    const bool ok = mem::read(addr, back.data(), back.size()) &&
                    memcmp(back.data(), it->second.orig.data(), back.size()) == 0;
    logx::add("[force] restore @ 0x%lx%s", (unsigned long)addr, ok ? "" : " -- masih ketimpa");
    g_forced.erase(it);
    saveForced();
    return ok;
}

bool isForced(uintptr_t addr, std::string* text) {
    auto it = g_forced.find(mem::untag(addr));
    if (it == g_forced.end()) return false;
    if (text) *text = it->second.text;
    return true;
}

void unforceAll() {
    std::vector<uintptr_t> keys;
    for (const auto& kv : g_forced) keys.push_back(kv.first);
    for (uintptr_t a : keys) unforceReturn(a);
}

size_t forcedCount() { return g_forced.size(); }

std::string forcedSummary() {
    if (g_forced.empty()) return "belum ada method yang dibikin balikin nilai";
    std::string s = std::to_string(g_forced.size()) + " method dibikin balikin nilai:";
    size_t shown = 0;
    for (const auto& kv : g_forced) {
        s += "\n  " + (kv.second.label.empty() ? std::string("?") : kv.second.label) +
             " -> " + kv.second.text;
        if (++shown >= 8) { s += "\n  ..."; break; }
    }
    return s;
}

// ---------------------------------------------------------------------------
//  catatan force-return ke disk
//
//  Kenapa perlu: alamat absolut nggak bisa dipakai lagi abis tool-nya keluar
//  (ASLR), tapi (module, rva) bisa. Jadi yang disimpan itu rva + byte stub +
//  byte ASLI, biar `--unforce all` masih bisa ngembaliin walau tool-nya baru
//  jalan lagi (atau HP-nya sempat nge-restart tool-nya).
// ---------------------------------------------------------------------------
bool restoreFromLib(const std::string& module, uint64_t rva, size_t len, std::string* err) {
    if (module.empty() || !rva || !len) { if (err) *err = "module/rva kosong"; return false; }
    // path dari /proc/<pid>/maps suka ada padding spasi di belakang
    std::string disk = mem::path(module.c_str());
    while (!disk.empty() && (disk.back() == ' ' || disk.back() == '\t' || disk.back() == '\n')) disk.pop_back();
    while (!disk.empty() && (disk.front() == ' ' || disk.front() == '\t')) disk.erase(0, 1);
    if (disk.empty()) { if (err) *err = ("path " + module + " nggak ada di maps").c_str();
                        logx::add("[force] %s nggak ada di maps -- nggak bisa baca versi disk", module.c_str());
                        return false; }
    std::ifstream f(disk, std::ios::binary);
    if (!f.is_open()) { if (err) *err = "gagal buka " + disk; return false; }
    std::vector<uint8_t> orig(len, 0);
    f.seekg((std::streamoff)rva, std::ios::beg);
    if (!f.read((char*)orig.data(), (std::streamsize)len)) {
        if (err) *err = "file-nya lebih pendek dari rva";
        return false;
    }
    f.close();
    const uintptr_t addr = mem::addrOfRvaIn(module.c_str(), rva);
    if (!addr) { if (err) *err = "rva itu nggak ke-map (lib-nya belum ke-load)"; return false; }
    if (!mem::write(addr, orig.data(), orig.size())) {
        if (err) *err = "gagal nulis ke memori";
        return false;
    }
    std::vector<uint8_t> back(orig.size(), 0);
    const bool ok = mem::read(addr, back.data(), back.size()) &&
                    memcmp(back.data(), orig.data(), back.size()) == 0;
    logx::add("[force] %s:0x%llx (%zu byte) dibalikin dari %s @0x%lx -- %s",
              module.c_str(), (unsigned long long)rva, len, disk.c_str(),
              (unsigned long)addr, ok ? "cocok" : "MASIH BEDA");
    if (!ok && err) *err = "tulisan ditolak";
    return ok;
}

std::string forcePath() {
    std::string p = defaultPath();                 // .../patch/<pkg>.txt
    const size_t dot = p.rfind(".txt");
    if (dot == std::string::npos) return p + "_forceret.txt";
    return p.substr(0, dot) + "_forceret.txt";
}

static std::string bytesHex(const std::vector<uint8_t>& v) {
    std::string s;
    char b[4];
    for (uint8_t c : v) { snprintf(b, sizeof(b), "%02X", c); s += b; }
    return s;
}

bool saveForced() {
    const std::string path = forcePath();
    if (g_forced.empty()) {
        remove(path.c_str());
        return true;
    }
    const size_t dirSlash = path.rfind('/');
    if (dirSlash != std::string::npos) mkdirP(path.substr(0, dirSlash).c_str());
    std::ofstream f(path, std::ios::trunc);
    if (!f.is_open()) return false;
    f << "# panxcz_libtool force-return\n"
         "# <module>:<rva> | <stub> | <byte asli> | <label> | <nilai>\n";
    for (const auto& kv : g_forced) {
        const Forced& g = kv.second;
        if (g.stub.empty() || g.orig.empty()) continue;
        char head[64];
        snprintf(head, sizeof(head), "%s:0x%llx",
                 g.module.empty() ? "?" : g.module.c_str(),
                 (unsigned long long)g.rva);
        f << head << " | " << hexOf(g.stub) << " | " << hexOf(g.orig) << " | "
          << g.label << " | " << g.text << "\n";
    }
    f.close();
    return true;
}

static size_t readForcedFile() {
    g_forcedFile.clear();
    std::ifstream f(forcePath());
    if (!f.is_open()) return 0;
    std::string line;
    while (std::getline(f, line)) {
        if (line.empty() || line[0] == '#') continue;
        // <module>:<rva> | <stub> | <orig> | <label> | <nilai>
        std::vector<std::string> parts;
        size_t pos = 0;
        while (true) {
            const size_t bar = line.find(" | ", pos);
            if (bar == std::string::npos) { parts.push_back(line.substr(pos)); break; }
            parts.push_back(line.substr(pos, bar - pos));
            pos = bar + 3;
        }
        if (parts.size() < 4) continue;
        const std::string& loc = parts[0];
        const size_t colon = loc.find(':');
        if (colon == std::string::npos) continue;
        FileForced e;
        e.module = loc.substr(0, colon);
        e.rva    = strtoull(loc.c_str() + colon + 1, nullptr, 16);
        if (!parseHexBytes(parts[1], e.stub, nullptr) ||
            !parseHexBytes(parts[2], e.orig, nullptr)) continue;
        e.label = parts[3];
        e.text  = parts.size() > 4 ? parts[4] : std::string();
        if (e.stub.empty() || e.orig.empty()) continue;
        if (e.module == "?") e.module.clear();
        g_forcedFile.push_back(std::move(e));
    }
    f.close();
    return g_forcedFile.size();
}

static uintptr_t addrOfForced(const FileForced& e) {
    if (!e.rva) return 0;
    if (!e.module.empty()) {
        const uintptr_t a = mem::addrOfRvaIn(e.module.c_str(), e.rva);
        if (a) return a;
    }
    // module nggak kecatat / nggak ke-load -> coba tafsir umum, terus lib il2cpp
    uintptr_t a = mem::addrOfRva(e.rva);
    if (a) return a;
    const uintptr_t base = il2::base();
    return base ? base + e.rva : 0;
}

size_t fileForcedCount() {
    if (g_forcedFile.empty()) readForcedFile();
    return g_forcedFile.size();
}

size_t restoreFromFile(const std::string& like, const char* why) {
    if (g_forcedFile.empty()) readForcedFile();
    if (g_forcedFile.empty()) return 0;
    size_t done = 0, keep = 0;
    std::vector<FileForced> rest;
    for (const auto& e : g_forcedFile) {
        const bool match = like.empty() || e.label == like ||
                           e.label.find(like) != std::string::npos;
        if (!match) { rest.push_back(e); continue; }
        const uintptr_t a = addrOfForced(e);
        if (!a) {
            logx::add("[force] %s: alamat rva 0x%llx nggak ketemu di proses ini -- dibiarkan di catatan",
                      e.label.c_str(), (unsigned long long)e.rva);
            rest.push_back(e);
            continue;
        }
        if (!mem::write(a, e.orig.data(), e.orig.size())) {
            logx::add("[force] %s: restore GAGAL tulis @ 0x%lx", e.label.c_str(), (unsigned long)a);
            rest.push_back(e);
            continue;
        }
        std::vector<uint8_t> back(e.orig.size(), 0);
        const bool ok = mem::read(a, back.data(), back.size()) &&
                        memcmp(back.data(), e.orig.data(), back.size()) == 0;
        logx::add("[force] restore %s @ 0x%lx%s (%s)", e.label.c_str(), (unsigned long)a,
                  ok ? "" : " -- masih ketimpa", why ? why : "dari catatan");
        if (ok) ++done; else rest.push_back(e);
    }
    if (done) {
        g_forcedFile.swap(rest);
        // tulis ulang file (kalau kosong, filenya dibuang)
        const std::string path = forcePath();
        if (g_forcedFile.empty()) { remove(path.c_str()); }
        else {
            std::ofstream f(path, std::ios::trunc);
            if (f.is_open()) {
                f << "# panxcz_libtool force-return\n"
                     "# <module>:<rva> | <stub> | <byte asli> | <label> | <nilai>\n";
                for (const auto& e : g_forcedFile) {
                    char head[64];
                    snprintf(head, sizeof(head), "%s:0x%llx",
                             e.module.empty() ? "?" : e.module.c_str(),
                             (unsigned long long)e.rva);
                    f << head << " | " << hexOf(e.stub) << " | " << hexOf(e.orig) << " | "
                      << e.label << " | " << e.text << "\n";
                }
            }
        }
        g_forcedFile.clear();
    }
    return done;
}

size_t reapplyFromFile() {
    if (g_forcedFile.empty()) readForcedFile();
    size_t done = 0;
    for (const auto& e : g_forcedFile) {
        const uintptr_t a = addrOfForced(e);
        if (!a) continue;
        if (mem::write(a, e.stub.data(), e.stub.size())) {
            ++done;
            logx::add("[force] pasang ulang %s @ 0x%lx <- %s", e.label.c_str(),
                      (unsigned long)a, e.text.c_str());
        }
    }
    if (done) logx::add("[force] %zu method tercatat dipasang ulang", done);
    return done;
}

bool autoApply() {
    if (!g_enabled) { logx::add("[patch] auto-apply dimatikan (--no-patch-auto)"); return false; }
    const std::string path = defaultPath();
    std::ifstream probe(path);
    if (!probe.is_open()) {
        logx::add("[patch] belum ada file patch buat %s (nggak apa-apa: %s)",
                  target::package().c_str(), path.c_str());
        return false;
    }
    probe.close();
    logx::add("[patch] auto-apply: %s", path.c_str());
    return loadFile(path, /*applyNow=*/true);
}

}  // namespace patch

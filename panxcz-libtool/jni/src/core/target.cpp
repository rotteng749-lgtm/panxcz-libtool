#include "core/target.h"

#include <cctype>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>

#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <cstdlib>
#include <ctime>

#include "core/il2cpp.h"
#include "core/log.h"
#include "core/mem.h"

namespace target {

static std::string g_pkg;
static std::string g_lib;          // "" = auto
static std::string g_dump;         // "" = cari sendiri
static std::string g_dumpDir;      // root folder dumper (default di bawah)
// Proses ENGINE yang beneran dipegang (nama cmdline lengkap). Unity sering
// naruh il2cpp di PROSES ANAK, bukan proses utama -- MLBB:
//   com.mobile.legends              (cuma shell/loader, nggak ada il2cpp)
//   com.mobile.legends:UnityKillsMe (di sinilah libcsharp.so/liblogic.so)
// Jadi setelah ketemu, nama itu DISIMPAN ke cfg -> jalan berikutnya langsung
// nempel ke proses yang bener (nggak nebak-nebak lagi).
static std::string g_engine;

// Nama root LAWAAS (v1.0..v1.7x). Dulu hasil dump ditulis ke sini; sekarang
// yang dipakai <root>/dumper/<pkg>/<versi>. Nilai lama di cfg jangan sampai
// nge-nimpa default baru -- jadi dicek di load().
static bool isLegacyDumpRoot(const std::string& d) {
    return d == "/sdcard/panxcz-tools" || d == "/sdcard/panxcz-tool" ||
           d == "/sdcard/panxcz-tool/dumper" || d == "/sdcard/panxcz";
}
static int         g_pidSel = 0;   // 0 = auto (cari pid sendiri)
static const char* kCfgTmp  = "/data/local/tmp/panxcz_libtool.cfg";
static const char* kCfgSd   = "/sdcard/panxcz/libtool.cfg";
static std::string g_cfgUsed = "-";

// Folder dumper: konvensinya /sdcard/panxcz-tools/dumper/<pkg>/<version>/*.cs.
// (Nama folder diambil dari package TANPA sub-proses, jadi
//  com.mobile.legends:UnityKillsMe -> .../dumper/com.mobile.legends/...)
// Ini juga folder default HASIL DUMP TOOL INI (dumpout) -- bisa diganti dari
// menu Setting atau argumen --outdir / cfg `dumpdir=`.
static const char* kDumpRoot = "/sdcard/panxcz-tools/dumper";
// Root lama (v3.5 ke bawah, hasil `tools/frida/run_dump.py`): tetap dicari
// biar dump yang udah ada nggak perlu dipindah.
static const char* kDumpRoot2 = "/sdcard/panxcz-tool/dumper";
// Root ketiga: layout lama run_dump.py (langsung <root>/<pkg>/<versi>/).
static const char* kDumpRoot3 = "/sdcard/panxcz-tools";

// lib yang paling sering dipakai game Unity/il2cpp, urut prioritas.
// MLBB itu KASUS KHUSUS: metadata-nya dipecah tiga lib (liblogic.so =
// mscorlib/System/UnityEngine, libfirst.so = Assembly-CSharp-firstpass,
// libcsharp.so = Assembly-CSharp). libil2cpp.so yang ada di APK-nya cuma
// stub 384 KB, jadi jangan sampai kepilih cuma karena namanya cocok.
static const char* kKnownLibs[] = {
    "libil2cpp.so", "liblogic.so", "libfirst.so", "libcsharp.so",
    "libunity.so", "libgame.so", "libmain.so",
    "libmonobdwgc-2.0.so", "libnative.so", "libUE4.so",
};

// lib anti-cheat / proteksi. Ini BUKAN engine: lib-nya ada di proses game
// walaupun game-nya cuma jalan di background (belum ada il2cpp sama sekali),
// dan dia ikut kelihatan "kayak il2cpp" karena nge-hook API il2cpp.
// Kalau ini yang kepilih, tool bakal nge-scan memori ANTI-CHEAT-nya sendiri --
// selain hasilnya ngawur, itu justru yang bikin game curiga. Jadi jangan
// pernah dipilih; kalau cuma ini yang ada, tunggu sampai engine beneran load.
static const char* kAntiCheatLibs[] = {
    "libanogs.so", "libanort.so", "libanogo.so", "libanogs2.so",
    "libsgmain.so", "libsgsecuritybody.so", "libtersafe.so", "libtprt.so",
    "libnesec.so", "libmobisec.so", "libdexhelper.so", "libjiagu.so",
    "libexec.so", "libexecmain.so", "libbeacon.so", "libmetasec.so",
    "libsecshell.so", "libgamesecurity.so", "libanort2.so",
};

bool isAntiCheatLib(const std::string& n) {
    for (const char* b : kAntiCheatLibs)
        if (n == b) return true;
    return false;
}

// ukuran module di proses (max end - min start dari /proc/<pid>/maps)
static uintptr_t moduleSize(const char* name) {
    uintptr_t lo = 0, hi = 0;
    const std::string suffix = std::string("/") + name;
    for (const auto& r : mem::regions()) {
        if (r.name.size() < suffix.size()) continue;
        if (r.name.compare(r.name.size() - suffix.size(), suffix.size(), suffix) != 0) continue;
        if (!lo || r.start < lo) lo = r.start;
        if (r.end > hi) hi = r.end;
    }
    return hi > lo ? hi - lo : 0;
}


static std::string trim(const std::string& s) {
    // Potong dulu di NUL pertama: nilai cfg bisa kebawa byte 0 (dulu ditulis
    // dari nama proses) dan itu bikin perbandingan nama proses/game gagal
    // padahal isinya bener.
    size_t b = 0, e = s.size();
    const size_t z = s.find('\0');
    if (z != std::string::npos) e = z;
    while (b < e && isspace((unsigned char)s[b])) ++b;
    while (e > b && isspace((unsigned char)s[e - 1])) --e;
    return s.substr(b, e - b);
}

// ---------------------------------------------------------------------------
// proses
// ---------------------------------------------------------------------------
static bool readCmdlineFirst(const char* pidStr, char* out, size_t cap) {
    char path[64];
    snprintf(path, sizeof(path), "/proc/%s/cmdline", pidStr);
    int fd = ::open(path, O_RDONLY);
    if (fd < 0) return false;
    ssize_t n = ::read(fd, out, cap - 1);
    ::close(fd);
    if (n <= 0) return false;
    out[n] = 0;
    return true;
}

// ---------------------------------------------------------------------------
// penjaga "proses sendiri"
//
// Nama proses tool disamarkan (lihat bypass.h), jadi dia bisa NYASAR ke daftar
// proses dan dipilih sendiri sebagai target -- kejadian di HP: [auto] milih
// "mali_render" (dirinya sendiri) terus attach ke diri sendiri. Jadi pid kita,
// rantai parent (mksh/ash/su yang ngejalanin tool), nama samaran kita, dan
// path tool wajib dibuang dari daftar kandidat.
// ---------------------------------------------------------------------------
static int procPpid(int pid) {
    char p[64];
    snprintf(p, sizeof(p), "/proc/%d/stat", pid);
    FILE* f = fopen(p, "re");
    if (!f) return 0;
    int ppid = 0;
    if (fscanf(f, "%*d (%*[^)]) %*c %d", &ppid) != 1) ppid = 0;
    fclose(f);
    return ppid;
}

static const std::string& selfComm() {          // nama proses kita (bisa disamarkan)
    static std::string s = [] {
        char b[64] = {0};
        FILE* f = fopen("/proc/self/comm", "re");
        if (f) { if (!fgets(b, sizeof(b), f)) b[0] = 0; fclose(f); }
        std::string t(b);
        while (!t.empty() && (t.back() == '\n' || t.back() == '\r' || t.back() == ' '))
            t.pop_back();
        return t;
    }();
    return s;
}

static bool isSelfProcess(int pid, const char* cmd) {
    if (pid == (int)getpid()) return true;
    for (int cur = (int)getppid(), i = 0; i < 4 && cur > 1; ++i) {
        if (cur == pid) return true;
        cur = procPpid(cur);
    }
    if (cmd && *cmd) {
        if (strstr(cmd, "panxcz") || strstr(cmd, "libtool")) return true;
        const std::string& me = selfComm();
        if (!me.empty() && me == cmd) return true;
    }
    return false;
}

std::vector<Proc> listProcesses() {
    std::vector<Proc> out;
    DIR* d = opendir("/proc");
    if (!d) return out;
    struct dirent* e;
    char buf[512];
    while ((e = readdir(d)) != nullptr) {
        const char* p = e->d_name;
        bool numeric = *p != 0;
        for (; *p; ++p) if (!isdigit((unsigned char)*p)) { numeric = false; break; }
        if (!numeric) continue;
        if (!readCmdlineFirst(e->d_name, buf, sizeof(buf))) continue;
        if (!buf[0] || buf[0] == ':') continue;                 // sub-proses / kernel thread
        // buang proses sistem Android yang jelas bukan game
        if (strncmp(buf, "com.android.", 12) == 0) continue;
        if (strncmp(buf, "android.", 8) == 0) continue;
        // buang tool sendiri + rantai parent-nya (lihat isSelfProcess)
        if (isSelfProcess(atoi(e->d_name), buf)) continue;
        Proc pr;
        pr.pid = atoi(e->d_name);
        pr.pkg = buf;
        // biar menu bisa nunjukin proses mana yang beneran punya engine il2cpp
        pr.score = mem::il2cppScore(pr.pid);
        out.push_back(pr);
    }
    closedir(d);
    return out;
}

std::string autoDetectPackage(int* scoreOut) {
    std::string best;
    int bestScore = -1;
    for (const auto& p : listProcesses()) {
        if (p.score <= bestScore) continue;
        bestScore = p.score;
        best = p.pkg;
    }
    if (scoreOut) *scoreOut = bestScore;
    // Skor minimum: proses yang cuma "kebetulan" punya maps panjang (skor 2)
    // atau app biasa (bonus app_libs = 20) jangan sampai kepilih jadi game.
    // Game beneran selalu >= 40 (liblogic/libunity) atau 100+ (libil2cpp).
    if (best.empty() || bestScore < 15) return std::string();
    const size_t c = best.find(':');
    return c == std::string::npos ? best : best.substr(0, c);
}

// ---------------------------------------------------------------------------
// konfigurasi
// ---------------------------------------------------------------------------
// cari pid dengan cmdline PERSIS sama (dipakai buat nempel langsung ke proses
// engine yang disimpan di cfg, mis. com.mobile.legends:UnityKillsMe)
static int findPidByCmdline(const std::string& want) {
    if (want.empty()) return 0;
    DIR* d = opendir("/proc");
    if (!d) return 0;
    int found = 0;
    struct dirent* e;
    char buf[512];
    while ((e = readdir(d)) != nullptr) {
        const char* p = e->d_name;
        bool numeric = *p != 0;
        for (; *p; ++p) if (!isdigit((unsigned char)*p)) { numeric = false; break; }
        if (!numeric) continue;
        if (!readCmdlineFirst(e->d_name, buf, sizeof(buf))) continue;
        if (want == buf) { found = atoi(e->d_name); break; }
    }
    closedir(d);
    return found;
}

static bool readCfg(const char* path, std::string& pkg, std::string& lib, int& pid,
                    std::string& dmp, std::string& dir, std::string& eng) {
    std::ifstream f(path);
    if (!f.is_open()) return false;
    std::string line;
    bool any = false;
    while (std::getline(f, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        std::string t = trim(line);
        if (t.empty() || t[0] == '#') continue;
        any = true;
        size_t eq = t.find('=');
        if (eq == std::string::npos) {
            // baris telanjang: baris pertama = package, kedua = lib
            if (pkg.empty()) pkg = t; else if (lib.empty()) lib = t;
            continue;
        }
        std::string k = trim(t.substr(0, eq));
        std::string v = trim(t.substr(eq + 1));
        if (k == "package" || k == "pkg" || k == "game") pkg = v;
        else if (k == "lib" || k == "il2cpp") lib = (v == "auto") ? "" : v;
        else if (k == "dump" || k == "dumpfile" || k == "dump_file") dmp = v;
        else if (k == "dumpdir" || k == "dump_dir") dir = v;
        else if (k == "pid") { int n = atoi(v.c_str()); pid = (n > 1) ? n : 0; }
        else if (k == "engine" || k == "proc" || k == "process") eng = v;
    }
    return any;
}

bool load() {
    static bool done = false;
    if (done) return true;
    done = true;

    std::string pkg, lib, dmp, dir, eng;
    int pid = 0;
    if (readCfg(kCfgTmp, pkg, lib, pid, dmp, dir, eng))      g_cfgUsed = kCfgTmp;
    else if (readCfg(kCfgSd, pkg, lib, pid, dmp, dir, eng)) g_cfgUsed = kCfgSd;
    g_engine = eng;
    if (!pkg.empty()) g_pkg = pkg;
    g_lib = lib;                       // kosong = auto
    // cfg lawas bisa nyimpen nama package di kolom lib (bug lama di parsing
    // argumen). Nama lib selalu ".so", jadi yang bukan ".so" dianggap auto.
    if (!g_lib.empty() && g_lib.find(".so") == std::string::npos) {
        logx::add("[target] cfg nulis lib='%s' (bukan nama lib) -- dianggap auto",
                  g_lib.c_str());
        g_lib.clear();
    }
    if (!dmp.empty()) g_dump = (dmp == "auto") ? "" : dmp;          // auto = cari sendiri
    // Folder dump: cfg LAWAS sering nunjuk root lama (tanpa "/dumper"), jadi
    // nilainya dianggap "belum diganti" dan default baru yang dipakai.
    if (!dir.empty() && dir != "auto" && !isLegacyDumpRoot(dir)) g_dumpDir = dir;
    else if (isLegacyDumpRoot(dir))
        logx::add("[target] cfg nunjuk folder dump lama (%s) -- dipakai default: %s",
                  dir.c_str(), kDumpRoot);
    g_pidSel = pid;
    if (!g_pkg.empty())
        logx::add("[target] cfg: package=%s lib=%s pid=%s engine=%s dump=%s (%s)", g_pkg.c_str(),
                  g_lib.empty() ? "auto" : g_lib.c_str(),
                  g_pidSel ? std::to_string(g_pidSel).c_str() : "auto",
                  g_engine.empty() ? "auto" : g_engine.c_str(),
                  g_dump.empty() ? "auto" : g_dump.c_str(),
                  g_cfgUsed.c_str());
    return true;
}

bool save() {
    std::ostringstream o;
    o << "# panxcz_libtool -- target game\n";
    o << "# package = nama paket game\n";
    o << "# lib     = lib il2cpp (auto = tebak sendiri)\n";
    o << "package=" << g_pkg << "\n";
    o << "lib=" << (g_lib.empty() ? "auto" : g_lib) << "\n";
    o << "# dump    = dump .cs buat tabel class-slot (auto = cari sendiri)\n";
    o << "dump=" << (g_dump.empty() ? "auto" : g_dump) << "\n";
    o << "# dumpdir = root folder dumper (<root>/<pkg>/<versi>/*.cs)\n";
    o << "dumpdir=" << (g_dumpDir.empty() ? kDumpRoot : g_dumpDir) << "\n";
    o << "# pid = proses spesifik (0/auto = pilih sendiri yang punya il2cpp)\n";
    o << "pid=" << g_pidSel << "\n";
    o << "# engine = proses yang punya lib il2cpp (MLBB: <pkg>:UnityKillsMe).\n";
    o << "#          diisi otomatis setelah attach -> jalan berikutnya nempel langsung.\n";
    o << "engine=" << g_engine << "\n";

    bool ok = false;
    const char* paths[] = { kCfgTmp, kCfgSd };
    for (const char* p : paths) {
        // /sdcard/panxcz mungkin belum ada
        if (strcmp(p, kCfgSd) == 0) {
            ::mkdir("/sdcard/panxcz", 0777);
        }
        std::ofstream f(p, std::ios::trunc);
        if (f.is_open()) {
            f << o.str();
            f.close();
            ::chmod(p, 0666);
            ok = true;
        }
    }
    if (ok) logx::add("[target] disimpan: %s%s%s", g_pkg.c_str(),
                      g_lib.empty() ? " (lib auto)" : (" " + g_lib).c_str(),
                      g_pidSel ? (" pid=" + std::to_string(g_pidSel)).c_str() : "");
    else    logx::add("[target] gagal nulis cfg");
    return ok;
}

const std::string& package() { return g_pkg; }
const std::string& engine()  { return g_engine; }
void setEngine(const std::string& e) { g_engine = trim(e); }
const std::string& lib()     { return g_lib; }
bool autoLib()               { return g_lib.empty(); }
std::string configPath()     { return g_cfgUsed; }

void setPackage(const std::string& p) {
    g_pkg = trim(p);
    // Ganti game = engine yang lama udah nggak relevan (lihat attach()).
    if (!g_engine.empty() && !g_pkg.empty() &&
        !(g_engine == g_pkg ||
          (g_engine.size() > g_pkg.size() &&
           g_engine.compare(0, g_pkg.size(), g_pkg) == 0 && g_engine[g_pkg.size()] == ':')))
        g_engine.clear();
}
void setLib(const std::string& l)     { g_lib = (trim(l) == "auto") ? "" : trim(l); }
void setDump(const std::string& p)    { g_dump = (trim(p) == "auto") ? "" : trim(p); }
const std::string& dump()            { return g_dump; }
void setDumpDir(const std::string& p) { g_dumpDir = (trim(p) == "auto") ? "" : trim(p); }
std::string dumpDir()                 { return g_dumpDir.empty() ? std::string(kDumpRoot) : g_dumpDir; }
std::string customDumpDir()           { return g_dumpDir; }
void setSelPid(int pid) { g_pidSel = (pid > 1) ? pid : 0; }
int  selPid()           { return g_pidSel; }

// ---------------------------------------------------------------------------
// cari file dump
// ---------------------------------------------------------------------------
struct DumpCand {
    std::string path;
    time_t      mtime = 0;
    long        size = 0;
    long        slots = -1;     // jumlah kelas yang punya slot (dihitung cuma kalau perlu)
    long        classes = -1;   // jumlah kelas (baris deklarasi class/struct/enum/interface)
    bool        full = false;   // nama nunjukin dump gabungan / lib pemilik metadata
    bool        native = false; // dump HASIL tool ini sendiri (bukan metadata frida/Il2CppDumper)
};

// Dump yang ditulis panxcz_libtool sendiri (header "panxcz_libtool native dump").
// Ini BUKAN katalog eksternal: isinya kelas yang di-resolve runtime PLUS
// instansiasi generic, jadi jumlahnya bengkak (MLBB: 37959 vs 28139 di metadata
// frida). Kalau dump ini dipakai sebagai katalog, tool jadi makan hasil
// keluarannya sendiri -> lingkar, dan jumlah kelasnya salah. Jadi kandidat
// native ditaruh di bawah dump metadata eksternal (frida/Il2CppDumper).
static bool isNativeDump(const std::string& path) {
    int fd = open(path.c_str(), O_RDONLY);
    if (fd < 0) return false;
    char buf[512];
    const ssize_t r = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (r <= 0) return false;
    buf[r] = '\0';
    return strstr(buf, "panxcz_libtool native dump") != nullptr;
}

// Hitung berapa KELAS yang ada di file (baris deklarasi "class/struct/enum/
// interface X" di kolom 0). Ini ukuran yang bener buat milih katalog: yang
// kelasnya paling banyak itu yang katalognya paling lengkap.
//
// KENAPA BUKAN JUMLAH SLOT (dulu salah di sini):
//   dump metadata hasil frida/Il2CppDumper (28139 kelas) justru punya slot
//   LEBIH SEDIKIT (6235) daripada dump native tool ini (8931 kelas, 8480 slot)
//   -- soalnya slot cuma keisi buat kelas yang udah di-resolve runtime, dan
//   yang di-resolve runtime itu cuma sebagian. Jadi kalau diranking pakai slot,
//   dump 28139 kelas (referensi yang bener) malah kalah sama dump 8931 kelas.
//   Kelas dulu, slot cuma tie-break.
static bool betterCand(const DumpCand& a, const DumpCand& b) {
    if (b.path.empty()) return true;
    // Dump metadata eksternal (frida/Il2CppDumper) SELALU menang atas dump
    // native tool ini: katalog yang bener cuma yang eksternal.
    if (a.native != b.native) return !a.native;
    if (a.classes != b.classes) return a.classes > b.classes;
    if (a.slots != b.slots)     return a.slots > b.slots;
    if (a.mtime != b.mtime)     return a.mtime > b.mtime;
    return a.full && !b.full;
}

// Hitung baris "// Class-slot candidates:" (bukan unknown) -- banyak kelas yang
// bisa di-resolve runtime. Cuma dipakai sebagai tie-break + pengaman gabung.
static long countSlotLines(const std::string& path) {
    int fd = open(path.c_str(), O_RDONLY);
    if (fd < 0) return -1;
    struct stat st;
    if (fstat(fd, &st) != 0 || st.st_size <= 0 || st.st_size > (long)(512 << 20)) {
        close(fd);
        return -1;
    }
    std::string buf;
    try { buf.resize((size_t)st.st_size); }
    catch (...) { close(fd); return -1; }
    size_t got = 0;
    while (got < buf.size()) {
        const ssize_t r = read(fd, &buf[got], buf.size() - got);
        if (r <= 0) break;
        got += (size_t)r;
    }
    close(fd);
    buf.resize(got);

    long n = 0;
    size_t i = 0;
    while (i < buf.size()) {
        size_t nl = buf.find('\n', i);
        if (nl == std::string::npos) nl = buf.size();
        const size_t len = nl - i;
        if (len > 25 && strncmp(buf.c_str() + i, "// Class-slot candidates:", 25) == 0) {
            const char* rest = buf.c_str() + i + 25;
            size_t rl = len - 25;
            while (rl && (*rest == ' ' || *rest == '\t')) { ++rest; --rl; }
            if (!(rl >= 7 && strncmp(rest, "unknown", 7) == 0)) ++n;
        }
        i = nl + 1;
    }
    return n;
}

// Satu pass baca file: hitung KELAS (deklarasi top-level) + slot sekaligus.
// Dipakai buat ranking kandidat dump. Kelas = ukuran katalog yang bener.
static void countCandLines(const std::string& path, long& classes, long& slots) {
    classes = -1;
    slots   = -1;
    int fd = open(path.c_str(), O_RDONLY);
    if (fd < 0) return;
    struct stat st;
    if (fstat(fd, &st) != 0 || st.st_size <= 0 || st.st_size > (long)(512 << 20)) {
        close(fd);
        return;
    }
    std::string buf;
    try { buf.resize((size_t)st.st_size); }
    catch (...) { close(fd); return; }
    size_t got = 0;
    while (got < buf.size()) {
        const ssize_t r = read(fd, &buf[got], buf.size() - got);
        if (r <= 0) break;
        got += (size_t)r;
    }
    close(fd);
    buf.resize(got);

    static const char* kKws[] = {"class", "struct", "enum", "interface"};
    long nc = 0, ns = 0;
    size_t i = 0;
    while (i < buf.size()) {
        size_t nl = buf.find('\n', i);
        if (nl == std::string::npos) nl = buf.size();
        const size_t len = nl - i;
        const char* p = buf.c_str() + i;
        // deklarasi kelas: keyword di kolom 0 lalu satu spasi
        if (len > 6 && p[0] != ' ' && p[0] != '\t' && p[0] != '/' && p[0] != '#') {
            for (const char* k : kKws) {
                const size_t kl = strlen(k);
                if (len > kl && strncmp(p, k, kl) == 0 && p[kl] == ' ') { ++nc; break; }
            }
        }
        if (len > 25 && strncmp(p, "// Class-slot candidates:", 25) == 0) {
            const char* rest = p + 25;
            size_t rl = len - 25;
            while (rl && (*rest == ' ' || *rest == '\t')) { ++rest; --rl; }
            if (!(rl >= 7 && strncmp(rest, "unknown", 7) == 0)) ++ns;
        }
        i = nl + 1;
    }
    classes = nc;
    slots   = ns;
}

// file .cs di dalam <dir> (nggak rekursif). Yang menang: slot-nya paling banyak
// (ukurannya paling "berisi"), terus file yang paling baru; nama yang nyebut lib
// pemilik metadata (libcsharp/liblogic/libfirst) dikasih bonus kecil.
static void collectDumpDir(const std::string& dir, std::vector<DumpCand>& out,
                           const std::string& nameFilter = std::string()) {
    DIR* d = opendir(dir.c_str());
    if (!d) return;
    struct dirent* e;
    while ((e = readdir(d)) != nullptr) {
        const std::string name = e->d_name;
        if (name.size() < 4) continue;
        if (!nameFilter.empty() && name.find(nameFilter) == std::string::npos) continue;
        // dump tes cepat (run_dump.py --classes N -> *.test.cs) jangan kepilih:
        // isinya cuma beberapa kelas, jadi kelas lain malah nggak ketemu.
        if (name.find(".test.") != std::string::npos) continue;
        const bool cs = name.compare(name.size() - 3, 3, ".cs") == 0;
        const bool txt = name.find("dump") != std::string::npos;
        if (!cs && !txt) continue;

        const std::string full = dir + "/" + name;
        struct stat st;
        if (stat(full.c_str(), &st) != 0) continue;
        if (!S_ISREG(st.st_mode) || st.st_size < 64 * 1024) continue;   // dump = gede

        // "full" = dump gabungan (dari tools/dump_merge.py, jadi multi-module)
        const bool isFull = name.find("full") != std::string::npos ||
                            name.find("merge") != std::string::npos;
        const bool owner = name.find("libcsharp") != std::string::npos ||
                           name.find("liblogic") != std::string::npos ||
                           name.find("libfirst") != std::string::npos;

        DumpCand c;
        c.path = full;
        c.size = st.st_size;
        c.mtime = st.st_mtime;
        c.slots = -1;
        c.classes = -1;
        c.full = isFull || owner;
        c.native = isNativeDump(full);
        out.push_back(c);
    }
    closedir(d);
}

// kandidat terbaik: kelas terbanyak > slot terbanyak > terbaru > "full"/lib owner
static DumpCand pickBestDump(std::vector<DumpCand>& cands, bool countLines) {
    for (auto& c : cands) {
        if (!countLines) continue;
        if (c.classes < 0 || c.slots < 0) {
            long cl = -1, sl = -1;
            countCandLines(c.path, cl, sl);
            if (c.classes < 0) c.classes = cl;
            if (c.slots   < 0) c.slots   = sl;
        }
    }
    DumpCand best;
    for (auto& c : cands)
        if (best.path.empty() || betterCand(c, best)) best = c;
    return best;
}

static std::string dirOf(const std::string& p) {
    const size_t s = p.find_last_of('/');
    return s == std::string::npos ? std::string() : p.substr(0, s);
}

static std::string baseName(const std::string& p) {
    const size_t s = p.find_last_of('/');
    return s == std::string::npos ? p : p.substr(s + 1);
}

// package dasar tanpa ":subproses" (MLBB -> com.mobile.legends)
static std::string pkgBase(const std::string& pkg) {
    const size_t c = pkg.find(':');
    return c == std::string::npos ? pkg : pkg.substr(0, c);
}

// Folder "versi game" (22.1.97.12061 / 1.132.1): cuma file dari folder versi
// yang sama yang boleh digabung -- RVA antar versi beda, gabungin = ngaco.
static bool versionKey(const std::string& path, std::string& key) {
    const std::string b = baseName(dirOf(path));
    bool digit = false, dot = false;
    for (const char c : b) {
        if (isdigit((unsigned char)c)) digit = true;
        if (c == '.') dot = true;
    }
    if (!digit || !dot) return false;
    key = b;
    return true;
}

// file dump ini punya package yang lagi dipilih? (folder-nya atau namanya)
static bool dumpMatchesPkg(const std::string& path, const std::string& pkg) {
    const std::string base = pkgBase(pkg);
    if (base.empty()) return true;
    if (baseName(path).find(base) != std::string::npos) return true;
    if (path.find("/" + base + "/") != std::string::npos) return true;
    return false;
}

// semua sub-folder di dalam <dir> (buat nyari <pkg>/<versi>)
static void listDirs(const std::string& dir, std::vector<std::string>& out) {
    DIR* d = opendir(dir.c_str());
    if (!d) return;
    struct dirent* e;
    while ((e = readdir(d)) != nullptr) {
        const std::string name = e->d_name;
        if (name == "." || name == "..") continue;
        const std::string full = dir + "/" + name;
        struct stat st;
        if (stat(full.c_str(), &st) == 0 && S_ISDIR(st.st_mode)) out.push_back(full);
    }
    closedir(d);
}

// Semua dump yang LAYAK dipakai buat package target. Biasanya satu file, tapi
// kalau dump-nya kepecah per lib (com.x_liblogic.so.cs + com.x_libcsharp.so.cs)
// semuanya dikembalikan supaya bisa digabung -> katalognya lengkap.
// Yang DIJAMIN: nggak akan mascilin dump punya game lain (dulu cfg `dump=` yang
// nunjuk ke dump game lain bikin kelas/RVA-nya ketuker).
std::vector<std::string> findDumpFiles() {
    std::vector<std::string> res;

    // 1. yang diminta user (cfg `dump=` / argumen --dump)
    if (g_dump == "auto") g_dump.clear();          // kalau cfg-nya lawas nulis "auto"
    if (!g_dump.empty()) {
        if (access(g_dump.c_str(), R_OK) != 0) {
            logx::add("[dump] '%s' nggak kebaca -- lanjut nyari sendiri", g_dump.c_str());
        } else if (dumpMatchesPkg(g_dump, g_pkg)) {
            res.push_back(g_dump);
            return res;
        } else {
            logx::add("[dump] cfg `dump=` -> %s itu punya game LAIN (target %s) -- "
                      "dilewat, nyari dump-nya sendiri",
                      baseName(g_dump).c_str(), pkgBase(g_pkg).c_str());
            g_dump.clear();   // biar nggak dilaporin berulang tiap dipanggil
        }
    }

    // 2. folder per-package: <root>/<pkg>/<versi>/*.cs -- tiga root tetap sama
    // Folder yang di-set user dicek DULUAN, tapi root standar tetap ikut dicari:
    // dump lengkap sering berada di root lain (mis. hasil dump_merge.py ada di
    // /sdcard/panxcz-tools tanpa "dumper"), dan yang dipilih tetap yang paling
    // lengkap (slot terbanyak).
    std::vector<std::string> roots;
    if (!g_dumpDir.empty()) roots.push_back(g_dumpDir);
    for (const char* r : {kDumpRoot, kDumpRoot2, kDumpRoot3}) {
        bool dup = false;
        for (const auto& e : roots) if (e == r) { dup = true; break; }
        if (!dup) roots.push_back(r);
    }

    std::vector<DumpCand> cands;
    const std::string base = pkgBase(g_pkg);
    for (const auto& r : roots) {
        // 2a. file lepas di root-nya (mis. dumper/<pkg>_native_full.cs)
        if (!base.empty()) collectDumpDir(r, cands, base);

        if (base.empty()) continue;
        std::vector<std::string> pkgDirs;
        struct stat st;
        const std::string direct = r + "/" + base;
        if (stat(direct.c_str(), &st) == 0 && S_ISDIR(st.st_mode)) pkgDirs.push_back(direct);
        const std::string withSub = r + "/" + g_pkg;
        if (!g_pkg.empty() && g_pkg != base &&
            stat(withSub.c_str(), &st) == 0 && S_ISDIR(st.st_mode))
            pkgDirs.push_back(withSub);
        if (pkgDirs.empty()) {          // nama folder beda? cocokin longgar
            std::vector<std::string> subs;
            listDirs(r, subs);
            for (const auto& s : subs) {
                const std::string n = baseName(s);
                if (n == base || n.compare(0, base.size(), base) == 0) pkgDirs.push_back(s);
            }
        }
        for (const auto& pd : pkgDirs) {
            collectDumpDir(pd, cands);
            std::vector<std::string> vers;
            listDirs(pd, vers);
            for (const auto& v : vers) collectDumpDir(v, cands);
        }
    }

    if (cands.empty() && base.empty()) {           // package belum di-set
        for (const auto& r : roots) { collectDumpDir(r, cands); collectDumpDir(r + "/dumper", cands); }
    }
    if (cands.empty()) {
        // 3. lokasi standar (file dump lepas) -- tetap wajib punya game yang bener
        static const char* kCand[] = {
            "/data/local/tmp/panxcz_dump.cs",
            "/data/local/tmp/dump.cs",
            "/sdcard/panxcz/dump.cs",
            "/sdcard/panxcz/libtool_dump.cs",
            "/sdcard/PanxczDump/dump.cs",
            "/sdcard/Download/dump.cs",
            "/sdcard/Download/panxcz_dump.cs",
        };
        for (const char* c : kCand) {
            if (access(c, R_OK) != 0) continue;
            if (!dumpMatchesPkg(c, g_pkg)) continue;
            logx::add("[dump] dipakai (lokasi standar): %s", c);
            res.push_back(c);
            return res;
        }
        logx::add("[dump] dump buat %s nggak ketemu di %s/<pkg>/<versi>/*.cs",
                  base.empty() ? "(package belum dipilih)" : base.c_str(), roots[0].c_str());
        return res;
    }

    const DumpCand best = pickBestDump(cands, true);   // sekalian ngitung kelas + slot
    if (cands.size() > 1) {   // biar jelas file mana yang dipertimbangin
        logx::add("[dump] %zu kandidat dump buat %s:", cands.size(), base.c_str());
        for (const auto& c : cands)
            logx::add("[dump]   %-46s %4zu MB, %ld kelas, %ld berslot%s",
                      baseName(c.path).c_str(), (size_t)(c.size / (1024 * 1024)),
                      c.classes, c.slots, c.path == best.path ? "  <- dipakai" : "");
    }

    res.push_back(best.path);
    if (cands.size() == 1)
        logx::add("[dump] dipakai: %s (%zu MB, %ld kelas, %ld berslot)",
                  baseName(best.path).c_str(), (size_t)(best.size / (1024 * 1024)),
                  best.classes, best.slots);

    // Semua file dari FOLDER VERSI YANG SAMA ikut dikembalikan supaya bisa
    // digabung -- TAPI penggabungannya sekarang diputusin di loadMany pakai
    // tingkat TUMPANG-TINDIH kelas, bukan asal gabung:
    //
    //   * dump PER-LIB yang saling MELENGKAPI (kelasnya beda) -> digabung, itu
    //     memang tujuan fitur ini (satu file aja nggak lengkap).
    //   * dump LENGKAP yang saling MENIMPA (dua-duanya punya kelas yang sama,
    //     mis. libcsharp.so.cs + liblogic.so.cs hasil frida yang tokennya
    //     sintetis per-run) -> yang kalah DIBUANG. Dulu malah digabung, dan
    //     itu yang bikin jumlah kelas nge-kerut dari 28139 ke ~27000.
    //
    // Di sini cuma disaring file yang boleh ikut digabung: yang JELAS lebih
    // kecil dari yang terbaik. File sebanding = saingan, bukan pelengkap.
    std::string vk;
    const bool haveVk = versionKey(best.path, vk);
    for (const auto& c : cands) {
        if (c.path == best.path) continue;
        // Dump native tool ini jangan ikut digabung: kelasnya (yang bengkak +
        // instansiasi generic) bakal nimpa/ngerusak katalog metadata yang bener.
        if (c.native && !best.native) {
            logx::add("[dump]   dilewat: %s (dump native tool ini, bukan katalog metadata)",
                      baseName(c.path).c_str());
            continue;
        }
        if (haveVk) {
            std::string k;
            if (!versionKey(c.path, k) || k != vk) continue;
        } else if (dirOf(c.path) != dirOf(best.path)) {
            continue;
        }
        // Aturan gabung: cuma file yang JELAS lebih kecil (dump per-lib
        // PELENGKAP) yang boleh digabung. File yang besarnya sebanding dengan
        // yang terbaik = dump LENGKAP saingan (dua-duanya nge-cover game yang
        // sama, mis. libcsharp.so.cs + liblogic.so.cs hasil frida). Kalau
        // digabung, kelas kembarnya saling nimpa dan token palsunya (sintetis
        // per-run) bikin jumlahnya malah NGE-KERUT, bukan nambah.
        if (best.classes > 0 && c.classes >= 0) {
            if (c.classes * 2 >= best.classes) {
                logx::add("[dump]   dilewat: %s (dump lengkap saingan: %ld vs %ld kelas terbaik)",
                          baseName(c.path).c_str(), c.classes, best.classes);
                continue;
            }
        } else if (best.slots > 0 && c.slots >= 0 && c.slots * 20 < best.slots) {
            logx::add("[dump]   dilewat: %s (cuma %ld kelas berslot, %ld di file terbaik)",
                      baseName(c.path).c_str(), c.slots, best.slots);
            continue;
        }
        res.push_back(c.path);
    }
    if (res.size() > 1)
        logx::add("[dump] %zu file versi %s dipertimbangkan (tumpang-tindih dicek saat load)",
                  res.size(), haveVk ? vk.c_str() : "(folder sama)");
    return res;
}

std::string findDumpFile() {
    const std::vector<std::string> v = findDumpFiles();
    return v.empty() ? std::string() : v[0];
}

// ---------------------------------------------------------------------------
// attach
// ---------------------------------------------------------------------------
bool attached() { return mem::attached(); }
int  pid()      { return mem::pid(); }

bool attach() {
    // pid yang dipilih manual (dari menu / cfg) lebih diprioritaskan: prosesnya
    // udah pasti, jadi nggak perlu nebak lagi.
    if (g_pidSel > 0) {
        if (mem::attachPid(g_pidSel, g_pkg.c_str())) return true;
        logx::add("[target] pid %d nggak bisa di-attach (proses mati?), balik ke auto",
                  g_pidSel);
        g_pidSel = 0;
    }

    // Proses ENGINE yang udah pernah ketemu (disimpan di cfg): langsung ke situ.
    // Contoh MLBB: yang punya il2cpp itu com.mobile.legends:UnityKillsMe, bukan
    // proses utamanya -- jadi nggak perlu nebak tiap jalan.
    //
    // TAPI cuma kalau engine itu memang milik package yang lagi diminta: kalau
    // user ganti game (mis. MLBB -> CODM), engine lama JANGAN dipakai -- kalau
    // dipakai, tool-nya nempel ke game yang salah.
    const bool engineOwnedByPkg =
        !g_engine.empty() &&
        (g_engine == g_pkg ||
         (g_engine.size() > g_pkg.size() &&
          g_engine.compare(0, g_pkg.size(), g_pkg) == 0 &&
          g_engine[g_pkg.size()] == ':'));
    if (!g_engine.empty() && !engineOwnedByPkg)
        logx::add("[target] engine di cfg ('%s') bukan punya %s -- dicari sendiri",
                  g_engine.c_str(), g_pkg.c_str());
    if (engineOwnedByPkg) {
        const int ep = findPidByCmdline(g_engine);
        if (ep > 1 && mem::attachPid(ep, g_engine.c_str())) {
            logx::add("[target] nempel ke proses engine dari cfg: %s (pid %d)",
                      g_engine.c_str(), ep);
            return true;
        }
        if (ep <= 1)
            logx::add("[target] proses engine '%s' belum jalan -- dicari sendiri",
                      g_engine.c_str());
    }
    if (g_pkg.empty()) {
        logx::add("[target] package belum di-set -- pilih game di tab TARGET");
        return false;
    }
    if (mem::attach(g_pkg.c_str())) return true;
    // Throttle: auto-mode ngecek tiap 0,5 detik -- tanpa ini logcat kebanjiran
    // ratusan baris "gagal attach" (dan itu juga kelihatan mencurigakan).
    static long lastFail = 0;
    const long now = (long)time(nullptr);
    if (now - lastFail >= 10) {
        lastFail = now;
        logx::add("[target] gagal attach ke %s (game jalan? root?)", g_pkg.c_str());
    }
    return false;
}

// ---------------------------------------------------------------------------
// cari lib il2cpp
// ---------------------------------------------------------------------------
// lib dianggap kandidat il2cpp kalau: namanya mengandung "il2cpp", atau
// global-metadata.dat muncul di sebelahnya (Unity selalu membukanya), atau
// di dekat lib itu ada string "il2cpp_domain_get".
static bool looksLikeIl2cpp(const mem::Region& first, const std::string& name) {
    if (name.find("il2cpp") != std::string::npos) return true;
    // path file .so-nya ada di folder yang sama dengan global-metadata.dat?
    size_t slash = first.name.find_last_of('/');
    if (slash != std::string::npos) {
        std::string dir = first.name.substr(0, slash);
        std::string meta = dir + "/global-metadata.dat";
        if (access(meta.c_str(), R_OK) == 0) return true;
    }
    return false;
}

std::string detectLib(bool force) {
    static std::string cached;
    if (!force && !cached.empty()) return cached;
    cached.clear();

    if (!mem::attached()) return cached;

    // 1. nama yang diketik user
    if (!g_lib.empty() && isAntiCheatLib(g_lib)) {
        logx::add("[target] '%s' itu library anti-cheat -- nggak dipakai buat scanning",
                  g_lib.c_str());
        return cached;
    }
    if (!g_lib.empty()) {
        if (mem::base(g_lib.c_str())) {
            logx::add("[target] lib dari cfg/menu: %s (%zu KB)", g_lib.c_str(),
                      (size_t)(moduleSize(g_lib.c_str()) / 1024));
            cached = g_lib;
            return cached;
        }
        logx::add("[target] lib '%s' nggak ke-load di proses ini", g_lib.c_str());
    }

    // 2. kandidat umum. Yang penting di sini: jangan kepilih lib STUB.
    //    MLBB punya libil2cpp.so 384 KB di APK (bukan engine-nya) sementara
    //    engine aslinya liblogic.so (27 MB) + libcsharp.so (104 MB).
    std::string first, biggest, anti;
    uintptr_t   firstSize = 0, biggestSize = 0;
    for (const char* k : kKnownLibs) {
        if (!mem::base(k)) continue;
        const uintptr_t sz = moduleSize(k);
        if (first.empty()) { first = k; firstSize = sz; }
        if (sz > biggestSize) { biggestSize = sz; biggest = k; }
    }
    if (first.empty()) {
        // nggak ada engine yang ke-load -- kalau yang ada cuma lib anti-cheat,
        // bilang apa adanya daripada dipilih terus nge-scan memori anti-cheat.
        for (const char* b : kAntiCheatLibs)
            if (mem::base(b)) { anti = b; break; }
        if (!anti.empty())
            logx::add("[target] cuma library anti-cheat yang ke-load (%s) -- "
                      "engine il2cpp belum ada, tunggu game beneran jalan",
                      anti.c_str());
    }
    if (!first.empty()) {
        cached = (firstSize < 1024 * 1024 && biggestSize >= 4ull * 1024 * 1024)
                     ? biggest : first;
        // lapis kedua: lib yang diketik user / dari cfg pun nggak boleh
        // anti-cheat (cfg lama bisa nyimpen libanogs.so sebagai "lib").
        if (isAntiCheatLib(cached)) {
            logx::add("[target] '%s' itu library anti-cheat, bukan engine -- dibatalkan",
                      cached.c_str());
            cached.clear();
            return cached;
        }
        logx::add("[target] lib il2cpp kedeteksi: %s (%zu KB)%s", cached.c_str(),
                  (size_t)(moduleSize(cached.c_str()) / 1024),
                  (cached != first) ? "  -- kandidat pertama cuma stub" : "");
        return cached;
    }

    // 3. lib yang punya global-metadata.dat di sebelahnya
    for (const auto& r : mem::regions()) {
        if (r.name.find(".so") == std::string::npos) continue;
        if (r.fileOff != 0) continue;
        size_t slash = r.name.find_last_of('/');
        if (slash == std::string::npos) continue;
        std::string base = r.name.substr(slash + 1);
        if (looksLikeIl2cpp(r, base) && mem::base(base.c_str())) {
            cached = base;
            logx::add("[target] lib il2cpp dari global-metadata: %s", base.c_str());
            return cached;
        }
    }

    logx::add("[target] nggak nemu lib il2cpp -- tulis manual namanya di tab TARGET");
    return cached;
}

}  // namespace target

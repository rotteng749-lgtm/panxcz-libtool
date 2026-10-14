#include "core/mem.h"
#include "core/log.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cctype>
#include <atomic>
#include <algorithm>
#include <mutex>
#include <unordered_set>
#include <dirent.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/uio.h>
#include <sys/syscall.h>
#include <cerrno>

namespace mem {

static int                  g_pid     = -1;
static int                  g_fd      = -1;
static bool                 g_rw      = false;
static std::vector<Region>  g_regions;

static void clearBadPages();   // definisi di bawah (cache halaman-jelek)

// ukuran blok waktu scan (biar hemat memori)
static const size_t kChunk = 4u * 1024u * 1024u;
// ambang "blok besar" (baca longgar + boleh dipecah di rawReadBig)
static const size_t kBigRead = 64u * 1024u;
// batas total byte yang discan satu kali panggilan scanQword.
// Nilai besar karena heap metadata il2cpp bisa ada di ujung address space.
static size_t g_scanBudget = 6ull * 1024 * 1024 * 1024;   // 6 GB

void setScanBudget(size_t bytes) { if (bytes >= 64u * 1024 * 1024) g_scanBudget = bytes; }
size_t scanBudget() { return g_scanBudget; }

size_t totalBytes(bool writableOnly) {
    size_t t = 0;
    for (const auto& r : g_regions) {
        if (!r.readable()) continue;
        if (writableOnly && !r.writable()) continue;
        t += r.size();
    }
    return t;
}

// ----------------------------------------------------------------------------
// proses
// ----------------------------------------------------------------------------
// baca cmdline process; return panjang isi (0 kalau kosong/gagal baca)
// PENTING: wajib pakai ::read (libc), bukan read() polos -- di dalam
// namespace mem ada mem::read() yang akan menutup nama itu.
static ssize_t readCmdline(const char* pidStr, char* buf, size_t cap) {
    char path[64];
    snprintf(path, sizeof(path), "/proc/%s/cmdline", pidStr);
    int fd = ::open(path, O_RDONLY);
    if (fd < 0) return -1;
    ssize_t n = ::read(fd, buf, cap - 1);
    ::close(fd);
    if (n < 0) return -1;
    buf[n] = 0;
    return n;
}

// cmdline Android suka di-padding NUL dan nama bisa ada tambahan proses
// (contoh "com.x:remote"), jadi bandingkan token pertama saja.
static bool cmdlineMatches(const char* buf, ssize_t n, const char* package) {
    if (n <= 0) return false;
    size_t plen = strlen(package);
    char first[256];
    size_t i = 0;
    for (; i < plen && i < (size_t)n; ++i) {
        char c = buf[i];
        if (c == 0 || c == ' ') break;
        first[i] = c;
    }
    first[i] = 0;
    if (strcmp(first, package) == 0) return true;
    // proses turunan: "com.pkg:service"
    return strncmp(first, package, plen) == 0 && first[plen] == ':';
}

// Skor proses ini punya engine il2cpp, dilihat dari /proc/<pid>/maps:
//   lib dengan "il2cpp" di namanya ...... 100  (paling yakin)
//   global-metadata.dat ke-map ........... 60
//   lib engine khas (liblogic/libgame) ... 40
//   lib engine di /app_libs/ (<data>/../app_libs) 20  (MLBB dkk)
//   2.000 map ke atas .................... +2  (proses "berat" = main game)
// Ini yang bikin tool-nya tetap ketemu lib walau Unity-nya jalan di proses
// anak (contoh MLBB: cmdline-nya "com.mobile.legends:UnityKillsMe").
int il2cppScore(int pid) {
    char path[64];
    snprintf(path, sizeof(path), "/proc/%d/maps", pid);
    FILE* f = fopen(path, "re");
    if (!f) return -1;

    int score = 0, lines = 0, bonus = 0;
    bool sawIl2cpp = false, sawLogic = false, sawMeta = false;
    char line[1024];
    while (fgets(line, sizeof(line), f)) {
        ++lines;
        if (strstr(line, "global-metadata")) sawMeta = true;
        if (strstr(line, "il2cpp"))          sawIl2cpp = true;
        else if (strstr(line, "liblogic.so") || strstr(line, "libgame.so") ||
                 strstr(line, "libil2cpp") || strstr(line, "libunity.so"))
            sawLogic = true;
        else if (strstr(line, "app_libs"))   bonus = 1;
        else if (strstr(line, "libanogs.so")) bonus = 1;   // Free Fire
    }
    fclose(f);

    if (sawMeta)    score += 60;
    if (sawIl2cpp)  score += 100;
    if (sawLogic)   score += 40;
    if (bonus)      score += 20;
    if (lines > 2000) score += 2;
    return score;
}

// Pilih pid yang paling mungkin memuat lib il2cpp dari semua proses yang
// cmdline-nya cocok dengan package (pernah salah pid: engine-nya di proses anak).
static int findPid(const char* package) {
    DIR* d = opendir("/proc");
    if (!d) {
        logx::add("[mem] opendir /proc gagal (errno=%d)", errno);
        return -1;
    }
    int found = -1, bestScore = -1000, npid = 0, nread = 0, nfail = 0;
    int firstErr = 0;
    char bestCmd[256] = {0};

    struct dirent* e;
    char buf[512];
    while ((e = readdir(d)) != nullptr) {
        bool numeric = true;
        for (const char* p = e->d_name; *p; ++p)
            if (!isdigit((unsigned char)*p)) { numeric = false; break; }
        if (!numeric || !*e->d_name) continue;
        ++npid;

        ssize_t n = readCmdline(e->d_name, buf, sizeof(buf));
        if (n < 0) { ++nfail; if (!firstErr) firstErr = errno; continue; }
        if (n == 0) continue;
        ++nread;

        if (!cmdlineMatches(buf, n, package)) continue;

        const int pid = atoi(e->d_name);
        int sc = il2cppScore(pid);
        // Proses anak dengan nama khas engine Unity (mis. MLBB punya
        // "com.mobile.legends:UnityKillsMe") hampir selalu yang memuat lib
        // il2cpp, sementara proses utama-nya cuma shell/loader.
        if (strstr(buf, ":Unity") || strstr(buf, ":unity") ||
            strstr(buf, ":il2cpp") || strstr(buf, ":game"))
            sc += 30;
        if (sc > bestScore) {
            bestScore = sc;
            found = pid;
            size_t k = 0;
            for (; k + 1 < sizeof(bestCmd) && k < (size_t)n && buf[k]; ++k)
                bestCmd[k] = buf[k];
            bestCmd[k] = 0;
        }
        // udah ketemu yang paling yakin -> nggak perlu lanjut
        if (bestScore >= 100) break;
    }
    closedir(d);

    if (found < 0) {
        logx::add("[mem] scan %d pid: %d kebaca, %d gagal buka (errno=%d)",
                  npid, nread, nfail, firstErr);
    } else {
        logx::add("[mem] pid=%d [%s] skor il2cpp=%d", found, bestCmd, bestScore);
        if (bestScore <= 0)
            logx::add("[mem] peringatan: pid ini belum ke-load lib il2cpp "
                      "(game masih loading?) -- coba 'Detect lib' lagi nanti");
    }
    return found;
}

// ----------------------------------------------------------------------------
// maps
// ----------------------------------------------------------------------------
// nama file pendek dari path maps (buat log self-check)
static std::string shortName(const std::string& path) {
    const size_t s = path.find_last_of('/');
    return s == std::string::npos ? path : path.substr(s + 1);
}

static bool parseMaps() {
    g_regions.clear();
    char path[64];
    snprintf(path, sizeof(path), "/proc/%d/maps", g_pid);
    FILE* f = fopen(path, "re");
    if (!f) return false;

    char line[1024];
    while (fgets(line, sizeof(line), f)) {
        uintptr_t s = 0, en = 0;
        uint64_t off = 0;
        char perms[8] = {0};
        int n = sscanf(line, "%lx-%lx %7s %lx", &s, &en, perms, &off);
        if (n < 4) continue;

        Region r;
        r.start = s;
        r.end = en;
        r.fileOff = off;
        memcpy(r.perms, perms, 4);
        r.perms[4] = 0;

        // nama file = setelah inode. maps ngasih kolom inode lebar (padded),
        // jadi lompatin dulu semua spasi sebelum path biar r.name mulai dari '/'
        // (kalau nggak, picker lib yang cek r.name[0]=='/' jadi kosong).
        char* p = line;
        int sp = 0;
        while (*p && sp < 5) { if (*p == ' ') sp++; p++; }
        while (*p == ' ' || *p == '\t') ++p;
        if (*p) {
            char* nl = strchr(p, '\n');
            if (nl) *nl = 0;
            r.name = p;
        }
        g_regions.push_back(r);
    }
    fclose(f);
    // urut alamat -- syarat buat regionOf() binary search di atas. maps biasanya
    // udah urut, tapi kalau nggak, tanpa sort hasilnya salah.
    std::sort(g_regions.begin(), g_regions.end(),
              [](const Region& a, const Region& b) { return a.start < b.start; });
    return !g_regions.empty();
}

// attach langsung ke pid tertentu (dipakai menu + argumen --pid).
// Balikin false kalau maps / /proc/pid/mem nggak kebaca.
bool attachPid(int pid, const char* label) {
    detach();
    if (pid <= 0) return false;
    g_pid = pid;
    if (!parseMaps()) {
        logx::add("[mem] gagal baca /proc/%d/maps (proses mati?)", pid);
        g_pid = -1;
        return false;
    }

    char path[64];
    snprintf(path, sizeof(path), "/proc/%d/mem", pid);
    g_fd = open(path, O_RDWR | O_CLOEXEC);
    if (g_fd >= 0) {
        g_rw = true;
    } else {
        g_fd = open(path, O_RDONLY | O_CLOEXEC);
        g_rw = false;
    }
    logx::add("[mem] pid=%d%s%s region=%zu mode=%s", g_pid, label ? " " : "",
              label && *label ? label : "", g_regions.size(), g_rw ? "rw" : "ro(root?)");
    if (g_fd < 0) {
        logx::add("[mem] /proc/pid/mem gagal dibuka (errno=%d)", errno);
        return false;
    }
    return true;
}

bool attach(const char* package) {
    detach();
    g_pid = findPid(package);
    if (g_pid <= 0) {
        logx::add("[mem] proses '%s' tidak jalan", package);
        g_pid = -1;
        return false;
    }
    if (!parseMaps()) {
        logx::add("[mem] gagal baca /proc/%d/maps", g_pid);
        g_pid = -1;
        return false;
    }

    char path[64];
    snprintf(path, sizeof(path), "/proc/%d/mem", g_pid);
    g_fd = open(path, O_RDWR | O_CLOEXEC);
    if (g_fd >= 0) {
        g_rw = true;
    } else {
        g_fd = open(path, O_RDONLY | O_CLOEXEC);
        g_rw = false;
    }
    logx::add("[mem] pid=%d region=%zu mode=%s", g_pid, g_regions.size(),
              g_rw ? "rw" : "ro(root?)");
    if (g_fd < 0) {
        logx::add("[mem] /proc/pid/mem gagal dibuka (errno=%d)", errno);
        return false;
    }
    return true;
}

void detach() {
    if (g_fd >= 0) close(g_fd);
    g_fd = -1;
    g_pid = -1;
    g_rw = false;
    g_regions.clear();
    clearBadPages();   // target baru = layout baru
}

bool  attached() { return g_pid > 0 && g_fd >= 0; }
int   pid()      { return g_pid; }

std::string procName() {
    if (g_pid <= 0) return std::string();
    char pidStr[24];
    snprintf(pidStr, sizeof(pidStr), "%d", g_pid);
    char buf[256] = {0};
    const ssize_t n = readCmdline(pidStr, buf, sizeof(buf));
    if (n <= 0) return std::string();
    // /proc/<pid>/cmdline itu argumen-nya dipisah NUL -- yang dipakai cuma
    // token pertama (nama proses). Kalau nggak dipotong di NUL, nama proses-nya
    // kebawa byte 0 (dulu kejadian: cfg nulis "engine=...:UnityKillsMe\0\0\0"
    // -> perbandingan nama proses gagal, padahal prosesnya ada).
    size_t len = 0;
    while (len < (size_t)n && buf[len]) ++len;
    return std::string(buf, len);
}
const std::vector<Region>& regions() { return g_regions; }
// Maps dibaca ulang (dlopen/mmap baru): halaman-jelek kemarin bisa jadi valid,
// jadi cache-nya dibuang -- pindaian berikut nemuin ulangnya kalau masih jelek.
bool  refresh()  {
    if (!attached() || !parseMaps()) return false;
    clearBadPages();
    return true;
}

// CATATAN KINERJA: ini dipanggil buat SETIAP qword kandidat waktu memori
// dipindai (jutaan kali). Versi linear dulu (loop semua region) jadi penyebab
// utama dump/scan "jalan >10 menit nggak selesai" di device yang maps-nya
// panjang -- sekarang binary search (maps selalu urut alamat).
const Region* regionOf(uintptr_t addr) {
    addr = untag(addr);
    if (g_regions.empty()) return nullptr;
    size_t lo = 0, hi = g_regions.size();
    while (lo < hi) {
        const size_t mid = (lo + hi) / 2;
        if (g_regions[mid].start <= addr) lo = mid + 1;
        else                             hi = mid;
    }
    if (!lo) return nullptr;
    const Region& r = g_regions[lo - 1];
    return (addr >= r.start && addr < r.end) ? &r : nullptr;
}

bool canRead(uintptr_t addr, size_t n) {
    addr = untag(addr);
    const Region* r = regionOf(addr);
    if (!r || !r->readable()) return false;
    return addr + n <= r->end;
}

bool canWrite(uintptr_t addr, size_t n) {
    addr = untag(addr);
    const Region* r = regionOf(addr);
    if (!r || !r->writable()) return false;
    return addr + n <= r->end;
}

// ----------------------------------------------------------------------------
// read / write
//
// CATATAN PORTABILITAS (ini yang dulu bikin dump "error mulu" di HP lain):
// pread /proc/<pid>/mem bisa MENOLAK blok besar (4 MB) di sebagian kernel, dan
// process_vm_readv sering diblokir SELinux. Kalau satu permintaan 4 MB gagal,
// dulu satu CHUNK penuh dilewat -- hasil pindainya jadi nol kelas padahal
// memorinya ke-map. Sekarang blok besar (>64 KB) dipecah sampai 64 KB dan
// bagian yang nggak kebaca di-nol-kan; baca kecil (struktur) tetap ketat
// (gagal = gagal), karena di situ data nol bisa nyamar jadi nilai valid.
// ----------------------------------------------------------------------------
static std::atomic<size_t> g_readFail{0};

size_t readFailures() { return g_readFail.load(std::memory_order_relaxed); }
void   resetReadFailures() { g_readFail.store(0, std::memory_order_relaxed); }

// ----------------------------------------------------------------------------
// Cache halaman-jelek: halaman yang pernah GAGAL dibaca (guard page / lubang
// di dalam region yang ke-map). Tanpa ini, tiap pindaian (dumpout, pool,
// strings, scanner) me-syscall ulang + menghitung ulang halaman yang sama --
// itu sumber "puluhan ribu gagal baca" padahal datanya tetap ketemu (di-nol).
// Granularitas = page size kernel. Di-clear tiap ganti target / refresh maps,
// karena layout memori bisa berubah (halaman jelek kemarin bisa jadi valid).
// ----------------------------------------------------------------------------
static std::mutex g_badMtx;
static std::unordered_set<uintptr_t> g_badPages;
static size_t    g_pgSize = 0;
static uintptr_t g_pgMask = 0;
static const size_t kBadPageCap = 131072;   // 2 GB kalau page 16 KB

static void pageInit() {
    if (g_pgMask) return;
    static std::mutex m;
    std::lock_guard<std::mutex> lk(m);
    if (g_pgMask) return;
    long ps = sysconf(_SC_PAGESIZE);
    if (ps != 4096 && ps != 16384) ps = 4096;
    g_pgSize = (size_t)ps;
    g_pgMask = ~((uintptr_t)ps - 1);
}

static uintptr_t pageBase(uintptr_t a) { pageInit(); return a & g_pgMask; }

static bool isBadPage(uintptr_t pb) {
    std::lock_guard<std::mutex> lk(g_badMtx);
    return g_badPages.find(pb) != g_badPages.end();
}

static void markBadPage(uintptr_t pb) {
    std::lock_guard<std::mutex> lk(g_badMtx);
    if (g_badPages.size() < kBadPageCap) g_badPages.insert(pb);
}

size_t badPageCount() {
    std::lock_guard<std::mutex> lk(g_badMtx);
    return g_badPages.size();
}

static void clearBadPages() {
    std::lock_guard<std::mutex> lk(g_badMtx);
    g_badPages.clear();
}

static bool rawReadOnce(uintptr_t addr, void* out, size_t n) {
    const ssize_t got = pread(g_fd, out, n, (off_t)addr);
    if (got == (ssize_t)n) return true;
    // fallback: process_vm_readv
    struct iovec l{out, n}, rr{(void*)addr, n};
    const ssize_t v = syscall(__NR_process_vm_readv, g_pid, &l, 1UL, &rr, 1UL, 0UL);
    return v == (ssize_t)n;
}

// Daun <=64 KB: dibaca PRESISI per halaman. Halaman yang udah ketahuan jelek
// di-nol-kan tanpa syscall + tanpa ngitung lagi; yang baru ketahuan jelek
// dihitung SEKALI terus diingat. Bonus vs dulu: satu halaman jelek nggak lagi
// bikin 64 KB ikut nol (yang bagus tetap kebaca).
static bool rawReadLeaf(uintptr_t addr, uint8_t* out, size_t n) {
    pageInit();
    size_t off = 0;
    bool ok = true;
    while (off < n) {
        const uintptr_t cur = addr + off;
        const uintptr_t pb = cur & g_pgMask;
        size_t chunk = (size_t)((pb + g_pgSize) - cur);
        if (chunk > n - off) chunk = n - off;
        if (isBadPage(pb)) {
            memset(out + off, 0, chunk);
            ok = false;
        } else if (!rawReadOnce(cur, out + off, chunk)) {
            markBadPage(pb);
            g_readFail.fetch_add(1, std::memory_order_relaxed);
            memset(out + off, 0, chunk);
            ok = false;
        }
        off += chunk;
    }
    return ok;
}

// blok besar: longgar. Bagian yang gagal dibaca = nol (bukan bikin seluruh
// permintaan gagal). Nol dianggap "bukan data", jadi pemanggil (pencari
// pointer/string) nggak pernah salah tafsir -- cuma ada bagian yang kelewat.
static bool rawReadBig(uintptr_t addr, uint8_t* out, size_t n) {
    if (rawReadOnce(addr, out, n)) return true;
    if (n <= (64u << 10)) return rawReadLeaf(addr, out, n);
    const size_t half = n / 2;
    const bool a = rawReadBig(addr, out, half);
    const bool b = rawReadBig(addr + half, out + half, n - half);
    return a && b;
}

static bool rawWrite(uintptr_t addr, const void* in, size_t n) {
    if (g_fd < 0) return false;
    ssize_t put = pwrite(g_fd, in, n, (off_t)addr);
    if (put == (ssize_t)n) return true;
    struct iovec l{(void*)in, n}, rr{(void*)addr, n};
    ssize_t v = syscall(__NR_process_vm_writev, g_pid, &l, 1UL, &rr, 1UL, 0UL);
    return v == (ssize_t)n;
}

bool read(uintptr_t addr, void* out, size_t n) {
    if (g_fd < 0 || !out || !n) return false;
    addr = untag(addr);
    uint8_t* dst = (uint8_t*)out;
    size_t done = 0;
    while (done < n) {
        uintptr_t cur = addr + done;
        const Region* r = regionOf(cur);
        if (!r || !r->readable()) return false;
        const size_t avail = (size_t)(r->end - cur);
        const size_t want = (n - done) < avail ? (n - done) : avail;
        if (want >= kBigRead) {
            rawReadBig(cur, dst + done, want);      // longgar (lihat catatan di atas)
        } else {
            // Gagal cepat: range pas satu halaman yang udah ketahuan jelek ->
            // tanpa syscall, tanpa ngitung lagi. Yang baru ketahuan jelek
            // ditandai (cuma kalau pas satu halaman, biar presisi).
            pageInit();
            const uintptr_t pb = cur & g_pgMask;
            const bool single = (pb == ((cur + want - 1) & g_pgMask));
            if (single && isBadPage(pb)) return false;
            if (!rawReadOnce(cur, dst + done, want)) {
                if (single) markBadPage(pb);
                g_readFail.fetch_add(1, std::memory_order_relaxed);
                return false;
            }
        }
        done += want;
    }
    return true;
}

bool write(uintptr_t addr, const void* in, size_t n) {
    if (g_fd < 0 || !in || !n) return false;
    addr = untag(addr);
    const uint8_t* src = (const uint8_t*)in;
    size_t done = 0;
    while (done < n) {
        uintptr_t cur = addr + done;
        const Region* r = regionOf(cur);
        if (!r) return false;
        size_t avail = (size_t)(r->end - cur);
        size_t want = (n - done) < avail ? (n - done) : avail;
        if (!rawWrite(cur, src + done, want)) return false;
        done += want;
    }
    return true;
}

// ----------------------------------------------------------------------------
// self-check lingkungan
//
// Dipakai kalau dump gagal di device lain: daripada nebak-nebak, di sini
// dicetak KEADAAN sebenarnya -- root/izin, mode /proc/pid/mem, jumlah region,
// dan yang paling penting: apakah baca 4 MB (blok yang dipakai scanner) jalan
// atau harus dipecah. Tiap baris ada verdict-nya, jadi log-nya bisa dibaca
// tanpa perlu nebak.
// ----------------------------------------------------------------------------
bool diag(const char* tag) {
    logx::add("[check] ---- %s ----", (tag && *tag) ? tag : "lingkungan");
    logx::add("[check] euid=%d (root=%s)", (int)geteuid(), geteuid() == 0 ? "ya" : "TIDAK");

    if (!attached()) {
        logx::add("[check] modul mem BELUM attach -- nggak ada proses game yang kebuka");
        logx::add("[check]   -> pastikan game jalan & tool dijalankan sebagai root");
        return false;
    }
    logx::add("[check] proses=%s pid=%d fd=%s region=%zu",
              procName().empty() ? "?" : procName().c_str(), g_pid,
              g_fd < 0 ? "GAGAL" : (g_rw ? "rw" : "r-"), g_regions.size());
    if (g_fd < 0) {
        logx::add("[check] /proc/%d/mem nggak kebuka (errno=%d) -- SELinux/root nolak",
                  g_pid, errno);
        logx::add("[check]   -> coba: su -c 'setenforce 0'  (atau kasih izin root ke tool)");
        return false;
    }

    // statistik region
    size_t rd = 0, wr = 0, big = 0;
    unsigned long bytes = 0;
    const Region* biggest = nullptr;
    for (const auto& r : g_regions) {
        if (!r.readable()) continue;
        ++rd;
        bytes += (unsigned long)(r.size() >> 10);
        if (r.writable()) ++wr;
        if (r.size() >= (1u << 20)) ++big;
        if (!biggest || r.size() > biggest->size()) biggest = &r;
    }
    logx::add("[check] region: %zu kebaca, %zu writable, %zu >1MB, total %lu MB",
              rd, wr, big, bytes >> 10);
    if (!rd) {
        logx::add("[check] nggak ada region yang kebaca -- maps kosong?");
        return false;
    }

    // uji baca nyata: 4 MB (blok yang dipakai scanner) + 8 byte
    const size_t before = g_readFail.load();
    std::vector<uint8_t> buf(4u << 20);
    int okBig = 0, failBig = 0, tested = 0;
    for (const auto& r : g_regions) {
        if (!r.readable() || r.size() < (2u << 20)) continue;
        if (tested >= 3) break;
        ++tested;
        const size_t n = r.size() >= buf.size() ? buf.size() : (r.size() & ~(size_t)0xFFF);
        if (!n) continue;
        const size_t failBefore = g_readFail.load();
        read(r.start, buf.data(), n);
        const bool clean = g_readFail.load() == failBefore;
        if (clean) ++okBig; else ++failBig;
        const std::string nm = r.name.empty() ? std::string("(anon)") : shortName(r.name);
        logx::add("[check] baca %zu MB @0x%lx di %s: %s", n >> 20,
                  (unsigned long)r.start, nm.c_str(),
                  clean ? "OK utuh" : "dipecah (ada bagian nggak kebaca)");
    }
    if (biggest) {
        uint64_t q = 0;
        const bool ok = read(biggest->start, &q, sizeof(q));
        logx::add("[check] region terbesar %zu MB @0x%lx: baca 8 byte %s (qword=%llx)",
                  biggest->size() >> 20, (unsigned long)biggest->start,
                  ok ? "OK" : "GAGAL", (unsigned long long)q);
    }
    const size_t failed = g_readFail.load() - before;   // total percobaan sub-blok yang gagal
    logx::add("[check] blok 4MB: %d jalan, %d dipecah/di-nol-kan | gagal baca total=%zu",
              okBig, failBig, failed);

    if (!failBig && !failed) {
        logx::add("[check] VERDICT: baca memori normal -- kalau dump tetap gagal, "
                  "masalahnya bukan di sini (kirim log dari baris [dumpout]/[pool])");
        return true;
    }
    logx::add("[check] VERDICT: sebagian blok besar nggak kebaca (kernel menolak blok "
              "besar). Tool sekarang tetap jalan lewat jalur pecah-64KB, tapi hasilnya "
              "bisa bolong di region itu.");
    return true;
}

// ----------------------------------------------------------------------------
// module
// ----------------------------------------------------------------------------
uintptr_t base(const char* moduleName) {
    uintptr_t best = 0;
    size_t len = strlen(moduleName);
    for (const auto& r : g_regions) {
        if (r.name.size() < len) continue;
        if (r.name.compare(r.name.size() - len, len, moduleName) != 0) continue;
        if (r.fileOff != 0) continue;               // segmen pertama = base
        if (!r.readable()) continue;
        if (!best || r.start < best) best = r.start;
    }
    return best;
}

// ----------------------------------------------------------------------------
// RVA yang stabil antar run (lihat penjelasan di mem.h)
// ----------------------------------------------------------------------------
uint64_t rvaOf(uintptr_t addr) {
    const Region* r = regionOf(addr);
    if (!r) return 0;
    return r->fileOff + (uint64_t)(addr - r->start);
}

void addrOfRvaAll(uint64_t rva, std::vector<uintptr_t>& out, size_t maxOut) {
    out.clear();
    // 3 giliran: (1) segmen kode, (2) writable, (3) sisanya yang kebaca.
    for (int pass = 0; pass < 3 && out.size() < maxOut; ++pass) {
        for (const auto& r : g_regions) {
            if (out.size() >= maxOut) break;
            if (!r.readable()) continue;
            const bool isExec = r.exec();
            const bool isWr   = r.writable();
            if (pass == 0 && !isExec) continue;
            if (pass == 1 && (isExec || !isWr)) continue;
            if (pass == 2 && (isExec || isWr)) continue;
            const uint64_t end = r.fileOff + (uint64_t)(r.end - r.start);
            if (rva < r.fileOff || rva >= end) continue;
            const uintptr_t a = r.start + (uintptr_t)(rva - r.fileOff);
            bool dup = false;
            for (uintptr_t o : out) if (o == a) { dup = true; break; }
            if (!dup) out.push_back(a);
        }
    }
}

uintptr_t addrOfRva(uint64_t rva) {
    std::vector<uintptr_t> all;
    addrOfRvaAll(rva, all, 1);
    return all.empty() ? 0 : all[0];
}

// Tafsir rva DIBATASI ke satu modul. Wajib ada: addrOfRva() nyari di SEMUA
// region, jadi nomor rva yang sama bisa "ketemu" di segmen kode lib LAIN yang
// file offset-nya kebetulan nutup rentang itu -- nyata kejadian: peek
// libanogs.so:0x50A9E8 dibalas alamat 0x705ba9e8 (segmen kode punya lib lain),
// padahal yang dimaksud ada di libanogs sendiri.
uintptr_t addrOfRvaIn(const char* moduleName, uint64_t rva) {
    if (!moduleName || !*moduleName) return 0;
    const size_t len = strlen(moduleName);
    uintptr_t any = 0, code = 0;
    for (const auto& r : g_regions) {
        if (!r.readable() || r.name.size() < len) continue;
        if (r.name.compare(r.name.size() - len, len, moduleName) != 0) continue;
        const uint64_t end = r.fileOff + (uint64_t)(r.end - r.start);
        if (rva < r.fileOff || rva >= end) continue;
        const uintptr_t a = r.start + (uintptr_t)(rva - r.fileOff);
        if (!any) any = a;
        if (r.exec()) { code = a; break; }
    }
    return code ? code : any;
}

// Tafsir VADDR (nilai yang masih relatif modul, belum ditambah bias). Dipakai
// buat lib yang di-load loader SENDIRI ala packer: nilai PT_DYNAMIC dan r_offset
// relokasinya masih vaddr mentah (nyata di libanogs/libanort Free Fire).
uintptr_t addrOfVaddrIn(const char* moduleName, uint64_t vaddr) {
    if (!moduleName || !*moduleName) return 0;
    const size_t len = strlen(moduleName);
    uintptr_t lo = 0, hi = 0;
    for (const auto& r : g_regions) {
        if (r.name.size() < len) continue;
        if (r.name.compare(r.name.size() - len, len, moduleName) != 0) continue;
        if (!lo || r.start < lo) lo = r.start;
        if (r.end > hi) hi = r.end;
    }
    if (!lo) return 0;
    const uintptr_t a = lo + (uintptr_t)vaddr;
    return (a >= lo && a < hi) ? a : 0;
}

std::string path(const char* moduleName) {
    size_t len = strlen(moduleName);
    for (const auto& r : g_regions) {
        if (r.name.size() < len) continue;
        if (r.name.compare(r.name.size() - len, len, moduleName) == 0) return r.name;
    }
    return {};
}

// ----------------------------------------------------------------------------
// cari string / pola
// ----------------------------------------------------------------------------
// scan per blok (region bisa ratusan MB, jangan malloc seluruhnya)
static uintptr_t findStringIn(const Region& r, const char* s, size_t len) {
    if (!r.readable() || r.size() < len + 1) return 0;

    const size_t chunk = kChunk;
    size_t cap = chunk + len + 1;
    uint8_t* buf = (uint8_t*)malloc(cap);
    if (!buf) return 0;

    uintptr_t hit = 0;
    uintptr_t a = r.start;
    size_t carry = 0;
    size_t budget = 0;

    while (a < r.end && budget < g_scanBudget) {
        size_t want = (size_t)(r.end - a);
        if (want > chunk) want = chunk;
        if (!read(a, buf + carry, want)) { a += want; carry = 0; continue; }
        budget += want;

        size_t total = carry + want;
        uint8_t* p = buf;
        uint8_t* fin = buf + total;
        while (p < fin) {
            uint8_t* m = (uint8_t*)memmem(p, (size_t)(fin - p), s, len);
            if (!m) break;
            // harus string utuh: byte setelahnya NUL
            if (m + len < fin && m[len] == 0) {
                hit = a - (uintptr_t)carry + (uintptr_t)(m - buf);
                break;
            }
            p = m + 1;
        }
        if (hit) break;

        carry = (total >= len) ? len : total;
        memmove(buf, buf + total - carry, carry);
        a += want;
    }
    free(buf);
    return hit;
}

uintptr_t findString(const char* s) {
    std::vector<uintptr_t> v;
    findStringsAll(s, v, 1, false);
    return v.empty() ? 0 : v[0];
}

// Kumpulkan SEMUA kemunculan string.
// Penting: IL2CPP menyalin metadata ke heap, jadi salinan yang di-rodata .so
// TIDAK punya pointer balik. Kita harus ikut memindai region writable.
// scan satu region, kumpulkan semua kemunculan string NUL-terminated
static void scanRegionForString(const Region& r, const char* s, size_t len,
                                std::vector<uintptr_t>& out, size_t maxHits) {
    if (out.size() >= maxHits) return;
    if (!r.readable() || r.size() < len + 1) return;

    const size_t chunk = kChunk;
    uint8_t* buf = (uint8_t*)malloc(chunk + len + 1);
    if (!buf) return;
    uintptr_t a = r.start;
    size_t carry = 0;

    while (a < r.end && out.size() < maxHits) {
        size_t want = (size_t)(r.end - a);
        if (want > chunk) want = chunk;
        if (!read(a, buf + carry, want)) { a += want; carry = 0; continue; }

        size_t total = carry + want;
        uint8_t* p = buf;
        uint8_t* fin = buf + total;
        while (p < fin) {
            uint8_t* m = (uint8_t*)memmem(p, (size_t)(fin - p), s, len);
            if (!m) break;
            if (m + len < fin && m[len] == 0)
                out.push_back(a - (uintptr_t)carry + (uintptr_t)(m - buf));
            p = m + 1;
        }
        carry = (total >= len) ? len : total;
        memmove(buf, buf + total - carry, carry);
        a += want;
    }
    free(buf);
}

// Cari string HANYA di region milik modul tertentu.
// Ini jalur tercepat: nama class il2cpp ada di rodata libil2cpp.so
// (~25 MB) jadi tidak perlu memindai 6 GB heap.
void findStringsInModule(const char* nameSubstr, const char* s,
                         std::vector<uintptr_t>& out, size_t maxHits) {
    out.clear();
    if (g_fd < 0 || !nameSubstr || !s || !*s) return;
    size_t len = strlen(s);
    for (const auto& r : g_regions) {
        if (r.name.find(nameSubstr) == std::string::npos) continue;
        scanRegionForString(r, s, len, out, maxHits);
        if (out.size() >= maxHits) break;
    }
}

void findStringsInRange(uintptr_t start, uintptr_t end, const char* s,
                        std::vector<uintptr_t>& out, size_t maxHits) {
    out.clear();
    if (g_fd < 0 || !s || !*s || end <= start) return;
    size_t len = strlen(s);
    const Region* r = regionOf(start);
    if (!r) return;
    if (end > r->end) end = r->end;

    const size_t chunk = kChunk;
    uint8_t* buf = (uint8_t*)malloc(chunk + len + 1);
    if (!buf) return;
    uintptr_t a = start;
    size_t carry = 0;
    while (a < end) {
        size_t want = (size_t)(end - a);
        if (want > chunk) want = chunk;
        if (!read(a, buf + carry, want)) { a += want; carry = 0; continue; }
        size_t total = carry + want;
        uint8_t* p = buf;
        uint8_t* fin = buf + total;
        while (p < fin) {
            uint8_t* m = (uint8_t*)memmem(p, (size_t)(fin - p), s, len);
            if (!m) break;
            if (m + len < fin && m[len] == 0)
                out.push_back(a - (uintptr_t)carry + (uintptr_t)(m - buf));
            p = m + 1;
        }
        carry = (total >= len) ? len : total;
        memmove(buf, buf + total - carry, carry);
        a += want;
    }
    free(buf);
}

void findStringsAll(const char* s, std::vector<uintptr_t>& out,
                    size_t maxHits, bool writableOnly) {
    out.clear();
    if (g_fd < 0 || !s || !*s) return;
    size_t len = strlen(s);

    auto scanRegion = [&](const Region& r) {
        if (out.size() >= maxHits) return;
        if (!r.readable() || r.size() < len + 1) return;

        const size_t chunk = kChunk;
        uint8_t* buf = (uint8_t*)malloc(chunk + len + 1);
        if (!buf) return;
        uintptr_t a = r.start;
        size_t carry = 0;

        while (a < r.end && out.size() < maxHits) {
            size_t want = (size_t)(r.end - a);
            if (want > chunk) want = chunk;
            if (!read(a, buf + carry, want)) { a += want; carry = 0; continue; }

            size_t total = carry + want;
            uint8_t* p = buf;
            uint8_t* fin = buf + total;
            while (p < fin) {
                uint8_t* m = (uint8_t*)memmem(p, (size_t)(fin - p), s, len);
                if (!m) break;
                if (m + len < fin && m[len] == 0)
                    out.push_back(a - (uintptr_t)carry + (uintptr_t)(m - buf));
                p = m + 1;
            }
            carry = (total >= len) ? len : total;
            memmove(buf, buf + total - carry, carry);
            a += want;
        }
        free(buf);
    };

    // writable (heap metadata) dulu, baru read-only (.so rodata)
    if (writableOnly) {
        for (const auto& r : g_regions) if (r.writable()) scanRegion(r);
        return;
    }
    for (const auto& r : g_regions) if (r.writable()) scanRegion(r);
    for (const auto& r : g_regions) if (r.readable() && !r.writable()) scanRegion(r);
}

// ----------------------------------------------------------------------------
// pola hex
// ----------------------------------------------------------------------------
struct SigByte { uint8_t v; bool any; };

static std::vector<SigByte> parseSig(const char* sig) {
    std::vector<SigByte> out;
    while (*sig) {
        while (*sig == ' ') ++sig;
        if (!*sig) break;
        if (sig[0] == '?' && (sig[1] == '?' || sig[1] == ' ' || sig[1] == 0)) {
            out.push_back({0, true});
            ++sig; if (*sig == '?') ++sig;
            continue;
        }
        char b[3] = {sig[0], sig[1], 0};
        out.push_back({(uint8_t)strtoul(b, nullptr, 16), false});
        sig += (sig[1] ? 2 : 1);
    }
    return out;
}

static bool sigMatch(const uint8_t* p, const std::vector<SigByte>& sig) {
    for (size_t i = 0; i < sig.size(); ++i)
        if (!sig[i].any && p[i] != sig[i].v) return false;
    return true;
}

uintptr_t findPatternIn(const Region& r, const char* sig) {
    if (!r.readable()) return 0;
    std::vector<SigByte> s = parseSig(sig);
    size_t plen = s.size();
    if (plen == 0 || r.size() < plen) return 0;

    const size_t chunk = kChunk;
    uint8_t* buf = (uint8_t*)malloc(chunk + plen);
    if (!buf) return 0;

    uintptr_t hit = 0;
    uintptr_t a = r.start;
    size_t carry = 0;
    size_t budget = 0;

    while (a < r.end && !hit && budget < g_scanBudget) {
        size_t want = (size_t)(r.end - a);
        if (want > chunk) want = chunk;
        if (!read(a, buf + carry, want)) { a += want; carry = 0; continue; }
        budget += want;

        size_t total = carry + want;
        if (total >= plen) {
            size_t lim = total - plen;
            for (size_t i = 0; i <= lim; ++i) {
                if (sigMatch(buf + i, s)) {
                    hit = a - (uintptr_t)carry + (uintptr_t)i;
                    break;
                }
            }
        }
        carry = (total >= plen) ? plen - 1 : total;
        memmove(buf, buf + total - carry, carry);
        a += want;
    }
    free(buf);
    return hit;
}

uintptr_t findPattern(uintptr_t start, uintptr_t end, const char* sig) {
    for (const auto& r : g_regions) {
        if (r.end <= start || r.start >= end) continue;
        uintptr_t h = findPatternIn(r, sig);
        if (h && h >= start && h < end) return h;
    }
    return 0;
}

// ----------------------------------------------------------------------------
// scan value
// ----------------------------------------------------------------------------
static void scanValue(const void* pat, size_t patLen, std::vector<uintptr_t>& out,
                      size_t maxHits, bool writableOnly) {
    out.clear();
    size_t budget = 0;
    uint8_t* buf = (uint8_t*)malloc(kChunk + patLen);
    if (!buf) return;

    for (const auto& r : g_regions) {
        if (!r.readable()) continue;
        if (writableOnly && !r.writable()) continue;
        if (r.size() < patLen) continue;

        uintptr_t a = r.start;
        while (a < r.end && out.size() < maxHits && budget < g_scanBudget) {
            size_t want = (size_t)(r.end - a);
            if (want > kChunk) want = kChunk;
            if (!read(a, buf, want)) { a += want; continue; }
            budget += want;

            size_t lim = (want >= patLen) ? want - patLen : 0;
            if (want >= patLen) {
                uint8_t* p = buf;
                uint8_t* fin = buf + lim + 1;
                while (p < fin) {
                    uint8_t* m = (uint8_t*)memmem(p, (size_t)(buf + want - p), pat, patLen);
                    if (!m) break;
                    uintptr_t addr = a + (uintptr_t)(m - buf);
                    if ((addr & 7) == 0) out.push_back(addr);
                    if (out.size() >= maxHits) break;
                    p = m + 1;
                }
            }
            a += want;
        }
        if (out.size() >= maxHits || budget >= g_scanBudget) break;
    }
    free(buf);
}

// Satu pass untuk banyak target sekaligus.
// Mayoritas word dibuang cuma dengan 2 perbandingan (lo/hi), jadi cepat.
void scanQwordSet(const uint64_t* vals, size_t n, std::vector<PtrHit>& out,
                  size_t maxHits, bool writableOnly) {
    out.clear();
    if (g_fd < 0 || !vals || !n) return;

    uint64_t lo = vals[0], hi = vals[0];
    for (size_t i = 1; i < n; ++i) {
        if (vals[i] < lo) lo = vals[i];
        if (vals[i] > hi) hi = vals[i];
    }

    uint8_t* buf = (uint8_t*)malloc(kChunk);
    if (!buf) return;

    for (const auto& r : g_regions) {
        if (!r.readable()) continue;
        if (writableOnly && !r.writable()) continue;
        if (r.size() < 8) continue;

        uintptr_t a = r.start;
        while (a < r.end && out.size() < maxHits) {
            size_t want = (size_t)(r.end - a);
            if (want > kChunk) want = kChunk;
            if (want < 8) break;
            if (!read(a, buf, want)) { a += want; continue; }

            size_t nwords = want / 8;
            for (size_t k = 0; k < nwords; ++k) {
                uint64_t w;
                memcpy(&w, buf + k * 8, 8);
                if (w < lo || w > hi) continue;
                for (size_t i = 0; i < n; ++i) {
                    if (vals[i] != w) continue;
                    out.push_back({a + k * 8, w});
                    if (out.size() >= maxHits) break;
                    break;
                }
                if (out.size() >= maxHits) break;
            }
            a += nwords * 8;
        }
        if (out.size() >= maxHits) break;
    }
    free(buf);
}

void scanQword(uint64_t value, std::vector<uintptr_t>& out, size_t maxHits, bool writableOnly) {
    scanValue(&value, sizeof(value), out, maxHits, writableOnly);
    if (out.empty()) {
        size_t total = totalBytes(writableOnly);
        logx::add("[mem] scan 0x%lx: 0 hasil | target %zu MB, budget %zu MB%s",
                  (unsigned long)value, total / 1048576, g_scanBudget / 1048576,
                  total > g_scanBudget ? " <-- BUDGET HABIS" : "");
    }
}

// Sama seperti scanQword, tapi byte yang di-mask boleh apa saja.
// Android 15 + MTE bikin pointer di memori game bawa "tag" di byte paling
// atas, jadi nyari pointer balik ke string metadata harus abaikan byte itu
// (mask 0x00FFFFFFFFFFFFFF) -- kalau nggak, hasilnya selalu 0.
void scanQwordMasked(uint64_t value, uint64_t mask, std::vector<uintptr_t>& out,
                     size_t maxHits, bool writableOnly) {
    out.clear();
    if (g_fd < 0) return;

    const uint64_t v  = value & mask;
    const uint64_t lo = v;
    const uint64_t hi = v | ~mask;

    uint8_t* buf = (uint8_t*)malloc(kChunk);
    if (!buf) return;
    size_t budget = 0;

    for (const auto& r : g_regions) {
        if (!r.readable()) continue;
        if (writableOnly && !r.writable()) continue;
        if (r.size() < 8) continue;

        uintptr_t a = r.start;
        while (a + 8 <= r.end && out.size() < maxHits && budget < g_scanBudget) {
            size_t want = (size_t)(r.end - a);
            if (want > kChunk) want = kChunk;
            if (want < 8) break;
            if (!read(a, buf, want)) { a += want; continue; }
            budget += want;

            const size_t nwords = want / 8;
            for (size_t k = 0; k < nwords; ++k) {
                uint64_t w;
                memcpy(&w, buf + k * 8, 8);
                if (w < lo || w > hi) continue;
                if ((w & mask) != v) continue;
                out.push_back(a + k * 8);
                if (out.size() >= maxHits) break;
            }
            a += want;
        }
        if (out.size() >= maxHits || budget >= g_scanBudget) break;
    }
    free(buf);
    if (out.empty())
        logx::add("[mem] scan masked 0x%lx: 0 hasil | budget %zu MB%s",
                  (unsigned long)value, g_scanBudget / 1048576,
                  budget >= g_scanBudget ? " <-- BUDGET HABIS" : "");
}

void scanDword(uint32_t value, std::vector<uintptr_t>& out, size_t maxHits) {
    scanValue(&value, sizeof(value), out, maxHits, true);
}

}  // namespace mem

// ============================================================================
//  bypass.cpp - lihat include/core/bypass.h (termasuk batasannya).
// ============================================================================
#include "core/bypass.h"

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <string>
#include <vector>

#include <dirent.h>
#include <fcntl.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <unistd.h>

#include "core/log.h"
#include "core/mem.h"
#include "core/target.h"

namespace bypass {
namespace {

bool        g_on = true;
std::string g_name = "mali_render";
std::string g_report = "belum dicek";
bool        g_renamed = false;
bool        g_panic = false;
uint64_t    g_lastTickMs = 0;

uint64_t nowMs() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

// file kerja yang jejaknya perlu dirapikan (izin 0600 + nama netral)
const char* kFiles[] = {
    "/data/local/tmp/panxcz_libtool.cfg",
    "/sdcard/panxcz/libtool.log",
    "/sdcard/panxcz/libtool.cfg",
};

void renameSelf() {
    if (g_renamed) return;
    prctl(PR_SET_NAME, g_name.c_str(), 0, 0, 0);
    // timpa cmdline: /proc/self/cmdline itu satu blok argv yang bisa ditulis
    // ulang selama panjangnya nggak nambah.
    FILE* f = fopen("/proc/self/cmdline", "r+");
    if (f) {
        char buf[512] = {0};
        const size_t n = fread(buf, 1, sizeof(buf) - 1, f);
        if (n) {
            memset(buf, 0, n);
            snprintf(buf, n, "%s", g_name.c_str());
            fseek(f, 0, SEEK_SET);
            fwrite(buf, 1, n, f);
            fflush(f);
        }
        fclose(f);
    }
    g_renamed = true;
    logx::add("[stealth] nama proses -> %s", g_name.c_str());
}

void tidyFiles() {
    for (const char* p : kFiles) {
        struct stat st;
        if (stat(p, &st) == 0) chmod(p, 0600);
    }
}

// apakah proses game punya fd yang nunjuk ke kita (atau file kita)?
bool gameLooksAtUs(std::string& what) {
    const int gpid = mem::pid();
    if (!mem::attached() || gpid <= 0) return false;

    char dir[64];
    snprintf(dir, sizeof(dir), "/proc/%d/fd", gpid);
    DIR* d = opendir(dir);
    if (!d) return false;                    // nggak bisa lihat = nggak bisa pastiin
    char self[32];
    snprintf(self, sizeof(self), "/proc/%d", (int)getpid());
    bool hit = false;
    while (struct dirent* e = readdir(d)) {
        if (e->d_name[0] == '.') continue;
        char path[128];
        snprintf(path, sizeof(path), "%s/%s", dir, e->d_name);
        char tgt[512] = {0};
        const ssize_t n = readlink(path, tgt, sizeof(tgt) - 1);
        if (n <= 0) continue;
        if (strstr(tgt, self) || strstr(tgt, "panxcz") || strstr(tgt, "frida") ||
            strstr(tgt, "gameguardian")) {
            what = tgt;
            hit = true;
            break;
        }
    }
    closedir(d);
    if (hit) return true;

    // jalur kedua: apakah maps game punya mapping ke file kita?
    for (const auto& r : mem::regions()) {
        if (r.name.empty()) continue;
        if (r.name.find("panxcz") != std::string::npos) {
            what = r.name;
            return true;
        }
    }
    return false;
}

}  // namespace

std::string procName() { return g_name; }
void setProcName(const std::string& n) {
    if (n.empty()) return;
    g_name = n;
    g_renamed = false;      // biar di-apply ulang
}
bool& enabled() { return g_on; }
const std::string& lastReport() { return g_report; }
void setLastReport(const std::string& s) { g_report = s; }

bool apply() {
    if (!g_on) {
        g_report = "stealth dimatikan";
        return false;
    }
    renameSelf();
    tidyFiles();
    g_report = "aktif (proses: " + g_name + ")";
    return true;
}

void setTitleFromArgv(int argc, char** argv) {
    if (argc < 1 || !argv || !argv[0] || g_name.empty()) return;
    const size_t cap = strlen(argv[0]);
    if (g_name.size() > cap) {
        g_report = "argv nggak bisa ditimpa (nama samaran kepanjangan)";
        return;
    }
    memset(argv[0], 0, cap);
    memcpy(argv[0], g_name.c_str(), g_name.size());
    for (int i = 1; i < argc; ++i) {
        if (!argv[i]) continue;
        const size_t n = strlen(argv[i]);
        if (n) memset(argv[i], 0, n);      // argumen bisa bocorin package/lib
    }
    logx::add("[stealth] argv ditimpa -> %s", g_name.c_str());
}

bool scanOnce(bool exitOnHit) {
    if (!g_on) { g_report = "stealth dimatikan"; return false; }
    std::string what;
    if (!gameLooksAtUs(what)) {
        g_report = "aman";
        return false;
    }
    g_report = "GAME NGE-CEK KITA: " + what;
    logx::add("[stealth] %s", g_report.c_str());
    if (exitOnHit) {
        g_panic = true;      // main loop yang nge-lepas overlay & keluar
        logx::add("[stealth] panic: lepas overlay + keluar (main loop)");
    }
    return true;
}

bool panicRequested() { return g_panic; }
void clearPanic() { g_panic = false; }

void tick() {
    if (!g_on) return;
    const uint64_t t = nowMs();
    if (t - g_lastTickMs < 10000) return;    // tiap ~10 detik
    g_lastTickMs = t;
    scanOnce(true);
}

}  // namespace bypass

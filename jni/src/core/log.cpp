#include "core/log.h"

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <pthread.h>
#include <android/log.h>

namespace logx {

static const int   kMax = 256;
static char        g_lines[kMax][512];
static int         g_w = 0;      // posisi tulis
static int         g_n = 0;      // jumlah terisi
static pthread_mutex_t g_mtx = PTHREAD_MUTEX_INITIALIZER;
static char        g_path[256] = {0};
static bool        g_file = false;

void clear() {
    pthread_mutex_lock(&g_mtx);
    g_w = g_n = 0;
    pthread_mutex_unlock(&g_mtx);
}

static void push(const char* s) {
    pthread_mutex_lock(&g_mtx);
    time_t t = time(nullptr);
    struct tm tmv;
    localtime_r(&t, &tmv);
    snprintf(g_lines[g_w], sizeof(g_lines[0]), "%02d:%02d:%02d %s",
             tmv.tm_hour, tmv.tm_min, tmv.tm_sec, s);
    __android_log_print(ANDROID_LOG_INFO, "panxcz_libtool", "%s", g_lines[g_w]);
    g_w = (g_w + 1) % kMax;
    if (g_n < kMax) ++g_n;
    pthread_mutex_unlock(&g_mtx);

    if (g_file && g_path[0]) {
        FILE* f = fopen(g_path, "a");
        if (f) { fprintf(f, "%s\n", s); fclose(f); }
    }
}

void add(const char* fmt, ...) {
    char buf[460];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    push(buf);
}

const char* line(int backIndex) {
    static char tmp[512];
    pthread_mutex_lock(&g_mtx);
    int idx = (g_w - 1 - backIndex + kMax * 2) % kMax;
    if (backIndex < 0 || backIndex >= g_n) tmp[0] = 0;
    else { strncpy(tmp, g_lines[idx], sizeof(tmp) - 1); tmp[sizeof(tmp) - 1] = 0; }
    pthread_mutex_unlock(&g_mtx);
    return tmp;
}

int count() {
    pthread_mutex_lock(&g_mtx);
    int n = g_n;
    pthread_mutex_unlock(&g_mtx);
    return n;
}

void setFile(const char* path) {
    if (!path || !*path) { g_file = false; g_path[0] = 0; return; }
    strncpy(g_path, path, sizeof(g_path) - 1);
    g_path[sizeof(g_path) - 1] = 0;
    g_file = true;
    FILE* f = fopen(g_path, "w");
    if (f) fclose(f);
}

bool fileEnabled() { return g_file; }

}  // namespace logx

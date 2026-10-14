#include "core/keys.h"
#include "core/log.h"

#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>
#include <pthread.h>
#include <linux/input.h>
#include <sys/ioctl.h>

namespace keys {

static int  g_volUp = 0;
static int  g_volDown = 0;
static bool g_run = false;
static pthread_t g_th;
static pthread_mutex_t g_mtx = PTHREAD_MUTEX_INITIALIZER;
static const int kMaxFd = 8;
static int  g_fd[kMaxFd];
static int  g_nfd = 0;

static bool hasVolume(int fd) {
    uint8_t bits[(KEY_MAX / 8) + 2];
    memset(bits, 0, sizeof(bits));
    if (ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(bits)), bits) < 0) return false;
    auto test = [&](int code) {
        return (bits[code / 8] >> (code % 8)) & 1;
    };
    return test(KEY_VOLUMEUP) || test(KEY_VOLUMEDOWN);
}

static void* worker(void*) {
    struct input_event ev;
    while (g_run) {
        bool got = false;
        for (int i = 0; i < g_nfd; ++i) {
            if (g_fd[i] < 0) continue;
            ssize_t n = read(g_fd[i], &ev, sizeof(ev));
            if (n != (ssize_t)sizeof(ev)) continue;
            got = true;
            if (ev.type != EV_KEY || ev.value != 1) continue;
            pthread_mutex_lock(&g_mtx);
            if (ev.code == KEY_VOLUMEUP)   ++g_volUp;
            if (ev.code == KEY_VOLUMEDOWN) ++g_volDown;
            pthread_mutex_unlock(&g_mtx);
        }
        if (!got) usleep(20000);
    }
    return nullptr;
}

void init() {
    if (g_run) return;
    DIR* d = opendir("/dev/input");
    if (!d) {
        logx::add("[keys] /dev/input tidak bisa dibuka");
        return;
    }
    struct dirent* e;
    while ((e = readdir(d)) != nullptr && g_nfd < kMaxFd) {
        if (strncmp(e->d_name, "event", 5) != 0) continue;
        char path[128];
        snprintf(path, sizeof(path), "/dev/input/%s", e->d_name);
        int fd = open(path, O_RDONLY | O_NONBLOCK);
        if (fd < 0) continue;
        if (!hasVolume(fd)) { close(fd); continue; }
        g_fd[g_nfd++] = fd;
    }
    closedir(d);
    if (!g_nfd) {
        logx::add("[keys] device volume tidak ketemu");
        return;
    }
    g_run = true;
    pthread_create(&g_th, nullptr, worker, nullptr);
    logx::add("[keys] volume key aktif (%d fd)", g_nfd);
}

void stop() {
    if (!g_run) return;
    g_run = false;
    pthread_join(g_th, nullptr);
    for (int i = 0; i < g_nfd; ++i)
        if (g_fd[i] >= 0) { close(g_fd[i]); g_fd[i] = -1; }
    g_nfd = 0;
}

int takeVolUp() {
    pthread_mutex_lock(&g_mtx);
    int v = g_volUp;
    g_volUp = 0;
    pthread_mutex_unlock(&g_mtx);
    return v;
}

int takeVolDown() {
    pthread_mutex_lock(&g_mtx);
    int v = g_volDown;
    g_volDown = 0;
    pthread_mutex_unlock(&g_mtx);
    return v;
}

}  // namespace keys

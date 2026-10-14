#include "core/abort.h"

#include <atomic>
#include <chrono>
#include <cstring>
#include <mutex>
#include <string>

namespace abortx {
namespace {

std::atomic<bool> g_flag{false};

std::mutex   g_lock;
std::string  g_reason;
std::string  g_phase;
double       g_beat = 0.0;      // nowSec() denyut terakhir (0 = belum ada)
float        g_base = 0.0f;     // awal slot progres
float        g_len  = 1.0f;     // panjang slot progres
std::atomic<float> g_frac{-1.0f};

// Waktu monotopik: pakai steady_clock, BUKAN waktu dinding -- jam device bisa
// diubah user/game dan itu bikin hitungan "sudah berapa lama" jadi ngawur.
double mono() {
    using clock = std::chrono::steady_clock;
    static const clock::time_point t0 = clock::now();
    return std::chrono::duration<double>(clock::now() - t0).count();
}

}  // namespace

void request(const char* reason) {
    std::lock_guard<std::mutex> lk(g_lock);
    g_flag.store(true, std::memory_order_relaxed);
    if (reason && *reason) g_reason = reason;
}

void clear() {
    std::lock_guard<std::mutex> lk(g_lock);
    g_flag.store(false, std::memory_order_relaxed);
    g_reason.clear();
    g_phase.clear();
    g_beat = 0.0;
    g_base = 0.0f;
    g_len  = 1.0f;
    g_frac.store(-1.0f, std::memory_order_relaxed);
}

bool requested() { return g_flag.load(std::memory_order_relaxed); }

// Salin ke buffer per-thread dulu: kalau pointer-nya dikembalikan sambil
// nyimpen lock, isinya bisa berubah (string-nya di-assign dari thread lain)
// dan pemanggilnya baca memori yang udah dibebasin.
const char* reason() {
    static thread_local std::string copy;
    std::lock_guard<std::mutex> lk(g_lock);
    copy = g_reason;
    return copy.c_str();
}

void beat(const char* phase, float frac) {
    std::lock_guard<std::mutex> lk(g_lock);
    g_beat = mono();
    if (phase && *phase) g_phase = phase;
    if (frac >= 0.0f) {
        float f = frac < 0.0f ? 0.0f : (frac > 1.0f ? 1.0f : frac);
        g_frac.store(g_base + g_len * f, std::memory_order_relaxed);
    }
}

void span(float base, float len) {
    std::lock_guard<std::mutex> lk(g_lock);
    g_base = base < 0.0f ? 0.0f : (base > 1.0f ? 1.0f : base);
    g_len  = len  < 0.0f ? 0.0f : (len  > 1.0f ? 1.0f : len);
}

float progress() { return g_frac.load(std::memory_order_relaxed); }

double beatAgeSec() {
    std::lock_guard<std::mutex> lk(g_lock);
    if (g_beat <= 0.0) return 0.0;              // belum pernah nge-denyut
    return mono() - g_beat;
}

const char* phase() {
    static thread_local std::string copy;
    std::lock_guard<std::mutex> lk(g_lock);
    copy = g_phase;
    return copy.c_str();
}

double nowSec() { return mono(); }

}  // namespace abortx

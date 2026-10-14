#include "core/scanner.h"
#include "core/abort.h"
#include "core/mem.h"
#include "core/log.h"

#include <atomic>
#include <cstring>
#include <cstdlib>
#include <cmath>

namespace scan {

static std::vector<Hit> g_hits;
static Type             g_type = Type::I32;

// Scan awal bisa lama (ratusan MB). Dulu dia jalan DI THREAD RENDER: layar
// beku dan tombol Stop nggak bisa diklik -- itulah keluhan "scan nggak bisa
// di-stop". Sekarang: UI manggil di thread sendiri, dan loop di sini ngecek
// flag ini tiap blok.
static std::atomic<bool> g_busy{false};
static std::atomic<bool> g_abort{false};

void requestAbort() { g_abort.store(true, std::memory_order_relaxed); }
void clearAbort()   { g_abort.store(false, std::memory_order_relaxed); }
bool abortRequested(){ return g_abort.load(std::memory_order_relaxed); }
bool busy()         { return g_busy.load(std::memory_order_relaxed); }

size_t typeSize(Type t) {
    switch (t) {
        case Type::I8:  return 1;
        case Type::I16: return 2;
        case Type::I32: return 4;
        case Type::I64: return 8;
        case Type::F32: return 4;
        case Type::F64: return 8;
    }
    return 4;
}

const char* typeName(Type t) {
    switch (t) {
        case Type::I8:  return "byte";
        case Type::I16: return "short";
        case Type::I32: return "int";
        case Type::I64: return "long";
        case Type::F32: return "float";
        case Type::F64: return "double";
    }
    return "?";
}

static void encode(Type t, double v, uint8_t out[8]) {
    memset(out, 0, 8);
    switch (t) {
        case Type::I8:  { int8_t  x = (int8_t)v;   memcpy(out, &x, 1); break; }
        case Type::I16: { int16_t x = (int16_t)v;  memcpy(out, &x, 2); break; }
        case Type::I32: { int32_t x = (int32_t)v;  memcpy(out, &x, 4); break; }
        case Type::I64: { int64_t x = (int64_t)v;  memcpy(out, &x, 8); break; }
        case Type::F32: { float   x = (float)v;    memcpy(out, &x, 4); break; }
        case Type::F64: { double  x = v;           memcpy(out, &x, 8); break; }
    }
}

static void decode(Type t, const uint8_t* in, double& v) {
    switch (t) {
        case Type::I8:  { int8_t  x; memcpy(&x, in, 1); v = x; break; }
        case Type::I16: { int16_t x; memcpy(&x, in, 2); v = x; break; }
        case Type::I32: { int32_t x; memcpy(&x, in, 4); v = x; break; }
        case Type::I64: { int64_t x; memcpy(&x, in, 8); v = x; break; }
        case Type::F32: { float   x; memcpy(&x, in, 4); v = x; break; }
        case Type::F64: { double  x; memcpy(&x, in, 8); v = x; break; }
    }
}

bool readAt(uintptr_t addr, Type t, double& out) {
    uint8_t buf[8];
    if (!mem::read(addr, buf, typeSize(t))) return false;
    decode(t, buf, out);
    return true;
}

bool writeAt(uintptr_t addr, Type t, double v) {
    uint8_t buf[8];
    encode(t, v, buf);
    return mem::write(addr, buf, typeSize(t));
}

// Baca ULANG nilai di alamat hasil scan (tanpa pindai ulang memori). Dipakai
// tombol "Start scan"/"Stop scan" di UI: nilainya hidup, jadi kelihatan mana
// yang berubah -- tapi cuma `max` hasil pertama biar tetap ringan per frame.
size_t refreshValues(Type t, size_t max) {
    size_t n = 0;
    for (auto& h : g_hits) {
        if (n >= max) break;
        double v = 0;
        if (readAt(h.addr, t, v)) h.value = v;
        ++n;
    }
    return n;
}

const std::vector<Hit>& hits() { return g_hits; }
size_t count() { return g_hits.size(); }
void   clear() { g_hits.clear(); }

// ----------------------------------------------------------------------------
// scan awal
// ----------------------------------------------------------------------------
static const size_t kChunk = 4u * 1024u * 1024u;
static const size_t kBudget = 768u * 1024u * 1024u;

bool first(Type t, double value, size_t maxHits) {
    g_hits.clear();
    g_type = t;
    g_abort.store(false, std::memory_order_relaxed);
    g_busy.store(true, std::memory_order_relaxed);
    abortx::beat("scan memori");
    // pastikan "busy" selalu kelepas walau keluar lewat return mana pun
    struct Unbusy {
        ~Unbusy() { g_busy.store(false, std::memory_order_relaxed); abortx::beat("scan selesai"); }
    } unbusy;
    size_t sz = typeSize(t);
    uint8_t pat[8];
    encode(t, value, pat);

    uint8_t* buf = (uint8_t*)malloc(kChunk + sz);
    if (!buf) { logx::add("[scan] malloc gagal"); return false; }

    size_t budget = 0;
    bool stopped = false;
    for (const auto& r : mem::regions()) {
        if (!r.readable() || !r.writable()) continue;
        if (r.size() < sz) continue;
        uintptr_t a = r.start;
        while (a < r.end && g_hits.size() < maxHits && budget < kBudget) {
            if (g_abort.load(std::memory_order_relaxed)) { stopped = true; break; }
            abortx::beat("scan memori",
                         kBudget ? (float)((double)budget / (double)kBudget) : -1.0f);
            size_t want = (size_t)(r.end - a);
            if (want > kChunk) want = kChunk;
            if (!mem::read(a, buf, want)) { a += want; continue; }
            budget += want;

            if (want >= sz) {
                uint8_t* p = buf;
                uint8_t* fin = buf + want - sz + 1;
                while (p < fin) {
                    uint8_t* m = (uint8_t*)memmem(p, (size_t)(fin - p), pat, sz);
                    if (!m) break;
                    Hit h;
                    h.addr = a + (uintptr_t)(m - buf);
                    h.value = value;
                    g_hits.push_back(h);
                    if (g_hits.size() >= maxHits) break;
                    p = m + 1;
                }
            }
            a += want;
        }
        if (stopped || g_hits.size() >= maxHits || budget >= kBudget) break;
    }
    free(buf);
    logx::add("[scan] tipe=%s value=%.4g -> %zu hasil%s (budget %zu MB)", typeName(t), value,
              g_hits.size(), stopped ? " [DIHENTIKAN]" : "", budget >> 20);
    return !g_hits.empty();
}

// ----------------------------------------------------------------------------
// refine
// ----------------------------------------------------------------------------
bool refine(int mode, double value) {
    // refine juga "busy": UI perlu tahu kapan hasilnya udah final (dan worker
    // di UI nggak boleh dianggap mati padahal masih nyaring hasil).
    g_busy.store(true, std::memory_order_relaxed);
    struct Unbusy {
        ~Unbusy() { g_busy.store(false, std::memory_order_relaxed); }
    } unbusy;

    std::vector<Hit> keep;
    keep.reserve(g_hits.size());

    size_t n = 0;
    for (const auto& h : g_hits) {
        if ((++n & 511) == 0 && g_abort.load(std::memory_order_relaxed)) break;
        double cur = 0;
        if (!readAt(h.addr, g_type, cur)) continue;

        bool ok = false;
        switch (mode) {
            case kExact:     ok = (fabs(cur - value) < 1e-4); break;
            case kChanged:   ok = (fabs(cur - h.value) > 1e-4); break;
            case kUnchanged: ok = (fabs(cur - h.value) <= 1e-4); break;
            case kIncreased: ok = (cur > h.value); break;
            case kDecreased: ok = (cur < h.value); break;
            default: ok = false;
        }
        if (!ok) continue;

        Hit n = h;
        n.value = cur;
        keep.push_back(n);
    }
    g_hits.swap(keep);
    logx::add("[scan] refine mode=%d -> %zu hasil", mode, g_hits.size());
    return !g_hits.empty();
}

bool setValue(size_t idx, double v) {
    if (idx >= g_hits.size()) return false;
    if (!writeAt(g_hits[idx].addr, g_type, v)) return false;
    g_hits[idx].value = v;
    return true;
}

bool setFrozen(size_t idx, bool on, double v) {
    if (idx >= g_hits.size()) return false;
    g_hits[idx].frozen = on;
    g_hits[idx].freezeVal = v;
    if (on) writeAt(g_hits[idx].addr, g_type, v);
    return true;
}

int frozenCount() {
    int n = 0;
    for (const auto& h : g_hits) if (h.frozen) ++n;
    return n;
}

void tick() {
    for (auto& h : g_hits) {
        if (!h.frozen) continue;
        writeAt(h.addr, g_type, h.freezeVal);
    }
}

}  // namespace scan

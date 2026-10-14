#include "core/safe.h"
#include "core/mem.h"

#include <cstring>
#include <cmath>

// ============================================================================
//  Cara kerja Safe* (SYBO) -- ini bukan tebakan, sudah dibuktikan dari data
//  nyata di HP (instance RunSessionVariable milik Coins):
//
//      _flooredValue (int, +0x10)          = 88
//      _offset (double)                    = -189.0
//      _value  (double)                    = -101.0
//      _value - _offset = -101 - (-189)    = 88      <-- cocok
//
//  PENTING: transform-nya aritmetika ARITMETIKA BIASA (double/float), BUKAN
//  operasi bit. Jadi _value = nilaiAsli + _offset, dan membacanya = _value -
//  _offset sebagai double. Kalau dihitung sebagai bit-pattern integer, hasilnya
//  sampah (-1.7e304 / -0.00) -- itu bug yang sempat ada di sini.
// ============================================================================

namespace safe {

// SUB = _value - _offset  (terbukti cocok di game ini)
static Mode g_mode = kSub;

const char* modeName(Mode m) { return modeName((int)m); }

const char* modeName(int m) {
    switch (m) {
        case kXor: return "XOR";
        case kAdd: return "ADD";
        case kSub: return "SUB";
        case kRaw: return "RAW";
    }
    return "?";
}

Mode mode() { return g_mode; }
void setMode(Mode m) {
    if (m >= 0 && m < kModeCount) g_mode = m;
}

// ----------------------------------------------------------------------------
// baca mentah
// ----------------------------------------------------------------------------
static bool load4(uintptr_t a, uint32_t& ob, uint32_t& vb) {
    uint32_t buf[2] = {0, 0};
    if (!mem::read(a, buf, sizeof(buf))) return false;
    ob = buf[0];
    vb = buf[1];
    return true;
}

static bool load8(uintptr_t a, uint64_t& ob, uint64_t& vb) {
    uint64_t buf[2] = {0, 0};
    if (!mem::read(a, buf, sizeof(buf))) return false;
    ob = buf[0];
    vb = buf[1];
    return true;
}

// ----------------------------------------------------------------------------
// dekode: int (aritmetika int)
// ----------------------------------------------------------------------------
static uint32_t decBits4(uint32_t vb, uint32_t ob, Mode m) {
    switch (m) {
        case kXor: return vb ^ ob;
        case kAdd: return (uint32_t)((int32_t)vb + (int32_t)ob);
        case kSub: return (uint32_t)((int32_t)vb - (int32_t)ob);
        case kRaw: return vb;
    }
    return vb;
}

static uint32_t encBits4(uint32_t value, uint32_t ob, Mode m) {
    switch (m) {
        case kXor: return value ^ ob;
        case kAdd: return (uint32_t)((int32_t)value - (int32_t)ob);
        case kSub: return (uint32_t)((int32_t)value + (int32_t)ob);
        case kRaw: return value;
    }
    return value;
}

// ----------------------------------------------------------------------------
// dekode: float (aritmetika float!)
// ----------------------------------------------------------------------------
static float decFloat4(uint32_t vb, uint32_t ob, Mode m) {
    float v = 0, o = 0;
    memcpy(&v, &vb, 4);
    memcpy(&o, &ob, 4);
    switch (m) {
        case kXor: {
            uint32_t x = vb ^ ob;
            float f = 0;
            memcpy(&f, &x, 4);
            return f;
        }
        case kAdd: return v + o;
        case kSub: return v - o;
        case kRaw: return v;
    }
    return v;
}

static uint32_t encFloat4(float value, uint32_t ob, Mode m) {
    float o = 0;
    memcpy(&o, &ob, 4);
    float r = (m == kSub) ? (value + o)
            : (m == kAdd) ? (value - o)
            : value;
    if (m == kXor) {
        uint32_t vbits = 0;
        memcpy(&vbits, &value, 4);
        return vbits ^ ob;
    }
    uint32_t bits = 0;
    memcpy(&bits, &r, 4);
    return bits;
}

// ----------------------------------------------------------------------------
// dekode: double (aritmetika double!)
// ----------------------------------------------------------------------------
static double decDouble8(uint64_t vb, uint64_t ob, Mode m) {
    double v = 0, o = 0;
    memcpy(&v, &vb, 8);
    memcpy(&o, &ob, 8);
    switch (m) {
        case kXor: {
            uint64_t x = vb ^ ob;
            double d = 0;
            memcpy(&d, &x, 8);
            return d;
        }
        case kAdd: return v + o;
        case kSub: return v - o;
        case kRaw: return v;
    }
    return v;
}

static uint64_t encDouble8(double value, uint64_t ob, Mode m) {
    double o = 0;
    memcpy(&o, &ob, 8);
    if (m == kXor) {
        uint64_t vbits = 0;
        memcpy(&vbits, &value, 8);
        return vbits ^ ob;
    }
    const double r = (m == kSub) ? (value + o)
                   : (m == kAdd) ? (value - o)
                   : value;
    uint64_t bits = 0;
    memcpy(&bits, &r, 8);
    return bits;
}

// ----------------------------------------------------------------------------
// API
// ----------------------------------------------------------------------------
bool readInt(uintptr_t a, int32_t& out) {
    uint32_t ob, vb;
    if (!load4(a, ob, vb)) return false;
    out = (int32_t)decBits4(vb, ob, g_mode);
    return true;
}

bool writeInt(uintptr_t a, int32_t v) {
    uint32_t ob, vb;
    if (!load4(a, ob, vb)) return false;
    uint32_t enc = encBits4((uint32_t)v, ob, g_mode);
    return mem::write(a + 4, &enc, 4);
}

bool readFloat(uintptr_t a, float& out) {
    uint32_t ob, vb;
    if (!load4(a, ob, vb)) return false;
    out = decFloat4(vb, ob, g_mode);
    return true;
}

bool writeFloat(uintptr_t a, float v) {
    uint32_t ob, vb;
    if (!load4(a, ob, vb)) return false;
    uint32_t enc = encFloat4(v, ob, g_mode);
    return mem::write(a + 4, &enc, 4);
}

bool readDouble(uintptr_t a, double& out) {
    uint64_t ob, vb;
    if (!load8(a, ob, vb)) return false;
    out = decDouble8(vb, ob, g_mode);
    return true;
}

bool writeDouble(uintptr_t a, double v) {
    uint64_t ob, vb;
    if (!load8(a, ob, vb)) return false;
    uint64_t enc = encDouble8(v, ob, g_mode);
    return mem::write(a + 8, &enc, 8);
}

// ----------------------------------------------------------------------------
// kalibrasi
// ----------------------------------------------------------------------------
void candidates4(uintptr_t a, double out[kModeCount]) {
    uint32_t ob = 0, vb = 0;
    for (int i = 0; i < kModeCount; ++i) out[i] = 0;
    if (!load4(a, ob, vb)) return;
    for (int i = 0; i < kModeCount; ++i)
        out[i] = (double)(int32_t)decBits4(vb, ob, (Mode)i);
}

void candidates4f(uintptr_t a, double out[kModeCount]) {
    uint32_t ob = 0, vb = 0;
    for (int i = 0; i < kModeCount; ++i) out[i] = 0;
    if (!load4(a, ob, vb)) return;
    for (int i = 0; i < kModeCount; ++i)
        out[i] = (double)decFloat4(vb, ob, (Mode)i);
}

void candidates8(uintptr_t a, double out[kModeCount]) {
    uint64_t ob = 0, vb = 0;
    for (int i = 0; i < kModeCount; ++i) out[i] = 0;
    if (!load8(a, ob, vb)) return;
    for (int i = 0; i < kModeCount; ++i)
        out[i] = decDouble8(vb, ob, (Mode)i);
}

}  // namespace safe

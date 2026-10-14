// ============================================================================
//  scanner.h - scanner value manual (scan / filter / edit / freeze)
// ============================================================================
#pragma once

#include <cstdint>
#include <cstddef>
#include <vector>

namespace scan {

enum class Type { I8 = 0, I16, I32, I64, F32, F64 };

struct Hit {
    uintptr_t addr      = 0;
    double    value     = 0;
    bool      frozen    = false;
    double    freezeVal = 0;
};

size_t      typeSize(Type t);
const char* typeName(Type t);

// mode refine
enum Refine {
    kExact = 0, kChanged, kUnchanged, kIncreased, kDecreased,
};

bool first(Type t, double value, size_t maxHits = 40000);
bool refine(int mode, double value = 0);

// --- Stop ------------------------------------------------------------------
// Scan awal itu berat (ratusan MB) dan dulu jalan di thread render -- jadi UI
// beku dan nggak bisa dihentikan ("scan nggak bisa di-stop"). Sekarang scan
// ngecek flag ini di tiap blok, dan UI menjalankannya di thread sendiri.
void requestAbort();
void clearAbort();
bool abortRequested();
bool busy();                       // true selama first()/refine() jalan
const std::vector<Hit>& hits();
size_t count();
void clear();

bool readAt(uintptr_t addr, Type t, double& out);
bool writeAt(uintptr_t addr, Type t, double v);
bool setValue(size_t idx, double v);
bool setFrozen(size_t idx, bool on, double v);
size_t refreshValues(Type t, size_t max = 512);   // baca ulang nilai hasil scan
void tick();                       // dipanggil tiap frame, nulis ulang yg di-freeze
int  frozenCount();

}  // namespace scan

// ============================================================================
//  safe.h - decode/encode struct "Safe*" milik SYBO
//
//  Layout (dari dump, relatif ke awal struct):
//      SafeInt    { int    _offset; int    _value; }   -> 8 byte  (offset 0x0 / 0x4)
//      SafeFloat  { float  _offset; float  _value; }   -> 8 byte
//      SafeDouble { double _offset; double _value; }   -> 16 byte (offset 0x0 / 0x8)
//      SafeLong   { long   _offset; long   _value; }   -> 16 byte
//
//  Nilai asli tidak disimpan apa adanya (game pakai obfuscation). Transform-nya
//  belum pasti, jadi mode bisa dipilih dan dikalibrasi dari tab Config:
//      XOR  -> bits(_value) ^ bits(_offset)     (paling umum)
//      ADD  -> _value + _offset
//      SUB  -> _value - _offset
//      RAW  -> _value apa adanya
// ============================================================================
#pragma once

#include <cstdint>

namespace safe {

enum Mode { kXor = 0, kAdd, kSub, kRaw, kModeCount };

const char* modeName(Mode m);
const char* modeName(int m);
Mode mode();
void setMode(Mode m);

// ---- 4 byte (SafeInt / SafeFloat) ----
bool readInt   (uintptr_t structAddr, int32_t& out);
bool writeInt  (uintptr_t structAddr, int32_t v);
bool readFloat (uintptr_t structAddr, float& out);
bool writeFloat(uintptr_t structAddr, float v);

// ---- 8 byte (SafeDouble / SafeLong) ----
bool readDouble (uintptr_t structAddr, double& out);
bool writeDouble(uintptr_t structAddr, double v);

// ---- kalibrasi ----
// isi out[0..3] dengan hasil tiap mode (urut: XOR, ADD, SUB, RAW)
void candidates4 (uintptr_t structAddr, double out[kModeCount]);   // tafsir SafeInt
void candidates4f(uintptr_t structAddr, double out[kModeCount]);   // tafsir SafeFloat
void candidates8 (uintptr_t structAddr, double out[kModeCount]);   // tafsir SafeDouble

}  // namespace safe

// ============================================================================
//  memview.h - hexdump + disassembler AArch64 buat lihat kode method dari luar.
//
//  ASAL: Android-LibTool-New/app/src/main/jni/Tool/ClassesTab.cpp
//  (ClassesTab::HexView / AssemblyView) -- di sana bacanya langsung dari
//  `method->methodPointer` (in-process) + Capstone via frida-gum. Di sini:
//  byte-nya dibaca tool external lewat /proc/<pid>/mem, decode-nya pakai
//  decoder kecil sendiri (tanpa Capstone, tanpa frida), jadi nggak nambah
//  dependensi native apa pun.
//
//  Decoder-nya SENGAJA cuma mencakup instruksi yang lazim nongol di
//  prolog/body/stub method (ret, movz/movk/movn, add/sub, branch, ldr/str,
//  ldp/stp, adrp/adr, br/blr, cbz/cbnz, tbz/tbnz, mov-reg, svc). Sisanya
//  dicetak ".word" -- jujur ketimbang ngarang mnemonic.
// ============================================================================
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace memview {

// Klasik 16 byte/baris: "0xADDR: XX XX ... |ascii|".
std::string hexdump(uintptr_t addr, const uint8_t* data, size_t n);

// Decode SATU instruksi AArch64 di `base + off`. Tulis baris
// "+0xOFF  HEX  mnemonic ops" (atau ".word" kalau nggak dikenal) ke `out`.
// Balikin 4 (selalu maju 4 byte).
size_t disasmOne(const uint8_t* code, uintptr_t base, size_t off, size_t avail,
                 std::string& out);

// Decode sampai `maxIns` instruksi. Berhenti lebih cepat kalau ketemu
// ret/br (cabang tak bersyarat) -- sama kayak AssemblyView yang berhenti di
// ret/bx/pop. Balikin hasil per baris.
std::vector<std::string> disasm(const uint8_t* code, size_t n, uintptr_t base,
                                size_t maxIns = 32);

}  // namespace memview

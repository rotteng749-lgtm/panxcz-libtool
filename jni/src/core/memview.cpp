#include "core/memview.h"

#include <cstdio>
#include <cstring>

namespace memview {

std::string hexdump(uintptr_t addr, const uint8_t* data, size_t n) {
    std::string o;
    char b[96];
    for (size_t i = 0; i < n; i += 16) {
        size_t row = n - i < 16 ? n - i : 16;
        snprintf(b, sizeof(b), "0x%lx: ", (unsigned long)(addr + i));
        o += b;
        for (size_t j = 0; j < 16; ++j) {
            if (j < row) {
                snprintf(b, sizeof(b), "%02X ", data[i + j]);
                o += b;
            } else {
                o += "   ";
            }
        }
        o += " |";
        for (size_t j = 0; j < row; ++j) {
            uint8_t c = data[i + j];
            o += (c >= 32 && c < 127) ? (char)c : '.';
        }
        o += "|\n";
    }
    return o;
}

namespace {

const char* reg(bool is64, unsigned n) {
    static const char* w[] = {
        "w0","w1","w2","w3","w4","w5","w6","w7","w8","w9","w10",
        "w11","w12","w13","w14","w15","w16","w17","w18","w19","w20",
        "w21","w22","w23","w24","w25","w26","w27","w28","w29","w30","wsp"
    };
    static const char* x[] = {
        "x0","x1","x2","x3","x4","x5","x6","x7","x8","x9","x10",
        "x11","x12","x13","x14","x15","x16","x17","x18","x19","x20",
        "x21","x22","x23","x24","x25","x26","x27","x28","x29","x30","sp"
    };
    if (n > 31) n = 31;
    return is64 ? x[n] : w[n];
}

uint32_t fetch(const uint8_t* code, size_t off) {
    uint32_t v = 0;
    memcpy(&v, code + off, 4);
    return v;
}

int64_t signExt(uint32_t v, int bits) {
    const int sh = 32 - bits;
    return (int64_t)(((int32_t)(v << sh)) >> sh);
}

}  // namespace

size_t disasmOne(const uint8_t* code, uintptr_t base, size_t off, size_t avail,
                 std::string& out) {
    char b[192];
    const uintptr_t pc = base + off;
    snprintf(b, sizeof(b), "+0x%zX  ", off);
    out = b;
    if (avail < 4) { out += "(habis)"; return avail; }
    const uint32_t op = fetch(code, off);
    snprintf(b, sizeof(b), "%08X  ", op);
    out += b;
    std::string m;
    const unsigned rd = op & 31, rn = (op >> 5) & 31, rm = (op >> 16) & 31;

    if (op == 0xD503201F) m = "nop";
    else if ((op & 0xFFFFF01F) == 0xD503201F) {
        // HINT (dan PAC/AUT/RET ke variant HINT) -- sangat umum di kode arm64
        // modern (Android/Free Fire pakai pointer auth hampir di tiap fungsi).
        switch (op) {
            case 0xD503233F: m = "paciasp"; break;
            case 0xD503237F: m = "pacibsp"; break;
            case 0xD50323BF: m = "autiasp"; break;
            case 0xD50323FF: m = "autibsp"; break;
            case 0xD50320BF: m = "retab";   break;
            default:
                snprintf(b, sizeof(b), "hint #0x%X", (op >> 5) & 0xFFFF);
                m = b;
        }
    }
    else if ((op & 0xFFFFFC1F) == 0xD65F0000) {
        // ret (umumnya x30)
        m = "ret";
        if (rd != 30) { snprintf(b, sizeof(b), "ret %s", reg(true, rd)); m = b; }
    } else if ((op & 0xFFDFFC1F) == 0xD61F0000) {
        snprintf(b, sizeof(b), "br %s", reg(true, rn)); m = b;
    } else if ((op & 0xFFDFFC1F) == 0xD63F0000) {
        snprintf(b, sizeof(b), "blr %s", reg(true, rn)); m = b;
    } else if ((op & 0x7F800000) == 0x52800000 || (op & 0x7F800000) == 0xD2800000 ||
               (op & 0x7F800000) == 0x12800000 || (op & 0x7F800000) == 0x92800000 ||
               (op & 0x7F800000) == 0x72800000 || (op & 0x7F800000) == 0xF2800000) {
        // movz / movn / movk (bedain dari byte teratas)
        const bool is64 = (op >> 31) & 1;
        const unsigned hw = (op >> 21) & 3;
        const unsigned imm = (op >> 5) & 0xFFFF;
        const uint8_t top = (op >> 24) & 0xFF;
        const char* nm = (top == 0x52 || top == 0xD2) ? "movz"
                       : (top == 0x12 || top == 0x92) ? "movn" : "movk";
        if (hw) snprintf(b, sizeof(b), "%s %s, #0x%X, lsl #%u", nm, reg(is64, rd), imm, hw * 16);
        else snprintf(b, sizeof(b), "%s %s, #0x%X", nm, reg(is64, rd), imm);
        m = b;
    } else if ((op & 0x7F800000) == 0x11000000 || (op & 0x7F800000) == 0x91000000 ||
               (op & 0x7F800000) == 0x51000000 || (op & 0x7F800000) == 0xD1000000 ||
               (op & 0x7F800000) == 0x71000000 || (op & 0x7F800000) == 0xF1000000) {
        // add/sub (+cmp = subs xzr) immediate
        const bool is64 = (op >> 31) & 1;
        const uint8_t top = (op >> 24) & 0xFF;
        const bool isSub = (top == 0x51 || top == 0xD1 || top == 0x71 || top == 0xF1);
        const bool isCmp = (top == 0x71 || top == 0xF1) && rd == 31;
        const unsigned imm = (op >> 10) & 0xFFF;
        const bool sh = (op >> 22) & 1;
        if (isCmp) snprintf(b, sizeof(b), "cmp %s, #0x%X%s", reg(is64, rn), imm, sh ? ", lsl #12" : "");
        else snprintf(b, sizeof(b), "%s %s, %s, #0x%X%s", isSub ? "sub" : "add",
                      reg(is64, rd), reg(is64, rn), imm, sh ? ", lsl #12" : "");
        m = b;
    } else if ((op & 0x7FA07C00) == 0x2A000000 || (op & 0x7FA07C00) == 0xAA000000) {
        // orr reg; Rn==xzr = mov
        const bool is64 = (op >> 31) & 1;
        if (rn == 31) snprintf(b, sizeof(b), "mov %s, %s", reg(is64, rd), reg(is64, rm));
        else snprintf(b, sizeof(b), "orr %s, %s, %s", reg(is64, rd), reg(is64, rn), reg(is64, rm));
        m = b;
    } else if ((op & 0xFC000000) == 0x14000000 || (op & 0xFC000000) == 0x94000000) {
        // b / bl
        const int64_t delta = signExt(op & 0x3FFFFFF, 26) << 2;
        snprintf(b, sizeof(b), "%s 0x%lx", (op >> 31) & 1 ? "bl" : "b",
                 (unsigned long)(pc + delta));
        m = b;
    } else if ((op & 0xFF000000) == 0x34000000 || (op & 0xFF000000) == 0x35000000 ||
               (op & 0xFF000000) == 0xB4000000 || (op & 0xFF000000) == 0xB5000000) {
        // cbz / cbnz
        const bool is64 = (op >> 31) & 1;
        const uint8_t top = (op >> 24) & 0xFF;
        const int64_t delta = signExt((op >> 5) & 0x7FFFF, 19) << 2;
        snprintf(b, sizeof(b), "%s %s, 0x%lx",
                 (top == 0x34 || top == 0xB4) ? "cbz" : "cbnz",
                 reg(is64, rd), (unsigned long)(pc + delta));
        m = b;
    } else if ((op & 0xFF000000) == 0x36000000 || (op & 0xFF000000) == 0x37000000) {
        // tbz / tbnz
        const unsigned bit = (((op >> 31) & 1) << 5) | ((op >> 19) & 63);
        const int64_t delta = signExt((op >> 5) & 0x3FFF, 14) << 2;
        snprintf(b, sizeof(b), "%s %s, #%u, 0x%lx",
                 ((op >> 24) & 0xFF) == 0x36 ? "tbz" : "tbnz",
                 reg(true, rd), bit, (unsigned long)(pc + delta));
        m = b;
    } else if ((op & 0xFF000000) == 0x18000000 || (op & 0xFF000000) == 0x58000000) {
        // ldr literal
        const bool is64 = (op & 0xFF000000) == 0x58000000;
        const int64_t delta = signExt((op >> 5) & 0x7FFFF, 19) << 2;
        snprintf(b, sizeof(b), "ldr %s, 0x%lx", reg(is64, rd), (unsigned long)(pc + delta));
        m = b;
    } else if ((op & 0x9F000000) == 0x10000000 || (op & 0x9F000000) == 0x90000000) {
        // adr / adrp
        const bool isPage = (op & 0x9F000000) == 0x90000000;
        const uint32_t imm = (((op >> 29) & 3) << 14) | (((op >> 5) & 0xFFFFF) << 2);
        const int64_t tgt = isPage ? ((int64_t)(pc & ~(uintptr_t)0xFFF) + signExt(imm >> 2, 21) * 4096)
                                   : ((int64_t)pc + signExt(imm, 21));
        snprintf(b, sizeof(b), "%s %s, 0x%lx", isPage ? "adrp" : "adr",
                 reg(true, rd), (unsigned long)tgt);
        m = b;
    } else if ((op & 0xFFC00000) == 0xB9400000 || (op & 0xFFC00000) == 0xF9400000 ||
               (op & 0xFFC00000) == 0xB9000000 || (op & 0xFFC00000) == 0xF9000000 ||
               (op & 0xFFC00000) == 0x39400000 || (op & 0xFFC00000) == 0x39000000 ||
               (op & 0xFFC00000) == 0x79400000 || (op & 0xFFC00000) == 0x79000000) {
        // ldr/str (unsigned imm, 32/64/byte/half)
        const unsigned sz = (op >> 30) & 3;
        const bool isLoad = (op & 0x400000) != 0;
        const unsigned bytes = sz == 0 ? 1 : sz == 1 ? 2 : sz == 2 ? 4 : 8;
        const unsigned imm = ((op >> 10) & 0xFFF) * bytes;
        const char* nm;
        bool r64 = false;
        if (bytes == 8) { nm = isLoad ? "ldr" : "str"; r64 = true; }
        else if (bytes == 4) { nm = isLoad ? "ldr" : "str"; }
        else if (bytes == 2) { nm = isLoad ? "ldrh" : "strh"; }
        else { nm = isLoad ? "ldrb" : "strb"; }
        if (imm) snprintf(b, sizeof(b), "%s %s, [%s, #0x%X]", nm, reg(r64, rd), reg(true, rn), imm);
        else snprintf(b, sizeof(b), "%s %s, [%s]", nm, reg(r64, rd), reg(true, rn));
        m = b;
    } else if ((op & 0xFF000000) == 0xA9000000 || (op & 0xFF000000) == 0xA8000000 ||
               (op & 0xFF000000) == 0x29000000 || (op & 0xFF000000) == 0x28000000) {
        // ldp / stp (signed-offset, pre-index "!", post-index).
        // Bedain load/store dari bit 22, mode dari P(bit24)/W(bit21).
        const bool is64 = (op & 0xFF000000) == 0xA9000000 || (op & 0xFF000000) == 0xA8000000;
        const bool isLoad = ((op >> 22) & 1) != 0;
        const bool pre = ((op >> 24) & 1) && ((op >> 21) & 1);
        const bool post = !((op >> 24) & 1) && ((op >> 21) & 1);
        const unsigned rt2 = (op >> 10) & 31;
        int64_t imm = signExt((op >> 15) & 0x7F, 7) * (is64 ? 8 : 4);
        char off[32];
        snprintf(off, sizeof(off), "0x%lX", (unsigned long)imm);
        if (pre) snprintf(b, sizeof(b), "%s %s, %s, [%s, #%s]!", isLoad ? "ldp" : "stp",
                          reg(is64, rd), reg(is64, rt2), reg(true, rn), off);
        else if (post) snprintf(b, sizeof(b), "%s %s, %s, [%s], #%s", isLoad ? "ldp" : "stp",
                          reg(is64, rd), reg(is64, rt2), reg(true, rn), off);
        else if (imm) snprintf(b, sizeof(b), "%s %s, %s, [%s, #%s]", isLoad ? "ldp" : "stp",
                          reg(is64, rd), reg(is64, rt2), reg(true, rn), off);
        else snprintf(b, sizeof(b), "%s %s, %s, [%s]", isLoad ? "ldp" : "stp",
                      reg(is64, rd), reg(is64, rt2), reg(true, rn));
        m = b;
    } else if ((op & 0xFFE0001F) == 0xD4000001) {
        snprintf(b, sizeof(b), "svc #0x%X", (op >> 5) & 0xFFFF);
        m = b;
    } else {
        snprintf(b, sizeof(b), ".word 0x%08X", op);
        m = b;
    }
    out += m;
    return 4;
}

std::vector<std::string> disasm(const uint8_t* code, size_t n, uintptr_t base,
                                size_t maxIns) {
    std::vector<std::string> lines;
    size_t off = 0, count = 0;
    while (off + 4 <= n && count < maxIns) {
        std::string line;
        disasmOne(code, base, off, n - off, line);
        lines.push_back(line);
        // Berhenti di cabang tak bersyarat / return (kayak AssemblyView).
        uint32_t op = 0;
        memcpy(&op, code + off, 4);
        off += 4;
        ++count;
        if (op == 0xD65F03C0 || (op & 0xFC000000) == 0x14000000 ||
            (op & 0xFFDFFC1F) == 0xD61F0000)
            break;
    }
    return lines;
}

}  // namespace memview

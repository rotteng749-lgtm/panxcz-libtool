// ============================================================================
//  libdump.cpp -- lihat include/core/libdump.h buat penjelasan lengkapnya.
//
//  Ringkas: (1) tulis gambar memori lib ke file (susunan FILE OFFSET), (2) baca
//  ELF-nya dari memori (header/segmen/NEEDED/export/import) + pindai string buat
//  nandain deteksi vektor dan JEJAK TOOL sendiri. Semuanya READ-ONLY.
// ============================================================================
#include "core/libdump.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <set>
#include <sys/stat.h>
#include <sys/types.h>

#include "core/abort.h"
#include "core/dumpout.h"      // dumpout::pkgVersion()
#include "core/il2cpp.h"
#include "core/log.h"
#include "core/mem.h"
#include "core/target.h"

namespace libdump {

// ---------------------------------------------------------------------------
// kata kunci
// ---------------------------------------------------------------------------
// "VEKTOR" = alat/cara yang biasa dipakai buat nyari cheat, root, atau hook.
// Yang ketemu di lib anti-cheat = itu yang mau dia cari (jadi yang perlu di-hide).
static const char* kVectorKeys[] = {
    "frida", "gum-js", "gumjs", "gadget", "xposed", "lsposed", "magisk",
    "kernelsu", "ksu", "zygisk", "riru", "shamiko", "susfs", "supersu",
    "superuser", "/sbin/", "/system/xbin", "/system/bin/su", "busybox",
    "gameguardian", "scanmem", "tracerpid", "/proc/self/maps", "/proc/self/status",
    "/proc/self/mem", "/proc/self/task", "/proc/self/cmdline", "ptrace", "inotify",
    "virtualapp", "island", "shelter", "parallel space", "multiapp", "qemu",
    "goldfish", "genymotion", "bluestacks", "ldplayer", "memu", "emulator",
    "signature", "checksum", "integrity", "tamper", "jdwp", "dl_iterate_phdr",
    "libanogs", "libanort", "libtersafe", "libtprt", "beacon", "metasec",
    "gamesecurity", "tencent", "garena", "anticheat", "anti-cheat", "anogs",
};

// "JEJAK TOOL" = nama/jalur yang bikin tool INI sendiri gampang ketemu.
// Kalau salah satu muncul di laporan lib anti-cheat, berarti itu yang wajib
// dibersihin/di-hide dulu.
static const char* kTraceKeys[] = {
    "panxcz", "libtool", "panxcz_libtool", "/data/local/tmp", "dumper",
    "mali_render", "scan-only", "--dumpout", "il2cpp_dump",
};

// Potong string panjang buat log: buang newline, maksimal ~110 karakter.
// String mentah dari blob bisa panjangnya beribu-ribu karakter dan bikin satu
// baris log nutupin puluhan baris lain (dulu kejadian: "detektor:" + blob 3 KB).
static std::string clip(const std::string& s, size_t max = 110) {
    std::string o;
    o.reserve(std::min(s.size(), max) + 3);
    for (const char c : s) {
        if (o.size() >= max) { o += "..."; break; }
        if (c == '\n' || c == '\r' || c == '\t') { o += ' '; continue; }
        if ((unsigned char)c < 0x20) continue;
        o += c;
    }
    return o;
}

const std::vector<std::string>& vectorKeys() {
    static std::vector<std::string> v = [] {
        std::vector<std::string> o;
        for (const char* k : kVectorKeys) o.push_back(k);
        return o;
    }();
    return v;
}

namespace {

// ---------------------------------------------------------------------------
// util
// ---------------------------------------------------------------------------
// /proc/<pid>/maps nulis path-nya rata kanan (banyak spasi di depan) -- kalau
// nggak dipotong, jalur di laporan/JSON ikut bawa spasi itu.
std::string trim(const std::string& s) {
    size_t b = 0, e = s.size();
    while (b < e && isspace((unsigned char)s[b])) ++b;
    while (e > b && isspace((unsigned char)s[e - 1])) --e;
    return s.substr(b, e - b);
}

std::string baseName(const std::string& p) {
    const size_t s = p.find_last_of('/');
    return s == std::string::npos ? p : p.substr(s + 1);
}

void mkdirP(const char* path) {
    std::string cur;
    for (const char* p = path; *p; ++p) {
        cur += *p;
        if (*p == '/' && cur.size() > 1) mkdir(cur.c_str(), 0775);
    }
    mkdir(path, 0775);
}

// Pencocokan kata kunci yang "berbatas kata". Tanpa ini, string blob besar di
// dalam lib (HTML/teks narasi bawaan Unity, nama kelas panjang) bikin hasilnya
// sampah: "islandsnotable" kena 'island', "beaconquotes" kena 'beacon'.
// Aturan: tepi kata kunci yang berupa huruf/angka nggak boleh nyambung sama
// huruf/angka di string-nya.
bool keyHit(const std::string& hay, const char* needleLower) {
    if (hay.empty() || !needleLower || !*needleLower) return false;
    const size_t kl = strlen(needleLower);
    auto wordish = [](unsigned char c) { return isalnum(c) || c == '_'; };
    const bool kb = wordish((unsigned char)needleLower[0]);
    const bool ka = wordish((unsigned char)needleLower[kl - 1]);
    for (size_t i = 0; i + kl <= hay.size(); ++i) {
        size_t k = 0;
        for (; k < kl; ++k)
            if (tolower((unsigned char)hay[i + k]) != needleLower[k]) break;
        if (k != kl) continue;
        const unsigned char before = i ? (unsigned char)hay[i - 1] : 0;
        const unsigned char after  = (i + kl < hay.size()) ? (unsigned char)hay[i + kl] : 0;
        if (kb && before && wordish(before)) continue;
        if (ka && after && wordish(after)) continue;
        return true;
    }
    return false;
}

std::string hex64(uint64_t v) {
    char b[32];
    snprintf(b, sizeof(b), "0x%llx", (unsigned long long)v);
    return b;
}

std::string sizeStr(size_t bytes) {
    char b[64];
    if (bytes >= (1u << 20)) snprintf(b, sizeof(b), "%.1f MB", (double)bytes / (double)(1u << 20));
    else                     snprintf(b, sizeof(b), "%zu KB", bytes / 1024);
    return b;
}

// ---------------------------------------------------------------------------
// struktur ELF (baca dari memori proses)
// ---------------------------------------------------------------------------
struct Ehdr {
    unsigned char ident[16];
    uint16_t type, machine;
    uint32_t version;
    uint64_t entry, phoff, shoff;
    uint32_t flags;
    uint16_t ehsize, phentsize, phnum, shentsize, shnum, shstrndx;
} __attribute__((packed));

struct Phdr {
    uint32_t type, flags;
    uint64_t offset, vaddr, paddr, filesz, memsz, align;
} __attribute__((packed));

struct Dyn {
    int64_t  tag;
    uint64_t val;
} __attribute__((packed));

struct Sym {
    uint32_t name;
    unsigned char info, other;
    uint16_t shndx;
    uint64_t value, size;
} __attribute__((packed));

struct Rela { uint64_t offset, info; int64_t addend; } __attribute__((packed));
struct Rel  { uint64_t offset, info; } __attribute__((packed));

constexpr int64_t DT_NULL_ = 0, DT_NEEDED_ = 1, DT_PLTRELSZ_ = 2, DT_HASH_ = 4,
                  DT_STRTAB_ = 5, DT_SYMTAB_ = 6, DT_RELA_ = 7, DT_RELASZ_ = 8,
                  DT_RELAENT_ = 9, DT_STRSZ_ = 10, DT_SYMENT_ = 11,
                  DT_INIT_ = 12, DT_FINI_ = 13, DT_SONAME_ = 14,
                  DT_RPATH_ = 15, DT_PLTREL_ = 20, DT_JMPREL_ = 23,
                  DT_INIT_ARRAY_ = 25, DT_FINI_ARRAY_ = 26, DT_INIT_ARRAYSZ_ = 27,
                  DT_FINI_ARRAYSZ_ = 28, DT_RUNPATH_ = 29,
                  DT_GNU_HASH_ = 0x6ffffef5;
// PT_GNU_EH_FRAME: di sinilah tabel .eh_frame_hdr nempel (bukan tag dynamic).
constexpr uint32_t PT_GNU_EH_FRAME_ = 0x6474e550;
// R_AARCH64_* yang bikin nama fungsinya kepas di relokasi:
constexpr uint32_t R_AARCH64_JUMP_SLOT_ = 1026, R_AARCH64_GLOB_DAT_ = 1025;

std::string readCStr(uintptr_t p, size_t cap = 512) {
    if (!p) return std::string();
    std::vector<char> buf(cap, 0);
    size_t got = cap;
    if (!mem::read(p, buf.data(), got)) {
        // coba separuh (string di ujung region)
        got = cap / 4;
        if (!mem::read(p, buf.data(), got)) return std::string();
    }
    buf.back() = 0;
    return std::string(buf.data());
}

struct RegionOf {
    const mem::Region* r = nullptr;
    uintptr_t start = 0, end = 0;
    uint64_t  fileOff = 0;
    std::string path;
};

std::vector<RegionOf> regionsOf(const std::string& lib) {
    std::vector<RegionOf> out;
    for (const auto& r : mem::regions()) {
        if (r.name.empty() || !r.readable()) continue;
        if (baseName(r.name) != lib) continue;
        RegionOf x;
        x.r = &r;
        x.start = r.start;
        x.end = r.end;
        x.fileOff = r.fileOff;
        x.path = trim(r.name);
        out.push_back(x);
    }
    std::sort(out.begin(), out.end(), [](const RegionOf& a, const RegionOf& b) {
        return a.start < b.start;
    });
    return out;
}

// ---------------------------------------------------------------------------
// 1. gambar memori (susunan file offset)
// ---------------------------------------------------------------------------
bool writeImage(const std::string& lib, const std::vector<RegionOf>& regs,
                const std::string& path, size_t maxMB, size_t& written) {
    uint64_t endOff = 0;
    for (const auto& r : regs)
        endOff = std::max(endOff, r.fileOff + (uint64_t)(r.end - r.start));

    const uint64_t cap = (uint64_t)maxMB << 20;
    if (endOff > cap) {
        logx::add("[libdump] %s: gambar memorinya %s -- dipotong ke %zu MB (pakai --ac-imgmax)",
                  lib.c_str(), sizeStr((size_t)endOff).c_str(), maxMB);
        endOff = cap;
    }

    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f.is_open()) return false;

    std::vector<uint8_t> buf(1u << 20);
    written = 0;
    for (const auto& r : regs) {
        if (abortx::requested()) return false;
        if (r.fileOff >= endOff) continue;
        uintptr_t a = r.start;
        uint64_t  off = r.fileOff;
        while (a < r.end && off < endOff) {
            size_t n = (size_t)std::min<uint64_t>(buf.size(), (uint64_t)(r.end - a));
            if (off + n > endOff) n = (size_t)(endOff - off);
            if (!n) break;
            // blok besar dibaca longgar (lihat mem::read): bagian yang nggak
            // kebaca di-nol-kan, jadi file-nya tetap punya offset yang bener.
            mem::read(a, buf.data(), n);
            f.seekp((std::streamoff)off);
            f.write((const char*)buf.data(), (std::streamsize)n);
            written += n;
            a += n;
            off += n;
            abortx::beat("libdump: gambar memori");
        }
    }
    f.flush();
    return true;
}


void writeRegions(const std::string& lib, const std::vector<RegionOf>& regs,
                  const std::string& path) {
    std::ofstream o(path, std::ios::trunc);
    if (!o.is_open()) return;
    o << "// " << lib << " -- peta segmen di proses (buat ubah alamat runtime <-> offset file)\n";
    o << "// alamat_runtime = region_start + (offset_file - fileOff)\n";
    o << "// pid=" << mem::pid() << "  proses=" << mem::procName() << "\n";
    for (const auto& r : regs) {
        const std::string perms = r.r ? std::string(r.r->perms) : std::string("?");
        o << hex64(r.start) << "  " << hex64(r.end) << "  " << perms
          << "  fileOff " << hex64(r.fileOff) << "  " << sizeStr((size_t)(r.end - r.start))
          << "  " << r.path << "\n";
    }
}

// ---------------------------------------------------------------------------
// 2. laporan ELF + string
// ---------------------------------------------------------------------------
struct Scan {
    size_t exports = 0, imports = 0, stringsTotal = 0;
    std::vector<std::string> exportLines;   // udah dibatasi
    std::vector<std::string> importLines;
    std::vector<std::string> vectors, traces;
    std::vector<std::string> needed;
    std::string soname, rpath;
    std::string elfInfo;
    std::vector<std::string> segLines;

    // -- "logic"-nya: API apa yang dipanggil lib ini ---------------------------
    // Dari relokasi (RELA/JMPREL). Ini yang bikin lib anti-cheat yang di-strip
    // total tetap kebaca: nama fungsinya (ptrace, inotify_add_watch,
    // dl_iterate_phdr, fopen, getpid, ...) ada di .dynstr, dan slot GOT-nya
    // ditulis rva-nya biar bisa diburu di IDA/Ghidra.
    size_t relocUses = 0;
    std::vector<std::string> relocLines;
    // Constructor/destructor: DT_INIT + DT_INIT_ARRAY. Buat lib anti-cheat,
    // ini PINTU MASUK logic-nya (bikin thread pemantau, pasang hook).
    uint64_t initRva = 0;
    std::vector<uint64_t> initArray, finiArray;
    // Daftar SEMUA fungsi dari .eh_frame_hdr (tetap ada walau di-strip).
    std::vector<uint64_t> funcRvas;
    std::string funcNote;
    // Simbol yang punya NAMA (export + constructor) -- dipakai buat bikin file
    // pendamping (.r2 / .ghidra.py / .map) yang bisa dipakai tanpa IDA.
    std::vector<std::pair<uint64_t, std::string>> namedSyms;
};

// ---------------------------------------------------------------------------
// jumlah simbol kalau tabel hash-nya nggak ada (lib yang di-pack/di-strip
// parser-nya sering buang DT_HASH/DT_GNU_HASH, padahal .dynsym-nya masih ada).
// Batas bawah yang PASTI bener: indeks simbol terbesar yang dipakai relokasi.
// ---------------------------------------------------------------------------
size_t symCountFromRelocs(size_t maxSymIdx) {
    return maxSymIdx ? maxSymIdx + 1 : 0;
}

// Cadangan terakhir: maju terus selama entri-nya masih masuk akal (name-nya
// nunjuk ke dalam .dynstr). Berhenti setelah 4 entri kosong beruntun.
size_t probeSymCount(uintptr_t symtab, size_t syment, uintptr_t strtab, size_t strsz) {
    if (!symtab || !strtab || syment < sizeof(Sym)) return 0;
    size_t i = 1, blank = 0;
    for (; i < 2000000; ++i) {
        if ((i & 8191) == 0) abortx::beat("libdump: hitung simbol");
        Sym sym{};
        if (!mem::read(symtab + i * syment, &sym, sizeof(sym))) break;
        if (!sym.name || (strsz && sym.name >= strsz)) {
            if (++blank >= 4) break;
            continue;
        }
        blank = 0;
    }
    return i;
}

// daftar simbol dari DT_SYMTAB/DT_STRTAB (+ DT_HASH / DT_GNU_HASH buat jumlahnya)
size_t symbolCount(uintptr_t symtab, uintptr_t hash, uintptr_t gnuHash) {
    if (gnuHash && mem::canRead(gnuHash, 16)) {
        const uint32_t nbuckets  = mem::rdp<uint32_t>(gnuHash);
        const uint32_t symoffset = mem::rdp<uint32_t>(gnuHash + 4);
        const uint32_t bloomSize = mem::rdp<uint32_t>(gnuHash + 8);
        if (!nbuckets || nbuckets > (1u << 20) || bloomSize > (1u << 22)) {
            if (hash && mem::canRead(hash, 8)) return mem::rdp<uint32_t>(hash + 4);
            return 0;
        }
        const uintptr_t buckets = gnuHash + 16 + (uintptr_t)bloomSize * 8;
        const uintptr_t chains  = buckets + (uintptr_t)nbuckets * 4;
        uint32_t maxIdx = symoffset;
        for (uint32_t i = 0; i < nbuckets; ++i) {
            const uint32_t b = mem::rdp<uint32_t>(buckets + (uintptr_t)i * 4);
            if (b > maxIdx) maxIdx = b;
        }
        if (!maxIdx) return symoffset;
        // jalanin chain sampai ketemu entri terakhir (LSB = 1)
        size_t i = maxIdx;
        for (int guard = 0; guard < 4000000; ++guard, ++i) {
            const uint32_t c = mem::rdp<uint32_t>(chains + (uintptr_t)(i - symoffset) * 4);
            if (!mem::canRead(chains + (uintptr_t)(i - symoffset) * 4, 4)) break;
            if (c & 1u) break;
        }
        return i + 1;
    }
    if (hash && mem::canRead(hash, 8)) return mem::rdp<uint32_t>(hash + 4);   // DT_HASH.nchain
    return 0;
}

// ---------------------------------------------------------------------------
// .eh_frame_hdr -> daftar SEMUA fungsi
// ---------------------------------------------------------------------------
// Ini kunci buat lib anti-cheat yang di-strip total (libanogs/libanort: nggak
// ada tabel simbol sama sekali). Tabel unwind-nya nggak bisa di-strip -- tanpa
// itu exception C++ nggak jalan. Isinya pasangan (alamat awal fungsi -> FDE)
// yang DIURUT, jadi tiap entri = satu fungsi. Hasilnya: daftar fungsi lengkap
// tinggal di-rebase rva-nya buat IDA/Ghidra (file .fns.txt).
struct EhHdr {
    uint8_t version = 0, ptrEnc = 0, cntEnc = 0, tblEnc = 0;
};

// lebar (byte) satu nilai ter-encode DW_EH_PE_* -- 0 = nggak didukung
size_t ehEncWidth(uint8_t e) {
    if (e & 0x80) return 0;                 // indirect -- nggak didukung
    switch (e & 0x0f) {
        case 0x01: case 0x09: return 1;     // ubyte / sleb128
        case 0x02: case 0x0a: return 2;     // uhalf / sleb128(2)
        case 0x03: case 0x0b: return 4;     // uword / sword
        case 0x04: case 0x0c: return 8;     // udword / sdword
        default:   return 0;
    }
}

// baca satu nilai di `at` sesuai encoding `e`. `next` maju; `p` = posisi field
// (buat pcrel). datarel-nya relatif ke awal .eh_frame_hdr.
bool ehRead(uint8_t e, uintptr_t at, uintptr_t datarelBase, uint64_t& out,
            uintptr_t& p) {
    const size_t w = ehEncWidth(e);
    if (!w) return false;
    int64_t v = 0;
    switch (e & 0x0f) {
        case 0x01: { uint8_t x = 0;  if (!mem::read(at, &x, 1)) return false; v = (int8_t)x;   break; }
        case 0x02: { int16_t x = 0;  if (!mem::read(at, &x, 2)) return false; v = x;            break; }
        case 0x03: { uint32_t x = 0; if (!mem::read(at, &x, 4)) return false; v = (int64_t)x;  break; }
        case 0x04: { uint64_t x = 0; if (!mem::read(at, &x, 8)) return false; v = (int64_t)x;  break; }
        case 0x0b: { int32_t x = 0;  if (!mem::read(at, &x, 4)) return false; v = x;           break; }
        case 0x0c: { int64_t x = 0;  if (!mem::read(at, &x, 8)) return false; v = x;           break; }
        default:   return false;    // sleb128 di header jarang; lewat aja
    }
    p = at + w;
    switch (e & 0x70) {
        case 0x00: out = (uint64_t)v; return true;                          // absptr
        case 0x10: out = (uint64_t)((int64_t)at + v); return true;          // pcrel
        case 0x30: out = (uint64_t)((int64_t)datarelBase + v); return true; // datarel
        default:   return false;
    }
}

// Balikin jumlah fungsi; rva tiap fungsi ditulis ke `out` (kosong kalau header
// .eh_frame_hdr-nya nggak ada / formatnya nggak kepahami).
size_t readFuncList(uintptr_t hdr, std::vector<uint64_t>& out, std::string& note,
                    size_t cap = 2000000) {
    if (!hdr || !mem::canRead(hdr, 12)) { note = "nggak ada .eh_frame_hdr (PT_GNU_EH_FRAME) di lib ini"; return 0; }
    uint8_t h[4] = {0};
    if (!mem::read(hdr, h, sizeof(h))) { note = ".eh_frame_hdr nggak kebaca"; return 0; }
    if (h[0] != 1) { note = ".eh_frame_hdr versi " + std::to_string((int)h[0]) + " (cuma versi 1 yang didukung)"; return 0; }

    uintptr_t p = hdr + 4;
    uint64_t ehPtr = 0, cnt = 0;
    if (!ehRead(h[1], p, hdr, ehPtr, p)) { note = "encoding .eh_frame_ptr nggak didukung"; return 0; }
    if (!ehRead(h[2], p, hdr, cnt, p)) { note = "encoding fde_count nggak didukung"; return 0; }
    if (!cnt || cnt > 4000000) { note = "fde_count mencurigakan (" + std::to_string((long long)cnt) + ")"; return 0; }

    const size_t ew = ehEncWidth(h[3]);
    if (!ew) { note = "encoding tabel .eh_frame_hdr nggak didukung"; return 0; }
    const uintptr_t table = p;
    out.reserve((size_t)std::min<uint64_t>(cnt, cap));
    for (uint64_t i = 0; i < cnt && out.size() < cap; ++i) {
        if (abortx::requested()) break;
        if ((i & 1023) == 0) abortx::beat("libdump: baca .eh_frame_hdr");
        uintptr_t at = table + (uintptr_t)(i * 2) * ew;
        uint64_t pc = 0, fde = 0;
        uintptr_t q = at;
        if (!ehRead(h[3], at, hdr, pc, q)) break;
        q = at + ew;
        if (!ehRead(h[3], at + ew, hdr, fde, q)) break;
        if (!pc) continue;
        const uint64_t rva = mem::rvaOf(pc);
        if (!rva) continue;
        out.push_back(rva);
    }
    note = "tabel " + std::to_string((long long)cnt) + " entri, " + std::to_string(out.size()) +
           " fungsinya nunjuk ke rva yang sah";
    return out.size();
}

void readElf(const std::string& lib, const std::vector<RegionOf>& regs, Scan& s,
             size_t maxExportLines) {
    const uintptr_t base = regs.empty() ? 0 : regs[0].start;
    if (!base) return;
    Ehdr e{};
    if (!mem::read(base, &e, sizeof(e))) return;
    if (memcmp(e.ident, "\x7f""ELF", 4) != 0) {
        s.elfInfo = "bukan ELF yang kebaca di base-nya (lib ini map-nya nggak mulai dari header)";
        return;
    }
    char b[192];
    snprintf(b, sizeof(b), "%s, %s, %s, entry %s, %u segmen",
             e.ident[4] == 2 ? "64-bit" : "32-bit",
             e.ident[5] == 1 ? "little-endian" : "big-endian",
             e.type == 3 ? "DYN (shared)" : (e.type == 2 ? "EXEC" : "?"),
             hex64(e.entry).c_str(), (unsigned)e.phnum);
    s.elfInfo = b;
    if (e.phnum > 128 || e.phentsize != sizeof(Phdr)) return;

    std::vector<Phdr> phs(e.phnum);
    if (!mem::read(base + e.phoff, phs.data(), sizeof(Phdr) * (size_t)e.phnum)) return;

    uintptr_t dynVaddr = 0, ehVaddr = 0;
    size_t dynSize = 0;
    for (const auto& p : phs) {
        if (p.type == 1 && p.memsz) {                       // PT_LOAD
            if (p.filesz && p.filesz == p.memsz)
                snprintf(b, sizeof(b), "  %s - %s  %c%c%c  fileOff %s  %s", 
                         hex64(base + p.vaddr).c_str(), hex64(base + p.vaddr + p.memsz).c_str(),
                         (p.flags & 4) ? 'r' : '-', (p.flags & 2) ? 'w' : '-', (p.flags & 1) ? 'x' : '-',
                         hex64(p.offset).c_str(), sizeStr((size_t)p.filesz).c_str());
            else
                snprintf(b, sizeof(b), "  %s - %s  %c%c%c  fileOff %s  %s (file %s + bss %s)",
                         hex64(base + p.vaddr).c_str(), hex64(base + p.vaddr + p.memsz).c_str(),
                         (p.flags & 4) ? 'r' : '-', (p.flags & 2) ? 'w' : '-', (p.flags & 1) ? 'x' : '-',
                         hex64(p.offset).c_str(), sizeStr((size_t)p.memsz).c_str(),
                         sizeStr((size_t)p.filesz).c_str(), sizeStr((size_t)(p.memsz - p.filesz)).c_str());
            s.segLines.push_back(b);
        } else if (p.type == 2) {                           // PT_DYNAMIC
            dynVaddr = (uintptr_t)p.vaddr;
            dynSize  = (size_t)p.memsz;
        } else if (p.type == PT_GNU_EH_FRAME_) {            // tabel fungsi (lihat readFuncList)
            ehVaddr = (uintptr_t)p.vaddr;
        }
    }

    // vaddr -> alamat runtime LEWAT PETA SEGMEN, bukan base+vaddr.
    // Alasan: ada lib yang segmennya dimap TERPISAH dengan alamat acak sendiri
    // (nyata di libunity/libil2cpp Free Fire: begitu dipakai base+vaddr, isi
    // PT_DYNAMIC-nya kebaca nol semua -- tag pertamanya DT_NULL, jadi seluruh
    // export/import/constructor hilang). Caranya: cari PT_LOAD yang mencakup
    // vaddr-nya -> jadi file offset -> cari region (dari /proc/pid/maps) yang
    // file offset-nya nutup nilai itu. Ini bener buat dua-duanya (nyambung
    // maupun terpisah).
    auto vaddrToAddr = [&](uintptr_t v) -> uintptr_t {
        for (const auto& p : phs) {
            if (p.type != 1 || !p.memsz) continue;
            if (v < p.vaddr || v >= p.vaddr + p.memsz) continue;
            const uint64_t foff = p.offset + (uint64_t)(v - p.vaddr);
            for (const auto& r : regs) {
                const uint64_t end = r.fileOff + (uint64_t)(r.end - r.start);
                if (foff < r.fileOff || foff >= end) continue;
                return r.start + (uintptr_t)(foff - r.fileOff);
            }
            return 0;
        }
        return 0;
    };

    const uintptr_t dynAddr = dynVaddr ? (vaddrToAddr(dynVaddr) ?
                                          vaddrToAddr(dynVaddr) : base + dynVaddr) : 0;
    const uintptr_t ehFrameHdrAddr = ehVaddr ? (vaddrToAddr(ehVaddr) ?
                                                vaddrToAddr(ehVaddr) : base + ehVaddr) : 0;
    if (dynVaddr && base + dynVaddr != dynAddr)
        logx::add("[libdump] %s: PT_DYNAMIC segmennya dimap terpisah -- alamat runtime "
                  "diambil dari peta segmen (%s), bukan base+vaddr (%s).",
                  lib.c_str(), hex64(dynAddr).c_str(), hex64(base + dynVaddr).c_str());
    if (!dynAddr || !dynSize) {
        logx::add("[libdump] %s: nggak ada PT_DYNAMIC yang ke-map -- lib ini di-pack/di-map "
                  "sendiri (header-nya nggak ikut ke-map), jadi daftar export/import "
                  "nggak ada di memori. Yang tetap kepakai: gambar memori (.img) + string.",
                  lib.c_str());
        return;
    }

    // ---- tafsir nilai di PT_DYNAMIC ----------------------------------------
    // Nilai tag-pointer (DT_STRTAB/DT_SYMTAB/DT_RELA/...) di memori cuma dua
    // kemungkinan:
    //   (a) ALAMAT ABSOLUT  -- loader sistem udah nambahin bias (kasus biasa)
    //   (b) VADDR MENTAH    -- lib di-load loader SENDIRI ala packer; nilainya
    //       masih kayak di file (nyata di libanogs/libanort Free Fire: DT_NEEDED
    //       isinya 0x0A23, DT_STRTAB 0x23D0 -- buktinya dari --peek)
    // Dulu yang kepakai cuma (a), jadi seluruh analisa lib anti-cheat kosong:
    // strtab nunjuk ke alamat yang nggak ada isinya (semua string kosong ->
    // [NEEDED] nggak kecetak, relokasi 0, constructor 0).
    auto inLib = [&](uintptr_t a) {
        for (const auto& r : regs)
            if (a >= r.start && a < r.end) return true;
        return false;
    };
    size_t dtRawAbs = 0, dtRawVaddr = 0, dtUnresolved = 0;
    auto dtPtr = [&](int64_t raw) -> uintptr_t {
        if (!raw) return 0;
        const uintptr_t v = (uintptr_t)raw;
        if (inLib(v)) { ++dtRawAbs; return v; }               // (a)
        const uintptr_t cand = vaddrToAddr(v);                // (b1) lewat peta segmen
        if (cand) { ++dtRawVaddr; return cand; }
        const uintptr_t cand2 = base + v;                     // (b2) nyambung
        if (inLib(cand2)) { ++dtRawVaddr; return cand2; }
        ++dtUnresolved;
        return 0;
    };

    // ---- tag dynamic dikumpulin DULU, baru dipakai --------------------------
    // Urutan tag di PT_DYNAMIC nggak dijamin linkernya: DT_NEEDED bisa nongol
    // SEBELUM DT_STRTAB, dan kalau langsung dibaca nama NEEDED-nya kosong (dulu
    // kejadian: [NEEDED] nggak nulis apa-apa padahal lib-nya jelas butuh libc).
    uintptr_t symtab = 0, strtab = 0, hash = 0, gnuHash = 0;
    uintptr_t jmprel = 0, relaTab = 0, initArrayAd = 0, finiArrayAd = 0, initFn = 0;
    size_t strsz = 0, syment = sizeof(Sym);
    size_t pltrelsz = 0, relasz = 0, relaent = sizeof(Rela);
    size_t initArraySz = 0, finiArraySz = 0;
    int64_t pltrel = 0;
    std::vector<uint64_t> strOffs;            // {nilai, jenis} yang perlu .dynstr

    const size_t nDyn = dynSize / sizeof(Dyn);
    std::vector<std::pair<int64_t, uint64_t>> firstTags;   // buat diagnosa
    for (size_t i = 0; i < nDyn; ++i) {
        Dyn d{};
        if (!mem::read(dynAddr + i * sizeof(Dyn), &d, sizeof(d))) break;
        if (d.tag == DT_NULL_) break;
        if (firstTags.size() < 16) firstTags.push_back({d.tag, d.val});
        switch (d.tag) {
            case DT_NEEDED_:    strOffs.push_back(d.val);       break;
            case DT_SONAME_:    strOffs.push_back(d.val | (1ull << 62)); break;
            case DT_RPATH_: case DT_RUNPATH_: strOffs.push_back(d.val | (2ull << 62)); break;
            case DT_SYMTAB_: symtab  = dtPtr(d.val); break;
            case DT_STRTAB_: strtab  = dtPtr(d.val); break;
            case DT_STRSZ_:  strsz   = (size_t)d.val;    break;
            case DT_SYMENT_: syment  = (size_t)d.val;    break;
            case DT_HASH_:   hash    = dtPtr(d.val); break;
            case DT_GNU_HASH_: gnuHash = dtPtr(d.val); break;
            case DT_JMPREL_:   jmprel     = dtPtr(d.val); break;
            case DT_PLTRELSZ_: pltrelsz   = (size_t)d.val;    break;
            case DT_PLTREL_:   pltrel     = d.val;            break;
            case DT_RELA_:     relaTab    = dtPtr(d.val); break;
            case DT_RELASZ_:   relasz     = (size_t)d.val;    break;
            case DT_RELAENT_:  if (d.val) relaent = (size_t)d.val; break;
            case DT_INIT_:     initFn     = dtPtr(d.val); break;
            case DT_INIT_ARRAY_:   initArrayAd = dtPtr(d.val); break;
            case DT_INIT_ARRAYSZ_: initArraySz = (size_t)d.val;    break;
            case DT_FINI_ARRAY_:   finiArrayAd = dtPtr(d.val); break;
            case DT_FINI_ARRAYSZ_: finiArraySz = (size_t)d.val;    break;
            default: break;
        }
    }
    for (uint64_t v : strOffs) {
        const int kind = (int)(v >> 62);
        const std::string n = readCStr(strtab + (v & 0x3ffffffffffffffull), 512);
        if (n.empty()) continue;
        if (kind == 0)      s.needed.push_back(n);
        else if (kind == 1) s.soname = n;
        else                s.rpath  = n;
    }
    if (!strOffs.empty() && s.needed.empty() && s.soname.empty() && s.rpath.empty())
        logx::add("[libdump] %s: DT_STRTAB ketemu tapi isinya nggak kebaca -- bias lib-nya "
                  "beda dari yang diasumsikan (peta segmennya aneh).", lib.c_str());
    if (dtRawVaddr)
        logx::add("[libdump] %s: %zu tag PT_DYNAMIC masih VADDR MENTAH (lib di-load loader "
                  "sendiri ala packer) -- ditafsir ulang ke alamat runtime.", lib.c_str(), dtRawVaddr);
    if (dtUnresolved)
        logx::add("[libdump] %s: %zu tag PT_DYNAMIC nggak bisa ditafsir (di luar segmen lib).",
                  lib.c_str(), dtUnresolved);

    // ---- relokasi: daftar API yang DIPANGGIL lib ini ------------------------
    // Ini yang bikin lib yang di-strip total tetap kebaca "logic"-nya: nama
    // fungsinya (ptrace, inotify_add_watch, dl_iterate_phdr, fopen, getpid, ...)
    // ada di .dynstr walau .dynsym/DT_HASH-nya dibuang, dan slot GOT-nya
    // ditulis rva-nya biar bisa langsung diburu di IDA/Ghidra.
    // r_offset di RELA itu ALAMAT SASARAN (slot GOT-nya), dan di lib yang
    // di-load loader sendiri nilainya juga masih vaddr (sama kayak tag-nya) --
    // dulu dibaca mentah, jadi semua baris [API] rva-nya 0x0.
    auto fixVaddr = [&](uint64_t raw) -> uintptr_t {
        if (!raw) return 0;
        const uintptr_t v = (uintptr_t)raw;
        if (inLib(v)) return v;
        const uintptr_t cand = vaddrToAddr(v);
        if (cand) return cand;
        const uintptr_t cand2 = base + v;
        return inLib(cand2) ? cand2 : 0;
    };

    size_t maxSymIdx = 0;
    std::set<std::string> relSeen;
    auto walkRel = [&](uintptr_t addr, size_t sz, size_t ent, bool plts) {
        if (!addr || !sz || ent < 16) return;
        const size_t n = sz / ent;
        for (size_t i = 0; i < n; ++i) {
            if ((i & 4095) == 0) abortx::beat("libdump: baca relokasi");
            uint64_t hd[2] = {0, 0};
            if (!mem::read(addr + i * ent, hd, sizeof(hd))) break;
            const size_t   symi = (size_t)(hd[1] >> 32);
            const uint32_t rtyp = (uint32_t)(hd[1] & 0xffffffffu);
            if (symi > maxSymIdx) maxSymIdx = symi;
            if (!symi || !symtab || !strtab) continue;
            Sym sym{};
            if (!mem::read(symtab + symi * syment, &sym, sizeof(sym))) continue;
            if (!sym.name || (strsz && sym.name >= strsz)) continue;
            const std::string nm = readCStr(strtab + sym.name, 192);
            if (nm.empty()) continue;
            ++s.relocUses;
            if (!plts || s.relocLines.size() >= 800) continue;
            if (!relSeen.insert(nm).second) continue;
            const uintptr_t tgt = fixVaddr(hd[0]);
            char lb[256];
            snprintf(lb, sizeof(lb), "  rva %-12s %-4s %s",
                     hex64(tgt ? mem::rvaOf(tgt) : 0).c_str(),
                     rtyp == R_AARCH64_JUMP_SLOT_ ? "JMP" :
                     (rtyp == R_AARCH64_GLOB_DAT_ ? "GOT" : "REL"), nm.c_str());
            s.relocLines.push_back(lb);
        }
    };
    if (jmprel && pltrelsz)
        walkRel(jmprel, pltrelsz, (pltrel == 7) ? sizeof(Rela) : sizeof(Rel), true);
    if (relaTab && relasz) walkRel(relaTab, relasz, relaent, false);

    // ---- jumlah simbol ------------------------------------------------------
    size_t nsym = 0;
    if (!symtab || !strtab || syment < sizeof(Sym)) {
        logx::add("[libdump] %s: PT_DYNAMIC nggak bawa DT_SYMTAB/DT_STRTAB (di-strip/pack) -- "
                  "export/import dari tabel simbol dilewat; relokasi, constructor, daftar "
                  "fungsi, dan string tetap dibaca.", lib.c_str());
        // Diagnosa: sebutkan tag yang ADA beserta nilainya -- tanpa ini, "kok "
        // kosong?" cuma bisa ditebak. Nilai 0 / tag yang hilang langsung kelihatan.
        {
            std::string t;
            static const struct { int64_t tag; const char* nm; } kNames[] = {
                {1, "NEEDED"}, {2, "PLTRELSZ"}, {4, "HASH"}, {5, "STRTAB"}, {6, "SYMTAB"},
                {7, "RELA"}, {8, "RELASZ"}, {10, "STRSZ"}, {11, "SYMENT"}, {12, "INIT"},
                {13, "FINI"}, {14, "SONAME"}, {20, "PLTREL"}, {23, "JMPREL"}, {25, "INIT_ARRAY"},
                {27, "INIT_ARRAYSZ"}, {0x6ffffef5, "GNU_HASH"},
            };
            char b[96];
            for (const auto& tv : firstTags) {
                const char* nm = "?";
                for (const auto& kn : kNames) if (kn.tag == tv.first) { nm = kn.nm; break; }
                snprintf(b, sizeof(b), "%s%s=%s", t.empty() ? "" : " ", nm,
                         hex64(tv.second).c_str());
                t += b;
            }
            logx::add("[libdump] %s: PT_DYNAMIC @%s (tag: %s)", lib.c_str(),
                      hex64(dynAddr).c_str(), t.c_str());
        }
    } else {
        nsym = symbolCount(symtab, hash, gnuHash);
        if (!nsym) nsym = symCountFromRelocs(maxSymIdx);
        if (!nsym) nsym = probeSymCount(symtab, syment, strtab, strsz);
        if (nsym > 4000000) nsym = 0;
        if (!nsym)
            logx::add("[libdump] %s: jumlah simbol nggak bisa ditentukan (nggak ada DT_HASH/"
                      "DT_GNU_HASH dan nggak ada relokasi) -- export/import dilewat.", lib.c_str());
        else if (!hash && !gnuHash)
            logx::add("[libdump] %s: tabel hash dibuang, jumlah simbol diturunkan dari relokasi "
                      "-> %zu simbol", lib.c_str(), nsym);
    }
    for (size_t i = 1; i < nsym; ++i) {
        if ((i & 8191) == 0) {
            if (abortx::requested()) return;
            abortx::beat("libdump: baca simbol");
        }
        Sym sym{};
        if (!mem::read(symtab + i * syment, &sym, sizeof(sym))) break;
        if (!sym.name) continue;
        if (strsz && sym.name > strsz) continue;
        const std::string nm = readCStr(strtab + sym.name, 256);
        if (nm.empty()) continue;
        const unsigned type = (unsigned)(sym.info & 0xf);
        if (!sym.shndx) {
            ++s.imports;
            if ((int)s.importLines.size() < 400) s.importLines.push_back(nm);
            continue;
        }
        // yang "didefinisikan" (ada di lib ini)
        if (type != 2 /*FUNC*/ && type != 1 /*OBJECT*/ && type != 10 /*IFUNC*/) continue;
        ++s.exports;
        const uintptr_t abs = fixVaddr(sym.value);
        const uint64_t er = abs ? mem::rvaOf(abs) : 0;
        if (er && type == 2) s.namedSyms.push_back({er, nm});
        if ((int)s.exportLines.size() < (int)maxExportLines) {
            char lb[256];
            // alamat runtime dari st_value (relatif modul) -- lewat peta segmen,
            // jadi kepakai juga buat lib yang mapping-nya terpisah-pisah.
            snprintf(lb, sizeof(lb), "  rva %-12s %-10s %s",
                     hex64(er).c_str(),
                     type == 2 ? "FUNC" : (type == 1 ? "OBJ" : "IFUNC"), nm.c_str());
            s.exportLines.push_back(lb);
        }
    }

    // ---- constructor/destructor (pintu masuk logic anti-cheat) -------------
    // DT_INIT_ARRAY: fungsi-fungsi yang jalan saat lib di-load -- di lib
    // anti-cheat dari sini dia nyalain thread pemantau, pasang ptrace/inotify,
    // dan hook. rva-nya didaftarin biar itu yang pertama diburu di IDA.
    if (initFn) s.initRva = mem::rvaOf(initFn);
    auto readPtrArr = [&](uintptr_t addr, size_t sz, std::vector<uint64_t>& out) {
        if (!addr || !sz) return;
        const size_t n = std::min<size_t>(sz / 8, 512);
        for (size_t i = 0; i < n; ++i) {
            uint64_t p = 0;
            if (!mem::read(addr + i * 8, &p, 8)) break;
            if (!p) continue;
            const uintptr_t a = inLib((uintptr_t)p) ? (uintptr_t)p : fixVaddr(p);
            const uint64_t rva = a ? mem::rvaOf(a) : 0;
            if (rva) out.push_back(rva);
        }
    };
    readPtrArr(initArrayAd, initArraySz, s.initArray);
    readPtrArr(finiArrayAd, finiArraySz, s.finiArray);

    // ---- daftar fungsi dari .eh_frame_hdr ----------------------------------
    readFuncList(ehFrameHdrAddr, s.funcRvas, s.funcNote);
}

// pindai string: tiap region lib (readable) dibaca per 1 MB, run printable
// dipotong, terus dicocokin ke kata kunci.
void scanStrings(const std::string& lib, const std::vector<RegionOf>& regs, Scan& s,
                 std::ofstream* all, size_t maxStrings, bool vectorsToo) {
    std::vector<uint8_t> buf(1u << 20);
    std::string carry;
    std::set<std::string> vectorSeen, traceSeen;

    auto handle = [&](const std::string& str) {
        if (str.size() < 5) return;
        ++s.stringsTotal;
        if (all && *all && s.stringsTotal <= maxStrings) *all << str << "\n";
        for (const char* k : kTraceKeys) {
            if (keyHit(str, k)) {
                if (traceSeen.insert(str).second && s.traces.size() < 200) s.traces.push_back(str);
                return;
            }
        }
        if (!vectorsToo) return;
        for (const char* k : kVectorKeys) {
            if (keyHit(str, k)) {
                if (vectorSeen.insert(str).second && s.vectors.size() < 400) s.vectors.push_back(str);
                return;
            }
        }
    };

    auto feed = [&](const uint8_t* p, size_t n, bool flushTail) {
        std::string cur = carry;
        carry.clear();
        for (size_t i = 0; i < n; ++i) {
            const unsigned char c = p[i];
            if (c >= 0x20 && c < 0x7f) {
                if (cur.size() < 400) cur += (char)c;
                continue;
            }
            if (cur.size() >= 5) handle(cur);
            cur.clear();
        }
        if (!flushTail) carry = cur;
        else if (cur.size() >= 5) handle(cur);
    };

    for (const auto& r : regs) {
        if (abortx::requested()) return;
        uintptr_t a = r.start;
        while (a < r.end) {
            size_t n = (size_t)std::min<uint64_t>(buf.size(), (uint64_t)(r.end - a));
            if (!mem::read(a, buf.data(), n)) break;
            feed(buf.data(), n, false);
            a += n;
            abortx::beat("libdump: pindai string");
        }
        feed(nullptr, 0, true);
        carry.clear();
    }
    (void)lib;
}

}  // namespace

// ---------------------------------------------------------------------------
// daftar lib
// ---------------------------------------------------------------------------
std::vector<std::string> autoLibs(bool allSo, bool systemLibs) {
    std::vector<std::string> out;
    std::set<std::string> seen;
    // package dasar (buang ":proses") -- dipakai buat ngenalin lib MILIK GAME
    // (path-nya di /data/app/...<package>.../lib/), bukan lib sistem Android.
    std::string pkg = target::package();
    const size_t c = pkg.find(':');
    if (c != std::string::npos) pkg = pkg.substr(0, c);

    // dihitung per NAMA lib (satu lib bisa punya belasan region di maps -- kalau
    // dihitung per region, angkanya jadi ngawur, mis. 1500 padahal lib-nya 400).
    std::set<std::string> sysSeen;
    for (const auto& r : mem::regions()) {
        if (r.name.empty()) continue;
        const std::string b = baseName(r.name);
        if (b.size() < 4 || b.compare(b.size() - 3, 3, ".so") != 0) continue;
        if (seen.count(b)) continue;
        const bool ac = target::isAntiCheatLib(b);
        const bool engine = !il2::libName().empty() && b == il2::libName();
        if (!allSo && !ac && !engine) continue;
        if (allSo && !ac && !engine && !systemLibs) {
            const bool gameOwned = !pkg.empty() && r.name.find(pkg) != std::string::npos;
            if (!gameOwned) { sysSeen.insert(b); continue; }
        }
        seen.insert(b);
        out.push_back(b);
    }
    if (!sysSeen.empty())
        logx::add("[libdump] %zu lib SISTEM dilewat (bukan punya game-nya). Mau ikut? "
                  "pakai --libdump-sys.", sysSeen.size());
    return out;
}

// ---------------------------------------------------------------------------
// jalan
// ---------------------------------------------------------------------------
bool run(const Options& o, Result& r) {
    if (!mem::attached()) {
        logx::add("[libdump] belum attach ke proses game");
        return false;
    }
    const bool autoMode = o.libs.empty();
    std::vector<std::string> libs = autoMode ? autoLibs(o.allSo, o.systemLibs) : o.libs;
    const size_t imgTotalCap = o.maxImageTotalMB << 20;
    size_t imgTotal = 0;
    if (libs.empty()) {
        logx::add("[libdump] nggak ada lib yang bisa di-dump (anti-cheat/engine-nya belum ke-load?)");
        return false;
    }

    const std::string pkg = target::package().empty() ? "unknown" : target::package();
    std::string dir = o.outDir;
    if (dir.empty()) {
        std::string base = pkg;
        const size_t c = base.find(':');
        if (c != std::string::npos) base = base.substr(0, c);
        dir = target::dumpDir() + "/" + base + "/" + dumpout::pkgVersion(base) + "/ac";
    }
    mkdirP(dir.c_str());
    r.dir = dir;

    logx::add("[libdump] mulai: %zu lib -> %s", libs.size(), dir.c_str());
    abortx::beat("libdump: mulai", 0.0f);

    size_t done = 0;
    for (const std::string& lib : libs) {
        if (abortx::requested()) break;
        abortx::span(0.6f, 0.4f);
        const std::vector<RegionOf> regs = regionsOf(lib);
        if (regs.empty()) {
            logx::add("[libdump] %s: nggak ke-load di proses ini -- dilewat", lib.c_str());
            continue;
        }
        Lib L;
        L.name = lib;
        L.path = regs[0].path;
        L.base = regs[0].start;
        L.segments = regs.size();
        for (const auto& x : regs) L.mappedBytes += (size_t)(x.end - x.start);

        logx::add("[libdump] %s: %zu segmen, %s ke-map (base %s)", lib.c_str(), regs.size(),
                  sizeStr(L.mappedBytes).c_str(), hex64(L.base).c_str());

        // ---- gambar memori ----
        // Mode otomatis: lib ENGINE (il2cpp) cuma dilaporin, gambar memorinya
        // nggak ditulis -- .so aslinya udah ada di /data/app/.../lib/ (ratusan
        // MB), yang penting di situ cuma nama kelas/method + vektor deteksinya.
        const bool skipImage = autoMode && !il2::libName().empty() && lib == il2::libName();
        if (skipImage)
            logx::add("[libdump] %s: gambar memori dilewat (lib engine -- .so-nya udah ada di "
                      "folder lib/; cuma laporannya yang ditulis)", lib.c_str());
        const bool budgetLeft = imgTotal < imgTotalCap;
        if (o.image && !skipImage && !budgetLeft)
            logx::add("[libdump] %s: gambar memori dilewat -- batas total %zu MB udah kepakai "
                      "(--ac-imgtotal). Laporannya tetap ditulis.", lib.c_str(), o.maxImageTotalMB);
        if (o.image && !skipImage && budgetLeft) {
            const std::string p = dir + "/" + lib + ".img";
            size_t written = 0;
            if (writeImage(lib, regs, p, o.maxImageMB, written)) {
                imgTotal += written;
                L.imagePath = p;
                logx::add("[libdump] %s: gambar memori %s -> %s", lib.c_str(),
                          sizeStr(written).c_str(), p.c_str());
            } else if (!abortx::requested()) {
                logx::add("[libdump] %s: GAGAL nulis gambar memori (%s) -- folder kebaca?",
                          lib.c_str(), p.c_str());
            }
            writeRegions(lib, regs, dir + "/" + lib + ".regions.txt");
        }

        // ---- laporan ----
        if (o.report) {
            Scan s;
            readElf(lib, regs, s, 4000);
            std::ofstream strs(dir + "/" + lib + ".strings.txt", std::ios::trunc);
            // Lib anti-cheat: cari vektor deteksi + jejak tool. Lib ENGINE: cuma
            // jejak tool (string blob-nya gede banget & isinya bukan detektor,
            // cuma bikin laporan penuh sampah). Modal --libdump-all: dua-duanya.
            const bool vectorsToo = target::isAntiCheatLib(lib) || o.allSo;
            scanStrings(lib, regs, s, &strs, o.maxStrings, vectorsToo);

            L.exports = s.exports;
            L.imports = s.imports;
            L.stringsTotal = s.stringsTotal;
            L.hits = s.vectors.size() + s.traces.size();
            L.vectors = s.vectors;
            L.traces  = s.traces;
            L.reportPath = dir + "/" + lib + ".txt";
            L.stringsPath = dir + "/" + lib + ".strings.txt";
            L.jsonPath = dir + "/" + lib + ".json";

            std::ofstream o1(L.reportPath, std::ios::trunc);
            if (o1.is_open()) {
                o1 << "=== " << lib << " ===\n";
                o1 << "path    : " << L.path << "\n";
                o1 << "base    : " << hex64(L.base) << "\n";
                o1 << "mapped  : " << sizeStr(L.mappedBytes) << " (" << L.segments << " segmen)\n";
                if (!s.soname.empty()) o1 << "soname  : " << s.soname << "\n";
                if (!s.rpath.empty())  o1 << "rpath   : " << s.rpath << "\n";
                o1 << "ELF     : " << s.elfInfo << "\n";
                if (!s.segLines.empty()) {
                    o1 << "\n[SEGMEN]\n";
                    for (const auto& l : s.segLines) o1 << l << "\n";
                }
                if (!s.needed.empty()) {
                    o1 << "\n[NEEDED] " << s.needed.size() << " lib\n";
                    for (const auto& n : s.needed) o1 << "  " << n << "\n";
                }
                o1 << "\n[EXPORT] " << L.exports << " symbol";
                if ((size_t)s.exportLines.size() < L.exports)
                    o1 << " (cuma " << s.exportLines.size() << " yang ditulis)";
                o1 << "\n";
                for (const auto& l : s.exportLines) o1 << l << "\n";

                o1 << "\n[IMPORT] " << L.imports << " symbol dari lib lain\n";
                {
                    std::set<std::string> uniq(s.importLines.begin(), s.importLines.end());
                    for (const auto& l : uniq) o1 << "  " << l << "\n";
                }

                // -- yang bikin "logic"-nya kebaca walau di-strip total --
                o1 << "\n[CONSTRUCTOR] pintu masuk logic (jalan saat lib di-load)\n";
                if (s.initRva) o1 << "  DT_INIT         rva " << hex64(s.initRva) << "\n";
                o1 << "  DT_INIT_ARRAY   " << s.initArray.size() << " fungsi\n";
                for (uint64_t r : s.initArray) o1 << "    rva " << hex64(r) << "\n";
                if (!s.finiArray.empty()) {
                    o1 << "  DT_FINI_ARRAY   " << s.finiArray.size() << " fungsi\n";
                    for (uint64_t r : s.finiArray) o1 << "    rva " << hex64(r) << "\n";
                }

                o1 << "\n[API YANG DIPANGGIL] " << s.relocUses << " pemakaian simbol lewat relokasi\n";
                o1 << "  (nama fungsinya tetap ada walau .dynsym-nya di-strip; rva = slot GOT-nya,\n"
                      "   jadi ini peta \"logic\"-nya: siapa manggil ptrace/inotify/dl_iterate_phdr dll.)\n";
                for (const auto& l : s.relocLines) o1 << l << "\n";

                o1 << "\n[FUNGSI] dari .eh_frame_hdr: " << s.funcRvas.size() << " fungsi\n";
                o1 << "  " << s.funcNote << "\n";
                o1 << "  (daftar rva-nya: " << lib << ".fns.txt -- rebase ke rva 0, "
                      "terus create function di IDA/Ghidra)\n";

                o1 << "\n[STRING MENARIK] dari " << L.stringsTotal << " string yang dipindai\n";
                o1 << "  -- JEJAK TOOL KITA (" << L.traces.size() << ") --\n";
                for (const auto& t : L.traces) o1 << "  !! " << t << "\n";
                o1 << "  -- VEKTOR DETEKSI (" << L.vectors.size() << ") --\n";
                for (const auto& v : L.vectors) o1 << "  ?? " << v << "\n";
                o1 << "\n(daftar string lengkap: " << lib << ".strings.txt)\n";
            }

            std::ofstream o2(L.jsonPath, std::ios::trunc);
            if (o2.is_open()) {
                auto jstr = [](const std::string& v) {
                    std::string s2 = "\"";
                    for (char c : v) {
                        if (c == '"' || c == '\\') { s2 += '\\'; s2 += c; }
                        else if ((unsigned char)c < 0x20) s2 += ' ';
                        else s2 += c;
                    }
                    s2 += "\"";
                    return s2;
                };
                o2 << "{\n  \"lib\": " << jstr(lib) << ",\n";
                o2 << "  \"path\": " << jstr(L.path) << ",\n";
                o2 << "  \"base\": " << jstr(hex64(L.base)) << ",\n";
                o2 << "  \"mappedBytes\": " << L.mappedBytes << ",\n";
                o2 << "  \"segments\": " << L.segments << ",\n";
                o2 << "  \"exports\": " << L.exports << ",\n";
                o2 << "  \"imports\": " << L.imports << ",\n";
                o2 << "  \"strings\": " << L.stringsTotal << ",\n";
                o2 << "  \"needed\": [";
                for (size_t i = 0; i < s.needed.size(); ++i)
                    o2 << (i ? ", " : "") << jstr(s.needed[i]);
                o2 << "],\n  \"jejak_tool\": [";
                for (size_t i = 0; i < L.traces.size(); ++i)
                    o2 << (i ? ", " : "") << jstr(L.traces[i]);
                o2 << "],\n  \"init_rva\": \"" << hex64(s.initRva) << "\",\n";
                o2 << "  \"constructors\": [";
                for (size_t i = 0; i < s.initArray.size(); ++i)
                    o2 << (i ? ", " : "") << jstr(hex64(s.initArray[i]));
                o2 << "],\n  \"api_calls\": " << s.relocUses << ",\n";
                o2 << "  \"functions\": " << s.funcRvas.size() << ",\n";
                o2 << "  \"vektor\": [";
                for (size_t i = 0; i < L.vectors.size(); ++i)
                    o2 << (i ? ", " : "") << jstr(L.vectors[i]);
                o2 << "]\n}\n";
            }

            // ---- daftar fungsi + file pendamping (bisa dipakai TANPA IDA) ----
            // Empat bentuk dari data yang sama:
            //   .fns.txt      : rva (+ ukuran, dari jarak ke fungsi berikutnya)
            //   .fns.r2       : script radare2  -> `r2 -a arm -b 64 <lib>.img -i <lib>.fns.r2`
            //   .fns.ghidra.py: post-script Ghidra (img di-import sebagai raw binary)
            //   .fns.map      : alamat+nama: symbol map, kebaca tools/editor apa pun
            if (!s.funcRvas.empty()) {
                std::vector<uint64_t> fns = s.funcRvas;
                std::sort(fns.begin(), fns.end());
                fns.erase(std::unique(fns.begin(), fns.end()), fns.end());

                std::string txt = "// " + lib + " -- daftar fungsi dari .eh_frame_hdr (" +
                                  std::to_string(fns.size()) + " fungsi)\n// " + s.funcNote +
                                  "\n// rva = file offset segmen = alamat DI DALAM " + lib +
                                  ".img (img-nya disusun mulai 0, jadi nggak perlu rebase)\n";
                char lb[96];
                for (size_t i = 0; i < fns.size(); ++i) {
                    uint64_t sz = (i + 1 < fns.size()) ? (fns[i + 1] - fns[i]) : 0;
                    if (sz > (1u << 20)) sz = 0;      // jarak kegedean -> bukan ukuran fungsi
                    if (sz) snprintf(lb, sizeof(lb), "rva %s sz 0x%llx\n",
                                     hex64(fns[i]).c_str(), (unsigned long long)sz);
                    else    snprintf(lb, sizeof(lb), "rva %s\n", hex64(fns[i]).c_str());
                    txt += lb;
                }
                {
                    std::ofstream o3(dir + "/" + lib + ".fns.txt", std::ios::trunc);
                    if (o3.is_open()) o3 << txt;
                    L.fnsPath = dir + "/" + lib + ".fns.txt";
                }

                // radare2: sekali jalan langsung punya semua fungsinya.
                {
                    std::string r2 = "# " + lib + " -- " + std::to_string(fns.size()) + " fungsi\n"
                                     "# pakai:  r2 -a arm -b 64 " + lib + ".img -i " + lib + ".fns.r2\n"
                                     "e asm.arch=arm\ne asm.bits=64\n";
                    for (const auto& ns : s.namedSyms)
                        r2 += "f sym." + ns.second + " " + hex64(ns.first) + "\n";
                    for (size_t i = 0; i < s.initArray.size(); ++i)
                        r2 += "f ctor" + std::to_string(i) + " " + hex64(s.initArray[i]) + "\n";
                    for (uint64_t r : fns) r2 += "af @ " + hex64(r) + "\n";
                    std::ofstream o4(dir + "/" + lib + ".fns.r2", std::ios::trunc);
                    if (o4.is_open()) { o4 << r2; L.r2Path = dir + "/" + lib + ".fns.r2"; }
                }

                // Ghidra: post-script. `.img` di-import sebagai Raw Binary (ARM/AARCH64),
                // base 0, terus script-nya bikin fungsi + label.
                {
                    std::string g = "# " + lib + " -- dibikin otomatis sama libdump\n"
                                    "# Import " + lib + ".img sebagai Raw Binary (ARM/AARCH64, base 0),\n"
                                    "# terus jalanin script ini (Script Manager > Run).\n"
                                    "fm = currentProgram.getFunctionManager()\n"
                                    "sp = currentProgram.getAddressFactory().getDefaultAddressSpace()\n"
                                    "ADDRS = [";
                    for (size_t i = 0; i < fns.size(); ++i) {
                        g += hex64(fns[i]);
                        if (i + 1 < fns.size()) g += ", ";
                    }
                    g += "]\nNAMES = [";
                    for (size_t i = 0; i < s.namedSyms.size(); ++i) {
                        g += "(" + hex64(s.namedSyms[i].first) + ", \"" + s.namedSyms[i].second + "\")";
                        if (i + 1 < s.namedSyms.size()) g += ", ";
                    }
                    g += "]\nfor a in ADDRS:\n"
                         "    addr = sp.getAddress(a)\n"
                         "    if fm.getFunctionAt(addr) is None:\n"
                         "        try:\n"
                         "            createFunction(addr, None)\n"
                         "        except:\n"
                         "            pass\n"
                         "for a, nm in NAMES:\n"
                         "    try:\n"
                         "        createLabel(sp.getAddress(a), nm, True)\n"
                         "    except:\n"
                         "        pass\n"
                         "print(\"libdump: %d fungsi, %d nama\" % (len(ADDRS), len(NAMES)))\n";
                    std::ofstream o5(dir + "/" + lib + ".fns.ghidra.py", std::ios::trunc);
                    if (o5.is_open()) o5 << g;
                }

                // .map: alamat + nama (export, constructor, slot GOT import).
                {
                    std::ofstream o6(dir + "/" + lib + ".fns.map", std::ios::trunc);
                    if (o6.is_open()) {
                        o6 << "# " << lib << " -- peta alamat/nama (blok  kode  data)\n"
                              "# alamat = rva di dalam " << lib << ".img (base 0)\n";
                        for (const auto& ns : s.namedSyms)
                            o6 << hex64(ns.first) << "  code  " << ns.second << "\n";
                        for (size_t i = 0; i < s.initArray.size(); ++i)
                            o6 << hex64(s.initArray[i]) << "  code  ctor" << i << "\n";
                        for (const auto& l : s.relocLines) {
                            // "  rva 0x50b5d0     JMP  sscanf" -> addr + nama
                            const size_t p1 = l.find("0x");
                            const size_t p2 = l.rfind(' ');
                            if (p1 == std::string::npos || p2 == std::string::npos || p2 <= p1) continue;
                            o6 << l.substr(p1, l.find(' ', p1) - p1) << "  data  "
                               << l.substr(p2 + 1) << " (slot GOT)\n";
                        }
                    }
                }
            }
            L.funcs = s.funcRvas.size();
            L.ctors = s.initArray.size() + (s.initRva ? 1 : 0);
            L.relocUses = s.relocUses;

            logx::add("[libdump] %s: export %zu, import %zu, string %zu | detektor %zu, jejak tool %zu"
                      " | ctor %zu, API %zu, fungsi %zu",
                      lib.c_str(), L.exports, L.imports, L.stringsTotal,
                      L.vectors.size(), L.traces.size(), L.ctors, L.relocUses, L.funcs);
            if (s.initRva || !s.initArray.empty()) {
                std::string ctor = s.initRva ? hex64(s.initRva) : std::string("-");
                for (size_t i = 0; i < s.initArray.size() && i < 8; ++i)
                    ctor += " " + hex64(s.initArray[i]);
                logx::add("[libdump]   ctor (pintu masuk logic): %s", ctor.c_str());
            }
            for (size_t i = 0; i < s.relocLines.size() && i < 25; ++i)
                logx::add("[libdump]   api:%s", s.relocLines[i].c_str());
            if (!s.funcRvas.empty())
                logx::add("[libdump]   %zu fungsi dari .eh_frame_hdr (%s) -> %s.fns.txt",
                          s.funcRvas.size(), s.funcNote.c_str(), lib.c_str());
            for (size_t i = 0; i < L.traces.size() && i < 20; ++i)
                logx::add("[libdump]   jejak tool: %s", clip(L.traces[i]).c_str());
            for (size_t i = 0; i < L.vectors.size() && i < 30; ++i)
                logx::add("[libdump]   detektor: %s", clip(L.vectors[i]).c_str());
        }

        r.libs.push_back(std::move(L));
        abortx::beat("libdump: selesai",
                     (float)((double)(++done) / (double)libs.size()));
    }

    r.ok = !r.libs.empty();
    size_t bytes = 0, total = 0;
    for (const auto& L : r.libs) { bytes += L.mappedBytes; ++total; }
    logx::add("[libdump] selesai: %zu lib (%zu MB) -> %s%s", total,
              (size_t)(bytes / (1024 * 1024)), dir.c_str(),
              abortx::requested() ? " (DIBATALKAN)" : "");
    return r.ok;
}

}  // namespace libdump

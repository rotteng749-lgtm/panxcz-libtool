// ============================================================================
//  gencode.h - generator snippet Dobby-C++ + GameGuardian-Lua dari method yang
//  udah ke-resolve (nama + RVA + module).
//
//  ASAL: Android-LibTool-New/app/src/main/jni/Tool/ClassesTab.cpp
//  (ClassesTab::GenerateCppCode / GenerateGGPatchCode) -- tool in-process yang
//  manggil il2cpp API langsung. Di sini logikanya di-port jadi fungsi string
//  MURNI: inputnya (class, method, rva, module, signature tertulis), jadi bisa
//  jalan di tool EXTERNAL (/proc/<pid>/mem) tanpa injeksi apa pun.
//
//  Yang SENGAJA nggak di-port (butuh hidup di dalam proses game):
//    - CallerView (RuntimeInvoke + boxing + ToString invoke)
//    - PatcherView/asant DobbyHook (mprotect + memcpy di memori sendiri)
//    - HookerView/DobbyInstrument + Frida-Gum Trace (hook di proses sendiri)
//    - Keyboard Unity TouchScreenKeyboard, GC::FindObjects, EGL overlay
//  Snippet yang dihasilkan modul ini TETAP guna: ditempel ke proyek inject
//  Dobby / script GG di luar tool ini.
// ============================================================================
#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace gencode {

// C# -> C++ (convertType di GenerateCppCode + tambahan tipe umum).
// Tipe yang nggak dikenal -> "void*" (sama kayak aslinya).
std::string cppType(const std::string& cs);

// "get_IsDead" / "List`1" / "op_Equality" -> identifier C++ yang aman.
std::string sanitize(const std::string& n);

struct Param {
    std::string type;   // apa adanya dari signature ("ulong", "bool", ...)
    std::string name;
};

struct Sig {
    std::string ret;    // tipe return C# ("" = nggak tahu, nama polos runtime)
    std::string name;   // nama method polos
    bool        isStatic = false;
    std::vector<Param> params;
};

// "static void SetPreInvite(ulong accountId, bool val)" / "void Update()" /
// "get_IsDead" -> Sig. Balikin false kalau string-nya kosong.
bool parseSig(const std::string& full, Sig& out);

struct Target {
    std::string cls;      // apa adanya diketik ("BattleManager")
    std::string method;   // apa adanya diketik ("GetParamsEnable")
    std::string module;   // "libcsharp.so"
    uint64_t    rva = 0;
    Sig         sig;      // hasil parseSig(nama resolved); boleh kosong
};

// Snippet hook Dobby (gaya GenerateCppCode). retExpr = ekspresi C++ buat
// return override ("true", "99999", "12.5f", "NULL"); "" = passthrough
// (cuma manggil old_, tanpa override).
std::string dobbyHook(const Target& t, const std::string& retExpr);

// Script GameGuardian (gaya GenerateGGPatchCode). retKind: "bool" / "i32" /
// "u32" / "void" / "" (nggak tahu). num = nilainya. pid = pid game (buat
// baca /proc/<pid>/maps -> index ELF). Kalau tipenya nggak bisa disintesis,
// emit komentar + arahan --patch.
std::string ggLua(const Target& t, int pid, const std::string& retKind, double num);

// Index segmen executable lib di daftar GG (getRangesList("lib")[i]).
// Port logika auto-detect GenerateGGPatchCode, tapi ke /proc/<pid>/maps
// (aslinya /proc/self/maps karena in-process). -1 = nggak ketemu.
int elfIndexFor(const char* lib, int pid);

// binarySearchClosest port: dari daftar (alamatKode, idx) yang UDAH
// diurutkan menaik, cari entri <= addr dengan jarak <= maxDist.
// Balikin idx entri, atau -1.
long nearestBelow(const std::vector<std::pair<uintptr_t, long>>& sorted,
                  uintptr_t addr, uint64_t maxDist = 0x1000);

// Tulis konten ke dir/nama (mkdir -p otomatis). Balikin path penuh,
// "" = gagal.
std::string saveFile(const std::string& dir, const std::string& name,
                     const std::string& content);

}  // namespace gencode

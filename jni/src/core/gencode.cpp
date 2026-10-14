#include "core/gencode.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <sstream>
#include <sys/stat.h>

namespace gencode {

std::string cppType(const std::string& cs) {
    // Peta convertType di GenerateCppCode (ClassesTab.cpp:1700) + tambahan.
    if (cs == "System.Void")    return "void";
    if (cs == "System.Boolean") return "bool";
    if (cs == "System.Byte")    return "unsigned char";
    if (cs == "System.SByte")   return "signed char";
    if (cs == "System.Int16")   return "short";
    if (cs == "System.UInt16")  return "unsigned short";
    if (cs == "System.Int32")   return "int";
    if (cs == "System.UInt32")  return "uint";
    if (cs == "System.Int64")   return "long long";
    if (cs == "System.UInt64")  return "ulong";
    if (cs == "System.Single")  return "float";
    if (cs == "System.Double")  return "double";
    if (cs == "System.Char")    return "unsigned short";
    if (cs == "System.String")  return "monoString*";
    if (cs == "System.Object")  return "void*";
    if (cs == "System.IntPtr")  return "void*";
    // Nama pendek ala dump ("void", "bool", "ulong", "string", ...) dipakai
    // apa adanya kalau jelas-jelas tipe C++ yang valid.
    if (cs == "void" || cs == "bool" || cs == "int" || cs == "uint" ||
        cs == "float" || cs == "double" || cs == "short" ||
        cs == "char" || cs == "long")
        return cs;
    if (cs == "ulong")  return "unsigned long";
    if (cs == "string") return "monoString*";
    // Array / pointer / generic / enum / tipe game -> void* (kayak aslinya:
    // non-primitif + bukan string = void*, enum = int tapi enum-nya nggak
    // kebaca dari luar jadi void* juga biar nggak ngarang).
    return "void*";
}

std::string sanitize(const std::string& n) {
    std::string o = n;
    for (char& c : o) {
        if (c == '.' || c == '<' || c == '>' || c == '`' || c == ' ')
            c = '_';
    }
    if (!o.empty() && isdigit((unsigned char)o[0])) o = "_" + o;
    return o;
}

static std::string trim(std::string s) {
    size_t b = 0;
    while (b < s.size() && isspace((unsigned char)s[b])) ++b;
    size_t e = s.size();
    while (e > b && isspace((unsigned char)s[e - 1])) --e;
    return s.substr(b, e - b);
}

bool parseSig(const std::string& full, Sig& out) {
    out = Sig();
    std::string s = trim(full);
    if (s.empty()) return false;
    const std::string kStatic = "static ";
    if (s.compare(0, kStatic.size(), kStatic) == 0) {
        out.isStatic = true;
        s = trim(s.substr(kStatic.size()));
    }
    const size_t par = s.find('(');
    if (par == std::string::npos) {
        // Nama polos runtime ("get_IsDead") -- tipe nggak diketahui.
        out.name = trim(s);
        return !out.name.empty();
    }
    std::string head = trim(s.substr(0, par));
    std::string body = s.substr(par + 1);
    const size_t close = body.rfind(')');
    if (close != std::string::npos) body = body.substr(0, close);
    // head = "Ret Name" (namespace + generic ikut di Name, biarin).
    const size_t sp = head.find_last_of(" \t");
    if (sp == std::string::npos) {
        out.name = head;
    } else {
        out.ret  = trim(head.substr(0, sp));
        out.name = trim(head.substr(sp + 1));
    }
    // body = "Tipe nama, Tipe nama, ..." ("ref Tipe nama" -> "Tipe nama").
    std::string cur;
    for (size_t i = 0; i <= body.size(); ++i) {
        char c = i < body.size() ? body[i] : ',';
        // Jangan belah koma di dalam generic "Dictionary<A, B>".
        if (c == ',' ) {
            int depth = 0;
            for (char d : cur) {
                if (d == '<') ++depth;
                else if (d == '>') --depth;
            }
            if (depth != 0) { cur += c; continue; }
            std::string p = trim(cur);
            cur.clear();
            // Buang modifier C# ("ref"/"out"/"in"/"params"/"[In]"/"[Out]").
            while (true) {
                bool cut = false;
                for (const char* kw : {"ref ", "out ", "in ", "params "}) {
                    if (p.compare(0, strlen(kw), kw) == 0) {
                        p = trim(p.substr(strlen(kw)));
                        cut = true;
                        break;
                    }
                }
                if (p.compare(0, 1, "[") == 0) {
                    size_t e = p.find(']');
                    if (e != std::string::npos) { p = trim(p.substr(e + 1)); cut = true; }
                    else break;
                }
                if (!cut) break;
            }
            if (!p.empty()) {
                Param pm;
                const size_t lsp = p.find_last_of(" \t");
                if (lsp == std::string::npos) { pm.type = p; pm.name = "arg"; }
                else { pm.type = trim(p.substr(0, lsp)); pm.name = trim(p.substr(lsp + 1)); }
                if (pm.name.empty()) pm.name = "arg";
                out.params.push_back(pm);
            }
        } else {
            cur += c;
        }
    }
    return !out.name.empty();
}

std::string dobbyHook(const Target& t, const std::string& retExpr) {
    // Struktur = GenerateCppCode: toggle bool + old_ typedef + hook +
    // DobbyHook. Bedanya: signature diambil dari parseSig (bukan Il2CppType
    // live), return override dari retExpr (bukan oMap/callResults live).
    std::string safe = sanitize(t.method.empty() ? t.cls : t.method);
    std::string retCpp = t.sig.ret.empty() ? "void*" : cppType(t.sig.ret);
    std::ostringstream ss;
    ss << "//" << t.cls << "::" << (t.method.empty() ? t.sig.name : t.method)
       << "  (" << (t.module.empty() ? "?" : t.module) << "+0x"
       << std::hex << std::uppercase << t.rva << std::dec << ")\n";
    ss << "bool Is" << safe << " = false;\n";
    ss << retCpp << " (*old_" << safe << ")(void* instance";
    for (const auto& p : t.sig.params) ss << ", " << cppType(p.type) << " " << p.name;
    ss << ");\n";
    ss << retCpp << " " << safe << "(void* instance";
    for (const auto& p : t.sig.params) ss << ", " << cppType(p.type) << " " << p.name;
    ss << ") {\n";
    ss << "\tif (instance != NULL) {\n";
    ss << "\t\tif (Is" << safe << ") {\n";
    if (!retExpr.empty() && retCpp != "void" && retCpp != "void*") {
        ss << "\t\t\treturn " << retExpr << ";\n";
    } else if (retExpr == "@ret" || (!retExpr.empty() && retCpp == "void")) {
        ss << "\t\t\treturn;\n";   // "@ret" = NOP-inject (void)
    } else {
        ss << "\t\t\t// Do something\n";
    }
    ss << "\t\t}\n";
    ss << "\t}\n";
    ss << "\treturn old_" << safe << "(instance";
    for (const auto& p : t.sig.params) ss << ", " << p.name;
    ss << ");\n";
    ss << "}\n\n";
    ss << "DobbyHook((void *)getAbsoluteAddress(\"" << t.module << "\", 0x"
       << std::hex << std::uppercase << t.rva << "), (void *) "
       << safe << ", (void **) &old_" << safe << ");";
    return ss.str();
}

int elfIndexFor(const char* lib, int pid) {
    // Port GenerateGGPatchCode: hitung segmen gabungan (GG merge segmen
    // tetangga yang perms-nya sama), yang executable = index-nya.
    // Aslinya /proc/self/maps (in-process); di sini /proc/<pid>/maps.
    if (!lib || !lib[0] || pid <= 0) return -1;
    char path[64];
    snprintf(path, sizeof(path), "/proc/%d/maps", pid);
    FILE* fp = fopen(path, "rt");
    if (!fp) return -1;
    int idx = 1, seg = 0, found = -1;
    (void)idx;
    uintptr_t lastEnd = 0;
    char lastPerms[8] = "";
    char line[512];
    while (fgets(line, sizeof(line), fp)) {
        if (!strstr(line, lib)) continue;
        uintptr_t start = 0, end = 0;
        char perms[8] = "";
        if (sscanf(line, "%lx-%lx %7s", &start, &end, perms) < 3) continue;
        if (start != lastEnd || strcmp(perms, lastPerms) != 0) ++seg;
        if (strstr(perms, "x")) { found = seg; break; }
        lastEnd = end;
        snprintf(lastPerms, sizeof(lastPerms), "%s", perms);
    }
    fclose(fp);
    return found;
}

std::string ggLua(const Target& t, int pid, const std::string& retKind, double num) {
    // Struktur = GenerateGGPatchCode: header setValues + base dari
    // getRangesList("lib")[elfIndex].start + patch MOV/RET.
    std::ostringstream ss;
    int elfIndex = elfIndexFor(t.module.c_str(), pid);
    if (elfIndex < 0) elfIndex = 1;
    ss << "-- GG Script: " << t.cls << "::" << t.method << "\n";
    ss << "function setValues(address, flags, value) gg.setValues({[1] = {address = address, flags = flags, value = value}}) end\n";
    ss << "il2cpp = gg.getRangesList(\"" << t.module << "\")[" << elfIndex << "].start\n\n";
    char addr[64];
    snprintf(addr, sizeof(addr), "il2cpp + 0x%llX", (unsigned long long)t.rva);
    bool emitted = false;
    if (retKind == "bool") {
        ss << "setValues(" << addr << ", 4, \"~A8 MOV W0, #" << (num != 0.0 ? "1" : "0") << "\")\n";
        ss << "setValues(" << addr << " + 4, 4, \"~A8 RET\")\n";
        emitted = true;
    } else if ((retKind == "i32" || retKind == "u32") && num >= 0 && num <= 65535 &&
               num == (double)(long long)num) {
        // MOV W0, #imm cuma muat 16 bit; yang lebih gede = MOVZ/MOVK manual.
        ss << "setValues(" << addr << ", 4, \"~A8 MOV W0, #" << (long long)num << "\")\n";
        ss << "setValues(" << addr << " + 4, 4, \"~A8 RET\")\n";
        emitted = true;
    } else if (retKind == "void") {
        ss << "setValues(" << addr << ", 4, \"~A8 RET\")\n";
        emitted = true;
    }
    if (!emitted) {
        ss << "-- Tipe '" << retKind << "' nggak bisa disintesis jadi MOV/RET.\n";
        ss << "-- Pakai --forceret / --patch di libtool, atau tulis byte manual:\n";
        ss << "-- target: " << t.module << "+0x"
           << std::hex << std::uppercase << t.rva << "\n";
    }
    return ss.str();
}

long nearestBelow(const std::vector<std::pair<uintptr_t, long>>& sorted,
                  uintptr_t addr, uint64_t maxDist) {
    if (sorted.empty()) return -1;
    size_t lo = 0, hi = sorted.size();
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (sorted[mid].first <= addr) lo = mid + 1;
        else hi = mid;
    }
    if (lo == 0) return -1;
    size_t i = lo - 1;
    if (addr - sorted[i].first > maxDist) return -1;
    return sorted[i].second;
}

namespace {
void mkdirP(const char* path) {
    char tmp[512];
    snprintf(tmp, sizeof(tmp), "%s", path);
    for (char* p = tmp + 1; *p; ++p)
        if (*p == '/') { *p = 0; ::mkdir(tmp, 0777); *p = '/'; }
    ::mkdir(tmp, 0777);
}
}  // namespace

std::string saveFile(const std::string& dir, const std::string& name,
                     const std::string& content) {
    mkdirP(dir.c_str());
    std::string path = dir + "/" + name;
    FILE* fp = fopen(path.c_str(), "wb");
    if (!fp) return "";
    fwrite(content.data(), 1, content.size(), fp);
    fclose(fp);
    return path;
}

}  // namespace gencode

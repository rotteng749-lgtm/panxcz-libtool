// ============================================================================
//  Panxcz LibTool - external ELF overlay il2cpp (ImGui + OpenGL ES)
//  by Panxcz & Freebuff
// ============================================================================
#include "main.h"

#include "core/abort.h"
#include "core/mem.h"
#include "core/metapool.h"
#include "core/il2cpp.h"
#include "core/scanner.h"
#include "core/keys.h"
#include "core/log.h"
#include "core/target.h"
#include "core/explore.h"
#include "core/offlinedump.h"
#include "core/dumpout.h"
#include "core/libdump.h"
#include "core/metapool.h"
#include "core/patch.h"
#include "core/gencode.h"
#include "core/memview.h"
#include "core/bypass.h"
#include <string>
#include <vector>
#include "ui/ui.h"

#include <atomic>
#include <unistd.h>
#include <pthread.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <csignal>
#include <csetjmp>
#include <cctype>
#include <ctime>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <android/log.h>

static const char* sigName(int s);   // definisinya di bawah (dipakai faultHandler)

static std::atomic<bool> main_thread_flag{true};
static std::atomic<bool> g_frame_done{true};

// --probe Namespace::Class  : cari class itu terus tulis hasilnya ke logcat
//                             (buat tes tanpa nyentuh layar sama sekali)
// --exit                    : keluar sendiri setelah probe selesai
static std::string g_probe;
static bool        g_probeExit = false;

// --forceret / --unforce (lihat runForceRet())
struct ForceSpec { std::string cls, method, value, type; };
static std::vector<ForceSpec>   g_forceSpecs;
static std::vector<std::string> g_unforceSpecs;
static std::string              g_forceFind;
static bool                     g_forceReapply = false;
static bool                     g_forceListFile = false;
static std::vector<std::string> g_fixCode;

// --gencode Class::Method[=nilai[:tipe]] : emit snippet hook Dobby-C++ buat
//   method itu (disimpan ke /sdcard/panxcz-tools/gencode/<pkg>/). Format spek
//   sama kayak --forceret; tanpa "=nilai" = template passthrough.
// --ggcode Class::Method[=nilai[:tipe]]  : emit script GameGuardian-Lua
//   (MOV W0,#v + RET buat bool/int kecil, sisanya arahan manual).
// --huntaddr 0x... : alamat kode -> method terdekat di pool (butuh --pool).
//   Port GenerateCppCode / GenerateGGPatchCode / binarySearchClosest dari
//   Android-LibTool-New (Tool/ClassesTab.cpp, Tool/Frida.cpp) -- versi string
//   murni buat tool external (tanpa injeksi, tanpa RuntimeInvoke).
static std::vector<ForceSpec>   g_genCode;
static std::vector<ForceSpec>   g_ggCode;
static std::string              g_huntAddr;

// --hexdump Class::Method[:len] | lib.so:0xRVA[:len] | 0xADDR[:len]
// --asmview  (bentuk sama) : port HexView / AssemblyView (ClassesTab.cpp) --
//   byte dibaca dari /proc/<pid>/mem, decode pakai memview (tanpa Capstone).
// --findobj Class[:max] : instance hidup (port ImGuiObjectSelector; GC live
//   scan diganti pindai klass-pointer ala explore::instances).
// --inspect Class@0xADDR : bedah field satu object (port ImGuiJson).
// --strings [filter]     : pindai string UTF-16 di memori (port Strings).
// --search kata [--by class|method|field] : cari di dump+pool (port FilterClasses).
static std::vector<std::string> g_hexDump;
static std::vector<std::string> g_asmView;
static std::vector<std::string> g_findObj;
static std::vector<std::string> g_inspect;
static bool                     g_strings = false;
static std::string              g_stringsFilter;
// --search boleh diulang; --by nempel ke --search BERIKUTNYA
// (--by class --search Foo --by method --search Bar).
struct SearchReq { std::string kw, by; };
static std::vector<SearchReq>   g_searches;
static std::string              g_searchBy = "class";

// --dumpout        : DUMP SENDIRI (root, tanpa frida) -- lihat core/dumpout.h
// --all-libs       : pindai semua module il2cpp (MLBB: liblogic+libfirst+libcsharp)
// --cols a,b       : pilih kolom (field,method,slot,token,parent)
// --ns X / --like X: filter kelas        --json / --header / --ptr
// --classes N      : batasi jumlah kelas (tes cepat)
// --scan-only      : scan aja (hitung kelas/field/method, nggak nulis file)
// --outdir <dir>   : folder hasil (default /sdcard/panxcz-tools/dumper/<pkg>/<versi>/)
// --stay           : jangan keluar sendiri abis dump (default: keluar)
// --headless       : JANGAN bikin overlay (ImGui/EGL/touch) -- kerja langsung
//                    jalan tanpa layar. Dipakai kalau ROM nolak surface dari
//                    proses luar (ANWC/SurfaceFlinger fault) tapi dump tetap
//                    pengin jalan. Implisit nyala kalau --dumpout/--libdump
//                    dipanggil bareng --exit dan overlay gagal dibikin.
static bool             g_dumpOut = false;
static bool             g_dumpStay = false;
static bool             g_headless = false;
static bool             g_headlessAuto = false;   // overlay fault -> fallback
static time_t           g_startTime = 0;      // buat "waktu" di RINGKASAN
// Hasil terakhir (buat RINGKASAN penutup). Diisi runDumpOut/runLibDump.
static bool             g_dumpRan = false, g_dumpOk = false;
static dumpout::Result  g_dumpRes;
static bool             g_ldRan = false;
static size_t           g_ldCount = 0;
static std::string      g_ldDir;
static dumpout::Options g_dumpOpt;
// --libdump / --ac : dump lib anti-cheat (+ engine): gambar memori + laporan
// deteksi vektor. Lihat core/libdump.h. Nyalanya juga ikut `--dumpout --all-libs`.
static bool             g_libDump = false;
static bool             g_libDumpAllSo = false;
static bool             g_libDumpOff = false;      // --no-ac
static bool             g_libDumpNoImg = false;
static bool             g_libDumpNoReport = false;
static std::string      g_libDumpOut;
static std::vector<std::string> g_libDumpLibs;
static size_t           g_libDumpImgMaxMB = 768;      // --ac-imgmax  (per lib)
static size_t           g_libDumpImgTotalMB = 1024;    // --ac-imgtotal (semua lib)
static bool             g_libDumpSys = false;          // --libdump-sys
// --huntmeth Class::Method : lacak layout MethodInfo dari satu method yang
// RVA-nya udah kita tahu (dari dump) -- buat diagnosa game ber-layout beda.
static std::string      g_huntMeth;
// --diagclass Class : diagnosa lengkap satu kelas (array/field/method/tabel
// accessor) -- dipakai buat nyari layout metadata game yang dimodif.
static std::string      g_diagClass;
// --miprobe Class::Method : hexdump MethodInfo satu method + lacak nama-nya.
static std::string      g_miProbe;
// --foffprobe Class : diagnosa ARRAY OFFSET FIELD di dalam Il2CppClass (il2cpp
//   baru nggak selalu nyimpen offset di FieldInfo) -- lihat explore::diagFieldOffsets.
static std::string      g_foffProbe;
// --patch-file <path> : muat daftar patch dari file (auto-apply pas game siap)
static std::string      g_patchFile;
// --peek <spec> : cetak byte di alamat tertentu ke log (buat ngecek hasil patch
//                 dan nyari alamat). spec = "0x7a123450" atau "libil2cpp.so:0x1234".
static std::string      g_peek;
static size_t           g_peekLen = 32;      // --peek-len N
// --pool : cari + kalibrasi pool MethodInfo global terus lapor hasilnya.
static bool             g_initPool = false;
// --typeprobe : diagnosa offset typeId di Il2CppClass (lihat metapool.h).
static bool             g_typeProbe = false;
// --check : diagnosa LINGKUNGAN (root/izin, /proc/pid/mem, region, baca blok
// 4 MB) + verdict di log. Dipakai kalau dump gagal di device lain: log-nya
// langsung nunjukin penyebabnya, nggak perlu nebak.
static bool             g_check = false;

// ---------------------------------------------------------------------------
// MODE OTOMATIS (`./panxcz_libtool.sh` -- TANPA argumen apa pun, ini default)
//
// Cukup satu perintah itu: kalau belum root, tool naikin diri sendiri lewat su;
// terus cari game sendiri, attach, dump sendiri. Nggak perlu ngetik
// "--dumpout --all-libs --json --header --ptr" dan nggak perlu pilih mode:
//   1. kalau target belum ada (cfg kosong) atau game-nya nggak jalan -> cari
//      SENDIRI proses yang punya lib il2cpp
//   2. attach + tunggu lib il2cpp ke-load
//   3. langsung dump sendiri (semua lib + .json/.h/.ptrs.tsv)
//   4. menu tetap dibuka (nggak keluar sendiri) biar bisa lanjut main-main
// ---------------------------------------------------------------------------
static bool             g_auto = false;
static const double     kAutoDumpDelaySec = 3.0;   // kasih waktu map/lib stabil dulu
static const double     kAutoFindSec      = 300.0; // nunggu game-nya dibuka (auto mode)
static const int        kAutoAttachTries  = 240;   // x0.5 s = 120 s nunggu prosesnya nongol
static const int        kAutoLibTries     = 180;   // x1 s   = 180 s nunggu lib il2cpp ke-load

// ---------------------------------------------------------------------------
// Naikin diri sendiri ke root, biar cukup `./panxcz-libtool.sh` (nggak perlu
// nulis `su -c ...` manual) -- dipakai main() sebelum apa-apa jalan.
// Balikin: exit code child kalau su-nya jalan (main() langsung balik),
//          -1 kalau harus lanjut sebagai user biasa.
// ---------------------------------------------------------------------------
static std::string shQuote(const std::string& s) {
    std::string o = "'";
    for (char c : s) { if (c == '\'') o += "'\\''"; else o += c; }
    o += "'";
    return o;
}

static int tryElevate(int argc, char** argv) {
    if (geteuid() == 0) return -1;                 // udah root, lanjut aja
    const char* off = getenv("PANXCZ_NO_SU");
    if (off && off[0] == '1') return -1;           // user matiin sendiri

    std::string cmd;
    for (int i = 0; i < argc; ++i) {
        if (i) cmd += ' ';
        cmd += shQuote(argv[i] ? argv[i] : "");
    }
    logx::add("[su] belum root -- nyoba naik sendiri lewat su (nggak mau: PANXCZ_NO_SU=1)");

    pid_t pid = fork();
    if (pid < 0) return -1;
    if (pid == 0) {
        setenv("PANXCZ_NO_SU", "1", 1);          // anaknya udah di dalam su
        // PATH dulu, terus path khas Magisk/KernelSU/Superuser (di beberapa
        // ROM su-nya nggak ada di PATH buat user biasa). PANXCZ_SU = path manual.
        const char* manual = getenv("PANXCZ_SU");
        if (manual && manual[0]) execl(manual, "su", "-c", cmd.c_str(), (char*)nullptr);
        execlp("su", "su", "-c", cmd.c_str(), (char*)nullptr);
        static const char* kSuPaths[] = {
            "/system/bin/su", "/system/xbin/su", "/sbin/su", "/su/bin/su",
            "/debug_ramdisk/su", "/data/adb/ksu/bin/su", "/data/adb/magisk/su",
            "/vendor/bin/su", "/product/bin/su",
        };
        for (const char* p : kSuPaths) execl(p, "su", "-c", cmd.c_str(), (char*)nullptr);
        _exit(127);                                 // 127 = su-nya nggak ada
    }

    int st = 0;
    bool reaped = false;
    for (int i = 0; i < 90 && !reaped; ++i) {       // tunggu max ~45 s
        pid_t r = waitpid(pid, &st, WNOHANG);
        if (r == pid) reaped = true;
        else if (r < 0) break;
        else usleep(500000);
    }
    if (!reaped) {                                  // su ngegantung (prompt ditolak?)
        kill(pid, SIGTERM);
        usleep(200000);
        kill(pid, SIGKILL);
        logx::add("[su] su nggak nyahut -- jalanin manual: su -c %s", cmd.c_str());
        return -1;
    }
    if (WIFEXITED(st) && WEXITSTATUS(st) == 127) {
        // Beberapa ROM (KernelSU + SUSFS) MWNYEMBUNYIIN su dari user biasa, jadi
        // exe-nya balik 127 ("inaccessible or not found") -- bukan bug tool-nya.
        logx::add("[su] su nggak ada / nggak kebaca dari user biasa (ROM-nya nyembunyiin "
                  "su). Jalanin dari terminal yang udah root (MT Manager: Terminal), "
                  "atau: su -c %s   |   su di path lain? PANXCZ_SU=<path> ./panxcz_libtool.sh",
                  cmd.c_str());
        return -1;
    }
    if (WIFEXITED(st)) {
        logx::add("[su] selesai sebagai root (exit %d)", WEXITSTATUS(st));
        return WEXITSTATUS(st);
    }
    return -1;                                      // mati kena sinyal -> lanjut sebagai user
}

// "a,b,c" -> {"a","b","c"} (spasi & entri kosong dibuang)
static void splitList(const char* s, std::vector<std::string>& out) {
    if (!s) return;
    std::string cur;
    for (const char* p = s; ; ++p) {
        if (!*p || *p == ',') {
            while (!cur.empty() && isspace((unsigned char)cur.back())) cur.pop_back();
            size_t b = 0;
            while (b < cur.size() && isspace((unsigned char)cur[b])) ++b;
            if (b < cur.size()) out.push_back(cur.substr(b));
            cur.clear();
            if (!*p) break;
        } else {
            cur += *p;
        }
    }
}

// ---------------------------------------------------------------------------
// pengaman: pembuatan surface lewat SurfaceFlinger bisa fault di ROM tertentu.
// Tanpa ini prosesnya mati bisu (SIGBUS/SIGSEGV tanpa log sama sekali).
// ---------------------------------------------------------------------------
static sigjmp_buf        g_jmp;
static volatile sig_atomic_t g_guard = 0;
static volatile int      g_sig = 0;

// Di luar blok init (g_guard == 0) dulu fault-nya bikin proses keluar BISU
// tanpa jejak sama sekali -- susah dibedain dari "dimatiin sistem". Sekarang
// alamat fault-nya selalu dicatat ke logcat sebelum keluar.
static void faultHandler(int sig, siginfo_t* si, void* /*uc*/) {
    if (g_guard) {
        g_guard = 0;
        g_sig = sig;
        siglongjmp(g_jmp, sig);
    }
    char buf[160];
    int n = snprintf(buf, sizeof(buf),
                     "[boot] FATAL %s di 0x%lx -- tool keluar (overlay/rom nolak?)",
                     sigName(sig), (unsigned long)(si ? (uintptr_t)si->si_addr : 0));
    if (n > 0) {
        // tulis langsung pakai syscall (handler sinyal: jangan alokasi/malloc)
        syscall(__NR_write, 2, buf, (size_t)n);
        __android_log_write(ANDROID_LOG_ERROR, "panxcz_libtool", buf);
    }
    _exit(139);
}

static void installFaultGuard() {
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = faultHandler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_NODEFER | SA_SIGINFO;
    sigaction(SIGSEGV, &sa, nullptr);
    sigaction(SIGBUS,  &sa, nullptr);
    sigaction(SIGABRT, &sa, nullptr);
}

static const char* sigName(int s) {
    switch (s) {
        case SIGSEGV: return "SIGSEGV";
        case SIGBUS:  return "SIGBUS";
        default:      return "signal";
    }
}

// ---------------------------------------------------------------------------
// --probe: cari class il2cpp terus tulis field/method-nya ke logcat.
// ---------------------------------------------------------------------------
// Ada kerjaan lain yang masih nyusul setelah probe/diagnosa? Kalau ada, tool
// TIDAK boleh keluar: teardown overlay bikin pembacaan memori gagal, jadi
// dump/scan-nya kosong atau nggak jalan sama sekali.
//
// (Ini dulu kebalik di beberapa tempat -- mis. `--huntmeth X --dumpout --exit`
//  keluar SEBELUM dump-nya mulai, dan dump yang gagal +
//  `--probe X --dumpout --exit` mati di tengah pindai. Sekarang satu helper,
//  satu arti.)
static bool g_dumpOutDone = false;   // dump kelas UDAH selesai (lihat workPending)
static bool g_libDumpDone = false;   // dump lib UDAH selesai

static bool workPending(bool ignorePool = false) {
    return (g_dumpOut && !g_dumpOutDone) || (g_libDump && !g_libDumpDone) ||
           (!ignorePool && g_initPool) || !g_diagClass.empty() ||
           !g_forceSpecs.empty() || !g_unforceSpecs.empty() || !g_forceFind.empty() ||
           g_forceReapply || g_forceListFile || !g_fixCode.empty() ||
           !g_genCode.empty() || !g_ggCode.empty() || !g_huntAddr.empty() ||
           !g_hexDump.empty() || !g_asmView.empty() || !g_findObj.empty() ||
           !g_inspect.empty() || g_strings || !g_searches.empty() ||
           !g_huntMeth.empty() || !g_miProbe.empty() || !g_foffProbe.empty();
}

static void runProbe() {
    if (g_probe.empty()) {
        // --exit tanpa --probe: langsung keluar. Kalau masih ada kerjaan lain
        // (dump / diagnosa kelas / lacak method) JANGAN -- teardown overlay
        // bikin pembacaan memori gagal, jadi hasil diagnosanya kosong melompong
        // (dulu kejadian: --diagclass ... --exit -> semua array "nggak ketemu").
        if (g_probeExit && !workPending()) ui::requestExit();
        return;
    }

    std::string ns, name = g_probe;
    const size_t sep = g_probe.find("::");
    if (sep != std::string::npos) {
        ns   = g_probe.substr(0, sep);
        name = g_probe.substr(sep + 2);
    }
    logx::add("[probe] nyari class %s ...", g_probe.c_str());

    if (!explore::resolve(name.c_str(), ns.empty() ? nullptr : ns.c_str())) {
        logx::add("[probe] GAGAL: class '%s' nggak ketemu", g_probe.c_str());
        if (g_probeExit && !workPending()) ui::requestExit();
        return;
    }

    const auto& k = explore::info();
    logx::add("[probe] OK %s::%s @0x%lx image=%s fields=%zu methods=%zu",
              k.ns.c_str(), k.name.c_str(), (unsigned long)k.addr,
              k.image.c_str(), k.fields.size(), k.methods.size());
    for (size_t i = 0; i < k.fields.size() && i < 15; ++i)
        logx::add("[probe]   field  %-28s off=0x%X%s", k.fields[i].name.c_str(),
                  k.fields[i].offset, k.fields[i].isStatic ? "  (static)" : "");

    // nilai field statis: bukti bahwa slot + static_fields hasil deteksi
    // beneran nunjuk data game (bukan cuma alamat yang kebetulan kebaca).
    if (k.statics) {
        logx::add("[probe] static_fields 0x%lx (+0x%X) -- nilai:",
                  (unsigned long)k.statics, k.staticsPtrOff);
        for (size_t i = 0; i < k.fields.size() && i < 15; ++i) {
            const auto& f = k.fields[i];
            if (!f.isStatic) continue;
            double num = 0;
            char sv[96];
            sv[0] = 0;
            const explore::FType t = explore::guessType(f.typeEnum);
            if (!explore::readValue(k.statics, f.offset, t, num, sv, sizeof(sv))) continue;
            if (t == explore::FType::String)
                logx::add("[probe]   value  %-24s = 0x%llX \"%s\"", f.name.c_str(),
                          (unsigned long long)(uint64_t)num, sv);
            else
                logx::add("[probe]   value  %-24s = %.0f (0x%llX)", f.name.c_str(), num,
                          (unsigned long long)(uint64_t)num);
        }
    }
    // method: RVA-nya relatif module pemilik kode (dump v3.4 nulis tag module-nya),
    // jadi sekalian dicetak alamat absolut + module-nya. @0x0 = module-nya nggak
    // ke-load di proses ini.
    for (size_t i = 0; i < k.methods.size() && i < 15; ++i) {
        const auto& m = k.methods[i];
        std::string tag = m.module.empty() ? std::string() : ("  [" + m.module + "]");
        logx::add("[probe]   method %-28s rva=0x%llX @0x%lx%s", m.name.c_str(),
                  (unsigned long long)m.rva, (unsigned long)m.ptr, tag.c_str());
    }

    if (g_probeExit && !g_dumpOut) ui::requestExit();
}

// ---------------------------------------------------------------------------
// --forceret Class::Method=nilai[:tipe]
//
// Nggak perlu nulis byte manual: method-nya dipilih pakai NAMA, tool yang nyusun
// stub "langsung balikin nilai"-nya (lihat core/patch.h). Contoh:
//
//   --forceret "Enemy::get_IsDead=true"          -> always dead
//   --forceret "GameManager::get_Gold=99999"     -> gold 99999
//   --forceret "Hero::TakeDamage=off"            -> method di-NOP (ret)
//   --forceret "Unit::get_Speed=12.5:float"      -> tipe dipaksa float
//   --unforce "Hero::TakeDamage"                 -> balikin byte aslinya
// ---------------------------------------------------------------------------
static void parseForceSpec(const std::string& in, ForceSpec& out) {
    std::string left = in, right;
    const size_t eq = in.find('=');
    if (eq != std::string::npos) { left = in.substr(0, eq); right = in.substr(eq + 1); }

    const size_t sep = left.find("::");
    if (sep != std::string::npos) {
        out.cls    = left.substr(0, sep);
        out.method = left.substr(sep + 2);
    } else {
        out.method = left;
    }
    // "nilai:tipe" (tipe opsional)
    const size_t colon = right.find(':');
    if (colon != std::string::npos) {
        out.value = right.substr(0, colon);
        out.type  = right.substr(colon + 1);
    } else {
        out.value = right;
    }
}

// Nama kelas di pool itu nama POLOS dari metadata ("Debug"), sedangkan yang
// diketik orang biasanya lengkap sama namespace ("UnityEngine.Debug"). Jadi
// yang dibandingin nama polosnya.
static std::string bareClassName(const std::string& n) {
    std::string s = n;
    const size_t bt = s.find('`');            // List`1 -> List
    if (bt != std::string::npos) {
        size_t e = bt + 1;
        while (e < s.size() && isdigit((unsigned char)s[e])) ++e;
        s = s.substr(0, bt) + s.substr(e);
    }
    const size_t dot = s.find_last_of('.');
    return dot == std::string::npos ? s : s.substr(dot + 1);
}

// nama method di menu kadang ikut signature ("bool get_IsDead()"); yang dicari
// orang cuma "get_IsDead".
static std::string bareMethodName(const std::string& n) {
    std::string s = n;
    const size_t par = s.find('(');
    if (par != std::string::npos) s = s.substr(0, par);
    const size_t sp = s.find_last_of(" \t");
    if (sp != std::string::npos) s = s.substr(sp + 1);
    return s;
}

static bool forceKindFromSpec(const ForceSpec& s, patch::Ret& kind, double& num,
                              uint64_t& ptr) {
    std::string v = s.value;
    num = 0;
    ptr = 0;
    while (!v.empty() && (v[0] == ' ' || v[0] == '\t')) v.erase(0, 1);
    while (!v.empty() && (v.back() == ' ' || v.back() == '\t')) v.pop_back();
    const std::string low = [&] { std::string t = v; for (char& c : t) c = (char)tolower((unsigned char)c); return t; }();

    if (!s.type.empty()) {
        if (!patch::retFromName(s.type, kind)) return false;
    } else if (v.empty() || low == "off" || low == "nop" || low == "void") {
        kind = patch::Ret::Void;                 // "method ini jangan jalan"
        return true;
    } else if (low == "true" || low == "false") {
        kind = patch::Ret::Bool;
    } else if (low.find('.') != std::string::npos) {
        kind = patch::Ret::Float;
    } else if (strlen(v.c_str()) > 10) {
        kind = patch::Ret::I64;
    } else {
        kind = patch::Ret::I32;
    }

    if (kind == patch::Ret::Bool) { num = (low == "true" || low == "1") ? 1.0 : 0.0; return true; }
    if (kind == patch::Ret::Ptr)  { ptr = v.empty() ? 0 : strtoull(v.c_str(), nullptr, 0); return true; }
    if (v.empty()) return false;
    num = strtod(v.c_str(), nullptr);
    return true;
}

// --forcefind METHOD : method itu ada di kelas mana aja (nama kelas + alamat
// kodenya). Dipakai buat nyusun spek --forceret kalau nama kelasnya nggak jelas.
static void runForceFind(const std::string& meth) {
    if (meth.empty()) return;
    if (!metapool::ready()) {
        logx::add("[force] --forcefind butuh pool (tambahin --pool)");
        return;
    }
    int n = 0;
    for (const auto& e : metapool::all()) {
        if (e.name.empty()) continue;
        if (bareMethodName(e.name) != meth) continue;
        std::string kn;
        if (!metapool::klassName(e.klass, kn)) kn = "?";
        logx::add("[force]   %s::%s  kode @0x%lx rva=0x%llx (%s)%s", kn.c_str(),
                  e.name.c_str(), (unsigned long)e.code, (unsigned long long)e.rva,
                  e.module.empty() ? "?" : e.module.c_str(),
                  e.code ? "" : "  [MethodInfo belum ada -> nggak bisa dipatch]");
        if (++n >= 25) { logx::add("[force]   ... (dipotong 25)"); break; }
    }
    logx::add("[force] %d method bernama '%s' di pool", n, meth.c_str());
}

static void runForceRet() {
    // --fixcode lib.so:0xRVA[:len] = balikin byte dari library di disk
    for (const auto& spec : g_fixCode) {
        const size_t colon = spec.find(':');
        if (colon == std::string::npos) { logx::add("[force] --fixcode '%s' butuh lib.so:0xRVA", spec.c_str()); continue; }
        std::string rvaStr = spec.substr(colon + 1);
        size_t len = 8;
        const size_t colon2 = rvaStr.find(':');
        if (colon2 != std::string::npos) { len = (size_t)strtoul(rvaStr.c_str() + colon2 + 1, nullptr, 0); rvaStr = rvaStr.substr(0, colon2); }
        std::string err;
        if (!patch::restoreFromLib(spec.substr(0, colon), strtoull(rvaStr.c_str(), nullptr, 16), len, &err))
            logx::add("[force] --fixcode %s GAGAL: %s", spec.c_str(), err.c_str());
    }

    // pasang ulang yang masih tercatat di file (game baru di-restart / lib
    // di-reload pas masuk match -> kodenya balik, catatannya masih ada)
    if (g_forceReapply) patch::reapplyFromFile();
    if (g_forceListFile) {
        logx::add("[force] catatan force-return: %s", patch::forcePath().c_str());
        logx::add("[force]   %zu method tercatat -- `--unforce all` buat ngembaliin semuanya",
                  patch::fileForcedCount());
    }
    if (g_forceFind.empty() && g_forceSpecs.empty() && g_unforceSpecs.empty()) {
        g_forceReapply = g_forceListFile = false;
        g_fixCode.clear();
        return;                                  // kerjaannya udah di atas
    }
    if (!g_forceFind.empty()) runForceFind(g_forceFind);

    // ---- restore dulu (bisa dipakai buat "matiin" tanpa nulis ulang) ----
    for (const auto& spec : g_unforceSpecs) {
        bool any = false;
        // alamat absolut ("0x7a123456") -> langsung
        if (spec.size() > 2 && spec.compare(0, 2, "0x") == 0) {
            any = patch::unforceReturn((uintptr_t)strtoull(spec.c_str(), nullptr, 16));
            logx::add("[force] --unforce %s -> %s", spec.c_str(), any ? "dibalikin" : "nggak ada yang dipatch");
            continue;
        }
        if (spec == "all" || spec == "*") {
            const size_t n = patch::restoreFromFile("", "--unforce all");
            logx::add("[force] --unforce all -> %zu method dibalikin (catatan: %s)",
                      n, patch::forcePath().c_str());
            continue;
        }
        const size_t sep = spec.find("::");
        std::string cls = spec, meth;
        if (sep != std::string::npos) { cls = spec.substr(0, sep); meth = spec.substr(sep + 2); }

        // catatan dari run sebelumnya (alamatnya beda, rva-nya sama)
        if (patch::restoreFromFile(spec, "--unforce")) any = true;
        if (any) {
            logx::add("[force] --unforce %s -> dibalikin (dari catatan)", spec.c_str());
            continue;
        }

        if (explore::resolve(cls.c_str(), nullptr)) {
            for (const auto& m : explore::info().methods) {
                if (!meth.empty() && bareMethodName(m.name) != meth) continue;
                if (patch::unforceReturn(m.ptr)) any = true;
            }
        }
        if (!any && metapool::ready()) {
            for (const auto& e : metapool::all()) {
                if (!e.code || e.name.empty()) continue;
                if (!meth.empty() && bareMethodName(e.name) != meth) continue;
                std::string kn;
                if (!metapool::klassName(e.klass, kn)) continue;
                if (kn != cls && bareClassName(kn) != bareClassName(cls)) continue;
                if (patch::unforceReturn(e.code)) any = true;
            }
        }
        logx::add("[force] --unforce %s -> %s", spec.c_str(), any ? "dibalikin" : "nggak ada yang dipatch");
    }

    // ---- pasang ----
    for (const auto& spec : g_forceSpecs) {
        if (spec.cls.empty() || spec.method.empty()) {
            logx::add("[force] spek dilewat (butuh Class::Method=nilai): %s", spec.method.c_str());
            continue;
        }
        patch::Ret kind = patch::Ret::I32;
        double num = 0;
        uint64_t ptr = 0;
        if (!forceKindFromSpec(spec, kind, num, ptr)) {
            logx::add("[force] nilai/tipe '%s' nggak kebaca", spec.value.c_str());
            continue;
        }
        if (kind == patch::Ret::Ptr && !ptr) {
            logx::add("[force] %s::%s: tipe object/string butuh in-process (nggak bisa dari luar) "
                      "-- dipasang 0/null", spec.cls.c_str(), spec.method.c_str());
        }
        const std::string label = spec.cls + "::" + spec.method;
        int hits = 0;

        // Jalur 1: kelas diresolusi dari memori (il2cpp standar).
        if (explore::resolve(spec.cls.c_str(), nullptr)) {
            for (const auto& m : explore::info().methods) {
                if (bareMethodName(m.name) != spec.method) continue;
                ++hits;
                std::string err;
                if (!patch::forceReturn(m.ptr, kind, num, ptr,
                                        spec.cls + "::" + bareMethodName(m.name),
                                        m.module, m.rva, &err))
                    logx::add("[force] %s GAGAL: %s", label.c_str(), err.c_str());
            }
        }

        // Jalur 2: lewat POOL MethodInfo -- ini yang jalan di MLBB (metadata-nya
        // dipecah 3 lib, jadi jalur cari-nama-kelas-di-memori ketemu tapi
        // daftar method-nya kosong). Nama kelas dibaca dari runtime.
        if (!hits && metapool::ready()) {
            for (const auto& e : metapool::all()) {
                if (!e.code || e.name.empty()) continue;
                if (bareMethodName(e.name) != spec.method) continue;
                std::string kn;
                if (!metapool::klassName(e.klass, kn)) continue;
                if (kn != spec.cls && bareClassName(kn) != bareClassName(spec.cls)) continue;
                ++hits;
                std::string err;
                if (!patch::forceReturn(e.code, kind, num, ptr,
                                        spec.cls + "::" + bareMethodName(e.name),
                                        e.module, e.rva, &err))
                    logx::add("[force] %s GAGAL: %s", label.c_str(), err.c_str());
            }
            if (hits) logx::add("[force] %s: %d method lewat pool (rva+module dari metadata)",
                                label.c_str(), hits);
        }
        if (!hits) {
            logx::add("[force] %s nggak ketemu (cek pakai --probe %s; di MLBB tambahin --pool)",
                      label.c_str(), spec.cls.c_str());
            // petunjuk: method dengan nama itu ada di kelas mana aja (kelasnya
            // sering beda tipis: namespace, pindah lib, atau generic `N)
            int hint = 0;
            if (metapool::ready()) {
                for (const auto& e : metapool::all()) {
                    if (hint >= 3 || e.name.empty()) continue;
                    if (bareMethodName(e.name) != spec.method) continue;
                    std::string kn;
                    if (!metapool::klassName(e.klass, kn)) continue;
                    logx::add("[force]   '%s' ada di kelas %s (kode @0x%lx)",
                              spec.method.c_str(), kn.c_str(), (unsigned long)e.code);
                    ++hint;
                }
            }
            if (!hint) logx::add("[force]   nama method '%s' juga nggak ada di pool", spec.method.c_str());
        }
    }
    // spek-nya udah dikerjain -> dibersihin, biar --exit nggak nunggu terus.
    // Yang nempel itu patch-nya (patch::g_forced), bukan spek argumennya.
    g_forceSpecs.clear();
    g_unforceSpecs.clear();
    g_forceFind.clear();
    g_fixCode.clear();
    g_forceReapply = g_forceListFile = false;
    if (patch::forcedCount())
        logx::add("[force] %s", patch::forcedSummary().c_str());
}

// ---------------------------------------------------------------------------
// --gencode / --ggcode : resolve Class::Method (jalur yang sama kayak
// --forceret: explore dulu, pool kalau perlu), terus emit snippet. READ-ONLY:
// nggak nulis memori game, cuma nulis file .cpp/.lua + log.
// ---------------------------------------------------------------------------
static bool genResolve(const ForceSpec& spec, gencode::Target& out, int& hits) {
    hits = 0;
    bool got = false;
    // Jalur 1: kelas diresolusi dari memori / dump offline.
    if (explore::resolve(spec.cls.c_str(), nullptr)) {
        for (const auto& m : explore::info().methods) {
            if (bareMethodName(m.name) != spec.method) continue;
            ++hits;
            if (got) continue;
            out.cls    = spec.cls;
            out.method = bareMethodName(m.name);
            out.module = m.module;
            out.rva    = m.rva;
            gencode::parseSig(m.name, out.sig);
            got = true;
        }
    }
    // Jalur 2: pool MethodInfo (MLBB).
    if (!got && metapool::ready()) {
        for (const auto& e : metapool::all()) {
            if (!e.code || e.name.empty()) continue;
            if (bareMethodName(e.name) != spec.method) continue;
            std::string kn;
            if (!metapool::klassName(e.klass, kn)) continue;
            if (kn != spec.cls && bareClassName(kn) != bareClassName(spec.cls)) continue;
            ++hits;
            if (got) continue;
            out.cls    = spec.cls;
            out.method = bareMethodName(e.name);
            out.module = e.module;
            out.rva    = e.rva;
            gencode::parseSig(e.name, out.sig);
            got = true;
        }
    }
    return got;
}

// ForceSpec --forceret ("nilai:tipe") -> ekspresi C++ + kind GG.
static void genRetExpr(const ForceSpec& spec, std::string& expr, std::string& kind) {
    expr.clear();
    kind.clear();
    if (spec.value.empty()) return;              // tanpa "=nilai": passthrough
    patch::Ret k = patch::Ret::I32;
    double num = 0;
    uint64_t ptr = 0;
    if (!forceKindFromSpec(spec, k, num, ptr)) return;
    char b[64];
    switch (k) {
        case patch::Ret::Void: expr = "@ret"; kind = "void"; break;
        case patch::Ret::Bool:
            expr = (num != 0.0) ? "true" : "false"; kind = "bool"; break;
        case patch::Ret::I16: case patch::Ret::I32:
            snprintf(b, sizeof(b), "%lld", (long long)num);
            expr = b; kind = "i32"; break;
        case patch::Ret::U16: case patch::Ret::U32:
            snprintf(b, sizeof(b), "%llu", (unsigned long long)num);
            expr = b; kind = "u32"; break;
        case patch::Ret::I64: case patch::Ret::U64:
            snprintf(b, sizeof(b), "%lldLL", (long long)num);
            expr = b; kind = "i64"; break;
        case patch::Ret::Float:
            snprintf(b, sizeof(b), "%gf", num);
            expr = b; kind = "f32"; break;
        case patch::Ret::Double:
            snprintf(b, sizeof(b), "%g", num);
            expr = b; kind = "f64"; break;
        case patch::Ret::Ptr: expr = "NULL"; kind = "ptr"; break;
    }
}

static void runGenCode() {
    if (g_genCode.empty() && g_ggCode.empty() && g_huntAddr.empty()) return;
    std::string pkg = target::package();
    if (pkg.empty()) pkg = "unknown";
    // Sub-proses dibuang kayak jalur dump (com.mobile.legends:UnityKillsMe).
    const size_t colon = pkg.find(':');
    if (colon != std::string::npos) pkg = pkg.substr(0, colon);
    const std::string dir = "/sdcard/panxcz-tools/gencode/" + pkg;

    for (const auto& spec : g_genCode) {
        gencode::Target t;
        int hits = 0;
        if (!genResolve(spec, t, hits)) {
            logx::add("[gen] %s::%s nggak ketemu (--probe %s; MLBB: + --pool)",
                      spec.cls.c_str(), spec.method.c_str(), spec.cls.c_str());
            continue;
        }
        std::string expr, kind;
        genRetExpr(spec, expr, kind);
        const std::string code = gencode::dobbyHook(t, expr);
        const std::string fn = gencode::sanitize(t.cls) + "_" +
                               gencode::sanitize(t.method) + ".cpp";
        const std::string path = gencode::saveFile(dir, fn, code + "\n");
        logx::add("[gen] %s::%s -> %s (%d hit%s%s)", t.cls.c_str(), t.method.c_str(),
                  path.empty() ? "(gagal nulis file)" : path.c_str(), hits,
                  hits == 1 ? "" : "s", t.sig.ret.empty() ? ", signature unknown" : "");
        if (path.empty()) logx::add("[gen] snippet:\n%s", code.c_str());
    }
    for (const auto& spec : g_ggCode) {
        gencode::Target t;
        int hits = 0;
        if (!genResolve(spec, t, hits)) {
            logx::add("[gen] %s::%s nggak ketemu (--probe %s; MLBB: + --pool)",
                      spec.cls.c_str(), spec.method.c_str(), spec.cls.c_str());
            continue;
        }
        std::string expr, kind;
        genRetExpr(spec, expr, kind);
        double num = 0;
        if (!expr.empty() && expr != "@ret" && expr != "NULL" && expr != "true" &&
            expr != "false")
            num = strtod(expr.c_str(), nullptr);
        else if (expr == "true")
            num = 1.0;
        const std::string lua = gencode::ggLua(t, mem::pid(), kind, num);
        const std::string fn = gencode::sanitize(t.cls) + "_" +
                               gencode::sanitize(t.method) + ".lua";
        const std::string path = gencode::saveFile(dir, fn, lua);
        logx::add("[gen] %s::%s -> %s", t.cls.c_str(), t.method.c_str(),
                  path.empty() ? "(gagal nulis file)" : path.c_str());
        if (path.empty()) logx::add("[gen] script:\n%s", lua.c_str());
    }
    if (!g_huntAddr.empty()) {
        const uintptr_t addr = (uintptr_t)strtoull(g_huntAddr.c_str(), nullptr, 0);
        if (!metapool::ready()) {
            logx::add("[gen] --huntaddr butuh pool (tambahin --pool)");
        } else {
            std::vector<std::pair<uintptr_t, long>> sorted;
            const auto& all = metapool::all();
            for (size_t i = 0; i < all.size(); ++i)
                if (all[i].code) sorted.emplace_back(all[i].code, (long)i);
            std::sort(sorted.begin(), sorted.end(),
                      [](const auto& a, const auto& b) { return a.first < b.first; });
            const long idx = gencode::nearestBelow(sorted, addr);
            if (idx < 0) {
                logx::add("[gen] 0x%lx: nggak ada method dalam 0x1000 ke bawah",
                          (unsigned long)addr);
            } else {
                const auto& e = all[(size_t)idx];
                std::string kn;
                if (!metapool::klassName(e.klass, kn)) kn = "?";
                logx::add("[gen] 0x%lx = %s::%s + 0x%lx (rva=0x%llx %s)",
                          (unsigned long)addr, kn.c_str(), e.name.c_str(),
                          (unsigned long)(addr - e.code),
                          (unsigned long long)e.rva,
                          e.module.empty() ? "?" : e.module.c_str());
            }
        }
    }
    g_genCode.clear();
    g_ggCode.clear();
    g_huntAddr.clear();
}

// Alamat kode dari (module, rva) -- tiga tafsir yang sama kayak --peek
// (rva-dalam-modul, vaddr, base+rva). Balikin 0 kalau nggak ketemu.
static uintptr_t codeAddrOf(const std::string& mod, uint64_t rva, const char** howOut) {
    if (howOut) *howOut = nullptr;
    uintptr_t addr = mem::addrOfRvaIn(mod.c_str(), rva);
    if (addr) { if (howOut) *howOut = "rva-modul"; return addr; }
    addr = mem::addrOfVaddrIn(mod.c_str(), rva);
    if (addr) { if (howOut) *howOut = "vaddr"; return addr; }
    addr = mem::addrOfRva(rva);
    if (addr) { if (howOut) *howOut = "rva-global"; return addr; }
    const uintptr_t b = mem::base(mod.c_str());
    if (!b) return 0;
    if (howOut) *howOut = "base+rva";
    return mem::untag(b) + rva;
}

// "Class::Method[:len]" / "lib.so:0xRVA[:len]" / "0xADDR[:len]" -> alamat +
// panjang. Balikin false kalau formatnya nggak kebaca.
static bool parseViewSpec(const std::string& in, gencode::Target& t, uintptr_t& addr,
                          size_t& len, size_t defLen, size_t maxLen) {
    t = gencode::Target();
    addr = 0;
    len = defLen;
    std::string s = in;
    // ":len" di ujung (angka desimal semua).
    const size_t lc = s.rfind(':');
    if (lc != std::string::npos && lc + 1 < s.size()) {
        bool digits = true;
        for (size_t i = lc + 1; i < s.size(); ++i)
            if (!isdigit((unsigned char)s[i])) { digits = false; break; }
        if (digits) {
            len = (size_t)strtoul(s.c_str() + lc + 1, nullptr, 10);
            if (!len) len = defLen;
            if (len > maxLen) len = maxLen;
            s = s.substr(0, lc);
        }
    }
    const size_t dc = s.find("::");
    if (dc != std::string::npos) {
        // Class::Method
        ForceSpec fs;
        fs.cls = s.substr(0, dc);
        fs.method = s.substr(dc + 2);
        int hits = 0;
        if (!genResolve(fs, t, hits) || !hits) return false;
        const char* how = nullptr;
        addr = codeAddrOf(t.module, t.rva, &how);
        if (!addr) return false;
        return true;
    }
    const size_t c1 = s.find(':');
    if (c1 != std::string::npos) {
        // lib.so:0xRVA
        t.module = s.substr(0, c1);
        t.rva = strtoull(s.c_str() + c1 + 1, nullptr, 0);
        const char* how = nullptr;
        addr = codeAddrOf(t.module, t.rva, &how);
        return addr != 0;
    }
    // 0xADDR mentah
    addr = mem::untag(strtoull(s.c_str(), nullptr, 0));
    return addr != 0;
}

static void runHexAsm() {
    for (const auto& spec : g_hexDump) {
        gencode::Target t;
        uintptr_t addr = 0;
        size_t len = 64;
        if (!parseViewSpec(spec, t, addr, len, 64, 256)) {
            logx::add("[view] --hexdump '%s' nggak kebaca (Class::Method / lib:0xRVA / 0xADDR)",
                      spec.c_str());
            continue;
        }
        std::vector<uint8_t> buf(len, 0);
        if (!mem::read(addr, buf.data(), buf.size())) {
            logx::add("[view] gagal BACA 0x%lx (%zu byte)", (unsigned long)addr, len);
            continue;
        }
        std::string ftext;
        const char* mark = patch::isForced(addr, &ftext) ? " [forceret]" : "";
        logx::add("[view] hex %s%s @0x%lx (%zu byte)%s%s",
                  t.method.empty() ? spec.c_str() : (t.cls + "::" + t.method).c_str(),
                  t.module.empty() ? "" : (" [" + t.module + "]").c_str(),
                  (unsigned long)addr, len, mark, ftext.empty() ? "" : (" " + ftext).c_str());
        const std::string dump = memview::hexdump(addr, buf.data(), buf.size());
        // logcat satu baris satu add (baris multi-\n suka kepotong).
        size_t b = 0;
        while (b < dump.size()) {
            size_t e = dump.find('\n', b);
            if (e == std::string::npos) e = dump.size();
            if (e > b) logx::add("[view]   %s", dump.substr(b, e - b).c_str());
            b = e + 1;
        }
    }
    for (const auto& spec : g_asmView) {
        gencode::Target t;
        uintptr_t addr = 0;
        size_t len = 160;
        if (!parseViewSpec(spec, t, addr, len, 160, 320)) {
            logx::add("[view] --asmview '%s' nggak kebaca (Class::Method / lib:0xRVA / 0xADDR)",
                      spec.c_str());
            continue;
        }
        std::vector<uint8_t> buf(len, 0);
        if (!mem::read(addr, buf.data(), buf.size())) {
            logx::add("[view] gagal BACA 0x%lx (%zu byte)", (unsigned long)addr, len);
            continue;
        }
        logx::add("[view] asm %s @0x%lx:",
                  t.method.empty() ? spec.c_str() : (t.cls + "::" + t.method).c_str(),
                  (unsigned long)addr);
        for (const auto& ln : memview::disasm(buf.data(), buf.size(), addr, 40))
            logx::add("[view]   %s", ln.c_str());
    }
    g_hexDump.clear();
    g_asmView.clear();
}

static void runFindObj() {
    for (const auto& spec : g_findObj) {
        std::string cls = spec;
        size_t max = 64;
        const size_t c = spec.rfind(':');
        if (c != std::string::npos && c + 1 < spec.size()) {
            bool digits = true;
            for (size_t i = c + 1; i < spec.size(); ++i)
                if (!isdigit((unsigned char)spec[i])) { digits = false; break; }
            if (digits) {
                max = (size_t)strtoul(spec.c_str() + c + 1, nullptr, 10);
                if (!max || max > 512) max = 64;
                cls = spec.substr(0, c);
            }
        }
        if (!explore::resolve(cls.c_str(), nullptr)) {
            logx::add("[obj] class '%s' nggak ketemu", cls.c_str());
            continue;
        }
        const auto vec = explore::instances(max);
        logx::add("[obj] %s: %zu instance%s", cls.c_str(), vec.size(),
                  vec.size() == max ? " (cap, masih ada lagi)" : "");
        for (size_t i = 0; i < vec.size() && i < 32; ++i)
            logx::add("[obj]   0x%lx", (unsigned long)vec[i]);
    }
    g_findObj.clear();
}

static void runInspect() {
    for (const auto& spec : g_inspect) {
        const size_t at = spec.find('@');
        if (at == std::string::npos) {
            logx::add("[obj] --inspect butuh 'Class@0xADDR': %s", spec.c_str());
            continue;
        }
        const std::string cls = spec.substr(0, at);
        const uintptr_t addr =
            mem::untag(strtoull(spec.c_str() + at + 1, nullptr, 0));
        if (!addr) {
            logx::add("[obj] alamatnya 0: %s", spec.c_str());
            continue;
        }
        if (!explore::resolve(cls.c_str(), nullptr)) {
            logx::add("[obj] class '%s' nggak ketemu", cls.c_str());
            continue;
        }
        const auto& k = explore::info();
        // Validasi ringan: qword pertama object = klass (untag) harus kelas ini.
        uintptr_t objKlass = 0;
        if (mem::rd(addr, objKlass) && mem::untag(objKlass) != mem::untag(k.addr))
            logx::add("[obj] AWAS: klass object @0x%lx (0x%lx) beda dari %s (0x%lx) -- "
                      "offset bisa ngaco", (unsigned long)addr, (unsigned long)objKlass,
                      cls.c_str(), (unsigned long)k.addr);
        logx::add("[obj] %s @0x%lx (%zu field):", cls.c_str(), (unsigned long)addr,
                  k.fields.size());
        char sv[128];
        for (size_t i = 0; i < k.fields.size() && i < 64; ++i) {
            const auto& f = k.fields[i];
            const uintptr_t base = f.isStatic ? k.statics : addr;
            if (!base) continue;
            double num = 0;
            sv[0] = 0;
            explore::FType t = explore::guessType(f.typeEnum);
            if (!f.typeName.empty()) {
                // Tipe tertulis (dump/runtime) lebih bisa dipercaya daripada
                // typeEnum mentah -- apalagi di MLBB yang layout FieldInfo-
                // nya dimodif (typeEnum runtime-nya sampah). Nama kelas tak
                // dikenal (mis. "BattleManager") = referensi object -> Ptr.
                const uint8_t e = dump::guessTypeEnum(f.typeName);
                if (e != 0xFF) t = explore::guessType(e);
                else if (f.typeEnum == 0 || k.fromDump) t = explore::FType::Ptr;
            }
            if (!explore::readValue(base, f.offset, t, num, sv, sizeof(sv))) continue;
            if (t == explore::FType::String)
                logx::add("[obj]   %s = \"%s\"%s", f.name.c_str(), sv,
                          f.isStatic ? "  (static)" : "");
            else if (t == explore::FType::Ptr)
                logx::add("[obj]   %s = 0x%llX%s", f.name.c_str(), (unsigned long long)num,
                          f.isStatic ? "  (static)" : "");
            else if (t == explore::FType::Float || t == explore::FType::Double)
                logx::add("[obj]   %s = %g%s", f.name.c_str(), num,
                          f.isStatic ? "  (static)" : "");
            else if (t == explore::FType::Bool)
                logx::add("[obj]   %s = %s%s", f.name.c_str(), num != 0 ? "true" : "false",
                          f.isStatic ? "  (static)" : "");
            else
                logx::add("[obj]   %s = %lld (0x%llX)%s", f.name.c_str(), (long long)num,
                          (unsigned long long)num, f.isStatic ? "  (static)" : "");
        }
    }
    g_inspect.clear();
}

// Pindai string UTF-16LE di memori (port Tool::Strings: filter + cap 500).
// GC-scan diganti pindai buta: tanpa klass System.String pun ketemu isinya.
static void runStrings() {
    if (!g_strings) return;
    g_strings = false;
    const size_t kCapHits = 500, kMinLen = 6, kMaxMB = 384;
    size_t scannedBytes = 0, hits = 0;
    auto scannedMB = [&] { return scannedBytes / (1024 * 1024); };
    // Heap (anon rw) dulu, baru sisanya -- string game kebanyakan di heap.
    for (int pass = 0; pass < 2 && hits < kCapHits; ++pass) {
        for (const auto& r : mem::regions()) {
            if (hits >= kCapHits || scannedMB() >= kMaxMB) break;
            if (!r.readable() || r.exec()) continue;
            const bool anon = r.name.empty() || r.name[0] != '/';
            if ((pass == 0) != anon) continue;
            if (pass == 0 && r.perms[1] != 'w') continue;
            size_t left = r.size();
            uintptr_t cur = r.start;
            const size_t kChunk = 256 * 1024;
            std::vector<uint8_t> buf(kChunk + 256, 0);
            size_t carry = 0;
            while (left && hits < kCapHits && scannedMB() < kMaxMB) {
                size_t want = left < kChunk ? left : kChunk;
                // Baca longgar: halaman nakal di-nol-kan, pindai tetap jalan.
                memmove(buf.data(), buf.data() + kChunk, carry);
                if (!mem::read(cur, buf.data() + carry, want)) break;
                const size_t total = carry + want;
                const uint16_t* u = (const uint16_t*)buf.data();
                const size_t nu = total / 2;
                size_t i = 0;
                char out[256];
                while (i < nu) {
                    uint16_t c = u[i];
                    if (c >= 0x20 && c < 0x7F) {
                        size_t j = i, o = 0;
                        while (j < nu && o + 1 < sizeof(out)) {
                            uint16_t d = u[j];
                            if (d >= 0x20 && d < 0x7F) out[o++] = (char)d;
                            else break;
                            ++j;
                        }
                        out[o] = 0;
                        // Syarat string beneran: diakhiri NUL + panjang cukup.
                        if (o >= kMinLen && j < nu && u[j] == 0) {
                            if (g_stringsFilter.empty() ||
                                strstr(out, g_stringsFilter.c_str())) {
                                const uintptr_t where =
                                    cur - carry + (uintptr_t)i * 2;
                                logx::add("[str] 0x%lx: %s", (unsigned long)where, out);
                                if (++hits >= kCapHits) break;
                            }
                        }
                        i = j ? j : i + 1;
                        if (i == 0) ++i;
                    } else {
                        ++i;
                    }
                }
                carry = total > 256 ? 256 : total;
                // Overlap: 256 byte terakhir jadi awalan chunk berikut.
                memmove(buf.data() + kChunk, buf.data() + total - carry, carry);
                cur += want;
                left -= want;
                scannedBytes += want;
            }
        }
    }
    logx::add("[str] %zu string%s, %zu MB dipindai%s", hits, hits == kCapHits ? " (cap 500)" : "",
              scannedMB(), g_stringsFilter.empty() ? "" : (" (filter '" + g_stringsFilter + "')").c_str());
    g_stringsFilter.clear();
}

static std::string lowerOf(const std::string& s) {
    std::string o = s;
    for (char& c : o) c = (char)tolower((unsigned char)c);
    return o;
}

// --search kata [--by class|method|field] (port FilterClasses tri-mode).
// --by nempel ke --search BERIKUTNYA, jadi boleh antri banyak.
static void runSearch() {
    if (g_searches.empty()) return;
    for (const auto& req : g_searches) {
        const std::string kw = lowerOf(req.kw);
        const std::string by = lowerOf(req.by.empty() ? "class" : req.by);
    const size_t kCap = 30;
    size_t shown = 0;
    if (by == "class") {
        if (!dump::loaded()) dump::loadAuto();
        if (!dump::loaded()) {
            logx::add("[find] --search class butuh dump offline (nggak ketemu)");
            return;
        }
        size_t scanned = 0;
        for (const auto& e : dump::classes()) {
            ++scanned;
            const std::string full = lowerOf(e.ns.empty() ? e.name : (e.ns + "." + e.name));
            if (full.find(kw) == std::string::npos) continue;
            logx::add("[find]   %s (%s, %s, %zu slot)", full.c_str(), e.assembly.c_str(),
                      e.module.c_str(), e.slots.size());
            if (++shown >= kCap) break;
        }
        logx::add("[find] %zu kelas cocok (%zu dipindai)", shown, scanned);
    } else if (by == "method") {
        if (!metapool::ready()) {
            logx::add("[find] --search method butuh pool (tambahin --pool)");
            return;
        }
        for (const auto& e : metapool::all()) {
            if (e.name.empty()) continue;
            if (lowerOf(bareMethodName(e.name)).find(kw) == std::string::npos &&
                lowerOf(e.name).find(kw) == std::string::npos)
                continue;
            std::string kn;
            if (!metapool::klassName(e.klass, kn)) kn = "?";
            logx::add("[find]   %s::%s  rva=0x%llx (%s)", kn.c_str(), e.name.c_str(),
                      (unsigned long long)e.rva, e.module.empty() ? "?" : e.module.c_str());
            if (++shown >= kCap) break;
        }
        logx::add("[find] %zu method cocok", shown);
    } else if (by == "field") {
        if (!dump::loaded()) dump::loadAuto();
        if (!dump::loaded()) {
            logx::add("[find] --search field butuh dump offline (nggak ketemu)");
            return;
        }
        const size_t kScanCap = 20000;
        size_t scanned = 0;
        for (const auto& e : dump::classes()) {
            if (shown >= kCap || scanned >= kScanCap) break;
            ++scanned;
            dump::ClassDetail d;
            if (!dump::detail(e, d)) continue;
            for (const auto& f : d.fields) {
                if (lowerOf(f.name).find(kw) == std::string::npos) continue;
                logx::add("[find]   %s::%s  %s off=0x%X%s", e.name.c_str(), f.name.c_str(),
                          f.type.c_str(), f.offset, f.isStatic ? " (static)" : "");
                if (++shown >= kCap) break;
            }
        }
        logx::add("[find] %zu field cocok (%zu kelas dipindai)", shown, scanned);
    } else {
        logx::add("[find] --by harus class|method|field: %s", req.by.c_str());
    }
    }
    g_searches.clear();
}

// --huntmeth Class::Method: lacak MethodInfo satu method (diagnosa layout).
// --diagclass Class: satu paket diagnosa struktur kelas (berat, tapi hasilnya
// lengkap di logcat). Dipakai buat nentuin layout MethodInfo di game yang
// metadata runtime-nya beda dari il2cpp standar (MLBB).
static void runDiagClass() {
    if (g_diagClass.empty()) return;

    std::string ns, name = g_diagClass;
    const size_t sep = g_diagClass.find("::");
    if (sep != std::string::npos) {
        ns   = g_diagClass.substr(0, sep);
        name = g_diagClass.substr(sep + 2);
    }

    const uintptr_t k = il2::findKlass(name.c_str(), ns.empty() ? nullptr : ns.c_str());
    if (!k) {
        logx::add("[diag] kelas '%s' nggak ketemu", g_diagClass.c_str());
    } else {
        logx::add("[diag] ===== kelas '%s' @0x%lx =====", g_diagClass.c_str(), (unsigned long)k);
        explore::huntClass(k, 8);
        explore::dumpArrays(k, 10);
        explore::dumpTargets(k, 12);
        explore::huntMethodTable(k, 6);
        explore::dumpPropMethodInfos(k, 4);
        logx::add("[diag] ===== selesai '%s' =====", g_diagClass.c_str());
    }
    if (g_probeExit && !workPending()) ui::requestExit();
}

// --foffprobe Class: cari array offset field di Il2CppClass (il2cpp baru).
static void runFoffProbe() {
    if (g_foffProbe.empty()) return;
    std::string ns, name = g_foffProbe;
    const size_t sep = g_foffProbe.find("::");
    if (sep != std::string::npos) {
        ns   = g_foffProbe.substr(0, sep);
        name = g_foffProbe.substr(sep + 2);
    }
    std::vector<uintptr_t> all;
    il2::findAllKlass(name.c_str(), all, 4);
    if (all.empty()) {
        logx::add("[foff] kelas '%s' nggak ketemu", g_foffProbe.c_str());
    } else {
        logx::add("[foff] ===== kelas '%s' @0x%lx =====", g_foffProbe.c_str(),
                  (unsigned long)all[0]);
        explore::diagFieldOffsets(all[0], 10);
    }
    if (g_probeExit && !workPending()) ui::requestExit();
}

// --libdump / --ac (alias) : dump lib anti-cheat + engine -- gambar memori (.img
// buat IDA/Ghidra) + laporan ELF/string (export, import, vektor deteksi, JEJAK
// TOOL). Nyalanya juga kalau `--dumpout --all-libs` (matiin pakai --no-ac).
static void runLibDump() {
    if (!g_libDump) return;

    libdump::Options o;
    o.libs      = g_libDumpLibs;      // kosong = auto (engine + semua anti-cheat)
    o.outDir    = g_libDumpOut;
    o.image     = !g_libDumpNoImg;
    o.report    = !g_libDumpNoReport;
    o.allSo      = g_libDumpAllSo || (g_libDumpLibs.empty() && g_dumpOpt.allLibs && g_libDump);
    o.systemLibs = g_libDumpSys;
    if (g_libDumpImgMaxMB)    o.maxImageMB = g_libDumpImgMaxMB;
    if (g_libDumpImgTotalMB)  o.maxImageTotalMB = g_libDumpImgTotalMB;

    logx::add("[libdump] lib yang bakal di-dump: %s%s",
              o.libs.empty() ? "(otomatis) " : "",
              [&] {
                  std::string s;
                  for (const auto& l : o.libs) { if (!s.empty()) s += ", "; s += l; }
                  return s.c_str();
              }());

    abortx::clear();
    libdump::Result r;
    if (!libdump::run(o, r)) {
        logx::add("[libdump] nggak ada yang ke-dump (game jalan? lib anti-cheat ke-load?)");
    } else {
        // libdump::run udah nyetak "selesai: N lib (M MB) -> folder"; di sini
        // cuma nyatet buat RINGKASAN penutup (dulu ada dua baris kembar).
        g_ldRan   = true;
        g_ldCount = r.libs.size();
        g_ldDir   = r.dir;
    }

    // Kerjaannya UDAH kelar -> tandai, biar workPending() nggak nganggep masih
    // ada yang dikerjain. Tanpa ini tool nggak pernah keluar sendiri walau
    // diminta `--exit`: tiap sesi ninggalin satu proses nyangkut di HP (kejadian
    // nyata: 9 proses `mali_render` numpuk).
    g_libDumpDone = true;
    // Sama kayak --dumpout: default keluar, `--stay` buat nahan menu tetep kebuka.
    if (!g_dumpStay) ui::requestExit();
}

// --miprobe Class::Method: diagnosa satu MethodInfo (lihat explore.h).
static void runMiProbe() {
    if (g_miProbe.empty()) return;

    std::string cls = g_miProbe, meth = g_miProbe;
    const size_t sep = g_miProbe.find("::");
    if (sep != std::string::npos) {
        cls  = g_miProbe.substr(0, sep);
        meth = g_miProbe.substr(sep + 2);
    } else {
        meth = "";
    }
    explore::probeMethodInfo(cls.c_str(), meth.c_str());
    if (g_probeExit && !workPending()) ui::requestExit();
}

static void runHuntMeth() {
    if (g_huntMeth.empty()) return;
    std::string cls = g_huntMeth, meth = g_huntMeth;
    const size_t sep = g_huntMeth.find("::");
    if (sep != std::string::npos) {
        cls  = g_huntMeth.substr(0, sep);
        meth = g_huntMeth.substr(sep + 2);
    }
    explore::huntMethodInfo(cls.c_str(), meth.c_str());
    if (g_probeExit && !workPending()) ui::requestExit();
}

// ---------------------------------------------------------------------------
// --dumpout: bikin dump .cs SENDIRI (native, root doang -- tanpa frida/PC).
//
// Dijalanin di bootWorker setelah lib il2cpp-nya aktif, jadi urutannya jelas:
// attach -> detect lib -> (opsional probe) -> dump -> (default) keluar.
// ---------------------------------------------------------------------------
// ---------------------------------------------------------------------------
// RINGKASAN: satu blok rapi di AKHIR kerja, biar nggak perlu ngubek-ngubek
// logcat buat tau hasilnya apa. Isinya cuma yang udah kejadian.
// ---------------------------------------------------------------------------
static void logSummary(double secs) {
    if (!mem::attached() && !g_dumpRan && !g_ldRan) return;
    logx::add("[ringkas] ====== RINGKASAN ======");
    if (mem::attached()) {
        logx::add("[ringkas] target  : %s (pid %d)",
                  mem::procName().empty() ? target::package().c_str() : mem::procName().c_str(),
                  mem::pid());
        logx::add("[ringkas] lib     : %s%s%s",
                  il2::libName().empty() ? target::lib().c_str() : il2::libName().c_str(),
                  dump::loaded() ? "  |  dump offline: " : "",
                  dump::loaded() ? dump::path().c_str() : "");
    }
    if (metapool::ready())
        logx::add("[ringkas] pool    : %zu method, %zu kelas, %zu punya nama (%s)",
                  metapool::stats().entries, metapool::stats().classes,
                  metapool::stats().named, metapool::stats().layout.c_str());
    if (g_dumpRan && g_dumpOk)
        logx::add("[ringkas] dump    : %zu kelas, %zu slot, %zu field, %zu method, %zu module%s",
                  g_dumpRes.classes, g_dumpRes.withSlot, g_dumpRes.fields,
                  g_dumpRes.methods, g_dumpRes.modules,
                  g_dumpRes.path.empty() ? "" : "");
    else if (g_dumpRan)
        logx::add("[ringkas] dump    : GAGAL (lihat log di atas)");
    if (g_dumpRan && g_dumpOk && !g_dumpRes.path.empty())
        logx::add("[ringkas] hasil   : %s", g_dumpRes.path.c_str());
    if (g_ldRan)
        logx::add("[ringkas] lib ac  : %zu lib -> %s", g_ldCount, g_ldDir.c_str());
    logx::add("[ringkas] waktu   : %.0f detik", secs);
    logx::add("[ringkas] baca gagal: %zu kali (%zu halaman jelek diingat, pindaian ulang nggak ngitung lagi)",
              mem::readFailures(), mem::badPageCount());
    logx::add("[ringkas] ========================");
}

static void runDumpOut() {
    if (!g_dumpOut) return;

    logx::add("[dumpout] mulai -- target=%s pid=%d module=%s kolom=%zu filter ns='%s' like='%s'",
              mem::procName().empty() ? target::package().c_str() : mem::procName().c_str(),
              mem::pid(),
              g_dumpOpt.allLibs ? "semua il2cpp" : il2::libName().c_str(),
              g_dumpOpt.cols.size(), g_dumpOpt.nsFilter.c_str(), g_dumpOpt.likeFilter.c_str());

    dumpout::Result r;
    const bool ok = dumpout::run(g_dumpOpt, r);
    g_dumpRan = true;
    g_dumpOk  = ok;
    g_dumpRes = r;
    if (!ok) {
        // Jangan cuma bilang "GAGAL": cetak keadaan lingkungannya sekalian
        // (root/izin/mode fd/region/baca blok besar) supaya penyebabnya bisa
        // dibaca langsung dari logcat di device mana pun.
        logx::add("[dumpout] GAGAL -- cek log di atas (game jalan? root? lib il2cpp ke-load?)");
        mem::diag("dump gagal");
    } else
        logx::add("[dumpout] hasil: %s | %zu kelas, %zu field, %zu method, %zu module, %zu slot",
                  r.path.c_str(), r.classes, r.fields, r.methods, r.modules, r.withSlot);

    g_dumpOutDone = true;      // biar workPending() nggak nahan tool tetep hidup
    // Kalau habis ini masih ada kerjaan (dump lib anti-cheat dari --all-libs),
    // JANGAN minta keluar: thread utama bakal `_exit` dan motong kerjaannya di
    // tengah jalan (nyata: ac-nya baru nyetak "lib yang bakal di-dump" langsung
    // mati). Yang minta keluar nanti runLibDump() setelah selesai.
    const bool libDumpNext = g_libDump || (!g_libDumpOff && g_dumpOpt.allLibs);
    if (!g_dumpStay && !libDumpNext) ui::requestExit();
}

// ---------------------------------------------------------------------------
// thread init: attach ke game + resolve object (jangan blokir render)
// ---------------------------------------------------------------------------
// ---------------------------------------------------------------------------
// RUNTIME PATCH -- dipanggil 2x: abis attach, terus lagi waktu lib engine siap.
//
// Kenapa 2x: patch "module:rva" butuh module-nya udah ke-map; di game besar
// lib engine + lib il2cpp-nya baru muncul beberapa detik setelah prosesnya ada.
// Yang kedua juga sekaligus nge-apply ulang (idempotent: byte yang udah sama
// nggak ditulis lagi, cuma dicek).
// ---------------------------------------------------------------------------
// --peek: baca + cetak byte (hex + ASCII) di satu alamat. Dipakai buat ngecek
// hasil patch dari luar tool, dan buat nyari alamat yang mau dipatch.
static void runPeek() {
    if (g_peek.empty()) return;
    uintptr_t addr = 0;
    const std::string spec = g_peek;
    const size_t colon = spec.rfind(':');
    if (colon != std::string::npos) {
        const std::string mod = spec.substr(0, colon);
        const uint64_t rva = strtoull(spec.substr(colon + 1).c_str(), nullptr, 0);
        // Tiga tafsir, urut dari yang paling sering bener:
        //   1. rva file-offset DI MODUL ITU (skema dump tool + patch)
        //   2. vaddr relatif modul (buat lib yang di-load loader sendiri ala
        //      packer -- nilai di PT_DYNAMIC/relokasinya masih vaddr mentah;
        //      nyata di libanogs/libanort Free Fire)
        //   3. tafsir lama: base + rva
        // Dua yang pertama DIBATASI ke modul yang disebut, biar nggak nyangkut
        // ke segmen lib lain yang file offset-nya kebetulan nutup rentang itu.
        addr = mem::addrOfRvaIn(mod.c_str(), rva);
        if (!addr) addr = mem::addrOfVaddrIn(mod.c_str(), rva);
        const char* how = addr ? "rva-modul" : nullptr;
        if (!addr) {
            addr = mem::addrOfRva(rva);
            if (addr) how = "rva-global";
        }
        if (!addr) {
            const uintptr_t b = mem::base(mod.c_str());
            if (!b) { logx::add("[peek] module '%s' nggak ketemu di proses ini", mod.c_str()); return; }
            addr = mem::untag(b) + rva;
            how = "base+rva";
        }
        logx::add("[peek] %s+0x%llX = 0x%lx (%s)", mod.c_str(), (unsigned long long)rva,
                  (unsigned long)addr, how ? how : "?");
    } else {
        addr = mem::untag(strtoull(spec.c_str(), nullptr, 0));
    }
    if (!addr) { logx::add("[peek] alamatnya 0 -- isinya?"); return; }

    const size_t n = g_peekLen ? (g_peekLen > 128 ? 128 : g_peekLen) : 32;
    std::vector<uint8_t> buf(n, 0);
    if (!mem::read(addr, buf.data(), buf.size())) {
        logx::add("[peek] gagal BACA 0x%lx (%zu byte) -- alamatnya kebaca nggak?",
                  (unsigned long)addr, n);
        return;
    }
    std::string hex, asc;
    char b[4];
    for (size_t i = 0; i < n; ++i) {
        snprintf(b, sizeof(b), "%02X", buf[i]);
        hex += b;
        hex += ' ';
        asc += (buf[i] >= 32 && buf[i] < 127) ? (char)buf[i] : '.';
        if (i == 31 && n > 32) { hex += "..."; break; }
    }
    logx::add("[peek] @0x%lx (%zu byte): %s | %s", (unsigned long)addr, n, hex.c_str(), asc.c_str());
}

static bool g_patchInit = false;
static void applyRuntimePatches(const char* when) {
    if (g_patchInit) {
        if (!patch::list().empty()) patch::applyAll();      // pasang ulang
        return;
    }
    g_patchInit = true;
    //  1. patch dari argumen CLI (--patch / --patch-file)
    //  2. daftar auto-apply per-package: <root>/patch/<pkg>.txt
    if (!patch::list().empty()) patch::applyAll();
    if (!g_patchFile.empty()) patch::loadFile(g_patchFile, true);
    else                      patch::autoApply();
    if (!patch::list().empty())
        logx::add("[patch] %s | %s", when, patch::summary().c_str());
}

static std::atomic<bool> g_bootDone{false};

static void* bootWorker(void*) {
    struct BootDone { ~BootDone() { g_bootDone.store(true); } } bootDoneGuard;
    keys::init();

    // Target-nya DIPILIH SAAT RUNTIME (argumen .sh / cfg / menu). Kalau belum
    // ada, tool-nya tetap jalan dan nyuruh milih di tab TARGET.
    target::load();

    // Dump offline (kalau ada) dimuat di sini: isinya tabel "class-slot" =
    // RVA cell yang isinya Il2CppClass*. Itu jalur yang bikin kelas di game
    // ber-metadata-pecah (MLBB) ketemu -- lihat core/offlinedump.h.
    if (!dump::loaded()) dump::loadAuto();

    // Mode otomatis: cfg bisa aja keisi NAMA PROSES, bukan nama package (mis.
    // dari versi lama yang sempat salah milih proses tool sendiri). Nama
    // package Android selalu ada titiknya -> kalau nggak ada, buang & cari ulang.
    if (g_auto && !target::package().empty() &&
        target::package().find('.') == std::string::npos) {
        logx::add("[auto] target di cfg = '%s' (itu nama proses, bukan package) "
                  "-- diabaikan, cari sendiri", target::package().c_str());
        target::setPackage("");
        target::save();
    }

    // ---- MODE OTOMATIS: cari game-nya SENDIRI (nggak perlu ngetik apa pun) ----
    if (g_auto && target::package().empty()) {
        logx::add("[auto] nyari game yang lagi jalan (proses yang punya lib il2cpp)...");
        const int tries = (int)(kAutoFindSec / 2.0);
        for (int i = 0; i < tries && target::package().empty(); ++i) {
            int sc = 0;
            const std::string p = target::autoDetectPackage(&sc);
            if (!p.empty()) {
                target::setPackage(p);
                target::setLib("");            // lib ikut auto (dari maps proses)
                target::setSelPid(0);
                target::save();
                logx::add("[auto] target: %s (skor il2cpp %d), lib auto", p.c_str(), sc);
                break;
            }
            // Belum ada game: JANGAN nyerah -- user sering ngejalanin tool dulu
            // baru buka game-nya. Tungguin (max kAutoFindSec), lapor tiap ~30 s.
            if (i == 0)
                logx::add("[auto] belum ada game yang kelihatan -- buka game-nya sekarang, "
                          "tool nempel sendiri (nunggu max %d detik)", (int)kAutoFindSec);
            else if ((i % 15) == 0)
                logx::add("[auto] masih nunggu (%d detik) -- buka game-nya, "
                          "atau pilih prosesnya manual di tab TARGET", i * 2);
            usleep(2000000);
        }
    }

    if (target::package().empty()) {
        logx::add("[boot] belum ada target -- pilih game di tab TARGET");
        return nullptr;
    }

    logx::add("[boot] menunggu proses %s ...", target::package().c_str());
    int wait = 0;
    const int attachTries = g_auto ? kAutoAttachTries : 60;
    while (wait < attachTries) {                     // tunggu max ~30 detik
        if (target::attach()) break;
        // Mode otomatis: target di cfg bisa aja game yang udah ditutup/diganti.
        // Setelah ~4 detik, pilih ulang sendiri dari proses yang jalan.
        // Auto: prosesnya dipilih ULANG berkala -- kalau game-nya baru dibuka
        // belakangan (atau target lama udah ditutup), langsung pindah sendiri.
        if (g_auto && (wait == 4 || (wait % 16) == 0)) {
            int sc = 0;
            const std::string p = target::autoDetectPackage(&sc);
            if (!p.empty() && p != target::package()) {
                logx::add("[auto] %s nggak jalan -- pindah ke %s (skor %d)",
                          target::package().c_str(), p.c_str(), sc);
                target::setPackage(p);
                target::setLib("");
                target::setSelPid(0);
                target::save();
            }
        }
        usleep(500000);
        ++wait;
    }
    // Kalau yang ke-attach ternyata BUKAN pemilik lib il2cpp (mis. cfg masih
    // nyimpen pid proses lain dari sesi sebelumnya), pid itu JANGAN diinget:
    // dibuang biar jalan berikutnya nyari sendiri lagi. Tanpa ini cfg bisa
    // nyangkut ke proses yang salah dan dump-nya gagal terus.
    if (mem::attached() && target::selPid() > 0 && mem::il2cppScore(mem::pid()) <= 0) {
        logx::add("[target] pid %d nggak punya lib il2cpp -- pid di cfg dibuang",
                  target::selPid());
        target::setSelPid(0);
        target::save();
    }

    if (!mem::attached()) {
        logx::add("[boot] GAGAL attach - pastikan game jalan & tool dijalankan sebagai root");
        if (g_check) mem::diag("--check: attach gagal");
        if (g_check && g_probeExit && !workPending()) ui::requestExit();
        return nullptr;
    }

    // Patch runtime dipasang di sini juga (module-nya udah ke-map pasti).
    applyRuntimePatches("abis attach");
    runPeek();

    // Lib engine sering baru ke-load belakangan (game masih di splash/
    // loading). Jadi jangan nyerah sekali coba -- cek terus sampai ~90 detik.
    // Tiap ~10 detik prosesnya dipilih ULANG: maps dan pid yang bener cuma
    // kelihatan setelah engine-nya beneran dimuat (MLBB: proses anak
    // "com.mobile.legends:UnityKillsMe").
    for (int i = 0; i < (g_auto ? kAutoLibTries : 90); ++i) {
        if (il2::init()) {
            // Proses yang BENERAN di-attach dicetak -- di MLBB engine-nya ada di
            // sub-proses (com.mobile.legends:UnityKillsMe), bukan proses utama.
            logx::add("[boot] proses target: %s (pid %d) | lib il2cpp: %s",
                      mem::procName().empty() ? target::package().c_str() : mem::procName().c_str(),
                      mem::pid(), il2::libName().c_str());
            // Catat proses ENGINE-nya ke cfg. Unity sering naruh il2cpp di proses
            // ANAK -- MLBB: com.mobile.legends:UnityKillsMe (proses utama-nya
            // nggak punya libcsharp/liblogic). Dengan dicatat, jalan berikutnya
            // tool langsung nempel ke proses itu, nggak nebak lagi.
            const std::string pn = mem::procName();
            if (!pn.empty() && pn != target::engine()) {
                target::setEngine(pn);
                target::save();
                logx::add("[target] proses engine disimpan: %s", pn.c_str());
            }

            // ---- MODE OTOMATIS: langsung dump sendiri ----
            // Dikasih jeda kecil dulu biar maps/table-nya stabil (game kadang
            // masih ngisi slot kelas di detik-detik pertama masuk).
            if (g_auto && !g_dumpOut) {
                g_dumpOut  = true;
                // menu tetap kebuka, KECUALI user minta --exit sendiri
                g_dumpStay = !g_probeExit;
                g_dumpOpt.allLibs = true;
                g_dumpOpt.json    = true;
                g_dumpOpt.header  = true;
                g_dumpOpt.ptr     = true;
                logx::add("[auto] dump otomatis: semua lib + .json + .h + .ptrs.tsv "
                          "(folder default /sdcard/panxcz-tools/dumper/<pkg>/<versi>)");
                abortx::beat("auto: siap-siap dump", 0.0f);
                usleep((useconds_t)(kAutoDumpDelaySec * 1000000.0));
                abortx::clear();               // mulai dari keadaan bersih
            }

            // ---- RUNTIME PATCH (core/patch.h) ----
            // Dipasang lagi di sini (udah pernah dicoba abis attach) -- lib
            // il2cpp/module-nya baru ke-load belakangan, jadi patch yang gagal
            // waktu itu sekarang bisa berhasil.
            applyRuntimePatches("lib engine siap");
            runPeek();
            if (g_check)
                mem::diag("--check: lingkungan + baca memori");
            runProbe();
            runDiagClass();
            runMiProbe();
            runFoffProbe();
            runHuntMeth();
            if (g_probeExit && !workPending()) ui::requestExit();
            if (g_initPool) {
                metapool::init();
                logx::add("[pool] siap=%d%s%s", (int)metapool::ready(),
                          metapool::lastError()[0] ? " -- " : "", metapool::lastError());
                for (const auto& s : metapool::stats().spaceInfo) logx::add("[pool]   %s", s.c_str());
                if (g_typeProbe) metapool::dumpTypeProbe();
                g_initPool = false;                 // pool-nya udah selesai
                if (g_probeExit && !workPending(true)) ui::requestExit();
            }
            runForceRet();
            runGenCode();
            runHexAsm();
            runFindObj();
            runInspect();
            runStrings();
            runSearch();
            runDumpOut();
            // `--dumpout --all-libs` sekalian dump lib anti-cheat-nya (matiin: --no-ac).
            if (!g_libDump && !g_libDumpOff && g_dumpOut && g_dumpOpt.allLibs) {
                g_libDump = true;
                g_libDumpAllSo = false;      // auto: engine + anti-cheat aja
                logx::add("[libdump] ikut nyala (--dumpout --all-libs). Matiin: --no-ac");
            }
            runLibDump();
            if (g_probeExit && !workPending()) ui::requestExit();
            return nullptr;
        }
        if (i == 0)
            logx::add("[boot] lib il2cpp belum ke-load -- nyoba terus tiap detik...");
        // Target-nya JELAS bukan proses il2cpp (mis. patch runtime ke proses
        // lain) -> jangan nunggu 90 detik; patch-nya udah kepasang, cukup.
        if (!patch::list().empty() && i == 4 && mem::il2cppScore(mem::pid()) <= 0) {
            logx::add("[boot] proses ini nggak punya lib il2cpp -- kerja patch "
                      "dianggap selesai");
            break;
        }
        usleep(1000000);
        if ((i % 10) == 9) {
            if (mem::attached()) mem::refresh();     // muat ulang maps
            if (target::selPid() == 0) target::attach();  // pilih ulang pid terbaik
        }
        if (!main_thread_flag.load()) return nullptr;   // tool-nya udah ditutup
    }
    logx::add("[boot] lib il2cpp masih nggak ketemu -- buka tab TARGET, "
              "pilih baris proses yang ada tanda *");
    if (g_check) mem::diag("--check: lib il2cpp nggak ketemu");
    // Target bukan game il2cpp tapi ada kerja yang harus jalan (patch runtime /
    // dump pakai module lain) -> jangan nunggu 90 detik, langsung dianggap
    // "kerja selesai" biar --exit beneran keluar.
    if (!patch::list().empty()) {
        runDumpOut();
        if (g_probeExit && !workPending()) ui::requestExit();
        return nullptr;
    }
    if (g_probeExit) ui::requestExit();
    return nullptr;
}

// ---------------------------------------------------------------------------
int main(int argc, char* argv[]) {
    // panxcz_libtool.sh <package> [lib] [--pid N]
    // --pid dipakai kalau satu game punya beberapa proses dan lib engine-nya
    // ada di proses anak (contoh MLBB: com.mobile.legends:UnityKillsMe).
    // MODE OTOMATIS: `./panxcz_libtool.sh` tanpa argumen = cari game + dump
    // sendiri. Argumen apa pun (package, --dumpout, --check, ...) = mode manual
    // seperti biasa, jadi nggak ada yang berubah buat pemakaian yang udah ada.
    // CUKUP JALANIN: kalau belum root, naikin diri sendiri lewat su (lihat
    // tryElevate). Jadi `./panxcz_libtool.sh` doang udah cukup.
    const int suRc = tryElevate(argc, argv);
    if (suRc >= 0) return suRc;

    g_auto = (argc <= 1);

    target::load();
    bool sawTarget = false;
    for (int i = 1; i < argc; ++i) {
        const char* a = argv[i];
        if (!a || !a[0]) continue;
        if (strcmp(a, "--auto") == 0) { g_auto = true; continue; }
        if (strcmp(a, "--manual") == 0) { g_auto = false; continue; }
        if (strcmp(a, "--pid") == 0 || strcmp(a, "-p") == 0) {
            if (i + 1 < argc && argv[i + 1]) target::setSelPid(atoi(argv[++i]));
            continue;
        }
        if (strcmp(a, "--dump") == 0) {
            if (i + 1 < argc && argv[i + 1]) target::setDump(argv[++i]);
            continue;
        }
        if (strncmp(a, "--dump=", 7) == 0) { target::setDump(a + 7); continue; }
        if (strcmp(a, "--dumpdir") == 0) {
            if (i + 1 < argc && argv[i + 1]) target::setDumpDir(argv[++i]);
            continue;
        }
        if (strncmp(a, "--dumpdir=", 10) == 0) { target::setDumpDir(a + 10); continue; }
        if (strcmp(a, "--probe") == 0) {
            if (i + 1 < argc && argv[i + 1]) g_probe = argv[++i];
            continue;
        }
        if (strncmp(a, "--probe=", 8) == 0) { g_probe = a + 8; continue; }
        if (strcmp(a, "--exit") == 0) { g_probeExit = true; continue; }
        // ---- dump sendiri (native, tanpa frida) ----
        if (strcmp(a, "--dumpout") == 0) { g_dumpOut = true; continue; }
        if (strcmp(a, "--all-libs") == 0 || strcmp(a, "--alllibs") == 0) {
            g_dumpOpt.allLibs = true;
            continue;
        }
        if (strcmp(a, "--stay") == 0) { g_dumpStay = true; continue; }
        if (strcmp(a, "--headless") == 0 || strcmp(a, "--no-ui") == 0) { g_headless = true; continue; }
        if (strcmp(a, "--libdump") == 0 || strcmp(a, "--ac") == 0) { g_libDump = true; continue; }
        if (strncmp(a, "--libdump=", 10) == 0) {
            g_libDump = true;
            splitList(a + 10, g_libDumpLibs);
            continue;
        }
        if (strcmp(a, "--libdump-all") == 0) {
            g_libDump = true;
            g_libDumpAllSo = true;
            continue;
        }
        if (strcmp(a, "--no-ac") == 0 || strcmp(a, "--no-libdump") == 0) { g_libDumpOff = true; continue; }
        if (strcmp(a, "--libdump-sys") == 0) { g_libDump = true; g_libDumpAllSo = true; g_libDumpSys = true; continue; }
        if (strncmp(a, "--ac-imgtotal=", 14) == 0) { g_libDumpImgTotalMB = (size_t)atoi(a + 14); continue; }
        if (strcmp(a, "--libdump-noimg") == 0) { g_libDumpNoImg = true; continue; }
        if (strcmp(a, "--libdump-noreport") == 0) { g_libDumpNoReport = true; continue; }
        if (strncmp(a, "--libdump-out=", 14) == 0) { g_libDumpOut = a + 14; continue; }
        if (strncmp(a, "--ac-imgmax=", 12) == 0) { g_libDumpImgMaxMB = (size_t)atoi(a + 12); continue; }
        // ---- tes UI (nggak perlu nge-tap): buka halaman tertentu atau
        //      muter sendiri tiap N detik. Tap dari `adb shell input` memang
        //      nggak nyampe ke overlay ini (overlay baca perangkat sentuh
        //      langsung), jadi dua flag ini buat mastiin semua halaman render.
        if (strcmp(a, "--page") == 0) {
            if (i + 1 < argc && argv[i + 1]) ui::setPage(atoi(argv[++i]));
            g_dumpStay = true;
            continue;
        }
        if (strncmp(a, "--page=", 7) == 0) { ui::setPage(atoi(a + 7)); g_dumpStay = true; continue; }
        if (strcmp(a, "--ui-cycle") == 0) {
            float sec = 2.0f;
            if (i + 1 < argc && argv[i + 1] && argv[i + 1][0] != '-') sec = (float)atof(argv[++i]);
            ui::setCycle(true, sec);
            g_dumpStay = true;
            continue;
        }
        if (strcmp(a, "--no-pool") == 0) { g_dumpOpt.noPool = true; continue; }
        if (strcmp(a, "--no-enrich") == 0) { g_dumpOpt.noEnrich = true; continue; }
        if (strcmp(a, "--no-dump-methods") == 0) { g_dumpOpt.noDumpMethods = true; continue; }
        if (strcmp(a, "--no-dump-classes") == 0) { g_dumpOpt.noDumpClasses = true; continue; }
        if (strcmp(a, "--stealth-off") == 0) { bypass::enabled() = false; continue; }
        if (strcmp(a, "--proc-name") == 0) {
            if (i + 1 < argc && argv[i + 1]) bypass::setProcName(argv[++i]);
            continue;
        }
        if (strcmp(a, "--pool") == 0) { g_initPool = true; continue; }
        if (strncmp(a, "--pool-layout=", 14) == 0) {
            metapool::setForcedLayout(atoi(a + 14));
            g_initPool = true;
            continue;
        }
        if (strcmp(a, "--pool-layouts") == 0) {
            for (size_t i = 0; i < metapool::layoutCount(); ++i)
                logx::add("[pool] layout %zu: %s", i, metapool::layoutTag(i));
            continue;
        }
        if (strcmp(a, "--typeprobe") == 0) { g_initPool = true; g_typeProbe = true; continue; }
        if (strcmp(a, "--check") == 0) { g_check = true; continue; }
        // --scan-only = hitung kelas/field/method aja (nggak nulis file). Dulu ini
        // nggak nge-set g_dumpOut, jadi kerjanya nggak pernah jalan (workPending()
        // ngerasa "nggak ada kerja" -> tool-nya malah langsung keluar).
        if (strcmp(a, "--scan-only") == 0) { g_dumpOpt.dryRun = true; g_dumpOut = true; continue; }
        if (strcmp(a, "--pool-scan") == 0) {
            if (i + 1 < argc && argv[i + 1]) metapool::setScanCapMB((size_t)atoi(argv[++i]));
            continue;
        }
        if (strcmp(a, "--json") == 0)   { g_dumpOpt.json = true; continue; }
        if (strcmp(a, "--header") == 0) { g_dumpOpt.header = true; continue; }
        if (strcmp(a, "--ptr") == 0)    { g_dumpOpt.ptr = true; continue; }
        if (strcmp(a, "--libs") == 0) {
            if (i + 1 < argc && argv[i + 1]) splitList(argv[++i], g_dumpOpt.libs);
            continue;
        }
        if (strcmp(a, "--cols") == 0) {
            if (i + 1 < argc && argv[i + 1]) splitList(argv[++i], g_dumpOpt.cols);
            continue;
        }
        if (strcmp(a, "--ns") == 0) {
            if (i + 1 < argc && argv[i + 1]) g_dumpOpt.nsFilter = argv[++i];
            continue;
        }
        if (strcmp(a, "--like") == 0) {
            if (i + 1 < argc && argv[i + 1]) g_dumpOpt.likeFilter = argv[++i];
            continue;
        }
        if (strcmp(a, "--classes") == 0) {
            if (i + 1 < argc && argv[i + 1]) g_dumpOpt.maxClasses = (size_t)atoi(argv[++i]);
            continue;
        }
        if (strcmp(a, "--diag") == 0) {
            if (i + 1 < argc && argv[i + 1]) g_dumpOpt.diag = (size_t)atoi(argv[++i]);
            continue;
        }
        if (strcmp(a, "--outdir") == 0) {
            if (i + 1 < argc && argv[i + 1]) g_dumpOpt.outDir = argv[++i];
            continue;
        }
        if (strncmp(a, "--outdir=", 9) == 0) { g_dumpOpt.outDir = a + 9; continue; }
        if (strncmp(a, "--dumpout=", 10) == 0) { g_dumpOut = true; g_dumpOpt.outDir = a + 10; continue; }
        if (strncmp(a, "--cols=", 7) == 0) { splitList(a + 7, g_dumpOpt.cols); continue; }
        if (strncmp(a, "--libs=", 7) == 0) { splitList(a + 7, g_dumpOpt.libs); continue; }
        if (strncmp(a, "--ns=", 5) == 0)   { g_dumpOpt.nsFilter = a + 5; continue; }
        if (strncmp(a, "--like=", 7) == 0) { g_dumpOpt.likeFilter = a + 7; continue; }
        if (strncmp(a, "--classes=", 10) == 0) { g_dumpOpt.maxClasses = (size_t)atoi(a + 10); continue; }
        if (strcmp(a, "--huntmeth") == 0) {
            if (i + 1 < argc && argv[i + 1]) g_huntMeth = argv[++i];
            continue;
        }
        if (strncmp(a, "--huntmeth=", 11) == 0) { g_huntMeth = a + 11; continue; }
        if (strcmp(a, "--diagclass") == 0) {
            if (i + 1 < argc && argv[i + 1]) g_diagClass = argv[++i];
            continue;
        }
        if (strncmp(a, "--diagclass=", 12) == 0) { g_diagClass = a + 12; continue; }
        if (strcmp(a, "--miprobe") == 0) {
            if (i + 1 < argc && argv[i + 1]) g_miProbe = argv[++i];
            continue;
        }
        if (strncmp(a, "--miprobe=", 10) == 0) { g_miProbe = a + 10; continue; }
        if (strcmp(a, "--foffprobe") == 0) {
            if (i + 1 < argc && argv[i + 1]) g_foffProbe = argv[++i];
            continue;
        }
        if (strncmp(a, "--foffprobe=", 12) == 0) { g_foffProbe = a + 12; continue; }
        // ---- force return value (lihat core/patch.h) ----
        if (strcmp(a, "--forceret") == 0) {
            if (i + 1 < argc && argv[i + 1]) {
                ForceSpec s;
                parseForceSpec(argv[++i], s);
                g_forceSpecs.push_back(s);
            }
            continue;
        }
        if (strncmp(a, "--forceret=", 11) == 0) {
            ForceSpec s;
            parseForceSpec(a + 11, s);
            g_forceSpecs.push_back(s);
            continue;
        }
        if (strcmp(a, "--unforce") == 0) {
            if (i + 1 < argc && argv[i + 1]) g_unforceSpecs.push_back(argv[++i]);
            continue;
        }
        if (strncmp(a, "--unforce=", 10) == 0) { g_unforceSpecs.push_back(a + 10); continue; }
        if (strcmp(a, "--forcefind") == 0) {
            if (i + 1 < argc && argv[i + 1]) g_forceFind = argv[++i];
            continue;
        }
        if (strncmp(a, "--forcefind=", 12) == 0) { g_forceFind = a + 12; continue; }
        if (strcmp(a, "--forceret-reapply") == 0) { g_forceReapply = true; continue; }
        // --fixcode lib.so:0xRVA[:len] -- balikin byte dari file .so di disk
        if (strcmp(a, "--fixcode") == 0) {
            if (i + 1 < argc && argv[i + 1]) g_fixCode.push_back(argv[++i]);
            continue;
        }
        if (strncmp(a, "--fixcode=", 10) == 0) { g_fixCode.push_back(a + 10); continue; }
        if (strcmp(a, "--forceret-file") == 0) {
            logx::add("[force] catatan: %s (%zu method)", patch::forcePath().c_str(),
                      patch::fileForcedCount());
            g_forceListFile = true;
            continue;
        }
        // ---- gencode (lihat core/gencode.h) ----
        if (strcmp(a, "--gencode") == 0) {
            if (i + 1 < argc && argv[i + 1]) {
                ForceSpec s; parseForceSpec(argv[++i], s); g_genCode.push_back(s);
            }
            continue;
        }
        if (strncmp(a, "--gencode=", 10) == 0) {
            ForceSpec s; parseForceSpec(a + 10, s); g_genCode.push_back(s); continue;
        }
        if (strcmp(a, "--ggcode") == 0) {
            if (i + 1 < argc && argv[i + 1]) {
                ForceSpec s; parseForceSpec(argv[++i], s); g_ggCode.push_back(s);
            }
            continue;
        }
        if (strncmp(a, "--ggcode=", 9) == 0) {
            ForceSpec s; parseForceSpec(a + 9, s); g_ggCode.push_back(s); continue;
        }
        if (strcmp(a, "--huntaddr") == 0) {
            if (i + 1 < argc && argv[i + 1]) g_huntAddr = argv[++i];
            continue;
        }
        if (strncmp(a, "--huntaddr=", 11) == 0) { g_huntAddr = a + 11; continue; }
        // ---- memview / inspect / search (lihat runHexAsm dkk) ----
        if (strcmp(a, "--hexdump") == 0) {
            if (i + 1 < argc && argv[i + 1]) g_hexDump.push_back(argv[++i]);
            continue;
        }
        if (strncmp(a, "--hexdump=", 10) == 0) { g_hexDump.push_back(a + 10); continue; }
        if (strcmp(a, "--asmview") == 0) {
            if (i + 1 < argc && argv[i + 1]) g_asmView.push_back(argv[++i]);
            continue;
        }
        if (strncmp(a, "--asmview=", 10) == 0) { g_asmView.push_back(a + 10); continue; }
        if (strcmp(a, "--findobj") == 0) {
            if (i + 1 < argc && argv[i + 1]) g_findObj.push_back(argv[++i]);
            continue;
        }
        if (strncmp(a, "--findobj=", 10) == 0) { g_findObj.push_back(a + 10); continue; }
        if (strcmp(a, "--inspect") == 0) {
            if (i + 1 < argc && argv[i + 1]) g_inspect.push_back(argv[++i]);
            continue;
        }
        if (strncmp(a, "--inspect=", 10) == 0) { g_inspect.push_back(a + 10); continue; }
        if (strcmp(a, "--strings") == 0) {
            g_strings = true;
            if (i + 1 < argc && argv[i + 1] && argv[i + 1][0] != '-')
                g_stringsFilter = argv[++i];
            continue;
        }
        if (strncmp(a, "--strings=", 10) == 0) {
            g_strings = true; g_stringsFilter = a + 10; continue;
        }
        if (strcmp(a, "--search") == 0) {
            if (i + 1 < argc && argv[i + 1]) {
                SearchReq r; r.kw = argv[++i]; r.by = g_searchBy;
                g_searches.push_back(r);
            }
            continue;
        }
        if (strncmp(a, "--search=", 9) == 0) {
            SearchReq r; r.kw = a + 9; r.by = g_searchBy;
            g_searches.push_back(r); continue;
        }
        if (strcmp(a, "--by") == 0) {
            if (i + 1 < argc && argv[i + 1]) g_searchBy = argv[++i];
            continue;
        }
        if (strncmp(a, "--by=", 5) == 0) { g_searchBy = a + 5; continue; }
        if (strncmp(a, "--pid=", 6) == 0) { target::setSelPid(atoi(a + 6)); continue; }
        if (strncmp(a, "--lib=", 6) == 0) { target::setLib(a + 6); sawTarget = true; continue; }
        // ---- patch runtime (lihat core/patch.h) ----
        if (strcmp(a, "--patch") == 0) {
            if (i + 1 < argc && argv[i + 1]) {
                std::string err;
                if (!patch::addSpec(argv[++i], &err))
                    logx::add("[patch] argumen --patch dilewat: %s", err.c_str());
            }
            continue;
        }
        if (strncmp(a, "--patch=", 8) == 0) {
            std::string err;
            if (!patch::addSpec(a + 8, &err))
                logx::add("[patch] argumen --patch= dilewat: %s", err.c_str());
            continue;
        }
        if (strcmp(a, "--patch-file") == 0 || strcmp(a, "--patchfile") == 0) {
            if (i + 1 < argc && argv[i + 1]) g_patchFile = argv[++i];
            continue;
        }
        if (strncmp(a, "--patch-file=", 13) == 0) { g_patchFile = a + 13; continue; }
        if (strncmp(a, "--patchfile=", 12) == 0) { g_patchFile = a + 12; continue; }
        if (strcmp(a, "--peek") == 0) {
            if (i + 1 < argc && argv[i + 1]) g_peek = argv[++i];
            continue;
        }
        if (strncmp(a, "--peek=", 7) == 0) { g_peek = a + 7; continue; }
        if (strcmp(a, "--peek-len") == 0) {
            if (i + 1 < argc && argv[i + 1]) g_peekLen = (size_t)atoi(argv[++i]);
            continue;
        }
        if (strncmp(a, "--peek-len=", 11) == 0) { g_peekLen = (size_t)atoi(a + 11); continue; }
        if (strcmp(a, "--no-patch-auto") == 0) { patch::setEnabled(false); continue; }
        if (strcmp(a, "--patch-auto") == 0)    { patch::setEnabled(true);  continue; }
        if (strncmp(a, "--", 2) == 0)     continue;      // argumen lain diabaikan
        // Angka doang = pid (biar nggak salah keisi ke kolom lib/package).
        bool numeric = true;
        for (const char* q = a; *q; ++q) if (!isdigit((unsigned char)*q)) { numeric = false; break; }
        if (numeric && strlen(a) > 1) { target::setSelPid(atoi(a)); sawTarget = true; continue; }

        // Positional bisa package ATAU lib. Aturannya:
        //   - package yang lagi dipakai, sub-prosesnya, atau package dasar-nya
        //     ("com.mobile.legends" vs "com.mobile.legends:UnityKillsMe") => package
        //   - ada titik tapi bukan ".so" (mis. "com.kiloo.subwaysurf")       => package
        //   - sisanya ("libcsharp.so", "auto")                               => lib
        const std::string& cur = target::package();
        const std::string  arg = a;
        const size_t m = arg.size() < cur.size() ? arg.size() : cur.size();
        const bool samePkg = !cur.empty() && m && strncmp(a, cur.c_str(), m) == 0;
        const bool looksPkg = strchr(a, '.') != nullptr && strstr(a, ".so") == nullptr;
        if (cur.empty() || samePkg || looksPkg) target::setPackage(a);
        else                                    target::setLib(a);
        sawTarget = true;
    }
    if (sawTarget || target::selPid() > 0) target::save();

    // Argumen udah selesai dibaca -> jejaknya dihapus (nama proses samaran).
    bypass::setTitleFromArgv(argc, argv);

    installFaultGuard();

    // ---- init overlay, dijaga dari fault ----
    // ROM tertentu (kejadian di Android 17) nolak surface dari proses luar:
    // bikin overlay langsung SIGSEGV. Dulu itu = proses keluar, jadi dump yang
    // nggak butuh layar pun nggak pernah jalan. Sekarang fault overlay cuma
    // memicu mode HEADLESS: kerja (attach + dump) tetap jalan, cuma tanpa menu.
    if (!g_headless && sigsetjmp(g_jmp, 1) != 0) {
        g_guard = 0;
        logx::add("[boot] FATAL %s waktu bikin overlay (SurfaceFlinger/ANWC)",
                  sigName(g_sig));
        logx::add("[boot] overlay ditolak ROM -- lanjut HEADLESS (dump tanpa menu)");
        g_headlessAuto = true;
    } else if (!g_headless) {
        g_guard = 1;

        screen_config();
        int abs_ScreenX = displayInfo.height > displayInfo.width ? displayInfo.height
                                                                 : displayInfo.width;
        native_window_screen_x = abs_ScreenX;
        native_window_screen_y = abs_ScreenX;

        if (!initGUI_draw(abs_ScreenX, abs_ScreenX, true)) {
            g_guard = 0;
            logx::add("[boot] initGUI_draw gagal -- lanjut HEADLESS");
            g_headlessAuto = true;
        } else {
            // readOnly=false -> bisa inject touch (macro) + input menu tidak bocor ke game
            if (!Touch_Init(displayInfo.width, displayInfo.height, displayInfo.orientation, false)) {
                logx::add("[boot] touch read-write gagal, fallback read-only");
                Touch_Init(displayInfo.width, displayInfo.height, displayInfo.orientation, true);
            }
            g_guard = 0;
        }
    }

    const bool headless = g_headless || g_headlessAuto;
    if (!headless) ui::init();
    g_startTime = time(nullptr);
    // Anti-deteksi: samarkan nama proses + rapikan file kerja (lihat bypass.h
    // buat batasannya -- ini ngurangin jejak, bukan nge-patch root-check game).
    bypass::apply();
    logx::add("[boot] Panxcz LibTool v1.1 start (universal il2cpp + dump class-slot)");
    if (headless)
        logx::add("[boot] MODE HEADLESS: tanpa overlay -- kerja jalan di thread, "
                  "log lewat logcat, keluar sendiri abis selesai");
    if (g_auto)
        logx::add("[auto] MODE OTOMATIS: cari game + attach + dump sendiri. "
                  "Stop/keluar lewat tombol di menu.");
    logx::add("[boot] layar %dx%d ori=%u", displayInfo.width, displayInfo.height,
              displayInfo.orientation);

    pthread_t th;
    pthread_create(&th, nullptr, bootWorker, nullptr);
    pthread_detach(th);

    if (headless) {
        // Tanpa EGL/surface: nggak ada frame buat digambar. Cukup tungguin
        // bootWorker sampai kerja (attach + dump) selesai, sambil ngejalanin
        // tick ringan yang cuma baca memori.
        int frame = 0;
        while (main_thread_flag.load() && !g_bootDone.load()) {
            if ((frame++ % 3) == 0) {
                scan::tick();
                bypass::tick();
                patch::tick();
                if (bypass::panicRequested()) {
                    logx::add("[stealth] panic aktif -- tool keluar sendiri");
                    break;
                }
            }
            usleep(20000);
        }
    } else {
        int frame = 0;
        while (main_thread_flag && !ui::exitRequested()) {
            g_frame_done.store(false);

            drawBegin();
            ui::handleKeys();
            ui::draw();
            drawEnd();

            g_frame_done.store(true);

            if ((frame++ % 3) == 0) {
                scan::tick();
                bypass::tick();
                patch::tick();          // pasang ulang kalau byte-nya ketimpa game
                // kedeteksi game (fd/maps nunjuk ke kita) -> lepas overlay & keluar
                if (bypass::panicRequested()) {
                    logx::add("[stealth] panic aktif -- tool keluar sendiri");
                    ui::requestExit();
                }
            }
        }
    }

    g_frame_done.store(true);
    while (!g_frame_done.load()) {}
    // Tunggu thread kerja benar-benar berhenti sebelum teardown -- kalau nggak,
    // `_exit` di bawah bisa nyetop di tengah dump (file-nya jadi setengah jadi).
    // Batasnya: --exit nunggu sampai 300 detik, tutup manual (tombol X/KELUAR)
    // cukup 3 detik (niatnya memang buru-buru keluar).
    {
        const double waitMax = g_probeExit ? 300.0 : 3.0;
        const double t0 = abortx::nowSec();
        while (!g_bootDone.load() && abortx::nowSec() - t0 < waitMax) usleep(100000);
    }
    logSummary((double)(time(nullptr) - g_startTime));
    // Hentikan dulu kerja berat yang masih jalan (dump/pool/scan), baru teardown.
    // Tanpa ini, keluar pas dump/scan lagi jalan bisa bikin proses pembacaan
    // memori lepas di tengah jalan -- gejalanya crash/exit nggak karuan.
    abortx::request("tool keluar");
    usleep(300000);
    logx::add("[boot] keluar -- lepas overlay & touch");
    if (!headless) ui::shutdown();
    scan::clear();
    mem::detach();
    logx::add("[boot] exit: bersih (exit code 0)");
    // Overlay-nya proses terpisah: game-nya sama sekali nggak disentuh, jadi
    // keluar di sini cukup buat ngebalikin layar ke game. Teardown EGL-nya
    // (shutdown()) di beberapa device melempar SIGABRT, makanya di-skip --
    // surface-nya otomatis dilepas SurfaceFlinger begitu prosesnya mati.
    _exit(0);
}

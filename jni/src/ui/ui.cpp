// ============================================================================
//  ui.cpp - menu PANXCZ LIBTOOL (universal il2cpp external tool)
//
//  Tab:
//    TARGET  - pilih game (package) + lib il2cpp, attach, auto-detect
//    CLASSES - ketik nama class -> fields (offset) + methods (RVA) + instance,
//              nilai field bisa dibaca & ditulis
//    PATCH   - tulis byte (hex) ke RVA lib il2cpp (bisa di-restore)
//    SCAN    - memory scanner biasa (first scan / refine / freeze / edit)
//    TOUCH   - macro gesture (swipe/tap) lewat uinput
//    CONFIG  - log, re-attach, info
//
//  Overlay ini proses terpisah, jadi tidak ada keyboard sistem -> semua input
//  teks lewat keypad sendiri (yang sudah ada di bawah).
// ============================================================================
#include "ui/ui.h"

#include "Android_draw/draw.h"
#include "Android_touch/TouchHelperA.h"
#include "ImGui/imgui.h"

#include "core/abort.h"
#include "core/mem.h"
#include "core/il2cpp.h"
#include "core/scanner.h"
#include "core/log.h"
#include "core/keys.h"
#include "core/target.h"
#include "core/explore.h"
#include "core/offlinedump.h"
#include "core/dumpout.h"
#include "core/metapool.h"
#include "core/patch.h"
#include "core/bypass.h"

#include <cstdio>
#include <cstdarg>
#include <cstring>
#include <cctype>
#include <cstdlib>
#include <cmath>
#include <algorithm>
#include <string>
#include <vector>
#include <pthread.h>
#include <unistd.h>

namespace ui {

// ---------------------------------------------------------------------------
// state
// ---------------------------------------------------------------------------
static bool   g_open   = true;
static bool   g_min    = false;
static bool   g_exitReq = false;   // tombol Keluar ditekan
static ImVec2 g_pos    = {-1, -1};
static ImVec2 g_size   = {660, 640};
static bool   g_drag   = false;
static ImVec2 g_ts{}, g_ps{};
static uint32_t g_ori  = 0xFFFFFFFF;
static bool   g_hud    = true;
static bool   g_showLog = false;
static int    g_page    = 1;      // halaman aktif (sidebar, gaya Astral)
static bool   g_cycle   = false;   // --ui-cycle: pindah halaman sendiri (buat tes)
static float  g_cycleSec = 2.0f;

static ImVec2 g_fabPos = {-1, -1};
static float  g_fabMoved = 0.0f;

bool visible() { return g_open; }
void setVisible(bool v) { g_open = v; }

// ---------------------------------------------------------------------------
// keypad internal
// ---------------------------------------------------------------------------
enum KbKind { KB_INT = 0, KB_FLOAT, KB_DOUBLE, KB_TEXT };

static bool   g_kbOpen   = false;
static int    g_kbKind   = KB_DOUBLE;
static void*  g_kbTarget = nullptr;
static char   g_kbBuf[64] = {0};
static void*  g_kbDone   = nullptr;

static bool kbTook(void* target) {
    if (g_kbDone == target) { g_kbDone = nullptr; return true; }
    return false;
}

static void kbCommit() {
    if (!g_kbTarget) return;
    switch (g_kbKind) {
        case KB_INT:    *(int32_t*)g_kbTarget = (int32_t)strtol(g_kbBuf, nullptr, 10); break;
        case KB_FLOAT:  *(float*)g_kbTarget   = strtof(g_kbBuf, nullptr); break;
        case KB_DOUBLE: *(double*)g_kbTarget  = strtod(g_kbBuf, nullptr); break;
        case KB_TEXT: {
            char* d = (char*)g_kbTarget;
            strncpy(d, g_kbBuf, 63);
            d[63] = 0;
            break;
        }
    }
    g_kbDone = g_kbTarget;
}

static bool kbField(const char* id, int kind, void* target, float w = 180.0f) {
    char show[80];
    switch (kind) {
        case KB_INT:    snprintf(show, sizeof(show), "%d", *(int32_t*)target); break;
        case KB_FLOAT:  snprintf(show, sizeof(show), "%.2f", (double)*(float*)target); break;
        case KB_DOUBLE: snprintf(show, sizeof(show), "%.0f", *(double*)target); break;
        default:        snprintf(show, sizeof(show), "%s", (const char*)target); break;
    }
    char lbl[128];
    snprintf(lbl, sizeof(lbl), "%s##%s", show[0] ? show : "(kosong)", id);

    const bool active = g_kbOpen && g_kbTarget == target;
    if (active) ImGui::PushStyleColor(ImGuiCol_Button, {0.16f, 0.46f, 0.78f, 1.0f});
    const bool clicked = ImGui::Button(lbl, {w, 0});
    if (active) ImGui::PopStyleColor();
    ImGui::SameLine();
    ImGui::TextDisabled("[%s]", (kind == KB_TEXT) ? "teks" : "angka");

    if (clicked) {
        g_kbKind = kind;
        g_kbTarget = target;
        snprintf(g_kbBuf, sizeof(g_kbBuf), "%s", (kind == KB_TEXT) ? (const char*)target : show);
        g_kbOpen = true;
    }
    return kbTook(target);
}

static void drawKeypad() {
    if (!g_kbOpen) return;
    const float sw = (float)displayInfo.width;
    const float sh = (float)displayInfo.height;
    const bool textMode = (g_kbKind == KB_TEXT);

    ImGui::SetNextWindowPos({sw * 0.5f, sh - 8.0f}, ImGuiCond_Always, {0.5f, 1.0f});
    ImGui::SetNextWindowBgAlpha(0.94f);
    ImGui::Begin("##keypad", nullptr,
                 ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                 ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar |
                 ImGuiWindowFlags_NoNav | ImGuiWindowFlags_AlwaysAutoResize |
                 ImGuiWindowFlags_NoFocusOnAppearing);

    ImGui::TextColored({0.5f, 0.9f, 1, 1}, "%s", textMode ? "KETIK TEKS" : "KETIK ANGKA");
    ImGui::Text(" > %s_", g_kbBuf);
    ImGui::Separator();

    auto push = [&](char c) {
        const size_t n = strlen(g_kbBuf);
        if (n + 1 < sizeof(g_kbBuf)) { g_kbBuf[n] = c; g_kbBuf[n + 1] = 0; }
    };

    static const char* kNumRows[] = {"1234567890", "-._", nullptr};
    static const char* kTxtRows[] = {"1234567890-", "qwertyuiop", "asdfghjkl_",
                                     "zxcvbnm./:", nullptr};
    const char** rows = textMode ? kTxtRows : kNumRows;

    const float bw = textMode ? 46.0f : 42.0f;
    for (int r = 0; rows[r]; ++r) {
        const char* row = rows[r];
        for (int i = 0; row[i]; ++i) {
            char lbl[3] = {row[i], 0, 0};
            if (i) ImGui::SameLine(0, 4.0f);
            if (ImGui::Button(lbl, {bw, 38.0f})) push(row[i]);
        }
    }

    if (ImGui::Button("C", {72, 36})) g_kbBuf[0] = 0;
    ImGui::SameLine(0, 4.0f);
    if (ImGui::Button("HAPUS", {86, 36})) {
        const size_t n = strlen(g_kbBuf);
        if (n) g_kbBuf[n - 1] = 0;
    }
    ImGui::SameLine(0, 4.0f);
    if (ImGui::Button("OK##kb", {130, 40})) { kbCommit(); g_kbOpen = false; }
    ImGui::SameLine(0, 4.0f);
    if (ImGui::Button("BATAL", {80, 36})) g_kbOpen = false;

    ImGui::End();
}

// ---------------------------------------------------------------------------
// bulatan mengambang
// ---------------------------------------------------------------------------
static void drawFab() {
    if (!g_open || !g_min) return;
    const float sw = (float)displayInfo.width;
    const float sh = (float)displayInfo.height;
    if (g_fabPos.x < 0) g_fabPos = {64.0f, sh * 0.42f};
    if (g_fabPos.x < 34) g_fabPos.x = 34;
    if (g_fabPos.x > sw - 34) g_fabPos.x = sw - 34;
    if (g_fabPos.y < 34) g_fabPos.y = 34;
    if (g_fabPos.y > sh - 34) g_fabPos.y = sh - 34;

    ImGui::SetNextWindowPos(g_fabPos, ImGuiCond_Always, {0.5f, 0.5f});
    ImGui::SetNextWindowBgAlpha(0.0f);
    ImGui::Begin("##fab", nullptr,
                 ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                 ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar |
                 ImGuiWindowFlags_NoNav | ImGuiWindowFlags_AlwaysAutoResize |
                 ImGuiWindowFlags_NoFocusOnAppearing);

    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const ImVec2 c = {p.x + 30.0f, p.y + 30.0f};
    const bool hot = ImGui::IsMouseHoveringRect({p.x, p.y}, {p.x + 60.0f, p.y + 60.0f});
    dl->AddCircleFilled(c, 29.0f, IM_COL32(10, 16, 24, hot ? 235 : 185), 40);
    dl->AddCircle(c, 29.0f, IM_COL32(90, 200, 255, 240), 40, 3.0f);
    dl->AddText({c.x - 13.0f, c.y - 8.0f}, IM_COL32(150, 225, 255, 255), "PH");

    ImGui::InvisibleButton("##fabbtn", {60, 60});
    if (ImGui::IsItemActive()) {
        const ImVec2 d = ImGui::GetIO().MouseDelta;
        if (fabsf(d.x) + fabsf(d.y) > 0.4f) {
            g_fabPos.x += d.x;
            g_fabPos.y += d.y;
            g_fabMoved += fabsf(d.x) + fabsf(d.y);
        }
    }
    if (ImGui::IsItemDeactivated()) {
        if (g_fabMoved < 8.0f) g_min = false;
        g_fabMoved = 0.0f;
    }
    ImGui::End();
}

// ---------------------------------------------------------------------------
// target game (package + lib il2cpp)
// ---------------------------------------------------------------------------
static char g_pkgBuf[96] = {0};
static char g_libBuf[64] = {0};
static int  g_pidSel = 0;          // 0 = auto (pilih proses yang punya il2cpp)

// pemilih package / lib (window terpisah, biar nggak perlu ngetik panjang)
static bool g_pkgPickOpen = false;
static bool g_libPickOpen = false;
static char g_pickFilter[64] = {0};
static std::vector<std::string> g_libNames;
static bool g_libNamesLoaded = false;
static int  g_libPickSel = -1;
static void collectLibs();        // definisinya di bawah (dipakai tab Target)
static std::vector<target::Proc> g_procs;
static bool g_procsLoaded = false;
static bool g_cfgTouched = false;

// dump offline (tabel class-slot) -- dimuat di thread, 36 MB jadi jangan
// ngeblok render
static char            g_dumpBuf[128] = {0};
static volatile bool   g_dumpBusy = false;
static volatile int    g_dumpDone = 0;      // 0 idle, 1 gagal, 2 sukses
static std::string     g_dumpMsg;
static pthread_t       g_dumpTh;

// ---------------------------------------------------------------------------
// kontrol DUMP SENDIRI (tombol di tab Info):
//   Start dump  = dump native SEKARANG (root doang, tanpa frida/PC)
//   Online dump = tunggu masuk MATCH dulu baru dump. Alasannya: MethodInfo baru
//                 dibikin engine waktu method-nya kepakai, jadi makin rame
//                 (makin banyak RVA ke-resolve) kalau di-dump pas match.
// ---------------------------------------------------------------------------
static volatile bool   g_ndBusy  = false;
static volatile int    g_ndStage = 0;    // 0 idle, 1 nunggu match, 2 dump, 3 ok, 4 gagal
static volatile bool   g_ndStop  = false;
static volatile bool   g_ndDry   = false;   // true = "Start scan" (hitung aja, nggak nulis)
static std::string     g_ndMsg;
static pthread_t       g_ndTh;
// folder hasil (kosong = default /sdcard/panxcz-tools/dumper/<pkg>/<versi>/)
static char            g_dirBuf[160] = {0};
static char            g_dirApplied[160] = {0};   // yang lagi dipakai (buat ditampilin)

static bool inMatch() {
    const uintptr_t k = il2::findKlass("BattleManager", nullptr);
    if (!k) return false;
    return il2::findInstance(k, 8) != 0;
}

static void* ndWorker(void*) {
    if (g_ndStage == 1) {
        for (int i = 0; i < 3600 && !g_ndStop; ++i) {   // max ~2 jam
            if (inMatch()) break;
            sleep(2);
        }
        if (g_ndStop || g_ndStage != 1) {
            g_ndMsg = g_ndStop ? "dibatalin" : "nunggu match: timeout";
            g_ndStage = 4;
            g_ndBusy = false;
            return nullptr;
        }
    }
    g_ndStage = 2;
    g_ndMsg = g_ndDry ? "scan jalan..." : "dump jalan...";
    dumpout::Options o;
    o.allLibs = true;
    o.json = true;
    o.header = true;
    o.ptr = true;
    o.dryRun = g_ndDry;
    if (g_dirApplied[0]) o.outDir = g_dirApplied;
    dumpout::Result r;
    const bool ok = dumpout::run(o, r);
    if (r.aborted) {
        g_ndMsg = "dihentikan (Stop)";
        g_ndStage = 4;
    } else if (!ok) {
        g_ndMsg = "gagal -- lihat logcat";
        g_ndStage = 4;
    } else if (g_ndDry) {
        char b[224];
        snprintf(b, sizeof(b), "scan OK: %zu kelas, %zu slot, %zu field, %zu method",
                 r.classes, r.withSlot, r.fields, r.methods);
        g_ndMsg = b;
        g_ndStage = 3;
    } else {
        g_ndMsg = "OK: " + r.path;
        g_ndStage = 3;
    }
    g_ndBusy = false;
    return nullptr;
}

// online=true -> tunggu masuk MATCH dulu; dry=true -> scan aja (nggak nulis)
static void startNativeDump(bool online, bool dry = false) {
    if (g_ndBusy) return;
    snprintf(g_dirApplied, sizeof(g_dirApplied), "%s", g_dirBuf);
    dumpout::clearAbort();
    g_ndBusy = true;
    g_ndStop = false;
    g_ndDry  = dry;
    g_ndStage = online ? 1 : 2;
    g_ndMsg = online ? "nunggu masuk match..."
                     : (dry ? "scan jalan..." : "dump jalan...");
    pthread_create(&g_ndTh, nullptr, ndWorker, nullptr);
    pthread_detach(g_ndTh);
}

static void* dumpWorker(void*) {
    std::string tried;
    bool ok;
    if (g_dumpBuf[0]) ok = dump::load(g_dumpBuf);
    else              ok = dump::loadAuto(&tried);
    g_dumpMsg = ok ? dump::path() : (tried.empty() ? std::string("nggak ada file dump") : tried);
    g_dumpDone = ok ? 2 : 1;
    g_dumpBusy = false;
    return nullptr;
}

static void startDumpLoad() {
    if (g_dumpBusy) return;
    g_dumpBusy = true;
    g_dumpDone = 0;
    pthread_create(&g_dumpTh, nullptr, dumpWorker, nullptr);
    pthread_detach(g_dumpTh);
}

static void syncTargetBuffers() {
    if (g_cfgTouched) return;
    snprintf(g_pkgBuf, sizeof(g_pkgBuf), "%s", target::package().c_str());
    snprintf(g_libBuf, sizeof(g_libBuf), "%s", target::lib().c_str());
    g_pidSel = target::selPid();
}

static void applyTarget() {
    target::setPackage(g_pkgBuf);
    target::setLib(g_libBuf);
    target::setSelPid(g_pidSel);
    target::save();
}

static void attachNow() {
    if (!target::attach()) return;
    if (il2::init()) {
        target::detectLib();
    }
}

// ---------------------------------------------------------------------------
// resolve worker (class explorer)
// ---------------------------------------------------------------------------
static char g_clsName[64] = {0};
static char g_clsNs[48]   = {0};
static std::vector<std::string> g_hist;
static std::vector<uintptr_t>   g_inst;
static int  g_instSel = 0;

static volatile bool g_resolving = false;
static volatile int  g_resDone = 0;
static pthread_t     g_resTh;

static void* resWorker(void*) {
    const bool ok = explore::resolve(g_clsName, g_clsNs[0] ? g_clsNs : nullptr);
    g_resDone = ok ? 2 : 1;
    g_resolving = false;
    return nullptr;
}

static void startResolve() {
    if (g_resolving) return;
    if (!g_clsName[0]) return;
    g_resolving = true;
    g_resDone = 0;
    std::string full = std::string(g_clsNs[0] ? g_clsNs : "") + (g_clsNs[0] ? "::" : "") + g_clsName;
    bool dup = false;
    for (const auto& h : g_hist) if (h == full) { dup = true; break; }
    if (!dup) g_hist.insert(g_hist.begin(), full);
    if (g_hist.size() > 24) g_hist.pop_back();

    pthread_create(&g_resTh, nullptr, resWorker, nullptr);
    pthread_detach(g_resTh);
}

// nilai field yang lagi ditampilkan (dicache, refresh manual / tiap ganti instance)
static std::vector<double>      g_fvalNum;
static std::vector<std::string> g_fvalStr;
static std::vector<int>         g_ftype;      // index FType per field
static char                     g_editBuf[96] = {0};
static int                      g_editField = -1;

static explore::FType ftypeOf(int idx) {
    if (idx < 0 || idx >= (int)g_ftype.size()) return explore::FType::Int32;
    return (explore::FType)g_ftype[idx];
}

// Field statis dibaca dari static_fields, bukan dari instance object.
static uintptr_t baseFor(const explore::Field& f) {
    const auto& K = explore::info();
    if (f.isStatic) return K.statics;
    if (g_inst.empty()) return 0;
    if (g_instSel >= (int)g_inst.size()) g_instSel = 0;
    return g_inst[g_instSel];
}

static void refreshValues() {
    const auto& K = explore::info();
    g_fvalNum.assign(K.fields.size(), 0.0);
    g_fvalStr.assign(K.fields.size(), std::string());
    if (g_ftype.size() != K.fields.size()) {
        g_ftype.resize(K.fields.size());
        for (size_t i = 0; i < K.fields.size(); ++i)
            g_ftype[i] = (int)explore::guessType(K.fields[i].typeEnum);
    }
    char tmp[128];
    for (size_t i = 0; i < K.fields.size(); ++i) {
        const uintptr_t base = baseFor(K.fields[i]);
        if (!base) continue;
        tmp[0] = 0;
        explore::readValue(base, K.fields[i].offset, ftypeOf((int)i), g_fvalNum[i], tmp, sizeof(tmp));
        g_fvalStr[i] = tmp;
    }
}

static void findInstances() {
    g_inst = explore::instances(64);
    g_instSel = 0;
    logx::add("[ui] %zu instance ditemukan", g_inst.size());
    refreshValues();
}

// ---------------------------------------------------------------------------
// patch runtime -- LOGIC-nya di core/patch.cpp (kepakai juga dari CLI
// --patch / auto-apply file). Di sini cuma input + tampilan daftarnya.
// ---------------------------------------------------------------------------
static char g_patchMod[48]   = "";          // module (kosong = lib il2cpp utama)
static char g_patchRva[32]   = "0x0";
static char g_patchHex[160]  = "";
static char g_patchSig[192]  = "";
static char g_patchFile[192] = "";
static bool g_patchAbs      = false;         // rva = alamat mutlak (heap/data)
static bool g_patchFileInit = false;
static char g_patchNote[192] = "";

static void patchNoteSet(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(g_patchNote, sizeof(g_patchNote), fmt, ap);
    va_end(ap);
}

// gabung "<module>:<rva> = <byte>" (atau "abs:<alamat> = <byte>") terus apply
static void patchAddByte() {
    const uint64_t rva = strtoull(g_patchRva, nullptr, 0);
    if (!rva) { patchNoteSet("RVA-nya masih 0"); return; }

    char spec[320];
    if (g_patchAbs) snprintf(spec, sizeof(spec), "abs:0x%llX = %s", (unsigned long long)rva, g_patchHex);
    else if (g_patchMod[0])
        snprintf(spec, sizeof(spec), "%s:0x%llX = %s", g_patchMod, (unsigned long long)rva, g_patchHex);
    else
        snprintf(spec, sizeof(spec), "0x%llX = %s", (unsigned long long)rva, g_patchHex);

    std::string err;
    if (!patch::addSpec(spec, &err)) {
        patchNoteSet("gagal: %s", err.c_str());
        logx::add("[patch] %s", err.c_str());
        return;
    }
    const size_t idx = patch::list().size() - 1;
    const bool ok = patch::applyEntry(idx);
    patchNoteSet(ok ? "terpasang + terbaca balik sama"
                    : "ditulis, tapi verifikasi gagal (cek log)");
}

// patch pakai pola: cari dulu, baru timpa
static void patchAddPattern() {
    char spec[448];
    snprintf(spec, sizeof(spec), "sig %s = %s -> %s", g_patchMod, g_patchSig, g_patchHex);
    std::string err;
    if (!patch::addSpec(spec, &err)) {
        patchNoteSet("gagal: %s", err.c_str());
        logx::add("[patch] %s", err.c_str());
        return;
    }
    const size_t idx = patch::list().size() - 1;
    const bool ok = patch::applyEntry(idx);
    patchNoteSet(ok ? "pola ketemu + terpasang"
                    : "pola nggak ketemu / gagal tulis (cek log)");
}

// ---------------------------------------------------------------------------
// touch helper (macro gesture)
// ---------------------------------------------------------------------------
static volatile int  g_macro = 0;
static volatile int  g_macroMs = 350;
static volatile float g_tapX = 0.5f, g_tapY = 0.8f;
static volatile bool g_thRun = false;
static pthread_t     g_th;
static bool          g_thStarted = false;
static char          g_thMsg[128] = {0};

static void swipe(int dir) {
    float w = (float)displayInfo.width, h = (float)displayInfo.height;
    float cx = w * 0.5f, cy = h * 0.5f;
    float d = (dir <= 2 ? h : w) * 0.16f;

    float x0 = cx, y0 = cy, x1 = cx, y1 = cy;
    switch (dir) {
        case 1: y0 = cy + d; y1 = cy - d; break;
        case 2: y0 = cy - d; y1 = cy + d; break;
        case 3: x0 = cx + d; x1 = cx - d; break;
        case 4: x0 = cx - d; x1 = cx + d; break;
        default: break;
    }
    const int steps = 8;
    Touch_Down(x0, y0);
    for (int i = 1; i <= steps; ++i) {
        float t = (float)i / steps;
        Touch_Move(x0 + (x1 - x0) * t, y0 + (y1 - y0) * t);
        usleep(9000);
    }
    Touch_Up();
}

static void* touchWorker(void*) {
    while (g_thRun) {
        int a = g_macro;
        if (a == 5) {
            float x = (float)displayInfo.width * g_tapX;
            float y = (float)displayInfo.height * g_tapY;
            Touch_Down(x, y);
            usleep(20000);
            Touch_Up();
        } else if (a >= 1 && a <= 4) {
            swipe(a);
        }
        usleep((useconds_t)g_macroMs * 1000);
    }
    return nullptr;
}

static void startTouchWorker() {
    if (g_thStarted) return;
    g_thRun = true;
    pthread_create(&g_th, nullptr, touchWorker, nullptr);
    pthread_detach(g_th);
    g_thStarted = true;
}

// ---------------------------------------------------------------------------
// tema PENAWARE (near-black + aksen biru, rail ikon + kartu membulat 13px)
//
// Paletnya dikumpulkan di satu tempat biar semua tab pakai warna yang sama
// (dulu tiap tab nulis warna sendiri-sendiri, jadi kelihatan campur).
// ---------------------------------------------------------------------------
//
// Palet & gaya: ngikutin UI referensi PenaWare -- latar near-black (0.05,0.05,
// 0.07), panel rail/kepala sedikit lebih terang (0.06,0.06,0.08), SATU warna
// aksen biru (0.3,0.6,1.0), sudut membulat 13px. Semua widget (tombol, toggle,
// kartu, progress) ambil warna dari sini, jadi ngganti tema cukup di blok ini.
const ImVec4 kBg      = ImColor(13, 13, 18);      // (0.05,0.05,0.07) latar jendela
const ImVec4 kCard    = ImColor(15, 15, 20);      // (0.06,0.06,0.08) panel rail/kepala
const ImVec4 kCardCap = ImColor(15, 15, 20);      // (0.06,0.06,0.08) kepala kartu
const ImVec4 kLine    = ImColor(27, 27, 34);      // garis 1px halus
const ImVec4 kAccent  = ImColor(77, 153, 255);    // (0.3,0.6,1.0) aksen utama
const ImVec4 kAccent2 = ImColor(102, 179, 255);   // (0.4,0.7,1.0) aksen hover
const ImVec4 kOk      = ImColor(77, 200, 140);
const ImVec4 kWarn    = ImColor(255, 184, 77);
const ImVec4 kDanger  = ImColor(247, 112, 112);
const ImVec4 kMuted   = ImColor(122, 126, 142);   // teks redup
const ImVec4 kTextHi  = ImColor(236, 238, 245);   // teks terang
const ImVec4 kBtn     = ImColor(20, 20, 26);      // (0.08,0.08,0.10) tombol
const ImVec4 kBtnHov  = ImColor(31, 31, 38);      // (0.12,0.12,0.15)
const ImVec4 kBtnAct  = ImColor(38, 38, 46);      // (0.15,0.15,0.18)
const ImVec4 kBtnOut  = ImColor(43, 43, 52);      // border tombol
const ImVec4 kPageAct = ImColor(31, 31, 38);      // latar item rail aktif
const ImVec4 kOff     = ImColor(38, 38, 46);      // toggle posisi OFF
const ImVec4 kDim     = ImColor(88, 92, 108);     // teks paling redup (item non-aktif)

// aksen dengan alpha (dipakai di mana-mana)
static inline ImVec4 kA(float a) { return {kAccent.x, kAccent.y, kAccent.z, a}; }

// toggle "switch" gaya UI referensi (track membulat + knob). Balikannya sama
// kayak ImGui::Checkbox (true waktu diklik), jadi bisa dipakai drop-in.
static bool toggleBare(const char* id, bool* v) {
    const float h = ImGui::GetTextLineHeight() + 2.f;
    const float w = h * 1.85f;
    const ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton(id, {w, h});
    const bool clicked = ImGui::IsItemClicked();
    const bool hov     = ImGui::IsItemHovered();
    if (clicked) *v = !*v;

    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 a = {p.x + 2, p.y + 1};
    const ImVec2 b = {p.x + w - 2, p.y + h - 1};
    const ImU32 track = ImGui::GetColorU32(*v ? kA(1.f) : (hov ? kBtnAct : kOff));
    dl->AddRectFilled(a, b, track, (b.y - a.y) * 0.5f);
    if (!*v) dl->AddRect(a, b, ImGui::GetColorU32(kBtnOut), (b.y - a.y) * 0.5f, 0, 1.f);
    const float r = (b.y - a.y) * 0.5f - 3.f;
    const float cx = *v ? (b.x - 3.f - r) : (a.x + 3.f + r);
    dl->AddCircleFilled({cx, (a.y + b.y) * 0.5f}, r,
                        ImGui::GetColorU32(*v ? ImVec4(0.05f, 0.05f, 0.07f, 1.f)
                                              : ImVec4(0.42f, 0.44f, 0.50f, 1.f)));
    return clicked;
}

static bool toggle(const char* label, bool* v) {
    ImGui::PushID(label);
    const bool clicked = toggleBare("##tg", v);
    ImGui::SameLine(0.f, 12.f);
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(label);
    ImGui::PopID();
    return clicked;
}

// item rail (gaya PenaWare): kotak membulat 13px dengan glyph di tengah,
// item aktif dapat latar biru transparan + batang aksen di kiri. Label penuh
// muncul sebagai tooltip waktu di-hover.
static bool navItem(const char* glyph, const char* label, bool active, float size) {
    const ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton(label, {size, size});
    const bool hov = ImGui::IsItemHovered();
    const bool clk = ImGui::IsItemClicked();

    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 a = {p.x, p.y};
    const ImVec2 b = {p.x + size, p.y + size};
    if (active)   dl->AddRectFilled(a, b, ImGui::GetColorU32(kA(0.16f)), 13.f);
    else if (hov) dl->AddRectFilled(a, b, ImGui::GetColorU32(kBtnHov), 13.f);
    if (active)
        dl->AddRectFilled({a.x + 3.f, a.y + size * 0.22f}, {a.x + 6.f, b.y - size * 0.22f},
                          ImGui::GetColorU32(kAccent), 3.f);
    const ImVec2 ts = ImGui::CalcTextSize(glyph);
    dl->AddText({a.x + (size - ts.x) * 0.5f, a.y + (size - ts.y) * 0.5f},
                ImGui::GetColorU32(active ? kAccent : (hov ? kTextHi : kDim)), glyph);
    if (hov) ImGui::SetTooltip("%s", label);
    return clk;
}

// tombol kecil membulat gaya header (chip)
static bool chip(const char* label, const ImVec4& col = kBtn, const ImVec4& txt = kTextHi) {
    const ImVec2 pad = {18.f, 10.f};
    const ImVec2 sz  = ImGui::CalcTextSize(label);
    ImGui::PushStyleColor(ImGuiCol_Button, col);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, {col.x + 0.03f, col.y + 0.03f, col.z + 0.04f, 1.f});
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, kBtnAct);
    ImGui::PushStyleColor(ImGuiCol_Text, txt);
    const bool r = ImGui::Button(label, {sz.x + pad.x, sz.y + pad.y});
    ImGui::PopStyleColor(4);
    return r;
}

// Judul seksi gaya kartu: batang aksen + judul HURUF BESAR + garis tipis
static void section(const char* text) {
    ImGui::Dummy({0, 10});
    const ImVec2 p = ImGui::GetCursorScreenPos();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float th = ImGui::GetTextLineHeight();
    const ImVec2 ts = ImGui::CalcTextSize(text);
    dl->AddRectFilled({p.x, p.y + 3}, {p.x + 3, p.y + th - 3}, ImGui::GetColorU32(kAccent), 2.f);
    dl->AddText({p.x + 12.f, p.y}, ImGui::GetColorU32(kTextHi), text);
    const float lineX = p.x + 12.f + ts.x + 14.f;
    const float lineEnd = ImGui::GetWindowPos().x + ImGui::GetWindowSize().x - 24.f;
    if (lineEnd > lineX)
        dl->AddLine({lineX, p.y + th * 0.5f}, {lineEnd, p.y + th * 0.5f},
                    ImGui::GetColorU32(kLine), 1.f);
    ImGui::Dummy({0, th + 2});
}

// Tombol aksi utama (aksen, bukan abu-abu seperti tombol biasa)
static bool accentButton(const char* label, const ImVec2& size = {0, 0},
                         const ImVec4& col = kAccent) {
    ImGui::PushStyleColor(ImGuiCol_Button,        {col.x, col.y, col.z, 0.16f});
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, {col.x, col.y, col.z, 0.30f});
    ImGui::PushStyleColor(ImGuiCol_ButtonActive,  {col.x, col.y, col.z, 0.44f});
    ImGui::PushStyleColor(ImGuiCol_Text, col);
    ImGui::PushStyleColor(ImGuiCol_Border, {col.x, col.y, col.z, 0.45f});
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.f);
    const bool r = ImGui::Button(label, size);
    ImGui::PopStyleVar();
    ImGui::PopStyleColor(5);
    return r;
}

// label kecil berwarna (buat nunjukin status singkat)
static void badge(const char* text, const ImVec4& col) {
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const ImVec2 sz = ImGui::CalcTextSize(text);
    ImGui::GetWindowDrawList()->AddRectFilled({p.x - 9, p.y - 3},
                                              {p.x + sz.x + 9, p.y + sz.y + 3},
                                              ImGui::GetColorU32({col.x, col.y, col.z, 0.14f}), 8.f);
    ImGui::TextColored(col, "%s", text);
}

// label : value (label redup, nilainya terang) -- dipakai di kartu status
static void kv(const char* k, const char* fmt, ...) {
    ImGui::TextColored(kMuted, "%s", k);
    ImGui::SameLine(0.f, 8.f);
    va_list ap;
    va_start(ap, fmt);
    ImGui::TextV(fmt, ap);
    va_end(ap);
}

void setPage(int n) { g_page = n < 1 ? 1 : (n > 7 ? 7 : n); }
int  page() { return g_page; }
void setCycle(bool on, float seconds) {
    g_cycle = on;
    if (seconds > 0.2f) g_cycleSec = seconds;
}

void init() {
    ImGuiStyle& s = ImGui::GetStyle();
    // Geometri PenaWare: sudut panel 13px, kontrol lebih kecil (frame 8/grab 8),
    // garis 1px, padding lega. (nilai eksplisit menang atas ScaleAllSizes 1.5f)
    s.WindowRounding      = 13.0f;
    s.ChildRounding       = 13.0f;
    s.FrameRounding       = 8.0f;
    s.GrabRounding        = 8.0f;
    s.PopupRounding       = 13.0f;
    s.TabRounding         = 8.0f;
    // Scrollbar gede biar gampang di-drag pake jari: track transparan,
    // grab pill tebal yang di-inset dikit dari tepi.
    s.ScrollbarRounding   = 8.0f;
    s.ScrollbarSize       = 22.0f;
    s.ScrollbarPadding    = 3.0f;
    s.WindowBorderSize    = 1.0f;
    s.ChildBorderSize     = 1.0f;
    s.FrameBorderSize     = 0.0f;
    s.PopupBorderSize     = 1.0f;
    s.SeparatorSize       = 1.0f;
    s.DisabledAlpha       = 0.45f;
    s.WindowPadding       = {16, 14};
    s.FramePadding        = {12, 9};
    s.ItemSpacing         = {10, 8};
    s.ItemInnerSpacing    = {8, 6};
    s.CellPadding         = {8, 6};
    s.GrabMinSize         = 30.0f;

    // semua warna ambil dari palet di atas -- bukan angka mentah lagi
    ImVec4* c = s.Colors;
    c[ImGuiCol_Text]                  = kTextHi;
    c[ImGuiCol_TextDisabled]          = kMuted;
    c[ImGuiCol_WindowBg]              = {kBg.x, kBg.y, kBg.z, 0.98f};
    c[ImGuiCol_ChildBg]               = {kCard.x, kCard.y, kCard.z, 0.94f};
    c[ImGuiCol_PopupBg]               = {kCardCap.x, kCardCap.y, kCardCap.z, 0.98f};
    c[ImGuiCol_Border]                = kLine;
    c[ImGuiCol_BorderShadow]          = {0.f, 0.f, 0.f, 0.f};
    c[ImGuiCol_FrameBg]               = kBtn;
    c[ImGuiCol_FrameBgHovered]        = kBtnHov;
    c[ImGuiCol_FrameBgActive]         = kBtnAct;
    c[ImGuiCol_Button]                = kBtn;
    c[ImGuiCol_ButtonHovered]         = kBtnHov;
    c[ImGuiCol_ButtonActive]          = kBtnAct;
    c[ImGuiCol_Header]                = kBtnHov;
    c[ImGuiCol_HeaderHovered]         = kBtnAct;
    c[ImGuiCol_HeaderActive]          = kA(0.30f);
    c[ImGuiCol_Separator]             = kLine;
    c[ImGuiCol_SeparatorHovered]      = kA(0.60f);
    c[ImGuiCol_SeparatorActive]       = kAccent;
    c[ImGuiCol_CheckMark]             = kAccent;
    c[ImGuiCol_SliderGrab]            = kAccent;
    c[ImGuiCol_SliderGrabActive]      = kAccent;
    c[ImGuiCol_ScrollbarBg]           = {0.f, 0.f, 0.f, 0.0f};
    c[ImGuiCol_ScrollbarGrab]         = {0.42f, 0.44f, 0.50f, 0.55f};
    c[ImGuiCol_ScrollbarGrabHovered]  = {0.55f, 0.58f, 0.64f, 0.75f};
    c[ImGuiCol_ScrollbarGrabActive]   = kA(0.90f);
    c[ImGuiCol_PlotHistogram]         = kAccent;
    c[ImGuiCol_PlotHistogramHovered]  = kAccent2;
    c[ImGuiCol_TableHeaderBg]         = kCardCap;
    c[ImGuiCol_TableBorderStrong]     = kLine;
    c[ImGuiCol_TableBorderLight]      = {kLine.x, kLine.y, kLine.z, 0.60f};
    c[ImGuiCol_TableRowBgAlt]         = {1.f, 1.f, 1.f, 0.015f};
    c[ImGuiCol_ModalWindowDimBg]      = {0.f, 0.f, 0.f, 0.72f};

    snprintf(g_dirBuf, sizeof(g_dirBuf), "%s", target::customDumpDir().c_str());
    syncTargetBuffers();
}

// ---------------------------------------------------------------------------
// tab: TARGET
// ---------------------------------------------------------------------------
static void tabTarget() {
    section("TARGET GAME");
    ImGui::TextDisabled("Satu biner buat SEMUA game Unity/il2cpp.");
    ImGui::TextDisabled("Isi nama paket + lib il2cpp-nya (lib boleh dikosongkan = auto).");

    ImGui::Separator();
    ImGui::Text("package:");
    ImGui::SetNextItemWidth(330);
    if (kbField("pkg", KB_TEXT, g_pkgBuf, 330)) g_cfgTouched = true;
    ImGui::SameLine();
    if (ImGui::Button("Daftar...##pkg")) { g_pickFilter[0] = 0; g_pkgPickOpen = true;
                                      if (!g_procsLoaded) {
                                          g_procs = target::listProcesses();
                                          g_procsLoaded = true;
                                      } }
    ImGui::Text("lib il2cpp:");
    ImGui::SetNextItemWidth(330);
    if (kbField("lib", KB_TEXT, g_libBuf, 330)) g_cfgTouched = true;
    ImGui::SameLine();
    if (ImGui::Button("Daftar...##lib")) { g_pickFilter[0] = 0; collectLibs();
                                      g_libPickOpen = true; }
    ImGui::TextDisabled("MLBB: liblogic.so + libfirst.so + libcsharp.so (metadata dipecah 3).");
    ImGui::TextDisabled("Unity umum / FF: libil2cpp.so.  Anti-cheat FF: libanogs.so.");

    ImGui::Text("pid (0 = auto):");
    ImGui::SetNextItemWidth(120);
    if (kbField("pid", KB_INT, &g_pidSel, 120)) g_cfgTouched = true;
    ImGui::SameLine();
    ImGui::TextDisabled("(auto = pilih proses yang paling punya lib il2cpp)");
    if (g_pidSel > 0) {
        ImGui::SameLine();
        if (ImGui::SmallButton("x")) { g_pidSel = 0; g_cfgTouched = true; }
    }

    if (ImGui::Button("Simpan + Attach", {180, 0})) {
        applyTarget();
        attachNow();
        g_cfgTouched = false;
    }
    ImGui::SameLine();
    if (ImGui::Button("Attach ulang")) attachNow();
    ImGui::SameLine();
    if (ImGui::Button("Auto lib")) {
        const std::string l = target::detectLib(true);
        if (!l.empty()) {
            snprintf(g_libBuf, sizeof(g_libBuf), "%s", l.c_str());
            target::setLib(l);
            target::save();
            attachNow();
        }
    }

    ImGui::Separator();
    section("STATUS");
    ImGui::Text("attached : %s", mem::attached() ? "YA" : "belum");
    ImGui::Text("pid      : %d%s", mem::pid(),
                mem::attached() && mem::il2cppScore(mem::pid()) <= 0
                    ? "  (lib il2cpp belum ke-load di sini)" : "");
    ImGui::Text("package  : %s", target::package().c_str());
    ImGui::Text("engine   : %s", target::engine().empty() ? "(belum ketemu)" : target::engine().c_str());
    ImGui::Text("lib      : %s", il2::libName().empty() ? "(belum)" : il2::libName().c_str());
    ImGui::Text("lib base : 0x%lx", (unsigned long)il2::base());
    ImGui::Text("cfg      : %s", target::configPath().c_str());
    ImGui::Text("region   : %zu", mem::regions().size());

    // ---- dump offline: tabel class-slot. Ini yang bikin kelas di game
    //      ber-metadata-pecah (MLBB) ketemu, karena scan nama class mentok.
    ImGui::Separator();
    section("DUMP OFFLINE (.cs)");
    if (dump::loaded()) {
        ImGui::Text("file  : %s", dump::path().c_str());
        ImGui::Text("kelas : %zu  (punya class-slot: %zu)", dump::classCount(), dump::slotCount());
        ImGui::TextDisabled("module: %s", dump::moduleSummary().c_str());
    } else {
        ImGui::TextDisabled("belum dimuat. Tool nyari sendiri di:");
        ImGui::TextDisabled("  %s/<package>/<versi>/*.cs", target::dumpDir().c_str());
        ImGui::TextDisabled("  /data/local/tmp/panxcz_dump.cs  atau  /sdcard/panxcz/dump.cs");
    }
    ImGui::Text("path :");
    ImGui::SetNextItemWidth(330);
    ImGui::InputText("##dumppath", g_dumpBuf, sizeof(g_dumpBuf));
    ImGui::SameLine();
    if (g_dumpBusy) {
        ImGui::TextColored({1, .8f, .3f, 1}, "baca...");
    } else if (g_dumpDone == 2) {
        ImGui::TextColored({.4f, 1, .6f, 1}, "OK (%zu kelas)", dump::classCount());
    } else if (g_dumpDone == 1) {
        ImGui::TextColored({1, .5f, .5f, 1}, "gagal: %s", g_dumpMsg.c_str());
    }
    if (ImGui::Button("Muat dump"))      startDumpLoad();
    ImGui::SameLine();
    if (ImGui::Button("Cari sendiri")) { g_dumpBuf[0] = 0; startDumpLoad(); }
    ImGui::SameLine();
    if (ImGui::Button("Lepas")) { dump::unload(); g_dumpDone = 0; }
    ImGui::TextDisabled("Dump cuma katalog alamat -- isinya tetap divalidasi runtime.");

    ImGui::Separator();
    if (ImGui::Button("Cari proses yang jalan")) {
        g_procs = target::listProcesses();
        g_procsLoaded = true;
    }
    ImGui::SameLine();
    ImGui::TextDisabled("%zu proses", g_procs.size());
    if (g_procsLoaded) {
        ImGui::BeginChild("##procs", ImVec2(0, 190), true);
        for (size_t i = 0; i < g_procs.size(); ++i) {
            char lbl[200];
            // tanda "*" = proses ini punya bukti lib il2cpp di maps-nya
            snprintf(lbl, sizeof(lbl), "%s %-6d  %s",
                     g_procs[i].score >= 40 ? "*" : " ", g_procs[i].pid,
                     g_procs[i].pkg.c_str());
            const bool sel = (target::selPid() ? target::selPid() == g_procs[i].pid
                                               : target::package() == g_procs[i].pkg);
            if (ImGui::Selectable(lbl, sel)) {
                snprintf(g_pkgBuf, sizeof(g_pkgBuf), "%s", g_procs[i].pkg.c_str());
                g_cfgTouched = true;
                target::setPackage(g_procs[i].pkg);
                // pilih pid persis yang di-klik (bukan pid pertama package-nya)
                target::setSelPid(g_procs[i].pid);
                applyTarget();
                attachNow();
                g_cfgTouched = false;
                g_pidSel = target::selPid();
            }
        }
        ImGui::EndChild();
    }
    ImGui::TextDisabled("Tip: panxcz_libtool.sh <package> [lib] [--pid N] -> langsung attach.");
    ImGui::TextDisabled("Tanda * = proses itu punya lib engine (klik barisnya kalau auto salah pid).");
}

// ---------------------------------------------------------------------------
// tab: CLASSES
// ---------------------------------------------------------------------------
// ---------------------------------------------------------------------------
// Popup "force return value" (mirip tab Patcher punya tool in-process):
//   pilih tipe -> atur nilainya (tanpa keyboard: tombol +/-) -> Apply.
// Yang dipasang itu stub "langsung balikin nilai" di awal kode method-nya.
// ---------------------------------------------------------------------------
static explore::Method g_forceUi;      // method yang lagi dipilih
static int           g_forceType = 1;  // index tipe (1 = bool)
static double        g_forceVal  = 1.0;

static void forceReturnPopup() {
    if (!ImGui::BeginPopup("forceret")) return;
    ImGui::TextDisabled("method: %s", g_forceUi.name.c_str());
    ImGui::Text("kode @ 0x%lx", (unsigned long)g_forceUi.ptr);
    if (!g_forceUi.module.empty()) ImGui::TextDisabled("module: %s (rva 0x%llX)",
                                                       g_forceUi.module.c_str(),
                                                       (unsigned long long)g_forceUi.rva);
    ImGui::Separator();

    static const struct { const char* n; patch::Ret r; } kT[] = {
        {"void (method-nya dimatiin)", patch::Ret::Void},
        {"bool", patch::Ret::Bool},
        {"int32", patch::Ret::I32},
        {"uint32", patch::Ret::U32},
        {"int64", patch::Ret::I64},
        {"float", patch::Ret::Float},
        {"double", patch::Ret::Double},
        {"object/string (0 = null)", patch::Ret::Ptr},
    };
    for (int i = 0; i < (int)(sizeof(kT) / sizeof(kT[0])); ++i) {
        if (ImGui::RadioButton(kT[i].n, g_forceType == i)) g_forceType = i;
    }
    const patch::Ret kind = kT[g_forceType].r;

    if (kind == patch::Ret::Bool) {
        if (ImGui::Button("true", {90, 0}))  g_forceVal = 1.0;
        ImGui::SameLine();
        if (ImGui::Button("false", {90, 0})) g_forceVal = 0.0;
    } else if (kind != patch::Ret::Void && kind != patch::Ret::Ptr) {
        ImGui::Text("nilai: %g", g_forceVal);
        const bool flt = (kind == patch::Ret::Float || kind == patch::Ret::Double);
        const double step = flt ? 0.5 : 1.0;
        if (ImGui::Button("-10", {52, 0})) g_forceVal -= step * 10;
        ImGui::SameLine(); if (ImGui::Button("-1", {52, 0})) g_forceVal -= step;
        ImGui::SameLine(); if (ImGui::Button("+1", {52, 0})) g_forceVal += step;
        ImGui::SameLine(); if (ImGui::Button("+10", {52, 0})) g_forceVal += step * 10;
        if (ImGui::Button("x2", {52, 0})) g_forceVal *= 2;
        ImGui::SameLine(); if (ImGui::Button("/2", {52, 0})) g_forceVal /= 2;
        ImGui::SameLine(); if (ImGui::Button("=0", {52, 0})) g_forceVal = 0;
        ImGui::SameLine(); if (ImGui::Button("=1", {52, 0})) g_forceVal = 1;
    }

    ImGui::Separator();
    if (ImGui::Button("APPLY", {120, 0})) {
        std::string err;
        if (patch::forceReturn(g_forceUi.ptr, kind, g_forceVal, 0, g_forceUi.name,
                               g_forceUi.module, g_forceUi.rva, &err)) {
            logx::add("[force] UI: %s <- %s (tipe %d)", g_forceUi.name.c_str(),
                      patch::retName(kind), g_forceType);
        } else {
            logx::add("[force] UI GAGAL %s: %s", g_forceUi.name.c_str(), err.c_str());
        }
        ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button("BALIKIN", {120, 0})) {
        patch::unforceReturn(g_forceUi.ptr);
        ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button("Tutup", {80, 0})) ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
}

static void tabClasses() {
    section("CLASS EXPLORER (il2cpp)");
    ImGui::TextDisabled("Ketik nama class-nya. Offset field + RVA method dibaca dari");
    ImGui::TextDisabled("metadata game yang lagi jalan, jadi nggak perlu dump.");

    ImGui::Text("class :");
    kbField("clsname", KB_TEXT, g_clsName, 240);
    ImGui::Text("ns    :");
    kbField("clsns", KB_TEXT, g_clsNs, 200);

    if (ImGui::Button("Cari class", {140, 0})) startResolve();
    ImGui::SameLine();
    if (g_resolving) ImGui::TextColored({1, .8f, .3f, 1}, "scanning...");
    else if (g_resDone == 2) ImGui::TextColored({.4f, 1, .6f, 1}, "ketemu");
    else if (g_resDone == 1) ImGui::TextColored({1, .5f, .5f, 1}, "nggak ketemu");

    if (!g_hist.empty()) {
        ImGui::SameLine();
        if (ImGui::Button("riwayat")) ImGui::OpenPopup("##hist");
        if (ImGui::BeginPopup("##hist")) {
            for (size_t i = 0; i < g_hist.size(); ++i) {
                if (ImGui::Selectable(g_hist[i].c_str())) {
                    const std::string& h = g_hist[i];
                    size_t sep = h.find("::");
                    if (sep == std::string::npos) {
                        snprintf(g_clsNs, sizeof(g_clsNs), "%s", "");
                        snprintf(g_clsName, sizeof(g_clsName), "%s", h.c_str());
                    } else {
                        snprintf(g_clsNs, sizeof(g_clsNs), "%s", h.substr(0, sep).c_str());
                        snprintf(g_clsName, sizeof(g_clsName), "%s", h.substr(sep + 2).c_str());
                    }
                    startResolve();
                }
            }
            ImGui::EndPopup();
        }
    }

    explore::KlassInfo& K = explore::info();
    if (!K.ok) {
        ImGui::Separator();
        ImGui::TextDisabled("(belum ada class yang di-resolve)");
        ImGui::TextDisabled("contoh nama yang umum ada di game Unity:");
        const char* sample[] = {"PlayerData", "GameManager", "Player", "Hero", "Monster",
                                "RunSessionData", "LocalUser", "SceneManager", "Time"};
        for (int i = 0; i < 9; ++i) {
            if (i) ImGui::SameLine();
            if (ImGui::SmallButton(sample[i])) {
                snprintf(g_clsName, sizeof(g_clsName), "%s", sample[i]);
                g_clsNs[0] = 0;
                startResolve();
            }
        }
        return;
    }

    ImGui::Separator();
    ImGui::TextColored({1, .85f, .45f, 1}, "%s%s%s  @0x%lx",
                       K.ns.c_str(), K.ns.empty() ? "" : ".", K.name.c_str(),
                       (unsigned long)K.addr);
    ImGui::Text("assembly : %s", K.image.c_str());
    ImGui::TextColored({.5f, .9f, 1, 1}, "sumber   : %s%s", K.source.c_str(),
                       K.fromDump ? "  (field/method dari dump)" : "");
    if (!K.dumpModule.empty())
        ImGui::Text("module   : %s", K.dumpModule.c_str());
    if (K.statics)
        ImGui::Text("statics  : 0x%lx (static_fields +0x%X)", (unsigned long)K.statics,
                    K.staticsPtrOff);
    ImGui::Text("fields   : %zu (ptr+0x%X stride 0x%X slot 0x%X)",
                K.fields.size(), K.fieldsPtrOff, K.fieldStride, K.fieldOffSlot);
    ImGui::Text("methods  : %zu (ptr+0x%X nameOfs 0x%X)",
                K.methods.size(), K.methodsPtrOff, K.methodNameOff);

    if (ImGui::Button("Cari instance", {150, 0})) findInstances();
    ImGui::SameLine();
    if (ImGui::Button("Refresh nilai")) refreshValues();
    ImGui::SameLine();
    if (ImGui::Button("Dump class")) explore::dumpTo("/sdcard/panxcz/libtool");
    ImGui::SameLine();
    ImGui::Checkbox("HUD", &g_hud);

    if (!g_inst.empty()) {
        ImGui::Text("instance : %d/%zu", g_instSel + 1, g_inst.size());
        ImGui::SameLine();
        if (ImGui::SmallButton("<")) { g_instSel = (g_instSel + (int)g_inst.size() - 1) % (int)g_inst.size(); refreshValues(); }
        ImGui::SameLine();
        if (ImGui::SmallButton(">")) { g_instSel = (g_instSel + 1) % (int)g_inst.size(); refreshValues(); }
        ImGui::SameLine();
        ImGui::TextDisabled("obj 0x%lx", (unsigned long)g_inst[g_instSel]);
    } else {
        ImGui::TextDisabled("instance : (tekan 'Cari instance')");
    }

    if (K.fields.empty()) ImGui::TextDisabled("(class ini nggak punya field / kalibrasi gagal)");
    else {
        ImGui::BeginChild("##fields", ImVec2(0, 300), true);
        static const char* kTypes[] = {"ptr", "int", "uint", "long", "float", "double", "bool", "string"};
        for (size_t i = 0; i < K.fields.size(); ++i) {
            const explore::Field& f = K.fields[i];
            ImGui::PushID((int)i);
            ImGui::AlignTextToFramePadding();
            ImGui::Text("%-26s", f.name.c_str());
            if (!f.typeName.empty()) {
                ImGui::SameLine(150);
                ImGui::TextDisabled("%.14s", f.typeName.c_str());
            }
            ImGui::SameLine(250);
            if (f.isStatic) ImGui::TextDisabled("+0x%03X S", f.offset);
            else            ImGui::TextDisabled("+0x%03X", f.offset);
            ImGui::SameLine(320);
            ImGui::SetNextItemWidth(96);
            if (ImGui::Combo("##t", &g_ftype[i], kTypes, 8)) refreshValues();
            ImGui::SameLine();
            // field statis nilaiya dari static_fields, jadi nggak butuh instance
            if (baseFor(f) != 0) {
                if (ftypeOf((int)i) == explore::FType::String)
                    ImGui::Text("\"%s\"", i < g_fvalStr.size() ? g_fvalStr[i].c_str() : "");
                else
                    ImGui::Text("%.3f  (0x%llX)", i < g_fvalNum.size() ? g_fvalNum[i] : 0.0,
                                (unsigned long long)(i < g_fvalNum.size() ? (uint64_t)g_fvalNum[i] : 0));
                ImGui::SameLine();
                if (ImGui::SmallButton("edit")) {
                    g_editField = (int)i;
                    if (ftypeOf((int)i) == explore::FType::String)
                        snprintf(g_editBuf, sizeof(g_editBuf), "%s",
                                 i < g_fvalStr.size() ? g_fvalStr[i].c_str() : "");
                    else
                        snprintf(g_editBuf, sizeof(g_editBuf), "%.6g",
                                 i < g_fvalNum.size() ? g_fvalNum[i] : 0.0);
                }
                if (g_editField == (int)i) {
                    ImGui::SameLine();
                    kbField("editf", KB_TEXT, g_editBuf, 120);
                    if (kbTook(g_editBuf)) {
                        const double num = atof(g_editBuf);
                        const bool ok = explore::writeValue(baseFor(f), f.offset,
                                                            ftypeOf((int)i), num, g_editBuf);
                        logx::add("[ui] tulis %s +0x%X = %s -> %s", K.name.c_str(), f.offset,
                                  g_editBuf, ok ? "OK" : "GAGAL");
                        g_editField = -1;
                        refreshValues();
                    }
                }
            }
            ImGui::PopID();
        }
        ImGui::EndChild();
    }

    if (!K.methods.empty()) {
        ImGui::TextDisabled("methods (RVA relatif module pemilik kode; tag [lib] = "
                            "kodenya di lib lain):", il2::libName().c_str());
        ImGui::SameLine();
        ImGui::Text("  force-return: %zu", patch::forcedCount());
        ImGui::BeginChild("##methods", ImVec2(0, 150), true);
        for (size_t i = 0; i < K.methods.size() && i < 400; ++i) {
            // RVA-nya relatif module pemilik kode; kalau module-nya bukan lib
            // yang lagi dipakai tool, tandanya ditulis biar nggak salah patch.
            const bool other = !K.methods[i].module.empty() &&
                               K.methods[i].module != il2::libName();
            ImGui::PushID((int)i);
            std::string forced;
            const bool isForced = patch::isForced(K.methods[i].ptr, &forced);
            if (isForced)
                ImGui::TextColored(kOk, "0x%08llX  %s  -> %s",
                                   (unsigned long long)K.methods[i].rva,
                                   K.methods[i].name.c_str(), forced.c_str());
            else
                ImGui::Text("0x%08llX  %s%s", (unsigned long long)K.methods[i].rva,
                            K.methods[i].name.c_str(),
                            other ? ("   [" + K.methods[i].module + "]").c_str() : "");
            ImGui::SameLine();
            if (isForced) {
                if (ImGui::SmallButton("balikin")) patch::unforceReturn(K.methods[i].ptr);
            } else {
                if (ImGui::SmallButton("ret")) {
                    g_forceUi = K.methods[i];
                    ImGui::OpenPopup("forceret");
                }
            }
            ImGui::PopID();
        }
        ImGui::EndChild();
        forceReturnPopup();
    }
}

// ---------------------------------------------------------------------------
// tab: PATCH
// ---------------------------------------------------------------------------
static void tabPatch() {
    if (!g_patchFileInit) {
        g_patchFileInit = true;
        snprintf(g_patchFile, sizeof(g_patchFile), "%s", patch::defaultPath().c_str());
        if (!g_patchMod[0] && !il2::libName().empty())
            snprintf(g_patchMod, sizeof(g_patchMod), "%s", il2::libName().c_str());
    }

    // ---- status + auto-apply ----
    section("RUNTIME PATCH (tulis byte ke memori game yang lagi jalan)");
    ImGui::Text("proses : %s (pid %d)", mem::procName().c_str(), mem::pid());
    ImGui::Text("lib    : %s  base 0x%lx", il2::libName().c_str(), (unsigned long)il2::base());
    ImGui::Text("status : %s", patch::summary().c_str());
    bool autoOn = patch::enabled();
    if (ImGui::Checkbox("auto-apply tiap tool jalan (file di bawah)", &autoOn))
        patch::setEnabled(autoOn);
    ImGui::TextDisabled("klik patch kapan aja bisa; yang ON bakal dipasang ulang sendiri "
                        "kalau byte-nya ketimpa game.");

    // ---- patch byte (module + RVA / alamat mutlak) ----
    section("PATCH BYTE");
    ImGui::Text("module:");
    kbField("pmod", KB_TEXT, g_patchMod, 240);
    ImGui::TextDisabled("contoh: libil2cpp.so / libanogs.so / (kosong = lib il2cpp di atas)");
    ImGui::Text("rva   :");
    kbField("prva", KB_TEXT, g_patchRva, 150);
    ImGui::SameLine();
    ImGui::Checkbox("alamat mutlak", &g_patchAbs);
    ImGui::Text("byte  :");
    kbField("phex", KB_TEXT, g_patchHex, 300);
    ImGui::TextDisabled("contoh: 'C0 03 5F D6' (ret) atau '1F 20 03 D5' (nop)");
    if (ImGui::Button("Tambah + apply", {170, 0})) patchAddByte();
    ImGui::SameLine();
    if (ImGui::Button("Verifikasi semua", {150, 0})) {
        const bool ok = patch::applyAll();
        patchNoteSet(ok ? "semua entri OK" : "ada yang gagal -- cek log");
    }

    // ---- patch pakai pola ----
    section("PATCH PAKAI POLA (kalau RVA-nya geser tiap update)");
    ImGui::Text("pola  :");
    kbField("psig", KB_TEXT, g_patchSig, 300);
    ImGui::TextDisabled("wildcard '??' per-byte. contoh: 48 8B ?? E0 ?? 40 F9");
    if (ImGui::Button("Cari + apply", {150, 0})) patchAddPattern();
    ImGui::SameLine();
    if (ImGui::Button("Cari aja (tes)", {140, 0})) {
        const uintptr_t a = patch::findSigAddr(g_patchMod, g_patchSig);
        if (a) patchNoteSet("ketemu di 0x%lx", (unsigned long)a);
        else   patchNoteSet("pola nggak ketemu");
    }

    if (g_patchNote[0]) ImGui::TextColored({0.55f, 0.95f, 0.65f, 1}, "%s", g_patchNote);

    // ---- daftar ----
    section("DAFTAR PATCH");
    if (ImGui::Button("Apply semua", {130, 0})) patch::applyAll();
    ImGui::SameLine();
    if (ImGui::Button("Restore semua", {150, 0})) patch::restoreAll();
    ImGui::SameLine();
    if (ImGui::Button("Bersihin##patch", {110, 0})) patch::clear();
    ImGui::SameLine();
    if (ImGui::Button("Simpan ke file##patch", {150, 0})) {
        if (patch::saveFile(g_patchFile)) patchNoteSet("disimpan: %s", g_patchFile);
        else patchNoteSet("gagal simpan: %s", patch::lastError());
    }
    ImGui::SameLine();
    if (ImGui::Button("Muat dari file", {150, 0})) {
        if (patch::loadFile(g_patchFile, true)) patchNoteSet("dimuat + di-apply: %s", g_patchFile);
        else patchNoteSet("gagal muat: %s", patch::lastError());
    }
    ImGui::Text("file  :");
    kbField("pfle", KB_TEXT, g_patchFile, 420);

    ImGui::BeginChild("##patches", ImVec2(0, 260), true);
    auto& plist = patch::list();
    for (int i = 0; i < (int)plist.size(); ++i) {
        ImGui::PushID(i);
        bool on = plist[i].on;
        if (ImGui::Checkbox("##p", &on)) patch::setOn((size_t)i, on);
        ImGui::SameLine();
        ImGui::TextColored(on ? ImVec4(.45f, 1, .6f, 1) : ImVec4(.7f, .7f, .7f, 1),
                           "%s", on ? "ON " : "off");
        ImGui::SameLine();
        ImGui::Text("%s", plist[i].label.c_str());
        ImGui::SameLine();
        ImGui::TextDisabled("%s | %zu byte%s%s",
                            plist[i].sig.empty()
                                ? (plist[i].module.empty() ? "(pola)" : plist[i].module.c_str())
                                : "pola",
                            plist[i].bytes.size(),
                            plist[i].verified ? " | terverifikasi" : " | BELUM terverifikasi",
                            plist[i].reapplies ? " | dipasang ulang" : "");
        ImGui::SameLine();
        if (ImGui::Button("X", {28, 0})) patch::removeAt((size_t)i);
        ImGui::PopID();
    }
    if (plist.empty()) ImGui::TextDisabled("(belum ada patch)");
    ImGui::EndChild();
}

// ---------------------------------------------------------------------------
// tab: SCAN
// ---------------------------------------------------------------------------
// ---------------------------------------------------------------------------
// tab INFO: status + kontrol dump sendiri + keluar dari tool
// ---------------------------------------------------------------------------
static volatile bool g_poolBusy = false;
static pthread_t     g_poolTh;
static void* poolWorker(void*) {
    metapool::init();                 // hormati Stop (metapool nanya ke abortx)
    g_poolBusy = false;
    return nullptr;
}

// ---------------------------------------------------------------------------
// watchdog worker
//
// Kalau thread-nya mati / macet di tengah kerja, dulu g_ndBusy/g_poolBusy tetap
// true -> UI nampilin "Stop" terus tapi ngekliknya nggak ngefek apa-apa
// ("scan/dump nggak bisa di-stop"). Sekarang: kerja berat nge-denyut (abortx::
// beat), jadi kalau lama nggak ada denyut, UI lepas kuncinya sendiri + nulis
// alasannya ke log.
// ---------------------------------------------------------------------------
static const double kStallSec = 60.0;

static void releaseStalledDump() {
    if (!g_ndBusy || g_ndStage != 2) return;      // stage 1 = nunggu match (wajar diam)
    if (abortx::beatAgeSec() < kStallSec) return;
    logx::add("[ui] worker dump diam %.0f s (fase terakhir: '%s') -- kunci dilepas",
              abortx::beatAgeSec(), abortx::phase());
    g_ndMsg = "worker berhenti di tengah jalan -- cek logcat";
    g_ndStage = 4;
    g_ndBusy = false;
}

static void releaseStalledPool() {
    if (!g_poolBusy) return;
    if (!abortx::requested() && abortx::beatAgeSec() < kStallSec) return;
    // Permintaan batal dikirim 2 detik lalu tapi masih belum ada denyut baru
    // -> anggap thread-nya udah nggak kerja.
    logx::add("[ui] pool method diam %.0f s (fase terakhir: '%s') -- kunci dilepas",
              abortx::beatAgeSec(), abortx::phase());
    g_poolBusy = false;
}

// ---------------------------------------------------------------------------
// progress bar kerja berat
//
// Tiap kerja berat (dump / scan / pool method) nge-denyut + ngirim fraksi
// progres ke abortx. Di sini digambar jadi bar: persentase kalau tahapnya bisa
// diukur, atau bar "jalan-jalan" (indeterminate) kalau nggak -- plus fase yang
// lagi dikerjain dan sudah berapa detik. Ini yang bikin kelihatan bedanya
// "masih kerja" vs "macet".
// ---------------------------------------------------------------------------
static bool   g_progActive = false;
static double g_progStart  = 0.0;
static char   g_progLast[96] = {0};

static void drawWorkProgress(float width = 0.0f) {
    const double age   = abortx::beatAgeSec();
    const bool   fresh = age < 3.0;
    if (fresh && !g_progActive) {
        g_progActive = true;
        g_progStart  = abortx::nowSec();
    }
    if (!fresh) { g_progActive = false; return; }

    const float  frac = abortx::progress();
    const double el   = abortx::nowSec() - g_progStart;
    char overlay[128];
    if (frac >= 0.0f)
        snprintf(overlay, sizeof(overlay), "%.0f%%  |  %s  |  %.0f s",
                 frac * 100.0f, abortx::phase(), el);
    else
        snprintf(overlay, sizeof(overlay), "%s  |  %.0f s", abortx::phase(), el);
    snprintf(g_progLast, sizeof(g_progLast), "%s", overlay);

    // frac < 0 -> ImGui gambar bar jalan-jalan (nggak ngarang persentase)
    const float show = frac >= 0.0f
                           ? frac
                           : -1.0f * (float)fmod(abortx::nowSec() * 0.5, 1.0);
    ImGui::ProgressBar(show, ImVec2(width, 0), overlay);
}

static void tabInfo() {
    // watchdog dulu (lihat releaseStalledDump/Pool) -- biar tombol Stop nggak
    // pernah nyangkut walau worker-nya mati
    releaseStalledDump();
    releaseStalledPool();

    // ---- status (kartu) ----
    section("STATUS");
    ImGui::BeginChild("##st", {0, 0},
                      ImGuiChildFlags_Borders | ImGuiChildFlags_AutoResizeY);
    kv("attached", "%s", mem::attached() ? "YA" : "belum");
    kv("proses", "%s (%d)",
       mem::procName().empty() ? target::package().c_str() : mem::procName().c_str(),
       mem::pid());
    // Proses engine yang disimpan di cfg -- Unity sering naruh il2cpp di proses
    // anak (MLBB: com.mobile.legends:UnityKillsMe), bukan proses utamanya.
    kv("engine", "%s", target::engine().empty() ? "(belum ketemu)" : target::engine().c_str());
    kv("lib", "%s   base 0x%lx",
       il2::libName().empty() ? "(belum)" : il2::libName().c_str(),
       (unsigned long)il2::base());
    kv("dump offline", "%s", dump::loaded() ? dump::path().c_str() : "(belum dimuat)");
    if (dump::loaded() && dump::files().size() > 1)
        ImGui::TextDisabled("   digabung dari %zu file (nama kelas/method jadi lengkap)",
                            dump::files().size());
    kv("folder hasil", "%s", target::dumpDir().c_str());
    kv("baca memori gagal", "%zu kali  (%zu halaman jelek -- jalan = normal)",
       mem::readFailures(), mem::badPageCount());
    ImGui::EndChild();
    // Self-check: kalau dump/scan gagal di device lain, tombol ini nyetak
    // KEADAAN sebenarnya (root, izin /proc/pid/mem, jumlah region, apakah baca
    // blok 4 MB jalan) + verdict-nya ke log.
    if (ImGui::Button("Cek lingkungan (hasil di log)", {240, 0})) mem::diag("menu Info");

    // ---- pool method global (daftar method lengkap dari runtime) ----
    section("POOL METHOD (runtime)");
    if (metapool::ready()) {
        const auto& st = metapool::stats();
        ImGui::BeginChild("##pool", {0, 0},
                          ImGuiChildFlags_Borders | ImGuiChildFlags_AutoResizeY);
        kv("method", "%zu   (%zu punya nama)", st.entries, st.named);
        kv("kelas", "%zu", st.classes);
        kv("typeId", "%zu dari %zu tipe (token kelas +0x%x, field indeks +0x%x)",
           metapool::typeMapSize(), st.typeRecords,
           metapool::typeTokenOffset(), metapool::typeOffset());
        if (st.typeFromTable)
            kv("tabel tipe", "%zu tipe tanpa MethodInfo ikut kepetakan", st.typeFromTable);
        kv("dipindai", "%zu MB, %zu kandidat", st.scannedMB, st.cand);
        ImGui::EndChild();
        if (ImGui::TreeNode("detail kalibrasi nama")) {
            for (const auto& s : st.spaceInfo) ImGui::TextDisabled("%s", s.c_str());
            ImGui::TreePop();
        }
    } else if (g_poolBusy) {
        ImGui::TextColored(kWarn, "baca pool... (bisa 1 menit) | fase: %s",
                           abortx::phase());
    } else {
        ImGui::TextDisabled("belum dibaca -- dump bakal baca otomatis.");
    }
    if (!g_poolBusy && accentButton("Baca pool method", {180, 0}, kAccent2)) {
        g_poolBusy = true;
        abortx::clear();
        pthread_create(&g_poolTh, nullptr, poolWorker, nullptr);
        pthread_detach(g_poolTh);
    }
    if (g_poolBusy) {
        ImGui::SameLine();
        if (accentButton("Stop##pool", {90, 0}, kDanger)) abortx::request("Stop pool method");
    }
    ImGui::SameLine();
    ImGui::TextDisabled("(%s)", metapool::ready() ? "siap" : metapool::lastError());

    // ---- dump + scan ----
    section("DUMP / SCAN");
    ImGui::TextColored(kMuted, "folder hasil");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(320);
    kbField("dumpdir", KB_TEXT, g_dirBuf, 320);
    ImGui::SameLine();
    if (accentButton("Simpan##dumpdir", {90, 0}, kAccent2)) {
        target::setDumpDir(g_dirBuf);
        target::save();
    }
    ImGui::SameLine();
    if (ImGui::Button("Default##dumpdir", {90, 0})) {
        g_dirBuf[0] = 0;
        target::setDumpDir("");
        target::save();
    }
    ImGui::TextDisabled("kosong = %s/<package>/<versi>/", target::dumpDir().c_str());

    if (!g_ndBusy) {
        if (accentButton("Start scan", {130, 0}, kAccent2)) startNativeDump(false, true);
        ImGui::SameLine();
        if (accentButton("Start dump", {130, 0})) startNativeDump(false, false);
        ImGui::SameLine();
        if (accentButton("Online dump (di match)", {200, 0})) startNativeDump(true, false);
    } else {
        if (accentButton("Stop##dump", {130, 0}, kDanger)) dumpout::requestAbort();
        ImGui::SameLine();
        ImGui::TextColored(kWarn, "%s | fase: %s", g_ndMsg.c_str(), abortx::phase());
    }
    if (g_ndStage == 3)
        ImGui::TextColored(kOk, "%s", g_ndMsg.c_str());
    else if (g_ndStage == 4)
        ImGui::TextColored(kDanger, "%s", g_ndMsg.c_str());
    // progress bar khusus kerja ini (fase + persen + detik)
    if (g_ndBusy || abortx::beatAgeSec() < 3.0) drawWorkProgress();
    if (g_dirApplied[0])
        ImGui::TextDisabled("folder dipakai: %s", g_dirApplied);
    ImGui::TextDisabled("Start scan = hitung kelas/field/method aja (nggak nulis file).");
    ImGui::TextDisabled("Online dump = nunggu masuk match dulu, biar RVA method lengkap.");

    // ---- anti-deteksi ----
    section("STEALTH (anti-deteksi)");
    kv("nama proses", "%s", bypass::procName().c_str());
    kv("dilihat game", "%s", bypass::lastReport().c_str());
    if (ImGui::Checkbox("Sembunyikan jejak tool", &bypass::enabled()))
        bypass::apply();
    ImGui::SameLine();
    if (ImGui::Button("Terapkan ulang")) bypass::apply();
    ImGui::SameLine();
    if (ImGui::Button("Cek sekarang")) bypass::scanOnce(true);
    ImGui::TextDisabled("Tool lepas overlay + keluar sendiri kalau game kebukti ngecek kita.");

    // ---- keluar ----
    section("KELUAR DARI TOOL");
    ImGui::TextDisabled("Tutup overlay + lepas touch, game-nya tetap jalan.");
    if (accentButton("Keluar dari tool##adv", {200, 0}, kDanger)) requestExit();
}

// ---------------------------------------------------------------------------
// worker memory scanner
//
// Dulu "First Scan" dipanggil LANGSUNG dari fungsi gambar: satu scan (sampai
// ratusan MB) ngeblok render -- overlay beku, tombol apa pun nggak bisa
// diklik, dan Stop cuma bisa diangan-angankan. Sekarang scan jalan di thread
// sendiri, UI tetap hidup, dan Stop beneran ngehentiin (scan::requestAbort).
// ---------------------------------------------------------------------------
struct ScanJob {
    bool         first = true;
    scan::Type   type  = scan::Type::I32;
    double       value = 0;
    int          mode  = 0;
};
static ScanJob      g_scanJob;
static volatile int g_scanRun = 0;      // 0 idle, 1 jalan
static pthread_t    g_scanTh;
static char         g_scanMsg[160] = {0};

static void* scanWorker(void*) {
    const ScanJob job = g_scanJob;
    if (job.first) scan::first(job.type, job.value);
    else           scan::refine(job.mode, job.value);
    snprintf(g_scanMsg, sizeof(g_scanMsg), "%s selesai: %zu hasil",
             job.first ? "scan" : "refine", scan::count());
    g_scanRun = 0;
    return nullptr;
}

static void startScanJob(bool first, scan::Type t, double v, int mode) {
    if (g_scanRun) return;
    g_scanJob.first = first;
    g_scanJob.type  = t;
    g_scanJob.value = v;
    g_scanJob.mode  = mode;
    g_scanMsg[0] = 0;
    scan::clearAbort();
    g_scanRun = 1;
    pthread_create(&g_scanTh, nullptr, scanWorker, nullptr);
    pthread_detach(g_scanTh);
}

// worker scanner mati/macet? lepas kuncinya biar UI nggak nyangkut
static void releaseStalledScan() {
    if (!g_scanRun) return;
    if (scan::busy() && !scan::abortRequested()) return;
    if (scan::busy() && scan::abortRequested()) {
        if (abortx::beatAgeSec() < 5.0) return;     // masih ngerespon Stop
        logx::add("[ui] scanner nggak berhenti setelah Stop (denyut %.0f s)",
                  abortx::beatAgeSec());
    }
    g_scanRun = 0;
}

static void tabScan() {
    static int   typeIdx = 2;
    static char  valBuf[32] = "100";
    static int   refineMode = 0;
    static char  editBuf[32] = {0};
    static int   editIdx = -1;
    static bool  live = false;          // Start scan: nilai di-refresh terus

    releaseStalledScan();

    static const scan::Type types[] = {
        scan::Type::I8, scan::Type::I16, scan::Type::I32,
        scan::Type::I64, scan::Type::F32, scan::Type::F64
    };
    const char* typeNames[] = {"byte", "short", "int", "long", "float", "double"};

    section("MEMORY SCANNER");
    ImGui::SetNextItemWidth(120);
    ImGui::Combo("tipe", &typeIdx, typeNames, 6);
    ImGui::SameLine();
    ImGui::TextDisabled("value:");
    ImGui::SameLine();
    kbField("scanv", KB_TEXT, valBuf, 150.0f);

    if (!g_scanRun) {
        if (accentButton("First Scan", {130, 0}))
            startScanJob(true, types[typeIdx], strtod(valBuf, nullptr), 0);
    } else {
        if (accentButton("Stop scan##first", {130, 0}, kDanger)) scan::requestAbort();
        ImGui::SameLine();
        ImGui::TextColored(kWarn, "scan jalan... (fase: %s)", abortx::phase());
    }
    ImGui::SameLine();
    ImGui::SetNextItemWidth(130);
    const char* rmodes[] = {"exact", "changed", "unchanged", "increased", "decreased"};
    ImGui::Combo("##rm", &refineMode, rmodes, 5);
    ImGui::SameLine();
    if (accentButton("Refine", {90, 0}, kAccent2))
        startScanJob(false, types[typeIdx], strtod(valBuf, nullptr), refineMode);
    ImGui::SameLine();
    if (ImGui::Button("Clear", {80, 0})) { scan::clear(); live = false; }
    if (g_scanMsg[0]) ImGui::TextDisabled("%s", g_scanMsg);
    if (g_scanRun || abortx::beatAgeSec() < 3.0) drawWorkProgress();

    // Start/Stop scan: "scan" di sini = baca ULANG nilai hasil scan yang ada
    // (bukan pindai memori lagi), jadi nilainya kelihatan hidup -- berguna buat
    // nandain alamat yang berubah pas di-game.
    ImGui::SameLine();
    if (!live) {
        if (accentButton("Start scan##live", {120, 0}, kOk)) live = true;
    } else {
        if (accentButton("Stop scan##live", {120, 0}, kDanger)) live = false;
        ImGui::SameLine();
        ImGui::TextColored(kOk, "live");
    }
    if (live) scan::refreshValues(types[typeIdx], 512);

    ImGui::Text("hasil: %zu   frozen: %d", scan::count(), scan::frozenCount());

    ImGui::BeginChild("##hits", ImVec2(0, 300), true);
    const auto& hits = scan::hits();
    int shown = 0;
    for (size_t i = 0; i < hits.size() && shown < 400; ++i, ++shown) {
        ImGui::PushID((int)i);
        bool fr = hits[i].frozen;
        if (ImGui::Checkbox("##f", &fr)) scan::setFrozen(i, fr, hits[i].value);
        ImGui::SameLine();
        ImGui::Text("0x%lx  %.4f", (unsigned long)hits[i].addr, hits[i].value);
        ImGui::SameLine();
        if (ImGui::SmallButton("edit")) {
            editIdx = (int)i;
            snprintf(editBuf, sizeof(editBuf), "%.4f", hits[i].value);
        }
        if (editIdx == (int)i) {
            ImGui::SameLine();
            kbField("edithit", KB_TEXT, editBuf, 110.0f);
            ImGui::SameLine();
            if (ImGui::SmallButton("OK")) {
                scan::setValue(i, strtod(editBuf, nullptr));
                editIdx = -1;
            }
        }
        ImGui::PopID();
    }
    if (hits.empty()) ImGui::TextDisabled("(belum ada hasil)");
    ImGui::EndChild();
    ImGui::TextDisabled("Value di-scan di semua region writable. Freeze ditulis ulang tiap frame.");
}

// ---------------------------------------------------------------------------
// tab: TOUCH
// ---------------------------------------------------------------------------
static void tabTouch() {
    section("TOUCH HELPER / MACRO");
    ImGui::TextDisabled("Inject lewat uinput (butuh root + device touchscreen)");
    if (g_thMsg[0]) ImGui::TextColored({1, 0.8f, 0.4f, 1}, "%s", g_thMsg);

    if (!g_thStarted) {
        if (ImGui::Button("Aktifkan touch helper", {220, 0})) {
            startTouchWorker();
            snprintf(g_thMsg, sizeof(g_thMsg), "touch worker jalan");
        }
    } else {
        int m = g_macro;
        ImGui::Text("mode aktif: %s", m == 0 ? "OFF" : m == 1 ? "SWIPE UP" :
                    m == 2 ? "SWIPE DOWN" : m == 3 ? "SWIPE LEFT" :
                    m == 4 ? "SWIPE RIGHT" : "TAP");
        if (ImGui::Button("Off")) g_macro = 0;
        ImGui::SameLine(); if (ImGui::Button("Swipe Up"))    g_macro = 1;
        ImGui::SameLine(); if (ImGui::Button("Swipe Down"))  g_macro = 2;
        ImGui::SameLine(); if (ImGui::Button("Swipe Left"))  g_macro = 3;
        ImGui::SameLine(); if (ImGui::Button("Swipe Right")) g_macro = 4;
    }

    ImGui::Separator();
    section("TAP BERULANG");
    int ms = g_macroMs;
    if (ImGui::SliderInt("interval (ms)", &ms, 50, 2000)) g_macroMs = ms;
    float tx = g_tapX, ty = g_tapY;
    if (ImGui::SliderFloat("tap X", &tx, 0.0f, 1.0f, "%.2f")) g_tapX = tx;
    if (ImGui::SliderFloat("tap Y", &ty, 0.0f, 1.0f, "%.2f")) g_tapY = ty;
    if (ImGui::Button("Start Tap")) g_macro = 5;

    ImGui::Separator();
    ImGui::TextDisabled("Screen: %dx%d ori=%u", displayInfo.width, displayInfo.height,
                        displayInfo.orientation);
}

// ---------------------------------------------------------------------------
// tab: CONFIG
// ---------------------------------------------------------------------------
static void tabConfig() {
    section("TOOLS");
    bool lf = logx::fileEnabled();
    if (ImGui::Checkbox("Log ke file", &lf))
        logx::setFile(lf ? "/sdcard/panxcz/libtool.log" : nullptr);
    ImGui::SameLine();
    ImGui::Checkbox("Tampilkan panel log", &g_showLog);

    if (ImGui::Button("Re-attach + re-detect lib", {200, 0})) attachNow();
    ImGui::SameLine();
    if (ImGui::Button("Detect lib ulang", {150, 0})) {
        mem::refresh();
        const std::string l = target::detectLib(true);
        if (!l.empty()) {
            snprintf(g_libBuf, sizeof(g_libBuf), "%s", l.c_str());
            g_libNamesLoaded = false;
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Lapor ke logcat", {150, 0})) {
        logx::add("[cfg] target=%s lib=%s pid=%d", target::package().c_str(),
                  il2::libName().c_str(), mem::pid());
    }

    // ---- dump: folder + start/stop ----
    section("DUMP / SCAN");
    ImGui::TextColored(kMuted, "folder hasil (kosong = default)");
    ImGui::SetNextItemWidth(340);
    kbField("setdir", KB_TEXT, g_dirBuf, 340);
    ImGui::SameLine();
    if (accentButton("Simpan##setdir", {90, 0}, kAccent2)) {
        target::setDumpDir(g_dirBuf);
        target::save();
    }
    ImGui::SameLine();
    if (ImGui::Button("Default##setdir", {90, 0})) {
        g_dirBuf[0] = 0;
        target::setDumpDir("");
        target::save();
    }
    kv("dipakai", "%s", target::dumpDir().c_str());

    if (!g_ndBusy) {
        if (accentButton("Start scan##set", {130, 0}, kAccent2)) startNativeDump(false, true);
        ImGui::SameLine();
        if (accentButton("Start dump##set", {130, 0})) startNativeDump(false, false);
        ImGui::SameLine();
        if (accentButton("Online dump##set", {130, 0})) startNativeDump(true, false);
    } else {
        if (accentButton("Stop##set", {130, 0}, kDanger)) dumpout::requestAbort();
        ImGui::SameLine();
        ImGui::TextColored(kWarn, "%s", g_ndMsg.c_str());
    }
    if (g_ndStage == 3)      ImGui::TextColored(kOk, "%s", g_ndMsg.c_str());
    else if (g_ndStage == 4) ImGui::TextColored(kDanger, "%s", g_ndMsg.c_str());

    // ---- keluar ----
    section("KELUAR");
    ImGui::TextDisabled("Tutup tool + lepas overlay (game-nya tetap jalan).");
    if (accentButton("Keluar dari tool##set", {200, 0}, kDanger)) requestExit();

    section("TENTANG");
    ImGui::TextDisabled("PANXCZ LIBTOOL v1.0 - universal il2cpp external tool");
    ImGui::TextDisabled("by Panxcz & Freebuff  |  arm64-v8a");
    kv("package", "%s", target::package().c_str());
    kv("lib", "%s", il2::libName().c_str());
    kv("cfg", "%s", target::configPath().c_str());
    kv("scan budget", "%zu MB", mem::scanBudget() / 1048576);
}

// ---------------------------------------------------------------------------
// baris yang bisa melipat: dipakai buat baris chip/badge yang jumlahnya
// berubah-ubah (kalau jendelanya disempitin, item berikutnya turun baris --
// dulu baris status bisa numpuk/ketutup di layar kecil).
// ---------------------------------------------------------------------------
static void sameLineOrWrap(float w) {
    const float x = ImGui::GetItemRectMax().x - ImGui::GetWindowPos().x;
    const float right = ImGui::GetWindowWidth() - 14.f;
    if (x + 8.f + w <= right) ImGui::SameLine(0.f, 8.f);
    else                     ImGui::NewLine();
}

// ---------------------------------------------------------------------------
// log panel
// ---------------------------------------------------------------------------
static bool g_logAutoScroll = true;
static char g_logFilter[48] = {0};

static void drawLog() {
    ImGui::SetNextWindowSize({620, 320}, ImGuiCond_FirstUseEver);
    ImGui::Begin("Log", &g_showLog);
    const int n = logx::count();
    if (ImGui::Button("Bersihin##log")) logx::clear();
    ImGui::SameLine();
    if (ImGui::Button("Simpan ke file##log")) {
        const char* p = "/sdcard/panxcz-tools/libtool.log";
        logx::setFile(p);          // setFile = buka mode tulis (nge-truncate)
        for (int i = n - 1; i >= 0; --i) logx::add("%s", logx::line(i));
        logx::setFile(nullptr);    // balik ke mode cuma-layar
        ImGui::SetClipboardText(p);
    }
    ImGui::SameLine();
    ImGui::Checkbox("auto-scroll", &g_logAutoScroll);
    ImGui::SameLine();
    ImGui::TextDisabled("%d baris", n);
    ImGui::SetNextItemWidth(180);
    kbField("logfilter", KB_TEXT, g_logFilter, 180);
    ImGui::Separator();
    ImGui::BeginChild("##logc");
    const bool hasFilter = g_logFilter[0] != 0;
    std::string f;
    for (const char* q = g_logFilter; *q; ++q) f.push_back((char)tolower((unsigned char)*q));
    for (int i = 0; i < n; ++i) {
        const char* ln = logx::line(i);          // 0 = paling baru
        if (hasFilter) {
            std::string low = ln;
            for (auto& c : low) c = (char)tolower((unsigned char)c);
            if (low.find(f) == std::string::npos) continue;
        }
        ImGui::TextUnformatted(ln);
    }
    if (g_logAutoScroll) ImGui::SetScrollHereY(1.0f);
    ImGui::EndChild();
    ImGui::End();
}

// ---------------------------------------------------------------------------
// HUD
// ---------------------------------------------------------------------------
static void drawHud() {
    if (!g_hud) return;
    ImGui::SetNextWindowPos({12, 12}, ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(0.55f);
    ImGui::Begin("##hud", nullptr,
                 ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                 ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoMove |
                 ImGuiWindowFlags_NoInputs);
    ImGui::TextColored({0.55f, 0.9f, 1, 1}, "PANXCZ LIBTOOL");
    ImGui::Text("%s", target::package().empty() ? "(belum pilih game)" : target::package().c_str());
    ImGui::Text("lib: %s", il2::libName().empty() ? "-" : il2::libName().c_str());
    const explore::KlassInfo& K = explore::info();
    if (K.ok) ImGui::Text("class: %s (%zu field)", K.name.c_str(), K.fields.size());
    ImGui::End();
}

// ---------------------------------------------------------------------------
// menu utama
// ---------------------------------------------------------------------------
// ---------------------------------------------------------------------------
// pemilih package / lib (window terpisah, biar nggak perlu ngetik panjang)
// ---------------------------------------------------------------------------
static bool pickHit(const std::string& s) {
    if (!g_pickFilter[0]) return true;
    std::string f, l = s;
    for (const char* p = g_pickFilter; *p; ++p) f.push_back((char)tolower((unsigned char)*p));
    for (char& c : l) c = (char)tolower((unsigned char)c);
    return l.find(f) != std::string::npos;
}

// Ambil semua nama lib yang lagi dimuat proses target (buat daftar pilihan).
static void collectLibs() {
    g_libNames.clear();
    g_libPickSel = -1;
    for (const auto& r : mem::regions()) {
        if (r.name.empty() || r.name[0] != '/') continue;
        if (r.name.find(".so") == std::string::npos) continue;
        const size_t sl = r.name.find_last_of('/');
        std::string b = (sl == std::string::npos) ? r.name : r.name.substr(sl + 1);
        bool dup = false;
        for (const auto& x : g_libNames) if (x == b) { dup = true; break; }
        if (!dup) g_libNames.push_back(b);
    }
    std::sort(g_libNames.begin(), g_libNames.end());
    g_libNamesLoaded = true;
}

static void chooseTarget(const std::string& pkg, int pid) {
    snprintf(g_pkgBuf, sizeof(g_pkgBuf), "%s", pkg.c_str());
    g_cfgTouched = true;
    target::setPackage(pkg);
    target::setSelPid(pid);
    applyTarget();
    attachNow();
    g_cfgTouched = false;
    g_pidSel = target::selPid();
}

static void drawPickers() {
    if (!g_open || g_min) return;

    if (g_pkgPickOpen) {
        ImGui::SetNextWindowSize({470, 400}, ImGuiCond_FirstUseEver);
        ImGui::Begin("Pilih package", &g_pkgPickOpen);
        ImGui::TextDisabled("Proses yang jalan sekarang (* = punya lib engine).");
        ImGui::SetNextItemWidth(300);
        kbField("filterpkg", KB_TEXT, g_pickFilter, 300);
        if (ImGui::Button("Muat ulang daftar")) {
            g_procs = target::listProcesses();
            g_procsLoaded = true;
        }
        ImGui::Separator();
        ImGui::BeginChild("##pkglist", {0, 290}, true);
        if (g_procsLoaded) {
            for (size_t i = 0; i < g_procs.size(); ++i) {
                if (!pickHit(g_procs[i].pkg)) continue;
                char lbl[220];
                snprintf(lbl, sizeof(lbl), "%s %-6d  %s",
                         g_procs[i].score >= 40 ? "*" : " ",
                         g_procs[i].pid, g_procs[i].pkg.c_str());
                const bool sel = target::selPid() == g_procs[i].pid;
                if (ImGui::Selectable(lbl, sel)) {
                    chooseTarget(g_procs[i].pkg, g_procs[i].pid);
                    g_pkgPickOpen = false;
                }
            }
        } else {
            ImGui::TextDisabled("(klik 'Muat ulang daftar')");
        }
        ImGui::EndChild();
        ImGui::End();
    }

    if (g_libPickOpen) {
        ImGui::SetNextWindowSize({520, 400}, ImGuiCond_FirstUseEver);
        ImGui::Begin("Pilih lib il2cpp", &g_libPickOpen);
        if (!mem::attached()) {
            ImGui::TextColored({1, 0.6f, 0.4f, 1}, "Belum attach -- pilih package dulu.");
        } else {
            ImGui::TextDisabled("Lib yang dimuat proses pid=%d (%zu buah):",
                                mem::pid(), g_libNames.size());
            if (!g_libNamesLoaded) collectLibs();
            ImGui::SetNextItemWidth(300);
            kbField("filterlib", KB_TEXT, g_pickFilter, 300);
            if (ImGui::Button("Refresh daftar lib")) collectLibs();
            ImGui::Separator();
            ImGui::BeginChild("##liblist", {0, 280}, true);
            for (size_t i = 0; i < g_libNames.size(); ++i) {
                const std::string& n = g_libNames[i];
                if (!pickHit(n)) continue;
                // tandai kandidat il2cpp biar gampang milih
                const bool likely = n.find("il2cpp") != std::string::npos ||
                                    n == "liblogic.so" || n == "libgame.so" ||
                                    n == "libunity.so" || n == "libanogs.so";
                char lbl[220];
                snprintf(lbl, sizeof(lbl), "%s %s", likely ? "*" : " ", n.c_str());
                if (ImGui::Selectable(lbl, (int)i == g_libPickSel)) {
                    g_libPickSel = (int)i;
                    snprintf(g_libBuf, sizeof(g_libBuf), "%s", n.c_str());
                    g_cfgTouched = true;
                    target::setLib(n);
                    applyTarget();
                    attachNow();
                    g_cfgTouched = false;
                }
            }
            ImGui::EndChild();
        }
        ImGui::End();
    }
}

void draw() {
    syncTargetBuffers();
    drawHud();
    drawFab();
    if (g_showLog) drawLog();
    drawPickers();
    drawKeypad();
    if (!g_open) return;
    if (g_min) return;

    float sw = (float)displayInfo.width;
    float sh = (float)displayInfo.height;
    if (g_pos.x < 0 || displayInfo.orientation != g_ori) {
        g_ori = displayInfo.orientation;
        g_pos = {sw * 0.5f, sh * 0.5f};
        if (g_size.x > sw) g_size.x = sw - 20;
        if (g_size.y > sh) g_size.y = sh - 20;
    }

    ImGuiIO& io = ImGui::GetIO();
    float wx = g_pos.x - g_size.x * 0.5f;
    float wy = g_pos.y - g_size.y * 0.5f;

    if (!io.MouseDown[0]) g_drag = false;
    if (!g_drag && io.MouseDown[0]) {
        float tx = io.MouseClickedPos[0].x, ty = io.MouseClickedPos[0].y;
        bool inHeader = tx >= wx && tx <= wx + g_size.x && ty >= wy && ty <= wy + 46.f;
        if (inHeader && (fabsf(io.MousePos.x - tx) + fabsf(io.MousePos.y - ty) > 8.f)) {
            g_drag = true; g_ts = {tx, ty}; g_ps = g_pos;
        }
    }
    if (g_drag && io.MouseDown[0]) {
        g_pos.x = g_ps.x + io.MousePos.x - g_ts.x;
        g_pos.y = g_ps.y + io.MousePos.y - g_ts.y;
    }

    ImGui::SetNextWindowPos(g_pos, ImGuiCond_Always, {0.5f, 0.5f});
    ImGui::SetNextWindowSize(g_size, ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSizeConstraints({340, 260}, {sw, sh});
    ImGui::Begin("##main", nullptr,
                 ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse |
                 ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoNav |
                 ImGuiWindowFlags_NoMove);

    g_size = ImGui::GetWindowSize();
    g_pos = {ImGui::GetWindowPos().x + g_size.x * 0.5f,
             ImGui::GetWindowPos().y + g_size.y * 0.5f};

    // ---- kepala jendela gaya PenaWare: strip gelap + judul biru + chip aksi ----
    {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const ImVec2 wp = ImGui::GetWindowPos();
        const ImVec2 ws = ImGui::GetWindowSize();
        dl->AddRectFilled(wp, {wp.x + ws.x, wp.y + 46.f},
                          ImGui::GetColorU32(kCardCap), 13.f, ImDrawFlags_RoundCornersTop);
        dl->AddLine({wp.x, wp.y + 46.f}, {wp.x + ws.x, wp.y + 46.f},
                    ImGui::GetColorU32(kLine), 1.f);
        dl->AddRectFilled({wp.x + 15.f, wp.y + 16.f}, {wp.x + 19.f, wp.y + 31.f},
                          ImGui::GetColorU32(kAccent), 2.f);
    }
    // Chip aksi kanan: lebarnya DIHITUNG dari teks aslinya, jadi di jendela
    // sempit label-nya ngependek sendiri & posisinya nggak mungkin nabrak judul
    // (dulu pakai offset tetap 250 px -- di layar kecil chip-nya kekancut).
    const float winW   = ImGui::GetWindowSize().x;
    const bool  narrow = winW < 620.f;
    const char* cHud = narrow ? "H" : "HUD";
    const char* cLog = narrow ? "L" : "LOG";
    const char* cOut = narrow ? "!" : "KELUAR";
    const float chipPitch = 6.f;
    float rowW = 0.f;
    for (const char* c : {cHud, cLog, "X", cOut})
        rowW += ImGui::CalcTextSize(c).x + 18.f + chipPitch;

    const float titleW = ImGui::CalcTextSize("PANXCZ").x + 5.f + ImGui::CalcTextSize("LIBTOOL").x;
    const float subW   = ImGui::CalcTextSize("universal il2cpp").x + 8.f;
    const float chipX  = winW - 14.f - rowW;
    const float titleStop = 31.f + titleW + 10.f;

    ImGui::SetCursorPos({31.f, 15.f});
    ImGui::TextColored(kTextHi, "PANXCZ");
    ImGui::SameLine(0.f, 5.f);
    ImGui::TextColored(kAccent, "LIBTOOL");
    if (chipX - titleStop > subW + 40.f) {   // subtitle cuma kalau masih lega
        ImGui::SameLine(0.f, 8.f);
        ImGui::TextDisabled("universal il2cpp");
    }
    ImGui::SetCursorPos({chipX > titleStop ? chipX : titleStop, 11.f});
    if (chip(cHud)) g_min = true;
    ImGui::SameLine(0.f, chipPitch);
    if (chip(cLog)) g_showLog = !g_showLog;
    ImGui::SameLine(0.f, chipPitch);
    if (chip("X")) g_open = false;
    ImGui::SameLine(0.f, chipPitch);
    if (chip(cOut, {kDanger.x, kDanger.y, kDanger.z, 0.16f}, kDanger)) requestExit();
    ImGui::SetCursorPos({14.f, 56.f});

    // ---- strip status cepat (otomatis turun baris kalau sempit) ----
    auto quickBadge = [&](const char* text, const ImVec4& col, bool first) {
        if (!first) sameLineOrWrap(ImGui::CalcTextSize(text).x + 20.f);
        badge(text, col);
    };
    bool firstBadge = true;
    if (mem::attached())
        quickBadge(mem::procName().empty() ? target::package().c_str() : mem::procName().c_str(),
                   kOk, firstBadge);
    else
        quickBadge("belum attach", kDanger, firstBadge);
    firstBadge = false;
    if (mem::attached()) {
        char nb[64];
        snprintf(nb, sizeof(nb), "pid %d", mem::pid());
        quickBadge(nb, kMuted, false);
    }
    if (metapool::ready()) {
        char nb[64];
        snprintf(nb, sizeof(nb), "pool %zu", metapool::stats().entries);
        quickBadge(nb, kAccent2, false);
    }
    if (dump::loaded()) {
        char nb[96];
        snprintf(nb, sizeof(nb), "dump %zu kelas", dump::classCount());
        quickBadge(nb, kAccent, false);
    }
    if (bypass::enabled()) quickBadge("stealth", kWarn, false);
    // progress bar kerja berat -- ada di semua tab, jadi kelihatan walau
    // menunya udah dipindah pas dump/scan jalan
    if (abortx::beatAgeSec() < 3.0) {
        sameLineOrWrap(230.f);
        ImGui::SetNextItemWidth(230);
        drawWorkProgress(230);
    }

    // ---- rail ikon + kartu konten (gaya PenaWare: rail tipis 70px di kiri) ----
    static const char* kPages[] = {"Info", "Target", "Classes", "Patch",
                                   "Scan", "Touch", "Config"};
    static const char* kGlyph[] = {"i", "T", "C", "P", "S", "H", "G"};
    if (g_cycle) {
        static double last = 0.0;
        const double now = ImGui::GetTime();
        if (last == 0.0) last = now;
        if (now - last >= (double)g_cycleSec) {
            last = now;
            g_page = (g_page % 7) + 1;
        }
    }
    const int kPageCount = (int)(sizeof(kPages) / sizeof(kPages[0]));
    float bodyH = ImGui::GetContentRegionAvail().y;
    if (bodyH < 140.f) bodyH = 140.f;

    // Rail: lebar tetap tipis (gaya PenaWare ~70px), isinya kotak ikon
    // membulat. Label lengkap muncul sebagai tooltip. Padding dalam dipangkas
    // biar kotak ikon 44px pas di tengah (70 - 2*13 = 44).
    const float railW  = 70.f;
    const float boxSz  = 44.f;
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {13.f, 14.f});
    ImGui::BeginChild("##rail", {railW, bodyH}, true, ImGuiWindowFlags_NoScrollbar);
    {
        const float bx = (railW - 26.f - boxSz) * 0.5f;   // 0 = pas tengah
        for (int i = 0; i < kPageCount; ++i) {
            ImGui::SetCursorPosX(bx);
            if (i) ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 6.f);
            if (navItem(kGlyph[i], kPages[i], g_page == i + 1, boxSz)) g_page = i + 1;
        }
    }
    ImGui::EndChild();
    ImGui::PopStyleVar();

    ImGui::SameLine(0.f, 10.f);
    ImGui::BeginChild("##content", {0, bodyH}, true);
    switch (g_page) {
        case 1:  tabInfo();    break;
        case 2:  tabTarget();  break;
        case 3:  tabClasses(); break;
        case 4:  tabPatch();   break;
        case 5:  tabScan();    break;
        case 6:  tabTouch();   break;
        default: tabConfig();  break;
    }
    ImGui::EndChild();

    ImGui::End();
}

void handleKeys() {
    if (keys::takeVolDown() > 0) g_min = !g_min;
    if (keys::takeVolUp() > 0)   g_hud = !g_hud;
}

void requestExit() { g_exitReq = true; }
bool exitRequested() { return g_exitReq; }

void shutdown() {
    g_thRun = false;
    keys::stop();
}

}  // namespace ui

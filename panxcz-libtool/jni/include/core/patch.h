// ============================================================================
//  patch.h - PATCH RUNTIME (nulis byte ke proses game yang lagi jalan)
//
//  KENAPA DIPISAH DARI UI
//  ----------------------
//  Dulu patch cuma ada di tab UI dan cuma bisa nempatch lib il2cpp utama lewat
//  RVA. Sekarang dipakai dari mana-mana: CLI (--patch), file patch per-package
//  (auto-apply pas boot), dan UI. Jadi logic-nya di sini.
//
//  YANG DIDUKUNG
//  -------------
//    * patch RVA di MODULE APA PUN  -> "libil2cpp.so:0x1234 = C0 03 5F D6"
//      (game seperti CODM punya beberapa lib: il2cpp + anti-cheat + engine)
//    * patch alamat ABSOLUT (heap/data) -> "abs:0x7a123450 = 01 00 00 00"
//    * patch lewat POLA byte (wildcard "??") -> "sig libil2cpp.so = 48 8B ?? E0 -> C0 03 5F D6"
//      Berguna kalau layout-nya bergeser tiap update.
//    * auto-apply dari file per-package: /sdcard/panxcz-tools/patch/<pkg>.txt
//    * VERIFIKASI: abis nulis, isinya dibaca ulang (jadi kelihatan kalau
//      tulisannya ditolak kernel / ketimpa game)
//    * DRIFT: game kadang nulis ulang kodenya (page di-COW-lagi, lib di-reload
//      pas masuk match) -> tick() ngecek dan nempatch ulang sendiri
//    * RESTORE: byte asli disimpan sebelum ditulis, jadi bisa dibalikin
//
//  JUJUR: ini murni nulis memori proses lewat /proc/<pid>/mem. Nggak ada hook,
//  nggak ada kode kita yang jalan di dalam game -- efeknya sama kayak ngedit
//  biner di disk, bedanya di sini RAM-nya yang diubah dan langsung aktif.
// ============================================================================
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace patch {

struct Entry {
    std::string label;                 // nama bebas (buat log/file)
    std::string module;                // "libil2cpp.so" / kosong = lib il2cpp utama
    uint64_t    rva      = 0;          // offset dari base module (atau alamat kalau absolute)
    bool        absolute = false;      // true -> rva itu alamat mutlak (abs:)
    std::string sig;                   // pola pencari (opsional). kosong = pakai rva
    std::vector<uint8_t> bytes;        // byte pengganti
    std::vector<uint8_t> orig;         // byte asli (direkam waktu apply pertama)
    uintptr_t   addr     = 0;          // alamat efektif terakhir (hasil resolve)
    bool        on       = false;      // lagi dipasang?
    bool        verified = false;      // terakhir dibaca ulang = sama dengan bytes
    bool        found    = false;      // pola-nya ketemu (khusus sig)
    int         reapplies = 0;         // berapa kali dipasang ulang karena drift
};

// ---- daftar ----------------------------------------------------------------
std::vector<Entry>& list();

// Tambah entri dari satu baris spek (sama formatnya dengan isi file patch):
//   libil2cpp.so:0x1234 = C0 03 5F D6   # ret
//   ../../assets/... (diabaikan)
// Balikin true kalau baris itu jadi entri. `err` diisi alasannya kalau gagal.
bool addSpec(const std::string& line, std::string* err = nullptr);

// Tambah entri patch berbasis pola.
bool addSig(const std::string& module, const std::string& sig,
            const std::string& hex, const std::string& label = std::string());

// Cari alamat pola doang (buat nge-tes pola, nggak nulis apa pun).
// `module` kosong = lib il2cpp yang aktif. 0 = nggak ketemu.
uintptr_t findSigAddr(const std::string& module, const std::string& sig);

// ---- aksi ------------------------------------------------------------------
// Resolve + tulis. Balikin true kalau berhasil (dan sudah diverifikasi).
bool applyEntry(size_t idx);
bool setOn(size_t idx, bool on);          // on=false -> balikin byte asli
bool applyAll();                          // semua entri yang ada byte-nya
bool restoreAll();
void removeAt(size_t idx);
void clear();

// Cek tiap entri yang ON: kalau byte-nya udah nggak sama (ditimpa game / lib
// di-reload), pasang ulang. Dipanggil berkala dari loop UI.
void tick();

// ---- file (auto-apply) -----------------------------------------------------
std::string defaultPath();                // <dumpdir>/../patch/<pkg>.txt
const std::string& loadedFrom();          // file terakhir yang dibaca
bool loadFile(const std::string& path, bool applyNow = true);
bool saveFile(const std::string& path);
bool autoApply();                          // muat defaultPath() + apply
bool& enabled();                           // auto-apply pas boot (default ON)
void  setEnabled(bool on);
const char* lastError();
std::string summary();                     // "3 patch (2 ON)" buat status UI

// ============================================================================
//  FORCE RETURN VALUE (mirip "Patcher" di tool in-process)
//
//  Kalau menulis byte di RVA cuma bisa "ganti instruksi jadi apa pun yang kita
//  ketik", ini yang bikin gampang: pilih METHOD-nya, pilih nilainya, tool yang
//  nyusun kodenya. Caranya sama seperti tool in-process yang populer:
//
//      awal kode method ditimpa STUB kecil yang langsung balikin nilai:
//
//          movz/movk  w0|x0|s0|d0, nilai     (per tipe)
//          ret                               (D65F03C0)
//
//  Jadi: method-nya TIDAK dipanggil sama sekali, tapi pemanggilnya dapat nilai.
//  Byte asli disimpan dulu -> bisa dibalikin (Restore) kapan saja.
//
//  Semua penulisan lewat /proc/<pid>/mem (nggak ada kode kita yang jalan di
//  dalam game) dan selalu dibaca ulang buat verifikasi. Kalau halaman kodenya
//  nggak bisa ditulis, itu dilaporkan (bukan diam-diam gagal).
// ============================================================================
enum class Ret { Void, Bool, I16, U16, I32, U32, I64, U64, Float, Double, Ptr };

const char* retName(Ret r);
// Nebak tipe dari tulisan singkat: "void", "bool", "i32"/"int", "u64", "f32"...
bool retFromName(const std::string& s, Ret& out);

// Susun stub-nya (8-24 byte) buat tipe `r` dan nilai num/ptr. false = tipe
// nggak didukung / nilai nggak masuk akal.
bool buildReturn(Ret r, double num, uint64_t ptr, std::vector<uint8_t>& out,
                 std::string* err = nullptr);

// Pasang stub di ALAMAT KODE method (bukan RVA). Balikin true kalau nulis +
// verifikasi berhasil. Byte asli direkam; entry-nya ikut di-restore oleh
// restoreAll() dan ikut di-apply ulang oleh tick() kalau ketimpa game.
//
// `module` + `rva` cuma buat CATATAN di file (biar bisa dibalikin walau tool-nya
// udah keluar: alamat asli nggak bisa dipakai lagi karena ASLR, rva bisa).
bool forceReturn(uintptr_t addr, Ret r, double num, uint64_t ptr,
                 const std::string& label, const std::string& module, uint64_t rva,
                 std::string* err = nullptr);
bool unforceReturn(uintptr_t addr);
bool isForced(uintptr_t addr, std::string* text = nullptr);
void unforceAll();
size_t forcedCount();
std::string forcedSummary();               // "2 method dibikin balikin nilai"

// ---- catatan ke disk (biar bisa dibalikin kapan aja) -----------------------
// Tiap pasang/bebas nulis ke <dir patch>/<pkg>_forceret.txt: module + rva +
// stub + byte asli. Isinya dipakai buat:
//   * `--unforce all`          -> semua yang pernah dipasang dibalikin
//   * `--forceret-reapply`     -> pasang ulang yang masih tercatat (stub-nya
//                                 nempel walau tool-nya baru jalan lagi)
std::string forcePath();
bool  saveForced();
// Bebasin dari catatan file. `like` kosong = semua. Balikin berapa yang dibalikin.
size_t restoreFromFile(const std::string& like, const char* why = nullptr);
// Pasang ulang semua yang tercatat (setelah game reload kodenya / tool restart).
size_t reapplyFromFile();
size_t fileForcedCount();

// Balikin byte di memori dari FILE .so DI DISK (nggak butuh catatan apa pun).
// Ini yang nyelametin kalau patch-nya udah kepasang tapi catatannya ilang /
// tool-nya keluar sebelum nyatet: kode di memori itu halaman COW, jadi byte
// aslinya masih utuh di library-nya. `len` = berapa byte yang dibalikin.
bool restoreFromLib(const std::string& module, uint64_t rva, size_t len,
                    std::string* err = nullptr);

}  // namespace patch

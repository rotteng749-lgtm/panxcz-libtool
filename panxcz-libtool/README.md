# Panxcz LibTool — universal il2cpp external tool

Tool **ELF external** (jalan sendiri, bukan `.so` yang di-inject) buat ngebaca &
ngutak-atik game Unity/il2cpp: menu ImGui + OpenGL ES, akses memori proses game
lewat `/proc/<pid>/mem`. Satu biner buat **semua game** — game mana yang dibaca
dipilih saat runtime (diketik), bukan di-bake pas build.

by **Panxcz & Freebuff**

```
panxcz-libtool/
├── build.sh                     # build pakai ndk-build (NDK dicari otomatis)
├── tools/
│   ├── ndk_detect.sh            # cari NDK versi apa pun (r21..r30, Termux, zip)
│   ├── install_onphone.sh       # push + jalanin di HP
│   ├── push_dump.sh             # kirim dump .cs ke folder dumper di HP
│   └── dump_native.sh           # jalanin --dumpout dari PC (adb), opsional tarik hasilnya
├── jni/
│   ├── Android.mk / Android-dbg.mk
│   ├── include/
│   │   ├── core/{mem,il2cpp,target,explore,scanner,safe,keys,log}.h
│   │   └── ui/ui.h
│   └── src/
│       ├── main.cpp             # boot: overlay + attach + (target dari argv/cfg)
│       ├── core/                # memori proses, resolver il2cpp, explorer, scanner
│       ├── ui/ui.cpp            # semua tab menu
│       ├── Android_draw/        # backend EGL/GLES + ImGui
│       ├── Android_touch/       # TouchHelperA (uinput)
│       └── ImGui/
└── out/panxcz_libtool.sh        # hasil build (ELF arm64, dijalankan pakai `sh`)
```

## Build

Hasil build keluar **tiga nama dari satu biner yang sama** (isinya identik):

| file | buat apa |
|---|---|
| `out/panxcz_libtool.sh` | nama lama -- semua script/perintah yang udah ada tetap jalan |
| `out/panxcz_libtool.elf` | jelas keliatan "ini ELF" (dikirim/upload/inspeksi) |
| `out/panxcz_libtool` | **payload ala tool client-server** (mis. aset daemon `Fucker` di app HKBOSS): ditaruh di `assets/`, app-nya nge-copy + `chmod 777` + `su -c <path>`, langsung jalan |

```bash
bash build.sh                        # NDK dicari sendiri
bash build.sh --ndk /path/android-ndk-r25c
bash build.sh --clean
```

Hasil: `out/panxcz_libtool.sh` (~1,2 MB, ELF arm64 PIE, stripped).

> Nama berkasnya `.sh` **sengaja**: satu file, gampang dikirim/disimpan. Isinya
> **ELF**, bukan skrip — jadi JALANINNYA LANGSUNG, jangan lewat `sh`:
>
> ```bash
> su -c '/data/local/tmp/panxcz_libtool.sh com.mobile.legends'     # benar
> su -c 'sh /data/local/tmp/panxcz_libtool.sh com.mobile.legends'  # mksh: syntax error
> ```
>
> (shell di Android itu mksh, nggak nge-exec ELF; `sh <elf>` = dia malah
> nganggepnya skrip.) File-nya udah 755, jadi tinggal dipanggil path-nya.

## Pakai di HP

Butuh **root** (buat buka `/proc/<pid>/mem` dan bikin surface overlay).

```bash
adb push out/panxcz_libtool.sh /data/local/tmp/
adb shell "su -c 'chmod 755 /data/local/tmp/panxcz_libtool.sh'"
```

### MODE OTOMATIS — cukup jalanin, nggak usah ngetik apa pun

```bash
/data/local/tmp/panxcz_libtool.sh        # CUKUP INI (nggak perlu `su -c` dulu)
```

Kalau dijalanin dari shell/Termux/MT Manager yang **belum root**, tool nyoba
naikin sendiri lewat `su` (kalau su-nya bukan di PATH, set `PANXCZ_SU=/path/ke/su`;
mau matiin: `PANXCZ_NO_SU=1`).

> Catatan jujur: di ROM yang **nyembunyiin `su`** dari user biasa (KernelSU +
> SUSFS), `su`-nya nggak kebaca sama sekali (`inaccessible or not found`) —
> itu bukan bug tool, memang gitu cara ROM-nya. Di situ jalanin dari terminal
> yang udah root (`MT Manager → Terminal`), atau `su -c '...'` — habis itu
> cukup satu perintah tadi, nggak perlu mode/argumen apa pun.
>
> Nama berkasnya `panxcz_libtool.sh` (garis bawah).

Yang dilakukannya sendiri:

1. **cari game-nya**: kalau target di cfg nggak jalan / cfg masih kosong / cfg
   keisi nama proses (bukan `com.xxx`), tool nyisir daftar proses dan milih yang
   punya lib il2cpp / global-metadata (skor tertinggi). Proses tool sendiri +
   rantai parent-nya (mksh/ash/su) dibuang dari daftar, jadi nggak akan attach
   ke diri sendiri; proses skor < 15 (bukan game) juga nggak kepilih.
   Sub-proses dipotong ke package dasar, jadi MLBB tetap nempel ke
   `com.mobile.legends:UnityKillsMe` (proses engine-nya);
2. **attach + tunggu lib il2cpp** ke-load. Tool **nggak nyerah** kalau game-nya
   belum dibuka: ditungguin sampai 5 menit (lapor tiap ~30 detik) — jadi
   urutannya bebas, mau buka tool dulu atau buka game dulu;
3. **dump sendiri**: semua lib il2cpp + `.cs .json .h .ptrs.tsv`, ke
   `/sdcard/panxcz-tools/dumper/<pkg>/<versi>/`;
4. **menu tetap kebuka** (nggak keluar sendiri) + progress bar fase/persen/detik,
   jadi bisa lanjut pakai tab lain.

Log-nya kelihatan seperti ini:

```
[su] belum root -- naikin sendiri lewat su (nggak mau: PANXCZ_NO_SU=1)
[boot] Panxcz LibTool v1.1 start (universal il2cpp + dump class-slot)
[auto] MODE OTOMATIS: cari game + attach + dump sendiri. Stop/keluar lewat tombol di menu.
[auto] nyari game yang lagi jalan (proses yang punya lib il2cpp)...
[auto] target: com.mobile.legends (skor il2cpp 222), lib auto
[boot] proses target: com.mobile.legends:UnityKillsMe (pid 20518) | lib il2cpp: libcsharp.so
[auto] dump otomatis: semua lib + .json + .h + .ptrs.tsv (folder default ...)
[dumpout] selesai: 8172 kelas (7734 slot, 68807 field, 109880 method, 75 module)
```

Kalau game-nya belum dibuka, yang muncul gini (tool nungguin, bukan mati):

```
[auto] belum ada game yang kelihatan -- buka game-nya sekarang, tool nempel sendiri (nunggu max 300 detik)
[auto] masih nunggu (30 detik) -- buka game-nya, atau pilih prosesnya manual di tab TARGET
```

Mau mode manual (package/lib/opsi diketik sendiri) tetap bisa seperti biasa —
begitu ada argumen apa pun, mode otomatis nggak aktif. `--auto` / `--manual`
bisa dipakai buat maksa salah satunya.

### Manual (kalau mau milih sendiri)

```bash
# game-nya jalanin dulu, terus:
su -c '/data/local/tmp/panxcz_libtool.sh com.mobile.legends libcsharp.so'
#                                              ^package          ^lib il2cpp
# atau tanpa lib (auto-detect):
su -c '/data/local/tmp/panxcz_libtool.sh com.kiloo.subwaysurf'
```

Argumen yang lain (boleh digabung, urutan bebas):

| Argumen | Fungsi |
|---|---|
| `<package>` | proses game (boleh `com.pkg:subproses`) |
| `[lib]` | nama lib il2cpp, atau `auto` |
| `--pid N` | paksa pid tertentu (kalau satu game punya beberapa proses) |
| `--probe Ns::Class` | cari class itu terus tulis field/method ke logcat, tanpa buka menu |
| `--exit` | keluar sendiri setelah probe (buat tes otomatis) |
| `--dump <file>` | dump `.cs` buat tabel class-slot (kosong = pakai jalur auto) |
| `--dumpdir <folder>` | root folder dumper (default `/sdcard/panxcz-tools/dumper`) |
| **`--dumpout`** | **bikin dump `.cs` SENDIRI** (native, root doang) -- lihat di bawah |
| `--all-libs` | waktu dumpout: pindai semua module il2cpp (MLBB: 3 lib) |
| `--cols a,b` | waktu dumpout: pilih kolom (`field,method,slot,token,parent`) |
| `--ns X` / `--like X` | waktu dumpout: filter kelas |
| `--json` / `--header` / `--ptr` | waktu dumpout: format tambahan (`.json` / `.h` / `.ptrs.tsv`) |
| `--classes N` | waktu dumpout: batasi jumlah kelas (tes cepat) |
| `--outdir <folder>` | waktu dumpout: folder hasil (default `/sdcard/panxcz-tools/dumper/<pkg>/<versi>/`) |
| `--scan-only` | dumpout: scan & hitung aja (nggak nulis file) |
| `--typeprobe` | diagnosa encoding typeId (lihat §pool MethodInfo) |
| `--diag N` | waktu dumpout: tulis qword mentah N kelas pertama (diagnosa layout) |
| `--stay` | waktu dumpout: jangan keluar sendiri setelah selesai |
| `--auto` / `--manual` | paksa mode otomatis / manual (lihat §MODE OTOMATIS) |
| `--check` | diagnosa lingkungan (root/izin/proc/region/baca blok 4 MB) + verdict |
| **`--gencode Class::Method[=nilai[:tipe]]`** | **emit snippet hook Dobby-C++** (port `GenerateCppCode` dari `Android-LibTool-New`) → `/sdcard/panxcz-tools/gencode/<pkg>/` |
| **`--ggcode Class::Method[=nilai[:tipe]]`** | **emit script GameGuardian-Lua** (port `GenerateGGPatchCode`) → folder yang sama |
| `--huntaddr 0x...` | alamat kode → method terdekat di pool (butuh `--pool`; port `binarySearchClosest`) |
| `--hexdump Class::Method[:len]` \| `lib.so:0xRVA[:len]` \| `0xADDR[:len]` | hexdump kode method (port `HexView`; tandai `[forceret]` kalau lagi dipatch) |
| `--asmview <sama>` | disassemble AArch64 (port `AssemblyView`; decoder sendiri, tanpa Capstone) |
| `--findobj Class[:max]` | instance hidup (port `ImGuiObjectSelector`) |
| `--inspect Class@0xADDR` | bedah semua field satu object (port `ImGuiJson`; cek klass + tandai static) |
| `--strings [filter]` | pindai string UTF-16 di memori, cap 500 (port `Strings`) |
| `--search kata [--by class\|method\|field]` | cari tri-mode di dump+pool (port `FilterClasses`); `--by` nempel ke `--search` berikutn—boleh antri banyak |
| `--no-dump-methods` | jangan isi daftar method dari dump offline |
| `--scan-only` | hitung kelas/field/method aja (nggak nulis file) |
| `--patch "<spec>"` | patch runtime (lihat §RUNTIME PATCH), bisa diulang |
| `--patch-file <f>` | muat daftar patch dari file terus apply |
| `--no-patch-auto` | jangan auto-apply daftar patch bawaan game itu |
| `--peek <spec>` | cetak byte di alamat (`0x7a123450` atau `libil2cpp.so:0x1234`) |
| `--peek-len N` | panjang yang dicetak `--peek` (default 32, maks 128) |
| `--page N` | buka halaman sidebar N langsung (1=Info … 7=Config), menu tetap kebuka |
| `--ui-cycle [detik]` | muter halaman sendiri tiap N detik (default 2) — buat ngecek semua halaman render |

```bash
# cek tanpa nyentuh layar:
su -c '/data/local/tmp/panxcz_libtool.sh com.mobile.legends --probe BattleManager --exit'
logcat -s panxcz_libtool
```

Atau pakai helper:

```bash
bash tools/install_onphone.sh 192.168.1.61:35313 com.mobile.legends liblogic.so
```

Kalau package-nya nggak dikasih, tool-nya cari sendiri (mode otomatis di atas),
atau lu pilih manual di tab **TARGET** (ada daftar proses yang lagi jalan).

## Target: game + lib il2cpp (bisa diketik)

| Game | package | lib il2cpp |
|---|---|---|
| Mobile Legends | `com.mobile.legends` | `libcsharp.so` (+ `liblogic.so`, `libfirst.so`) |
| Subway Surfers | `com.kiloo.subwaysurf` | `libil2cpp.so` |
| Unity umum | (paket game) | `libil2cpp.so` |
| Free Fire | `com.dts.freefireth` / `...max` | `libil2cpp.so` (anti-cheat: `libanogs.so`) |

Urutan sumber target:

1. argumen `.sh` → `<package> [lib] [--pid N]`
2. `/data/local/tmp/panxcz_libtool.cfg` → `package=` / `lib=` / `pid=` (ditulis menu)
3. `/sdcard/panxcz/libtool.cfg`

### Kalau satu game punya beberapa proses

Unity sering naruh engine-nya di **proses anak**, jadi proses pertama yang
cmdline-nya cocok belum tentu yang bener. Contohnya MLBB:

```
com.mobile.legends                     <- shell/loader, lib engine NGGAK di sini
com.mobile.legends:UnityKillsMe        <- di sini libcsharp.so/liblogic.so (skor 252)
```

Nama proses engine itu **disimpan otomatis** ke cfg (`engine=`) begitu tool
berhasil nempel (dan il2cpp-nya kedeteksi), jadi jalan-jalan berikutnya tool
langsung nempel ke proses yang bener — nggak nebak-nebak lagi:

```
[target] cfg: package=com.mobile.legends lib=auto pid=auto engine=com.mobile.legends:UnityKillsMe
[target] nempel ke proses engine dari cfg: com.mobile.legends:UnityKillsMe (pid 24812)
[boot] proses target: com.mobile.legends:UnityKillsMe (pid 24812) | lib il2cpp: libcsharp.so
[target] proses engine disimpan: com.mobile.legends:UnityKillsMe
```

Kalau engine-nya belum jalan (game belum dibuka), baris itu dilewat dan tool
mencari sendiri seperti biasa. Dan kalau `pid=` di cfg nyangkut ke proses yang
**bukan** pemilik lib il2cpp (mis. sisa sesi sebelumnya), pid itu dibuang sendiri
(`[target] pid N nggak punya lib il2cpp -- pid di cfg dibuang`) biar nggak nyangkut
ke proses yang salah terus-terusan. Nama engine bisa dilihat di tab **TARGET**
dan **INFO** (baris `engine`).

### MLBB: metadata dipecah 3 lib (JANGAN pakai `libil2cpp.so`)

MLBB bukan Unity biasa. Di `app_libs/` ada tiga lib, dan tiap lib pegang
metadata-nya sendiri:

| lib | isinya |
|---|---|
| `liblogic.so` (27 MB) | part 1: `mscorlib.dll`, `System.*`, `UnityEngine.*` |
| `libfirst.so` (10 MB) | part 2: `Assembly-CSharp-firstpass.dll` |
| `libcsharp.so` (104 MB) | part 3: `Assembly-CSharp.dll`, `GameEntryAOT`, `HybirdAOT` |

`libil2cpp.so` yang ada di APK-nya cuma **stub 384 KB** — bukan engine-nya.
Tool-nya udah tahu ini: kandidat yang < 1 MB dibuang kalau ada kandidat lain
≥ 4 MB (`[target] lib il2cpp kedeteksi: libcsharp.so -- kandidat pertama cuma stub`).

Makanya tool-nya **skor tiap proses** dari `/proc/<pid>/maps` (`*` di menu =
punya lib engine) dan otomatis milih yang paling tinggi. Nama proses anak yang
khas (`:Unity…`, `:il2cpp`, `:game`) dapat bonus skor. Kalau mau pilih sendiri:
tab **TARGET** → tombol **Daftar…** di baris `package` (window daftar proses,
klik = langsung attach ke pid itu), atau tulis nomornya di kolom `pid`
(kosong/0 = otomatis).

`lib` boleh **dikosongkan** (`lib=auto`): lib il2cpp dicari sendiri dengan urutan
nama yang umum (`libil2cpp.so`, `liblogic.so`, `libfirst.so`, `libcsharp.so`,
`libunity.so`, …) — dengan aturan tambahan **stub dibuang**: kalau kandidat
pertama < 1 MB dan ada kandidat lain ≥ 4 MB, yang gede yang dipakai (itu kasus
MLBB). Konsisten dengan MLBB: jalur terbaiknya tetap `libcsharp.so`, karena
di situlah `Assembly-CSharp.dll` (class game-nya).

## RUNTIME PATCH (tulis byte ke memori game yang lagi jalan)

Nulis byte ke proses yang **lagi jalan** — bukan ngedit APK, bukan hook: murni
`/proc/<pid>/mem`. Logic-nya di `core/patch.cpp`, dipakai dari UI (tab **PATCH**)
dan CLI.

```bash
# 1) patch RVA di module apa pun (bukan cuma lib il2cpp)
su -c "/data/local/tmp/panxcz_libtool.sh --patch 'libil2cpp.so:0x1A2B3C = C0 03 5F D6'"

# 2) patch alamat mutlak (data/heap)
su -c "/data/local/tmp/panxcz_libtool.sh --patch 'abs:0x7A12A000 = 01 00 00 00'"

# 3) patch pakai POLA (kalau RVA-nya geser tiap update)
su -c "/data/local/tmp/panxcz_libtool.sh --patch 'sig libil2cpp.so = 48 8B ?? E0 -> C0 03 5F D6'"

# 4) dari file + langsung bareng dump
su -c "/data/local/tmp/panxcz_libtool.sh --patch-file /sdcard/panxcz/patch.txt --dumpout --exit"

# 5) ngecek isi memori (buat mastiin patch-nya nempel)
su -c "/data/local/tmp/panxcz_libtool.sh --peek libil2cpp.so:0x1A2B3C --peek-len 16 --exit"
```

Yang dikerjain tiap patch:

1. **resolve** alamat (base module + RVA, `abs:`, atau cari pola `??`)
2. **rekam byte asli** dulu (sekali) — ini yang dipakai buat restore
3. **tulis** byte-nya, terus **baca ulang** buat verifikasi (`TULISAN DITOLAK/
   KETIMPA` di log kalau kernel/game nolak — jadi nggak ada "ketipu sukses")
4. **anti-drift**: tiap detik byte-nya dicek ulang; kalau game nulis ulang
   kodenya (page di-COW lagi / lib di-reload pas masuk match), patch-nya
   **dipasang ulang sendiri** (`[patch] byte-nya ketimpa balik @ 0x… -- pasang ulang`)
5. **restore**: tombol `Restore semua` / ganti checkbox jadi OFF nulis balikin
   byte aslinya

## FORCE RETURN VALUE (pilih method, pilih nilainya)

Nulis byte manual buat "method ini balikin nilai X" itu ribet: harus tahu
encoding `movz/movk/fmov` + `ret`-nya. Jadi tool yang nyusun sendiri — sama
kayak tab *Patcher* punya tool in-process (`MethodInfo->methodPointer` ditimpa
stub kecil), bedanya di sini nulisnya lewat `/proc/<pid>/mem`.

```bash
# bool: 1 = true, 0 = false
su -c "/data/local/tmp/panxcz_libtool.sh com.mobile.legends:UnityKillsMe --pool \
       --forceret 'BloomHdrRuntimeParamsProxy::GetParamsEnable=false' --exit"

# angka (int64 pakai :i64, kalau nggak ditulis ditebak dari nilainya)
su -c "... --forceret 'GameManager::get_Gold=99999' --exit"

# float	su -c "... --forceret 'Unit::get_Speed=12.5:float' --exit"
# void → method-nya dimatiin (langsung ret)
su -c "... --forceret 'Hero::TakeDamage=off' --exit"

# method itu ada di kelas mana aja (kalau nama kelasnya nggak yakin)
su -c "... --pool --forcefind GetParamsEnable --exit"

# balikin byte aslinya
su -c "... --unforce 'BloomHdrRuntimeParamsProxy::GetParamsEnable' --exit"
su -c "... --unforce all --exit"          # semua yang pernah dipasang

# balikin dari file .so DI DISK (nggak butuh catatan apa pun)
su -c "... --fixcode libfirst.so:0x6bfb68:8 --exit"
```

Di UI: tab **CLASSES** → daftar method → tombol kecil **`ret`** di tiap baris →
pilih tipe + atur nilainya pakai tombol (nggak butuh keyboard) → **APPLY**.
Method yang udah dipasang kelihatan hijau (`-> bool (false)`) dengan tombol
**balikin**.

Stub yang dipakai (semua **sudah dicocokin byte-per-byte sama assembler
AArch64 asli**, bukan cuma percaya rumus):

| tipe | yang ditulis | contoh byte |
|---|---|---|
| void | `ret` | `C0 03 5F D6` |
| bool | `mov w0,#0/1; ret` | `00 00 80 52 C0 03 5F D6` |
| int32/uint32 | `movz/movk w0; ret` | (2-3 instruksi) |
| int64 | `movz+movk x0 (4x16bit); ret` | (2-5 instruksi) |
| float | bit-nya ke `w0`, terus `fmov s0,w0; ret` | `... 00 00 27 1E C0 03 5F D6` |
| double | bit-nya ke `x0`, terus `fmov d0,x0; ret` | `... 00 00 67 9E C0 03 5F D6` |
| object/string | alamatnya ke `x0` (0 = null) | (5 instruksi) |

Catatan jujur:

* Tipe **string/object** cuma bisa dipasang `0` (null) dari luar — bikin string
  il2cpp baru itu perlu jalan di dalam game (butuh injeksi). Itu yang belum ada.
* Patch-nya **tetap nempel walau tool-nya udah keluar** (memang gitu maunya).
  Karena itu tiap pasang **dicatat** ke
  `…/patch/<package>_forceret.txt` — isinya `module:rva | stub | byte asli |
  nama | nilai`. Alamat absolut nggak disimpan (ASLR), jadi `--unforce` dijalanin
  di sesi/tool berikutnya tetap bisa (toh rva-nya sama).
* Ada juga jalur "nggak butuh catatan": `--fixcode lib:rva:len` nulis balik
  byte aslinya dari **file .so di disk** (kode di memori itu halaman COW, jadi
  byte asli masih utuh di library-nya). Ini yang dipakai buat ngembaliin patch
  yang catatannya udah keburu ilang.
* Kalau halaman kode-nya nggak bisa ditulis, itu **dilaporkan** (`TULISAN
  DITOLAK/KETIMPA`), bukan diam-diam "sukses".

### Auto-apply per game (bawaan)

Daftar patch disimpan per package:

```
/sdcard/panxcz-tools/patch/<pkg>.txt
```

Formatnya sama kayak contoh di atas (satu baris satu patch) dan **dibaca otomatis
tiap tool jalan** (mau mode otomatis atau manual). Isinya:

```
# komentar pakai '#'
libil2cpp.so:0x1A2B3C = C0 03 5F D6   # ret
sig libil2cpp.so = 48 8B ?? E0 -> C0 03 5F D6   # pola
abs:0x7A12A000 = 01 00 00 00
```

Matiin auto-apply: `--no-patch-auto` atau checkbox di tab PATCH. Di tab PATCH ada
tombol `Simpan ke file` / `Muat dari file`, `Apply semua`, `Restore semua`, dan
checkbox per-patch (ON/OFF) — tiap baris nunjukin alamat efektif + status
`terverifikasi`, plus berapa kali kena "pasang ulang".

Catatan penting (jujur):

* patch **nggak otomatis dibalikin** waktu tool ditutup — patch-nya nempel di
  proses game sampai `Restore semua` ditekan atau game-nya direstart;
* `--patch` yang ditulis di CLI dibedakan dari pola yang ambigu: kalau satu pola
  muncul di banyak tempat, yang dipilih match **paling rendah alamatnya** dan
  kalau ada match yang isinya udah sama dengan byte pengganti, itu yang dipakai
  (jadi jalan kedua nggak nempatch kemunculan lain);
* ini ngubah memori proses **online** — game ber-anti-cheat bisa nganggepnya
  curang. Risiko ada di sisi lu, tool-nya nggak nyembunyiin itu.

### Bentuk dump: katalog + runtime digabung

Kalau ada dump offline (metadata/frida) yang termuat, daftar method **katalog
jadi dasar** — itu yang punya signature penuh `static void Foo(ulong a, bool b)`.
Method runtime yang namanya belum ada di katalog ikut ditambahkan, dan entri
katalog yang RVA-nya 0 diisi dari runtime (RVA runtime selalu valid karena
dipetakan ulang tiap run). Field juga digabung: penanda `static` + field statis
yang nggak muncul di array `FieldInfo` runtime diambil dari katalog.

Jadi hasil native = **lengkap seperti dump metadata** + **RVA yang selalu valid**
(0 baris `RVA: unknown`, sedangkan dump frida v3.4 masih ~11 rb).

Katalog di-parse **satu kelas sekali jalan lalu dibuang**, bukan semuanya sekaligus
(28 rb kelas x 200 rb method = ratusan MB string → proses dibunuh kernel/OOM).

Catatan: dump frida menandai anggota enum sebagai `static ... static_fields offset: 0x0`
dan menulis enum sebagai `class`. Yang dicatat native mengikuti jenis sebenarnya
(`enum`) dan tidak memalsukan static di situ.

## Hasil tes lintas game (SM-A155F, 22.1.97 MLBB / 1.132.1 FF / 1.6.57 CODM / 3.69.1 Subway)

| game | lib il2cpp | katalog | hasil dump |
|---|---|---|---|
| MLBB | `libcsharp.so` + `liblogic` + `libfirst` | frida | 28.139 kelas / **199.239 method** (identik dgn dump frida) / **0 RVA unknown** |
| Free Fire | `libil2cpp.so` 438 MB | native | 11.291 kelas / 9.337 method / RVA unknown **0** — merge isi 3.185 kelas |
| Subway Surfers | `libil2cpp.so` | **tidak ada** | 8.116 kelas / 78.726 method — jalur runtime murni, merge dilewati tanpa ganggu |
| CODM | il2cpp di dalam `libunity.so` | native (13 KB) | 323 kelas / 0 field / 3 method — **batasan yang sudah diketahui**, metadata-nya belum ke-resolve di layar awal |

Layout il2cpp kepakai berbeda per game dan dipilih otomatis dari hasil kalibrasi:
MLBB `structur, skor 19` · FF & Subway `ptr+0x98 / name+0x18 / klass+0x20, skor 24`
· pool FF `il2cpp-2021 (klass+0x20, nama+0x18, stride 0x48)` 98% · pool Subway
`il2cpp-2019 (klass+0x18, nama+0x10, stride 0x50)`.

## Hasil tes di game lain

### Free Fire (`com.dts.freefireth`, Unity il2cpp standar)

Runtime dump **jalan** di proses yang lagi main (bukan cuma MLBB):

```
[il2] libil2cpp.so base=0x79c507b000
[dumpout] layout field : OK ptraddr+0x80 stride 0x20 name+0x0 type+0x8 offset+0x18 token+0x1c
[dumpout] layout method: OK ptr+0x98 stride 0x8 name+0x18 klass+0x20
[dumpout] 3457 kelas dari tabel class-slot
[dumpout] +8523 kelas dari pool method -> total 11980
[pool] 46481 method, 18109 kelas | 352 MB dipindai
[dumpout] selesai: 11980 kelas (3457 slot, 50350 field, 289614 method, 70 module)
```

* layout `FieldInfo`/`MethodInfo` di il2cpp versi FF **beda** dari MLBB (`ptr+0x98`,
  `name+0x18`, `klass+0x20`) — dan ke-kalibrasi sendiri tanpa di-hardcode;
* hasil dump di `/sdcard/panxcz-tools/dumper/com.dts.freefireth/1.132.1/`
  (`.cs` 26 MB, `.h` 30 MB, `.json` 44 MB, `.ptrs.tsv` 29 MB):
  **0 "RVA: unknown"**, 282.792 method ber-RVA, 0 token kosong;
* catatan jujur: **9.551 dari 50.350 field (19%) offset-nya 0x0** — di il2cpp
  baru, sebagian kelas nggak nyimpen offset di `FieldInfo` (offset-nya di array
  terpisah yang belum ke-resolve). Sisanya (81%) offset-nya asli.

Runtime patch juga dites di FF, **net-zero** (ditulis lalu dibalikin dalam satu
sesi, jadi memori game-nya nggak berubah):

```
[patch] apply @ 0x79c507b000 (4 byte) [7F 45 4C 46  ->  7E 45 4C 46]
[patch] apply @ 0x79c507b000 (4 byte) [7E 45 4C 46  ->  7F 45 4C 46]
[patch] apply semua: 2 sukses, 0 gagal (2 entri)
[peek]  @0x79c507b000 (8 byte): 7F 45 4C 46 02 01 01 00  | .ELF....     <- udah balik
```

Kalau dua entri nempatch alamat yang sama, anti-drift-nya sekarang tahu itu
memang disengaja (nggak "pasang ulang" terus-terusan).

### Call of Duty Mobile (`com.garena.game.codm`, 1.6.57) — attach jalan, il2cpp-nya masih kebaca tipis

CODM-nya sekarang **bisa nyala** di HP ini, dan tool **attach + nulis dump** ke
prosesnya (tanpa crash, tanpa FATAL):

```
[mem] pid=30571 [com.garena.game.codm] skor il2cpp=62
[target] lib il2cpp kedeteksi: libunity.so (247388 KB)
[boot] proses target: com.garena.game.codm (pid 30571) | lib il2cpp: libunity.so
[ringkas] dump    : 114 kelas, 114 slot, 0 field, 0 method, 3 module
[ringkas] waktu   : 16 detik
```

Catatan jujur soal CODM: APK-nya **nggak punya `libil2cpp.so`** — kode il2cpp +
metadatanya nempel di dalam **`libunity.so` (236 MB)**, dan tool ngenalin itu
sendiri lewat deteksi lib (`libunity.so` ada di daftar kandidat). Yang kebaca
baru "kerangka"-nya (114 kelas berslot, 0 field/method) karena prosesnya masih
di awal start (lib baru kebuka sebagian, metadata-nya belum di-resolve).
Field/method-nya dites lagi begitu CODM-nya masuk lobby/match.

### Free Fire lagi (in-lobby): pool-nya baru sebagian

Waktu dites di sesi ini FF cuma kebuka sampai layar awal (libil2cpp baru ke-map
182 MB dari ~477 MB pas in-lobby), jadi angkanya kecil — dan itu **jujur
ketulis** di ringkasan, bukan nampilin angka lama:

```
[ringkas] pool    : 2925 method, 609 kelas, 2925 punya nama (il2cpp-2021 ...)
[ringkas] dump    : 617 kelas, 83 slot, 247 field, 2938 method, 32 module
```

Di sesi sebelumnya, waktu FF-nya **in-lobby**, angka yang sama keluar
`46.481 method / 11.980 kelas / 289.614 method` (lihat bagian di atas). Jadi yang
berubah cuma keadaan game-nya, bukan tool-nya.

## Tab menu

| Tab | Isi |
|---|---|
| **TARGET** | package + lib (diketik lewat keypad sendiri) + kolom `pid`, tombol **Daftar…** buka **window terpisah** berisi daftar proses (`*` = punya lib engine) / daftar lib yang dimuat proses target (`*` = kandidat il2cpp) — klik satu baris = langsung attach, status pid/base/region + **status dump offline** (jumlah kelas/slot, tombol Muat/Cari sendiri/Lepas) |
| **CLASSES** | ketik nama class → resolve: **fields + offset** (statis ditandai `S`), **methods + RVA**, instance hidup, nilai field dibaca & **diedit** (field statis dibaca dari `static_fields`), dump class ke `/sdcard/panxcz/libtool/` |
| **PATCH** | tulis byte hex ke **RVA lib il2cpp** (apply / restore / list) |
| **SCAN** | memory scanner: first scan, refine (exact/changed/unchanged/incr/decr), freeze, edit |
| **TOUCH** | macro gesture lewat uinput (swipe 4 arah, tap berulang) |
| **CONFIG** | log ke file, re-attach + re-detect, detect ulang lib, **Keluar dari tool**, info |

Pas lib il2cpp-nya aktif, proses yang BENERAN di-attach dicetak ke log -- di MLBB
engine-nya ada di sub-proses, bukan proses utama:

```
[boot] proses target: com.mobile.legends:UnityKillsMe (pid 26930) | lib il2cpp: libcsharp.so
```

**Keluar:** tombol **KELUAR** di header menu atau **CONFIG → Keluar dari tool**.
Tool-nya ngelepas overlay dan mati bersih (`exit code 0`) — game-nya sama sekali
nggak diganggu. Dari shell juga bisa: `pkill -f panxcz_libtool`.

**Volume Down** = minimize/expand, **Volume Up** = HUD on/off. Pas minimize ada
bulatan mengambang yang bisa digeser (ketuk = menu balik).

## Nggak ada keyboard sistem

Overlay ini proses terpisah, jadi `ImGui::InputText` nggak bisa diketik. Semua
input teks pakai **keypad sendiri** di bawah layar (angka, huruf, `C`, `HAPUS`,
`OK`/`BATAL`).

## Dump offline (`.cs`) — tabel `class-slot`

Ini yang bikin kelas di **MLBB** ketemu. Di sana metadata-nya dipecah tiga lib
dan nama class **nggak punya pointer balik** ke `Il2CppClass` — jadi scan-string
mentok (`[il2] 'BattleManager': 0 kandidat, semua ditolak`).

Dump punya baris yang persis menjawab itu:

```
// Module 3: libcsharp.so / Assembly-CSharp.dll
// Namespace: ; TypeDefIndex: 4032; Token: 0x2000e4d
// Class-slot candidates: 0x635b290
class BattleManager
{
    static BattleManager Instance;  // static_fields offset: 0x0; token: 0x4007c74
    // RVA: 0x32C6BC8
    static void SetPreInvite(ulong accountId, bool val);
}
```

`Class-slot candidate` = RVA (relatif module tempat sel-nya) dari sebuah sel yang
isinya `Il2CppClass*`. Rantainya:

```
libcsharp_base + 0x635b290  ->  Il2CppClass*  ->  +0xa8 static_fields  ->  +offset
```

Cara pakainya — tool **nyari sendiri**, urutannya:

| # | lokasi |
|---|---|
| 1 | cfg `dump=` / argumen `--dump <file>` **— dipakai cuma kalau file itu punya game yang lagi dipilih** |
| 2 | **`/sdcard/panxcz-tools/dumper/<pkgname>/<versionname>/*.cs`** |
| 3 | `/sdcard/panxcz-tool/dumper/<pkg>/<versi>/` (root lama) + `/sdcard/panxcz-tools/<pkg>/<versi>/` (dump frida v3.5) + file lepas di root (`<pkg>_native_full.cs`) |
| 4 | `/data/local/tmp/panxcz_dump.cs`, `/sdcard/panxcz/dump.cs`, `/sdcard/Download/dump.cs`, … |

Folder `<pkg>/<versi>` diisi otomatis dari package target (sub-proses dibuang:
`com.mobile.legends:UnityKillsMe` → `…/dumper/com.mobile.legends/…`) dan
**versi**-nya dari APK yang lagi jalan. Root-nya bisa diganti: cfg `dumpdir=`
atau argumen `--dumpdir <folder>` (root standar tetap ikut dicari, yang dipakai
tetap yang paling lengkap).

**Semua file dari VERSI YANG SAMA digabung jadi satu katalog** (bukan cuma satu
file). Ini yang bikin daftar kelas/method-nya lengkap kalau dump-nya kepecah
per lib — contoh nyata di MLBB (7 file, 167 MB):

```
[dump] 7 kandidat dump buat com.mobile.legends:
[dump]   com.mobile.legends_native_full.cs                 5 MB, 6339 kelas berslot
[dump]   dump.cs                                          34 MB, 18626 kelas berslot  <- dipakai
[dump]   com.mobile.legends_libcsharp.so.cs               36 MB, 6235 kelas berslot
[dump]   com.mobile.legends_full.cs                       38 MB, 6461 kelas berslot
[dump]   dilewat: com.mobile.legends_native_libunity.cs (cuma 114 kelas berslot, 18626 di file terbaik)
[dump] 7 file versi 22.1.97.12061 digabung jadi satu katalog
[dump] gabungan: 85450 kelas kembar dibuang
[dump] gabungan 7 file: 46706 kelas, 23058 punya slot, 169 module (167 MB)
```

Aturan yang dipakai (semuanya kelihatan di log, nggak ada yang diem-diem):

* **dump game lain nggak akan kepakai.** Dulu cfg `dump=` yang nunjuk ke dump
  Free Fire bikin MLBB dibaca pakai katalog FF (kelas + RVA ketuker) — sekarang
  file yang package-nya beda dilewat dan tool nyari dump game-nya sendiri:
  `[dump] cfg \`dump=\` -> com.dts.freefireth_native_full.cs itu punya game LAIN (target com.mobile.legends) -- dilewat`;
* **cuma file dari folder versi yang sama** yang digabung (RVA antar versi beda,
  digabung = ngaco), dan **kelas kembar dibuang** biar jumlah kelas nggak
  kelihatan dobel;
* **dump setengah jalan dilewat**: file yang kelas-berslot-nya kurang dari ¼
  file terbaik (mis. hasil run pas game baru kebuka: 114 kelas vs 18.626) nggak
  ikut digabung.

```bash
# cara paling gampang: script-nya yang nyusun folder + naikin + tes probe
bash tools/push_dump.sh 192.168.1.61:40729 "/home/panxcz/Documents/kont/dump (2).cs"

# atau manual:
#   /sdcard/panxcz-tools/dumper/com.mobile.legends/22.1.97.12061/dump.cs
```

Aturan yang dipegang: **sel-nya selalu divalidasi runtime.** Sel itu diisi game
saat metadata-usage-nya di-resolve; sebelum itu isinya masih token
(`0x0603d19d`) — dan kelas baru di-ngecek dari `name` (+0x10) + `namespace`
(+0x18) + `image` (harus `*.dll`). Jadi dump basi (game update) **bukan** bikin
crash, cuma bikin kandidatnya ditolak. Kalau ditolak semua, log-nya bilang:

```
[il2/dump] BattleManager: 2 sel dicoba (kuat 0, lemah 1, nggak nyambung 1)
```

Isi file dump yang kepakai dijumlahin di log:

```
[dump] dipakai: dump.cs (34 MB, 18626 kelas berslot)
[dump] gabungan 7 file: 46706 kelas, 23058 punya slot, 169 module (167 MB)
[dump]   + dump.cs
[dump]   + com.mobile.legends_full.cs
[dump]   liblogic.so -> mscorlib.dll (20982)
[dump]   libcsharp.so -> Assembly-CSharp.dll (75420)
```

Soal **pointer ber-tag (MTE)**: `Il2CppClass*` di MLBB disimpan sebagai
`0xb400007a967549d0` (tag di byte paling atas). Semua baca/tulis sekarang
nge-normalisasi alamat (`mem::untag`), jadi validasi `image` dan baca instance
tetap jalan.

## Dump SENDIRI (native, root doang) -- `--dumpout`

Dulu dump cuma bisa dari luar (frida + Python di PC). Sekarang **tool-nya sendiri
bisa nge-dump**, langsung di HP, tanpa frida dan tanpa PC:

```bash
su -c '/data/local/tmp/panxcz_libtool.sh com.mobile.legends:UnityKillsMe \
       --dumpout --all-libs --json --header --ptr --exit'
```

Hasilnya:

```
/sdcard/panxcz-tools/com.mobile.legends/22.1.97.12061/
    com.mobile.legends_native_full.cs        <- dump (format Il2CppDumper)
    com.mobile.legends_native_full.json
    com.mobile.legends_native_full.h         <- tabel siap-patch
    com.mobile.legends_native_full.ptrs.tsv
```

Folder `<pkg>/<versi>` diisi otomatis (package tanpa sub-proses + `versionName`
dari `dumpsys`). Kalau `--all-libs` nggak dipakai, cuma lib yang lagi kepake yang
dipindai, dan nama file-nya `<pkg>_native_<lib>.cs`.

Dari PC (adb) ada helper-nya:

```bash
bash tools/dump_native.sh 192.168.1.61:40729 com.mobile.legends:UnityKillsMe --json --header
PULL=1 bash tools/dump_native.sh 192.168.1.61:40729      # sekalian tarik ke ./dumps/
```

### Gimana caranya (nggak perlu simbol / global-metadata)

1. **Cari tabel class-slot dari bentuknya.** Di module il2cpp ada tabel yang
   isinya `Il2CppClass*`. Tabel itu dipindai per region data module: qword yang
   isinya pointer ke struktur `Il2CppClass` (nama kelas printable di `+0x10`,
   `image` `*.dll` di `+0x00`) dikumpulkan, terus divalidasi penuh supaya sampah
   kebuang (percobaan pertama, cuma cek nama seperti string: 62 rb "kelas" palsu
   dengan nama kayak `" 5?|z"`).
2. **Tiap kelas dibaca** nama / namespace / `image` / `parent` (offset parent
   dicari statistik: offset mana yang isinya paling banyak nunjuk ke kelas lain
   di tabel) / field / method.
3. **Layout `FieldInfo` DIKALIBRASI, bukan di-bake.** Kalibrasi lama (tebak
   stride + token `0x04xxxxxx` di `slot-4`) mentok di MLBB, jadi dipakai
   kalibrasi **struktur**: array yang entry-nya punya STRING nama + pointer balik
   ke kelas itu sendiri, terus dicari di offset berapa ada pasangan
   `{dword offset kecil, dword token 0x04xxxxxx}` (urutan bebas). Hasil live di
   MLBB:

   ```
   [dumpout] layout field : OK ptraddr+0x78 stride 0x20 name+0x0 type+0x8 offset+0x18 token+0x1c [struktur, skor 24]
   ```

   Tabel kelasnya juga udah benar-benar beda dari il2cpp standar: nama di `+0x0`,
   `Il2CppType*` di `+0x8`, pointer kelas di `+0x10`, dan `uint64` di `+0x18` =
   `(token << 32) | offset` -- **token SETELAH offset**, itu yang dulu bikin
   kalibrasi lama nyerah.
4. **Dump juga langsung dipakai balik**: baris `// Class-slot candidates` yang
   ditulis bikin file hasilnya bisa jadi input tool ini sendiri (udah dites:
   `--dump /sdcard/panxcz-tools/.../native_full.cs --probe BattleManager`).

Isi `.h`-nya ada dua tabel: `kPanxczSym` (method + RVA, kosong kalau game-nya
belum bisa dibaca native) dan `kPanxczClassSlot` (RVA cell `Il2CppClass*`, dipakai
buat nge-resolve kelas).

Contoh hasil live (MLBB, layar login):

```
[dumpout] libcsharp.so: 54225 cell kandidat
[dumpout] 6331 kelas dari tabel class-slot
[dumpout] layout field : OK ptr+0x78 stride 0x20 name+0x0 type+0x8 offset+0x18 token+0x1c
[dumpout] selesai: 6331 kelas (6331 slot, 59078 field, 53 module) -> ..._native_full.cs
[dump] ..._native_full.cs: 6331 kelas, 6331 punya slot, 53 module (4 MB)
[il2/dump] BattleManager -> 0x7a7aa14e80  (libcsharp.so + 0x635b290) Assembly-CSharp.dll
[exp] fields: ptr+0x78 stride 0x20 slot 0x18 (layout struktur) -> 45 field
```

### Batasan yang jujur

- **Field statis nggak ditandai** di dump ini: metadata runtime nggak nyimpen
  penandanya (offset instance + token-nya tetap asli).
- **Daftar method belum bisa dibaca native dari MLBB.** Layout `MethodInfo`
  di sana nggak ketemu sama pencari struktur (udah dicoba sampai
  `klass+0x220`, semua stride, plus diagnosa mentah) -- di MLBB `MethodInfo`
  nggak nempel di array kelas dan pointer kodenya ada di tabel terpisah. Jadi
  kalau butuh RVA method, tetap pakai dump metadata (`frida`) atau jalur lama;
  kelas/field/class-slot-nya udah native. Di game il2cpp standar method-nya
  ikut kebaca.
- Jumlah kelas tergantung kondisi game (tabel usage diisi sambil kelas dipakai):
  dump sambil di **lobby/in-match** dapet lebih banyak daripada di layar login.
- Dump native ditulis dengan nama `*_native*.cs` supaya **nggak nimpa** dump
  metadata (`*_full.cs` / `*_libcsharp.so.cs`) -- kalau dua-duanya ada, yang
  slotnya lebih banyak yang dipakai tool.

## Class explorer: jalur runtime (game Unity biasa)

Kalau scan-string berhasil, offset field & RVA method dibaca dari **metadata
il2cpp di memori game** (bukan dari `offsets.h` hasil dump):

1. nama class dicari sebagai string di memori → semua pointer balik discan →
   kandidat `Il2CppClass` divalidasi (image-nya `*.dll`, nama persis);
2. **array `FieldInfo`** dan **array `MethodInfo**`** di dalam `Il2CppClass`
   dikalibrasi sendiri: semua kombinasi (offset pointer di klass, stride entry,
   posisi slot) diuji, yang lolos validasi paling banyak yang dipakai.
   - entry dinyatakan valid kalau `name` string yang wajar,
   - token field harus `0x04xxxxxx`,
   - `MethodInfo.klass` harus menunjuk balik ke class itu;
3. tipe field ditebak dari `Il2CppTypeEnum` (int/uint/long/float/double/bool/ptr/string).

Jadi kalau game update, offset-nya tetap kebaca — selama layout il2cpp-nya masih
versi yang sama (24–31). Semua langkah itu dicetak ke log
(`logcat -s panxcz_libtool`), termasuk hasil kalibrasinya:

```
[exp] fields  : ptr+0x80 stride 0x20 slot 0x14 -> 37 field
[exp] methods : ptr+0xD8 nameOfs 0x10 -> 214 method
[exp] Player::Hero @0x7f2c1a0040 | 37 field, 214 method | Assembly-CSharp.dll
```

### typeId kelas: pool-nya sekarang LENGKAP (bukan cuma 2.066 dari 17.770 tipe)

Awalnya cuma **~2.066 tipe** yang punya daftar method (sisanya bolong karena
`MethodInfo`-nya belum dibikin engine). Sekarang **semuanya kepetakan**, lewat
tiga jalur yang semuanya "cari sendiri saat jalan":

1. **token TypeDef di `Il2CppClass` (+0xfc)** — dicari dengan voting banyak
   kelas, plus geseran `K` **per file metadata** (space 1/2/3 di MLBB beda semua:
   `K=-1`, `K=-1`, `K=371`);
2. **tabel tipe** (`Il2CppClass*[]` yang diindeks indeks tipe) — ditemukan dengan
   vote `base = alamat - indeks*8`, jadi tipenya tetap kepetakan walaupun
   `MethodInfo`-nya belum ada;
3. **record `MethodInfo`** yang emang udah ada.

Hasil di MLBB 22.1.97.12061 (in-lobby):

```
typeId -> kelas: 2892 dari MethodInfo, 3150 dari token, 17346 dari tabel = 20496 tipe
  (tabel record punya 17770 tipe)  -> 17522 dari 17770 tipe kepetakan ke kelas
tabel tipe space 3: 20314 entry, 17346 nyambung (kosong 346, pointer rusak 0,
                    token nggak nyambung 25, udah kepetakan 2587)
gabungan: 24095 kelas, 149575 method  (record 150770, belum ada RVA 129542)
nama method: 28448 -> 45603 (space 2 & 3 ikut kekalibrasi namanya: 1132/1132,
             15815/15815)
```

Jadi **149.575 dari 150.770** record method kebaca (99,2%), dan itu yang bikin
`--forceret` bisa milih method pakai nama kelas + nama method.

### Verifikasi pool: dibedah apa adanya

Dulu log-nya bilang `verifikasi vs dump offline: 0%` — itu **menyesatkan**, bukan
pool-nya salah: dump hasil tool offline di MLBB banyak nulis
`// RVA: unknown (null_entry)` (RVA-nya nggak kebaca offline), jadi pengecekannya
tidak punya bahan. Sekarang log-nya misahin:

```
[pool] verifikasi vs dump (independen): 12/2001 RVA+nama cocok, 1982 nama cocok doang
       (dump nggak nulis RVA-nya); kelas nggak ada di dump 0, dump nggak punya RVA 0,
       beda beneran 6
```

6 yang "beda beneran" itu satu kelas ber-nama `Enumerator` (nama kelas itu ada
belasan di namespace beda, sedangkan yang diketik cuma nama polos) — contohnya
dicetak ke log. Yang penting: **dari 1.982 yang namanya cocok, nggak ada satu pun
RVA yang beda** (`rva ada tapi nama beda: 0`), jadi RVA + nama yang ditulis pool
konsisten.

### Kalau jalur runtime gagal (MLBB)

Class tetap kebaca (dari class-slot), terus **field + method-nya diambil dari
dump** — jadi di menu tetap kelihatan offset & RVA-nya:

```
[il2/dump] BattleManager -> 0x7af09f66c0  (libcsharp.so + 0x635b290) Assembly-CSharp.dll
[exp] fields: kalibrasi gagal (class tanpa field?)
[exp] runtime nggak bisa baca field -- diambil dari dump (45 field, 121 method, module libcsharp.so)
# atau, buat kelas yang emang nggak punya field instance (System.Array, interface,
# enum kosong) -- dulu kelas beginian nggak dapet apa-apa:
[exp] runtime nggak bisa baca method -- diambil dari dump (0 field, 71 method, module liblogic.so)
[exp] klass 0x7af09f66c0 image=0xb400007a967549d0 'Assembly-CSharp.dll'
[exp] static_fields +0xa8 -> 0x7ccc55bc40 (64 byte field statis)
[exp] BattleManager | 45 field, 121 method | Assembly-CSharp.dll | sumber=dump
```

Di menu, baris `sumber:` nunjukin kelasnya ketemu dari mana (`scan` / `dump`),
dan baris `static_fields` nunjukin pointer statisnya. Field statis ditandai `S`
dan nilainya dibaca dari `static_fields` (bukan dari instance).

#### RVA lintas-module (dump v3.4)

Di MLBB kode satu kelas bisa nyebar ke beberapa lib, jadi dumper kita (v3.4)
nulis module pemilik kode di belakang RVA-nya:

```
    // RVA: 0x128EBE0 (liblogic.so)      <- cuma ditulis kalau bukan lib yang di-attach
    int System.Collections.ICollection.get_Count();
```

Parser-nya baca tag itu, dan alamat absolutnya dihitung dari base module itu
(`mem::base(module)`) — jadi method lintas-lib pun alamatnya ketemu, bukan 0:

```
[il2/dump] Array -> 0x7a82d488d8  (libcsharp.so + 0x621e598) mscorlib.dll
[exp] runtime nggak bisa baca method -- diambil dari dump (0 field, 71 method, module liblogic.so)
[probe]   method int System.Collections.ICollection.get_Count() rva=0x128EBE0 @0x7a8f3bfbe0  [liblogic.so]
```

(`0x7a8e131000` = base liblogic di maps + `0x128EBE0` = `0x7a8f3bfbe0` — dicek
langsung di HP.) Tag `[lib]` di menu CLASSES nunjukin RVA-nya bukan di lib yang
lagi dipakai, biar nggak salah patch. Dump lama (tanpa tag) tetap jalan: modulnya
diambil dari baris `// Module n:` kelasnya.

Baca nilai field butuh `Il2CppClass` runtime — dan itu cuma ada kalau game-nya
**lagi jalan di depan** (in-lobby/in-match). Kalau game-nya di background,
statics-nya kebaca 0 (bukan bug: memang belum diisi game).

## Method LENGKAP tanpa "RVA: unknown" (pool MethodInfo)

Di MLBB, `Il2CppClass` **nggak** nyimpen daftar method yang lengkap: array di
`klass+0x90` cuma nunjuk sebagian (nyata: `BattleManager` 20 dari 121). Makanya
dump yang cuma baca array kelas hasilnya bolong-bolong.

Yang dipakai MLBB: semua `MethodInfo` dialokasikan di **satu pool global**,
deret rapat, stride `0x50`. Satu entry:

```
+0x00 methodPointer (alamat kode absolut)     +0x10 Il2CppClass* (pemilik)
+0x28 -> record metadata 0x20 byte            +0x30..+0x4F sisa
```

Record metadata itu **tabel method-definition** yang LENGKAP buat semua kelas:

```
+0x00 index nama (byte atas = nomor file metadata, 3 byte bawah = offset)
+0x04 ID tipe pemilik   (byte atas = nomor file metadata, 3 byte bawah = index tipe)
+0x14 token 0x06xxxxxx  (MethodDef)
```

TypeId-nya kebukti berformat `(nomor file << 24) | index tipe` — live di MLBB
nilainya `0x01000066` .. `0x03004F53`. Yang **nggak** bisa: ngubah typeId jadi
`Il2CppClass*`. Il2CppClass nggak nyimpen index tipe itu (dicek empat tafsir —
mentah, 24 bit bawah, `<<1`, `>>1` — 400 sampel x 0x800 byte: **nol kecocokan**,
lihat `--typeprobe`). Jadi peta typeId tetap dibangun dari `MethodInfo`
(2.6 rb tipe), dan `--typeprobe` dipakai buat nge-verifikasi ulang di game lain.

Jadi jalurnya: pindai heap → kumpulin pool → kelompokkan per kelas lewat
`+0x10`, dan **daftar method-nya dilengkapi dari record metadata** (jadi nama +
token semua method kebaca, walau `MethodInfo`-nya belum dibikin engine).
RVA-nya nyusul: kalau engine udah bikin `MethodInfo`-nya (makin banyak kalau
dump sambil in-match) RVA-nya dari runtime, kalau belum dilengkapi dari dump
offline `--enrich`.

Nama method disimpan **terenkripsi-per-file** di metadata; index-nya di-resolve
sebagai `mapping[space] + (index - delta[space])`. `delta` **nggak di-hardcode**:
di-kalibrasi waktu jalan dari tiga anchor (dump offline kalau ada, tabel
properti kelas, dan rangkaian panjang nama) lalu divote. Contoh hasil live:

```
[pool] record method-def: 154316 record, 18775 tipe pemilik, 2464 kepetakan ke kelas
[pool] 26787 method, 8589 kelas, 39944 punya nama | 465 MB dipindai, fase 0x20
[pool]   space 1 -> global-metadata.dat        (delta 0xfc1ed4, 1245 suara, nama 26268/27445)
[pool]   space 2 -> global-first-metadata.dat  (delta 0x1feaa3c,  182 suara, nama 1135/1135)
[pool]   space 3 -> global-csharp-metadata.dat (delta 0x2d0d86c,  182 suara, nama 10733/10734)
```

Kelas yang punya `MethodInfo` di pool tapi **nggak punya cell class-slot**
sekarang ikut ke-dump (slot-nya kosong). Ini bukan teoretis: kelas
**`AirWall` = tembok MLBB** justru ke-include gara-gara ini (dulu kelewat di
semua dump).

Hasil nyata di HP (BattleManager, dump native sendiri): **121/121 method, semua
RVA keisi** (passing sama dump referensi) — sebelumnya cuma 20 dan sisanya
`unknown`.

```bash
--pool                 # cari + kalibrasi pool terus lapor hasilnya (diagnosa)
--no-pool              # jangan pakai pool (buat banding)
--no-enrich            # jangan lengkapi RVA dari dump offline
--pool-scan 4096       # batas heap yang dipindai (MB, default 2048)
--pool-layouts         # daftar layout MethodInfo yang dicoba
--pool-layout 2        # PAKSA satu layout (buat diagnosa; 2 = il2cpp-2021)
```

### Layout MethodInfo dicari OTOMATIS (nggak hardcode lagi)

Layout di atas itu **MLBB**. Game il2cpp standar (Unity 2020+, nyata di
**Free Fire**) beda: nama method ADA di dalam structnya sendiri:

```
+0x00 methodPointer   +0x18 const char* name   +0x20 Il2CppClass*
```

Dulu layout ini nggak didukung: di FF pool-nya kedeteksi pakai layout MLBB
(kelas "valid" kebetulan), tapi dword yang dibaca sebagai indeks nama itu isinya
**string** — makanya log-nya muncul `space 101/109/116 -> nggak kekalibrasi`
(101/109/116 = huruf `e`/`m`/`t`) dan nama method 0.

Sekarang 6 layout dicoba sekaligus dalam **satu kali pindai heap** (yang dibaca
cuma buffer yang udah ada — nggak ada syscall tambahan), lalu dipilih yang
menang. Yang dipakai buat milih: **entry-nya ada nggak di array method kelasnya
sendiri** (`Il2CppClass+0x98`, stride 8) — bukti mandiri, struktur lain yang
"kebetulan mirip" nggak mungkin nongol di situ. Kalau array kelas nggak bisa
dipakai, baru dicocokin ke dump offline, terus ke jumlah entry.

Hasil nyata di Free Fire (in-lobby):

```
[pool] layout 'MLBB (klass+0x10, record+0x28, stride 0x50)': 0% entry-nya ada di array method kelasnya
[pool] layout 'il2cpp-2021 (klass+0x20, nama+0x18, stride 0x50)': 73% ...
[pool] layout 'il2cpp-2021 (klass+0x20, nama+0x18, stride 0x48)': 77% ...
[pool] layout DIPAKAI: il2cpp-2021 (klass+0x20, nama+0x18, stride 0x48) -- 52516 entry, 52516 nama kebaca
[pool] contoh: List`1::IndexOf rva=0x97ABED0 (libil2cpp.so)
[pool] verifikasi vs dump offline: rva ada tapi nama beda: 0
```

Baris terakhir itu intinya: **kalau RVA-nya ketemu di dump, nama-nya SELALU
sama** (0 beda tulisan). Sisa yang "nggak cocok" semuanya karena kelas/method
itu belum ada di dump (dump lobby memang lebih sedikit dari pool runtime), bukan
karena salah baca.

### RVA sekarang **stabil antar run** (penting buat nge-patch)

RVA = **file offset segmen + jarak di dalam segmen** (`mem::rvaOf()`), bukan
`alamat - mem::base(modul)`. Alasannya: `libil2cpp.so` Free Fire ~1,6 GB dan
segmennya dimap **terpisah** (lihat `/proc/pid/maps`: file offset-nya lompat
`0x0 → 0x3222000 → 0x6f44000 → 0x903f000`), jadi "base" jadi nggak karuan:
method yang SAMA bisa kena RVA `0xE7946260` di satu run dan `0x1B79FED0` di run
lain. RVA - cara lama - cuma valid di run itu, jadi dump-nya nggak bisa dipakai
lagi buat nge-patch. `rvaOf()` kebalikannya (`mem::addrOfRva`) dipakai semua
jalur `module:rva` (patch, `--peek`, `--huntmeth`, resolusi class-slot).
Buat modul yang mapping-nya **nyambung** (MLBB), dua-duanya ngasih angka yang
**sama persis** — jadi nggak ada yang berubah di MLBB.

## Dump sambil IN-MATCH (online dump)

`MethodInfo` baru dibikin engine waktu method-nya kepakai, jadi dump di lobby
= RVA-nya sebagian. Dua cara untuk dump pas match:

- **CLI:** `su -c '... --dumpout --all-libs --json --header --ptr --exit'`
  pas lagi main (dump-nya jalan di proses terpisah, game-nya nggak di-pause).
- **Menu:** tab **Info** → `Start dump` (sekarang) atau `Online dump (di match)`
  (nunggu `BattleManager` instance muncul dulu, terus dump sendiri).

## Menu

Tampilannya disamain dengan UI referensi (Astral V39): **latar hampir hitam**
(`#0F0F0F`), **kartu `#111112`** dengan outline tipis `#202123`, satu warna
aksen hijau `#76BB75`, sudut membulat kecil, dan strip kepala gelap
(`#141517`) setinggi 46 px.

Tata letaknya:

- **kepala** — batang aksen + `PANXCZ LIBTOOL` + chip kanan: `HUD`, `LOG`,
  `X`, `KELUAR` (merah).
- **strip status** — badge proses/pool/dump/stealth + progress bar kerja berat
  (kelihatan di halaman mana pun).
- **sidebar** (158 px) — daftar halaman; yang aktif dikasih latar + batang
  aksen di kiri (gaya referensi).
- **kartu konten** — halaman aktif render di kartu sendiri, jadi ada dua
  bidang terpisah seperti UI referensi.

Semua warna diambil dari satu palet di `ui.cpp` (`kBg`, `kCard`, `kAccent`, …),
jadi ganti tema cukup di blok itu. Semua halaman pakai helper yang sama biar
nggak campur-campur.

**Tab Info** — status attach/lib/dump, kartu pool method (method/kelas/typeId/
MB dipindai, detail kalibrasi di tree), kontrol dump, stealth, keluar.

**Tab Config (Setting)** — tools (log/re-attach/detect lib), **folder dump +
Start scan / Start dump / Online dump / Stop**, dan `Keluar dari tool`.

Kontrol dump:

| tombol | gunanya |
|---|---|
| `Start scan` | hitung kelas/slot/field/method **aja** (nggak nulis file) — buat ngukur keadaan match sekarang |
| `Start dump` | dump penuh sekarang ke folder hasil |
| `Online dump (di match)` | nunggu instance `BattleManager` muncul dulu, baru dump |
| `Stop` | batalin di tengah jalan (`dumpout::requestAbort()`, dicek tiap chunk scan + tiap kelas; nggak nulis file setengah jadi) |
| folder hasil | bisa diketik; `Simpan` / `Default`. Default: `/sdcard/panxcz-tools/dumper/<package>/<versi>/` |

Tab **Scan** juga punya `Start scan`/`Stop scan`: baca ULANG nilai di alamat
hasil scan (bukan pindai memori lagi) tiap frame, jadi kelihatan mana yang
hidup — 512 hasil pertama biar tetap ringan.

## ID tembok & obstacle (dari dump in-match)

Contoh hasil nyata (dump pas MLBB di depan, `--dumpout --all-libs`, 8.943 kelas
/ 73.245 field / 34.306 method). Yang dipakai buat navigasi auto-farm:

| kelas | class-slot | field penting | method (RVA, libcsharp.so) |
|---|---|---|---|
| `AirWall` (tembok) | – (dari pool) | `int m_id` @0x10, `int m_iType` @0x14 | `Contains` 0x4565710, `LineIntersection` 0x456571C, `IsForbidWalkTo` 0x4565724 |
| `MultiAirWall` | 0x62286A0 | `Il2CppObject[] _rects` @0x18 | `Contains` 0x33498C0 / 0x3349A10 / 0x3349AAC, `IsForbidWalkTo` 0x334995C |
| `Muf_AirBlockData` | 0x623DA00 | `bool _bAdd` @0x3C, `int _iId` @0x40, `_iType` @0x44, `double _fLineWidth` @0x48, `_vPos1` @0x50, `_vPos2` @0x60, `int _iCollision` @0x70 | – |
| `Muf_AirBlock` | 0x623E430 | – | – |
| `Blocker` | 0x623E608 | `bool m_blocked` @0x50 | – |
| `BlockerData` | 0x623D988 | `Il2CppObject Port_Out` @0x40 | – |
| `ChangeObstacle` | 0x6233150 | `int m_iIsBaseNode` @0x28, `m_iObstacleIndex` @0x2C, `m_iVisible` @0x30, `m_iInitVisible` @0x34 | 2 method: 0x2BA2864, 0x2BA28F8 |

`Muf_AirBlockData` yang paling kepakai buat auto-farm: tembok di sana berupa
**garis** (`_vPos1` -> `_vPos2`) plus `_fLineWidth` dan `_iCollision`, jadi
jalur joy-nya bisa diuji "nyentuh segmen ini nggak" sebelum tap.

Cari yang lain sendiri:

```bash
python3 tools/frida/run_dump.py --help          # (jalur frida, lama)
# atau langsung di file dump:
grep -n "^class .*\(Wall\|Blocker\|Obstacle\|Terrain\|Bush\)" com.mobile.legends_native_full.cs

## Anti-deteksi (bypass.h) — batasannya jujur

`bypass::apply()` ngelakuin:

1. **nama proses disamarkan** — `prctl(PR_SET_NAME)` **dan** timpa argv, jadi
   `/proc/<pid>/cmdline` + `comm` kebaca `mali_render` (dibuktiin di HP:
   `pgrep panxcz` udah nggak nemu proses tool-nya lagi). Bisa diganti:
   `--proc-name nama`.
2. **file kerja** (cfg/log) dirapikan jadi `0600`.
3. **pantau balik** — tiap ~10 detik tool baca `/proc/<game>/fd` + maps game;
   kalau ada fd/mapping yang nunjuk ke `/proc/<pid-tool>` atau berisi
   `panxcz`/`frida`/`gameguardian`, berarti game kebukti lagi meriksa kita →
   tool **langsung lepas overlay & keluar sendiri** (mode panik).

4. **libanti-cheat nggak pernah dipakai buat scanning** — daftar hitam
   (`kAntiCheatLibs`): `libanogs.so`, `libanort.so`, `libsgmain.so`,
   `libtersafe.so`, `libtprt.so`, `libmisc` dst. Ini penting buat game yang
   anti-cheat-nya nge-hook API il2cpp (Free Fire: `libanogs.so`): lib itu ikut
   kelihatan "kayak il2cpp" walaupun engine-nya belum ke-load sama sekali.
   Dulu tool sempat **salah pilih `libanogs.so` sebagai lib il2cpp** dan
   mulai scan memori anti-cheat-nya — sekarang:

   ```
   [target] cuma library anti-cheat yang ke-load (libanogs.so) -- engine il2cpp belum ada, tunggu game beneran jalan
   [target] nggak nemu lib il2cpp -- tulis manual namanya di tab TARGET
   ```

   (jadi game yang jalan di background / belum masuk lobby nggak diusik.)

5. **log nggak kebanjiran** — auto-mode ngecek target tiap 0,5 detik; pesan
   `gagal attach` di-throttle 10 detik sekali (dulu bisa ratusan baris logcat
   per menit, dan itu sendiri mencurigakan).

Yang **TIDAK** diklaim: nyembunyiin root/Magisk dari game, nge-block anti-cheat
yang jalan di kernel, atau nyembunyiin `/proc/<pid>/maps` (itu hak kernel).
Ini pengurang jejak, bukan jaminan bebas ban.

> Catatan (bukti, bukan tebakan): pas Free Fire-nya mati sendiri, di logcat
> jejak tool-nya **nggak ada sama sekali** sebelum kejadian — tool malah lagi
> gagal attach karena prosesnya udah nggak ada. Yang ketemu justru bug di atas
> (tool mau nge-scan `libanogs.so`), dan itu udah ditutup.

## Dump lib anti-cheat & semua lib game (`--libdump`)

Dump il2cpp cuma ngomong soal kelas/field/method **game**. Yang nggak pernah
kelihatan justru **lib anti-cheat-nya** — padahal itu yang nentuin apa aja yang
harus di-hide. Jadi ada jalur terpisah buat lib-nya.

Dump-nya **keluar sendiri** setelah selesai (`--stay` buat nahan menunya tetap
kebuka). Dulu nggak: `workPending()` masih nganggep kerjaan dump "belum selesai",
jadi tiap sesi ninggalin satu proses nyangkut di HP (kejadian nyata: 9 proses
`mali_render` numpuk).

Semua **read-only**, nggak nulis apa pun ke memori game:

```bash
su -c '/data/local/tmp/panxcz_libtool.sh com.dts.freefireth --libdump --exit'          # engine + semua lib anti-cheat
su -c '/data/local/tmp/panxcz_libtool.sh com.dts.freefireth --libdump-all --exit'      # + lib milik game-nya sendiri
su -c '/data/local/tmp/panxcz_libtool.sh com.dts.freefireth --libdump=libanogs.so --exit'   # pilih sendiri
su -c '/data/local/tmp/panxcz_libtool.sh com.dts.freefireth --libdump-sys --exit'      # ikut SEMUA .so (termasuk sistem)
```

Nyala sendiri juga kalau `--dumpout --all-libs` (matiin pakai `--no-ac`).
Opsi: `--libdump-noimg`, `--libdump-noreport`, `--libdump-out=<dir>`,
`--ac-imgmax=<MB>` (per lib, default 768), `--ac-imgtotal=<MB>` (total semua lib,
default 1024 — lewat batas, gambar memori dilewat tapi laporannya tetap ditulis).

Hasilnya di `/sdcard/panxcz-tools/dumper/<pkg>/<versi>/ac/`, per lib 6-9 file:

| file | isinya |
|---|---|
| `<lib>.img` | gambar memori susunan **file offset** (rva = offset file, base 0) — bahan mentah buat IDA/Ghidra/r2 |
| `<lib>.regions.txt` | peta segmen: alamat runtime ↔ file offset |
| `<lib>.txt` | laporan: segmen, NEEDED, export, import, **API yang dipanggil**, **constructor**, **fungsi**, string mencurigakan |
| `<lib>.json` | versi mesin-baca dari laporan di atas |
| `<lib>.strings.txt` | semua string yang kepindai |
| `<lib>.fns.txt` | daftar rva **semua fungsi** + ukurannya (jarak ke fungsi berikutnya) |
| `<lib>.fns.r2` | script **radare2** — langsung jadi semua fungsinya, tanpa IDA |
| `<lib>.fns.ghidra.py` | post-script Ghidra (img di-import raw binary, base 0) |
| `<lib>.fns.map` | peta alamat+nama (kode & data/slot GOT) buat tool atau editor apa pun |

**Nggak perlu IDA.** `.img` itu isi memori yang disusun pakai **file offset**
(rva = offset di file, base 0), jadi nggak ada rebase-rebasean, dan `.fns.*` udah
bikin semuanya langsung kepakai — termasuk buat lib anti-cheat yang section
header-nya dihapus sama packer-nya (di Free Fire: 28 entri section isinya
nol/garbage).

Bukti (hasil dump `libanogs.so` dites di PC ini):

```
$ python3 -m py_compile libanogs.so.fns.ghidra.py   -> lolos
$ r2 -q -a arm -b 64 -i libanogs.so.fns.r2 libanogs.so.img
  -> 'pd 6 @ 0x1b12c0' keluar: bti c / adrp x0, 0x505000 / add x0, x0, 0x950
                               b sym.imp.__cxa_finalize   (ARM64 yang bener)
  -> 'aflc' 402 fungsi dari 60 baris 'af'; 2000 sampel rva pertama di .img
     semuanya bukan nol
```

Cara paling cepat:

```bash
# radare2 -- fungsi + nama langsung dari .fns.r2 (.img itu raw: arch manual)
r2 -q -a arm -b 64 -i libanogs.so.fns.r2 libanogs.so.img
# di dalam r2: pd 5 @ sym.AnoSDKInit / axt @ sym.AnoSDKInit / af @ 0x1b12c0

# Ghidra: import .img sebagai Raw Binary (language ARM/AARCH64, base 0),
# terus jalanin libanogs.so.fns.ghidra.py dari Script Manager
```

Baca laporan tanpa tool apa pun juga bisa: `<lib>.txt` (manusia) dan
`<lib>.json` (mesin) — segmen, NEEDED, export, import, API yang dipanggil,
constructor, jumlah fungsi, dan string mencurigakan semuanya sudah di situ.

Yang bikin dump ini kepakai (bukan cuma kumpulan byte):

- **API yang dipanggil** dari relokasi (RELA/JMPREL): nama fungsinya tetap kepas
  walau `.dynsym`-nya dibuang, plus rva slot GOT-nya masing-masing.
  Di `libanogs.so` hasilnya: `ptrace`, `inotify_init`, `inotify_add_watch`,
  `dl_iterate_phdr`, `prctl`, `syscall`, `popen`, `execl`, `fork`, `kill`,
  `access`, `stat`, `statfs`, `readlink`, `opendir`, `readdir`, `mprotect` —
  itu daftar alat deteksinya, langsung kebaca.
- **Constructor** (`DT_INIT` + `DT_INIT_ARRAY`): fungsi yang jalan saat lib
  di-load — di lib anti-cheat dari sini dia nyalain thread pemantau + pasang hook.
- **Daftar fungsi** dari `.eh_frame_hdr`: nggak bisa di-strip (exception C++
  butuh itu), jadi lib yang di-strip total pun tetap kebaca **semua fungsinya**
  (`libanogs.so` 16.202 fungsi, `libanort.so` 2.139, `libunity.so` 59.332).

### Yang ketemu (Free Fire `com.dts.freefireth`, 1.132.1, in-lobby)

```
libanogs.so : export 21, import 216, string 10145 | JEJAK TOOL 0, VEKTOR 3 | ctor 64, API 261, fungsi 16202
libanort.so : export  5, import 145, string  1981 | JEJAK TOOL 0, VEKTOR 3 | ctor  4, API 152, fungsi  2139
libil2cpp.so: export  0, import 297, string 79505 | JEJAK TOOL 1, VEKTOR 5 | ctor  1, API 314, fungsi 457820
libunity.so : export  5, import 414, string 72500 | JEJAK TOOL 0, VEKTOR 26 | ctor 0, API 436, fungsi 59332
libcrashlytics-common.so, libcrashlytics.so, libFirebaseCppApp-12_10_0.so,
libunity_encoder_plugin.so, libmain.so -- ikut ter-dump (--libdump-all)
```

Catatan teknis yang penting (dulu bikin semuanya kosong):

1. **Lib anti-cheat Free Fire di-load loader SENDIRI ala packer.** Nilai di
   `PT_DYNAMIC` dan `r_offset` relokasinya masih **vaddr mentah**, bukan alamat
   absolut (`--peek libanogs.so:0x50a9e8` → tag pertama `01 … 23 0A 00 00` =
   `DT_NEEDED` 0x0A23). Tool sekarang coba dua-duanya dan nulis ke log yang mana
   yang dipakai. Sebelum ini: `export 0, import 0, ctor 0, API 0`.
2. **`PT_DYNAMIC` dicari lewat peta segmen, bukan `base+vaddr`.** Di
   `libunity.so`/`libil2cpp.so` segmennya dimap terpisah (selisihnya 0x8000),
   jadi `base+vaddr` ngasih alamat yang isinya nol — tag pertamanya kebaca
   `DT_NULL` dan seluruh export/import hilang. Sekarang vaddr-nya diubah ke file
   offset, terus dicocokin ke region `/proc/<pid>/maps`.
3. **Angka RVA di laporan nggak lagi 0x0.** `r_offset` relokasi juga masih vaddr
   mentah di lib-nya, jadi slot GOT-nya harus ditafsir dulu.

## Ringkasan penutup + rapihan output & UI

Setiap kerja yang **selesai** (dump / scan / dump lib) sekarang ditutup satu blok
ringkasan, jadi hasilnya nggak perlu dicari-cari di 100-an baris logcat:

```
[ringkas] ====== RINGKASAN ======
[ringkas] target  : com.mobile.legends:UnityKillsMe (pid 31891)
[ringkas] lib     : libcsharp.so  |  dump offline: dump.cs +6
[ringkas] pool    : 25217 method, 5704 kelas, 28434 punya nama (MLBB (klass+0x10, record+0x28, stride 0x50))
[ringkas] dump    : 5730 kelas, 5166 slot, 52975 field, 86577 method, 67 module
[ringkas] hasil   : /sdcard/panxcz-tools/dumper/com.mobile.legends/22.1.97.12061/com.mobile.legends_native_full.cs
[ringkas] lib ac  : 12 lib -> /sdcard/panxcz-tools/dumper/com.mobile.legends/22.1.97.12061/ac
[ringkas] waktu   : 55 detik
[ringkas] ========================
```

Yang lain yang dirapihin di output:

* prefix disamain (`detektor:` / `jejak tool:` / `ctor:` — dulu `??` / `!!` / `>>`),
  dan string panjang **dipotong** (dulu satu blob 3 KB bikin satu baris log
  nutupin puluhan baris);
* baris kembar dihapus (`[libdump] selesai:` doang, nggak dobel sama `hasil:`),
  `[libdump] selesai` sekarang sekalian nyebut total MB-nya;
* baris contoh pool dipendekin (buang `reg=<path panjang>+0x...` yang bikin
  satu baris ~200 karakter);
* jumlah kelas berslot yang belum kehitung ditulis `-` (dulu `-1`).

**Urutan keluar yang bener:** kalau `--dumpout --all-libs` dipakai, tool nggak
keluar setelah dump utama — sekarang nunggu dump lib anti-cheat selesai dulu,
baru keluar. Sebelum ini thread utamanya `_exit` di tengah jalan, jadi dump
`ac/` cuma kebuka judulnya (nyata kejadian). Di jalur keluar juga ada jeda
terbatas (300 detik kalau pakai `--exit`, 3 detik kalau tombol X/KELUAR ditekan).

**Rapihan UI (menu ImGui):**

* **kepala jendela**: chip `HUD / LOG / X / KELUAR` lebarnya dihitung dari teks
  aslinya dan rata kanan, plus mode sempit (`H`/`L`/`!`) — dulu offset-nya tetap
  `250 px`, jadi di jendela kecil chip-nya nabrak judul;
* **strip status** nambah `pid` + `pool N` + `dump N kelas` dan otomatis turun
  baris kalau jendelanya sempit;
* **sidebar**: lebarnya ikut jendela (132–176 px) + penanda halaman `MENU 3/7`;
* **log**: tombol **Simpan ke file** (`/sdcard/panxcz-tools/libtool.log`, path
  langsung kekopi ke clipboard), kotak **filter**, **auto-scroll** ke baris
  paling baru, tombol **Bersihin**;
* tab **INFO**: kartu status nambah baris folder hasil + keterangan "digabung
  dari N file" kalau dump offline-nya hasil gabungan.

## Batasan (jujur)

- **Butuh root.** Tanpa root, `/proc/<pid>/mem` nggak kebuka dan menunya kosong.
- Tool ini **baca/tulis memori**, bukan hook. Kalau game-nya ngecek integritas
  kode, patch byte di tab PATCH bisa ketahuan — itu risiko pemakai.
- **Sudah diuji di HP** (SM-A155F, Android 15, root KSU): attach auto nyasar ke
  proses yang bener (MLBB → `:UnityKillsMe`), lib engine ketemu (`libcsharp.so`,
  kandidat stub `libil2cpp.so` dibuang), **kelas MLBB kebaca** lewat tabel
  class-slot dump (`BattleManager`, `SystemData` — name + `Assembly-CSharp.dll`
  terverifikasi dari live), field/method-nya kebaca dari dump, dan keluar bersih
  (`exit code 0`).
- Yang **belum** dibuktiin: nilai field statis saat game-nya beneran in-lobby/
  in-match. Waktu dites, proses MLBB-nya di background (layar HP-nya lagi di app
  lain) jadi statics-nya nol — `static_fields` +0xa8 sendiri udah kecocokan
  sama hasil verifikasi tool MLBB yang lain. Cek ulang: buka lobby, terus
  `--probe BattleManager --exit` → lihat baris `[probe] value`.
- Kalau dump-nya nggak ada, MLBB cuma bisa lewat scan-string — yang di sana
  sering gagal (makanya dump jadi wajib buat MLBB).
- **Nama + daftar method** sekarang nggak butuh dump (kalibrasi delta nama jalan
  sendiri). Yang masih butuh dump cuma **RVA method yang belum dibikin engine**
  (`--no-enrich` buat lihat bedanya); di-match jumlah itu turun banyak.
- Method `generic`/`inflated` (MethodSpec) cuma muncul kalau engine udah bikin
  versi instansiasinya — itu juga lebih lengkap kalau dump sambil in-match.
- Pool method dibaca sekali per sesi (cache di RAM). Game restart = baca ulang.

## Kalau gagal di HP/device lain (dan tombol Stop)

Ini yang bikin beda antara "jalan di HP gua" dan "error mulu di HP lain":

- **Baca memori blok besar.** `pread` ke `/proc/<pid>/mem` dan
  `process_vm_readv` nggak sama perilakunya di tiap kernel/ROM (process_vm_readv
  sering diblokir SELinux). Dulu satu blok 4 MB yang gagal = satu chunk penuh
  dilewat, jadi hasil pindainya nol kelas tanpa sebab yang kelihatan. Sekarang
  blok besar **dipecah sampai 64 KB** dan bagian yang nggak kebaca di-nol-kan;
  baca kecil (struktur) tetap ketat. Angka `baca memori gagal` ada di menu
  **Info → STATUS**.
- **Region heap beda nama.** Pool MethodInfo ada di heap: `libc_malloc`
  (Android lama), `scudo` (default Android 11+), `jemalloc`, `tcmalloc`. Dulu
  yang dikenali cuma `libc_malloc` — di HP ber-scudo, tahap pertama nggak nemu
  apa-apa sehingga tahap kedua nge-scan SEMUA region writable (lambat banget).
- **Pencarian region.** `regionOf()` (dipanggil buat tiap qword kandidat) dulu
  linear atas semua region maps → jadi penyebab utama dump/scan "jalan >10
  menit". Sekarang binary search + saringan murah (nilai, sejajar 8, region
  data bukan kode) sebelum makan syscall.
- **Batas tabel record method-def DENGAN KLASTER (ini bug "dump nggak pernah
  selesai").** Panjang tabel record dulu dihitung dari min..max alamat record
  yang ketemu, terus disisir 0x20 per langkah. Satu pointer *outlier* saja sudah
  cukup bikin rentangnya ratusan MB–GB, dan loop-nya nyisir ujung ke ujung —
  praktis nggak pernah selesai (nyata di MLBB in-lobby: rentangnya 1,4 GB; dump
  yang sama di layar login cepat). Sekarang alamat record **diklaster** dulu
  (tabelnya satu blok berdekatan), blok terpadat yang dipakai, sisanya dianggap
  outlier, dan lebar maksimumnya 64 MB. Hasil live di MLBB: dari nggantung →
  `baca tabel record ... (4 MB)` lalu lanjut sampai `exit code 0`.
- **Progress bar.** Semua kerja berat ngirim fase + persen ke `abortx`, jadi
  header menu nampilin bar (persentase kalau terukur, bar jalan-jalan kalau
  nggak) + nama fase + sudah berapa detik. Ada di semua tab.
- **Nggak ada lagi kerja yang jalan tanpa batas.** Kalibrasi layout punya
  **budget 45 detik**, kalibrasi nama (anchor) **40 detik**; kalau habis, yang
  udah kedapet tetap dipakai dan alasannya masuk log. Pindai pool nge-log
  progres tiap 256 MB.
- **Tiap fase dicatat lamanya** (`[dumpout] fase '...': N s`), jadi dari logcat
  langsung kelihatan fase mana yang lambat.
- **Stop = satu tombol buat semua.** `abortx` dipakai dumper, pool MethodInfo,
  kalibrasi layout (explore), dan memory scanner — dulu Stop cuma nge-set flag
  dumper, makanya "ngeklik Stop tapi jalan terus".
- **Scan nggak lagi ngeblok UI.** "First Scan" jalan di thread sendiri (dulu
  di fungsi gambar → overlay beku & Stop nggak bisa diklik).
- **Watchdog worker.** Kerja berat nge-denyut tiap fase; kalau 60 detik nggak
  ada denyut, UI lepas kuncinya sendiri + nulis alasannya (nggak ada lagi state
  "sedang dump" yang nyangkut).

### Daftar method yang kosong diisi dari dump offline

Pool runtime cuma punya MethodInfo buat method yang udah dibikin engine. Kelas
kaya `BattleManager` bisa nongol di dump tanpa satu method pun kalau pool-nya
belum punya entry-nya (nyata kejadian: 0 dari 121). Sekarang kalau satu kelas
masih kosong setelah pool + array runtime dicoba, daftar method-nya diambil
dari **dump offline** (versi game yang sama): nama + RVA + module, formatnya sama
persis, jadi tetap kebaca balik sama tool ini.

```
[dumpout] +5717 kelas yang daftar method-nya diambil dari dump offline
[dumpout] selesai: 8164 kelas (7726 slot, 68749 field, 109917 method, 75 module)
                          ^ 30.349 sebelum ini -- BattleManager dapat 121 method
```

Matiin kalau nggak mau: `--no-dump-methods`.

### Nutup paksa / nyari prosesnya

Stealth aktif secara default, jadi nama prosesnya **bukan** `panxcz_libtool`
(argv-nya ditimpa) — nyari pakai itu nggak ketemu:

```bash
su -c 'ps -A -o PID,NAME | grep mali_render'     # nama samaran default
su -c 'kill -9 <pid>'                            # atau lewat tombol KELUAR di menu
```

Nama samarannya bisa diganti: `--proc-name <nama>` atau di tab Config.

### Diagnosa: `--check`

Kalau dump/scan gagal, jalanin ini dulu — hasilnya langsung ke logcat:

```bash
su -c '/data/local/tmp/panxcz_libtool.sh com.mobile.legends:UnityKillsMe --check --exit'
# atau dari PC:
#   bash tools/dump_native.sh <serial> --check
```

Yang dicetak (contoh di HP yang jalan normal):

```
[check] euid=0 (root=ya)
[check] proses=com.mobile.legends:UnityKillsMe pid=26930 fd=rw region=4213
[check] region: 3890 kebaca, 2401 writable, 812 >1MB, total 11042 MB
[check] baca 4 MB @0x...: OK utuh
[check] VERDICT: baca memori normal -- ...
```

Kalau di HP lain `fd=GAGAL`, `region=0`, atau `VERDICT: sebagian blok besar
nggak kebaca`, penyebabnya kelihatan dari baris itu (dan tool tetap nyoba lewat
jalur pecah-64KB). **Dump yang gagal sekarang otomatis nyetak blok `[check]`
ini juga** — jadi cukup kirim logcat-nya, nggak perlu nebak.
- Offset/metadata il2cpp cuma ada di game yang memang **Unity + il2cpp**
  (bukan Unity Mono, bukan Unity "custom engine" tanpa API il2cpp).
- Segala risiko akun/ban ditanggung pemakai.

## Force-return: hasil tes di HP (MLBB 22.1.97.12061, live)

```
[force] BlurRuntimeParamsProxy::GetParamsEnable @ 0x7a88acab68 <- bool (false)
        (8 byte, terpasang) -- stub: 00 00 80 52 C0 03 5F D6
[force] BlurRuntimeParamsProxy::GetParamsEnable: 1 method lewat pool (rva+module dari metadata)

# catatannya
libfirst.so:0x6bfb68 | 00 00 80 52 C0 03 5F D6 | 08 80 40 39 68 00 00 35 | BlurRuntimeParamsProxy::GetParamsEnable | bool (false)
                     ^ stub (mov w0,#0; ret)     ^ byte ASLI: ldrb w8,[x0,#0x20]; cbnz w8 -> itu bentuk asli getter bool

# sesi BARU (beda proses, ASLR beda) -> restore dari catatan:
[force] restore BlurRuntimeParamsProxy::GetParamsEnable @ 0x7a88acab68 (--unforce)
-> dibalikin & catatannya dibersihin

# restore tanpa catatan (langsung dari library di disk):
[force] libfirst.so:0x6bfb68 (8 byte) dibalikin dari
        /data/data/com.mobile.legends/app_libs/libfirst.so @0x7a88acab68 -- cocok
```

Method-nya dipilih lewat **pool** (`--forcefind` nyari di 24k kelas / 149k method),
jadi tetap kepakai di MLBB yang metadata-nya dipecah 3 lib (di sana jalur
"cari nama kelas di memori" nggak jalan).

## Status build

**v1.2 (UI + anti-cheat lib)** — `out/panxcz_libtool.sh`
md5 `2dedbc68be388c7dc8e18f23db22fd86`. Yang dites di HP:

- overlay baru jalan (screenshot portrait: strip kepala `#141517` + batang
  aksen 15 px dari tepi jendela, kartu `#111112` dominan, aksen `#76BB75` di
  judul/sidebar) — dicek dari `screencap` + hitung warna per piksel.
- **semua 7 halaman render** (`--ui-cycle 2`, screenshot tiap 2,2 s): md5 area
  konten beda-beda tiap halaman, dan batang aksen sidebar pindah persis 54 px
  per item (389→443→497→605→659). Nggak ada FATAL/SIGSEGV/panic.
- Free Fire **lagi jalan penuh** (`libil2cpp.so` 477 MB + `global-metadata.dat`
  + `libunity.so` + `libanogs.so`): tool milih **libil2cpp.so** (bukan anogs),
  `--scan-only` selesai ~35 s → 6891 kelas / 48436 field / 192059 method,
  nggak nulis file, dan FF-nya tetap hidup.
- Free Fire versi background (cuma `libanogs.so`/`libanort.so` yang load):
  tool **nolak** lib anti-cheat, nggak nge-scan apa-apa.

**v1.6 (build jadi payload + dump lib tetap `.img`)** — md5 `d93edb987f437ee78097a12f4bb23163`
(sama buat `out/panxcz_libtool` `.elf` `.sh` — tiga nama, satu biner).

- **Dump lib nggak diubah jadi `.elf`.** Sempat dicoba (nulis ELF utuh: section per
  segmen + `.symtab`), terus **dibatalkan/di-revert** atas permintaan: yang dimau
  adalah *build*-nya bisa dipakai sebagai `.sh` (ELF executor), bukan output
  dumper-nya. Sekarang `--libdump` balik ke `.img` + `.fns.*` + laporan (v1.5),
  dan opsi `--libdump-noelf` udah nggak ada lagi.
- Build nambah `out/panxcz_libtool` (tanpa ekstensi, 755, payload ala
  `assets/Fucker`) di samping `.sh` dan `.elf`. Dites di HP: `su -c
  /data/local/tmp/panxcz_libtool ...` jalan (bukan cuma yang `.sh`).
- `file out/panxcz_libtool` → `ELF 64-bit LSB pie executable, ARM aarch64`.
- **ELF executor `.sh` dites sebagai payload app** (pola HKBOSS: asset → copy ke
  `filesDir` → `chmod 777` → `su -c <path>`): asset `Fucker.sh` (isi ELF aarch64,
  `7f45 4c46`) dites di HP persis urutan itu → proses hidup (`29306 Fucker.sh`).
  Paket siap-kirim: `out/HKBOSS-sh-payload.zip` (project HKBOSS dengan payload
  `.sh` + `payload-opsional/panxcz_libtool.sh` + `README-SH-PAYLOAD.md`).

**v1.5 (keluar sendiri + file pendamping tanpa IDA)** — md5 `d93edb987f437ee78097a12f4bb23163`.
Dites di Free Fire: `--libdump=libanogs.so --exit` selesai lalu **prosesnya hilang
sendiri** (0 proses nyangkut, sebelumnya 9 numpuk). File pendamping buat
`libanogs.so` diverifikasi: `.fns.ghidra.py` lolos `py_compile`, dan r2 (2.9.x)
benar-benar bikin fungsinya dari `.img` + `.fns.r2` — `pd 6 @ 0x1b12c0` keluar
`bti c / adrp / add / b sym.imp.__cxa_finalize` (kode ARM64 yang bener), 2000
sampel rva pertama semuanya nunjuk ke isi `.img` yang bukan nol.

**v1.4 (dump lib + logic anti-cheat)** — `--libdump` / `--libdump-all` /
`--libdump-sys` / `--libdump=<lib,...>`. Dites di Free Fire in-lobby: 9 lib milik
game ke-dump (`/sdcard/panxcz-tools/dumper/com.dts.freefireth/1.132.1/ac/`, 44 MB
termasuk gambar memori), 1.551 lib sistem dilewat (bukan punya game) dan itu
ditulis di log; FF tetap hidup sesudahnya, 0 proses tool nyangkut. Detail +
contoh hasil ada di bagian **"Dump lib anti-cheat & semua lib game"** di atas.

**v1.3 (layout pool otomatis + RVA stabil)** — md5 `060dae8af654f01f6f2af4d2e602da7d`.
Dites di Free Fire `com.dts.freefireth` in-lobby (`--pool --dump <dump>`):

- pool **52.516 method, 52.516 punya nama** (sebelumnya 36.465 method, **0 nama**).
  Layout milih sendiri: `il2cpp-2021 (klass+0x20, nama+0x18, stride 0x48)` —
  77% entry-nya ada di array method kelasnya, sementara layout `MLBB` dan
  `il2cpp-2019` kena **0%** (jadi jelas mana yang bener).
- verifikasi silang ke dump offline: **kok kalau RVA-nya ketemu, namanya SELALU
  sama** (`rva ada tapi nama beda: 0`). Yang nggak ketemu memang kelas/method
  yang belum ada di dump lobby itu.
- dump FF baru (rva = file offset segmen) bisa dibaca balik sama tool sendiri, dan
  RVA-nya nggak lagi berubah antar run (sebelumnya `0xE7946260` vs `0x1B79FED0`
  buat method yang sama) — jadi offset-nya bisa dipakai buat nge-patch.
  Dump lama (rva relatif base) dicadangkan jadi `..._oldbase.cs/.json` dan
  sengaja nggak ikut kepilih lagi oleh auto-load.

> Tip tes UI: tap dari `adb shell input tap` **nggak nyampe** ke overlay ini
> (overlay baca perangkat sentuh langsung, bukan lewat InputManager). Jadi buat
> ngecek halaman pakai `--page N` / `--ui-cycle`; klik beneran pakai jari.


Diuji ulang di HP (SM-A155F, Android 15, root KSU) setelah v1.5 — MLBB
`com.mobile.legends:UnityKillsMe` **pas lagi in-match (practice)** dan in-lobby,
dump penuh 5 lib (`--dumpout --all-libs --json --header --ptr --exit`):

```
[dumpout] fase 'pindai class-slot': 5.3 s      75669 cell kandidat (5 lib)
[dumpout] fase 'kalibrasi layout': 19.6 s      field OK (struktur) / method GAGAL (MLBB: pakai pool)
[pool] tabel record diklaster: ... (13402 record, 4 MB) -- outlier 1,4 GB dibuang
[pool] baca tabel record 0x...(4 MB)           <- dulu nggantung di sini
[pool] 26446 method, 8116 kelas, 38218 punya nama
[pool]   space 1 -> global-metadata.dat (delta 0xfc1ed4, 25830/27104 nama)
[pool]   space 2 -> global-first-metadata.dat (1101/1101)
[pool]   space 3 -> global-csharp-metadata.dat (11287/11287)
[dumpout] fase 'pool method': 30.9 s
[dumpout] +5717 kelas yang daftar method-nya diambil dari dump offline
[dumpout] selesai: 8164 kelas (7726 slot, 68749 field, 109917 method, 75 module)
[dumpout] hasil: /sdcard/panxcz-tools/dumper/com.mobile.legends/22.1.97.12061/..._native_full.cs
```

Total ~58 detik (dulu nggak pernah selesai). Hasil yang diverifikasi di dump itu:

- `BattleManager`: 45 field runtime (token persis `0x4007C74` dst, sama kaya
dump pembanding) + **121 method dengan RVA** (`SetPreInvite` 0x32C6BC8,
`isPreInviteSuccess` 0x32C6C40, `get_m_iAllyTower1Count` 0x32C6CC8, ...).
- Tembok/obstacle: `AirWall`, `MultiAirWall`, `Blocker`, `Muf_AirBlockData` —
  semuanya kebaca (lihat §ID tembok).
- Dump-nya bisa dibaca BALIK sama tool sendiri: `--dump <file>` →
  `8164 kelas, 7726 punya slot, 75 module` + probe `BattleManager` tetap 121 method.

Diuji juga di game Unity il2cpp lain di device yang sama (biar MLBB-nya nggak
perlu dibuka berulang):

```
[dumpout] libil2cpp.so: 11371 cell kandidat (total 11371) [gagal baca=1]     0.0 s
[dumpout] fase 'pindai class-slot': 5.8 s
[dumpout] layout field : OK ptraddr+0x80 stride 0x20 ... [struktur, skor 13]
[dumpout] layout method: OK ptr+0x98 stride 0x8 ...      [kalibrasi lama, skor 24]
[dumpout] fase 'kalibrasi layout': 6.7 s
[pool] pindai tahap 1: 256 MB, 0 kandidat          <- progres tiap 256 MB
[pool] pindai tahap 2: 1026 MB, 185345 kandidat
[dumpout] fase 'pool method': 20.9 s
[dumpout] selesai: 150 kelas (150 slot, 437 field, 1308 method, 27 module)
[dumpout] hasil: /sdcard/panxcz-tools/dumper/<pkg>/<versi>/..._native_libil2cpp.cs
```

(dites di satu game Unity il2cpp lain di device yang sama, biar MLBB-nya nggak
perlu dibuka: fase, progres, kalibrasi, dan tulis file semuanya jalan; keluar
bersih `exit code 0`). Kalau di HP lain gagal, kirim log `[dumpout]`/`[check]`
— tiap fase sekarang ada waktunya, jadi penyebabnya bisa ditunjuk langsung.


```
out/panxcz_libtool.sh   ELF arm64, ~1,6 MB   (v1.7: + RINGKASAN penutup tiap
                                              kerja, + output log dirapihin
                                              (prefix konsisten, string panjang
                                              dipotong, baris kembar dibuang),
                                              + dump offline DIGABUNG dari semua
                                              file versi yang sama (MLBB: 7 file
                                              / 167 MB -> method 28.039 jadi
                                              86.577), + dump game lain & dump
                                              setengah jalan dilewat, + keluar
                                              nunggu kerjaan kelar (dump lib
                                              anti-cheat nggak kepotong lagi),
                                              + UI: chip header rata kanan,
                                              status bisa turun baris, sidebar
                                              ikut jendela, log ada simpan/filter,
                                              md5 efbb2404d16d1d2cb5b29236d41d660b
                                              (v1.6: + payload .sh/.elf/nama polos
                                              + dump lib anti-cheat + .fns.r2 /
                                              .fns.ghidra.py / .fns.map)
                                              (v1.5: + read blok besar anti-gagal,
                                              + regionOf binary search &
                                              saringan pointer (dump/scan jauh
                                              lebih cepat di device lambat),
                                              + satu tombol Stop buat SEMUA
                                              kerja berat + budget waktu per
                                              fase, + worker scanner (UI nggak
                                              beku), + watchdog worker,
                                              + kelas outlier di tabel record
                                              method-def dibuang (bug "dump nggak
                                              pernah selesai"), + progress bar
                                              fase/persen/detik di header,
                                              + --check / tombol "Cek lingkungan")
                                              + MODE OTOMATIS (tanpa argumen,
                                              naik root sendiri, RUNTIME PATCH,
                                              engine=<pkg>:UnityKillsMe di cfg,
                                              dites: MLBB + Free Fire),
                                              md5 ed24086adb81099a35387ede933926be
                                              (v1.4: + kelas dari pool method ikut
                                              ke-dump (AirWall/tembok dulu
                                              kelewat), + folder hasil
                                              /sdcard/panxcz-tools/dumper/...,
                                              + Start scan / Stop, + tema UI baru
                                              + typeId encoding (--typeprobe))
```

**v1.7 (force-return value + pool typeId lengkap)** — `out/panxcz_libtool.sh`
md5 `39d067b3302d31c1f4f7b6a140960bac`. Yang dites di HP (MLBB 22.1.97.12061
lagi jalan, pid 17288):

- **typeId kelas dikejar sampai penuh**: tabel tipe ketemu di space 3
  (20.314 entry, 17.346 nyambung) + token TypeDef `+0xfc` -> **20.496 tipe
  kepetakan dari 17.770**, dan **149.575 method** kebaca (dari 150.770 record).
  Nama method 28.448 -> 45.603 (space 2 & 3 ikut kekalibrasi).
- **Verifikasi pool dibedah apa adanya** (bukan lagi "0% cocok" yang
  menyesatkan): 1.982 nama cocok dari 2.001 sampel, 6 mismatch semuanya kelas
  `Enumerator` (nama kembar antar namespace), 0 RVA yang beda.
- **Dump gabungan diperbaiki**: dump hasil tool lain yang isinya
  `// RVA: unknown (null_entry)` nggak lagi nimpa kelas yang RVA-nya nyata
  (873 kelas diganti; nama method yang kepetakan 28.448 -> 45.603, dampaknya
  kelihatan di `--forceret` juga).
- **FORCE RETURN VALUE** (baru, lihat bagiannya): stub AArch64 disusun sendiri
  (`movz/movk/fmov` + `ret`), diuji byte-per-byte sama assembler asli. Dites
  hidup di MLBB: `BlurRuntimeParamsProxy::GetParamsEnable=false` -> byte
  `00 00 80 52 C0 03 5F D6` terpasang & terverifikasi, byte asli
  (`08 80 40 39 68 00 00 35`) kesimpen, terus dibalikin di sesi baru dari
  catatannya, dan diuji juga jalur restorasi dari file `.so` di disk
  (`--fixcode`, diverifikasi "cocok").
- **UI**: tiap baris method di tab CLASSES ada tombol `ret` -> popup pilih tipe
  (void/bool/int32/uint32/int64/float/double/object-null) + atur nilai pakai
  tombol (nggak butuh keyboard) + `APPLY`/`BALIKIN`; method yang kepasang
  keliatan hijau. `--ui-cycle` jalan bersih, keluar `exit code 0`, 0 proses
  nyangkut.

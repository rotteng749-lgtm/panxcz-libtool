#pragma once

namespace ui {

void init();              // tema + state awal (dipanggil setelah ImGui ready)
void draw();              // render menu + HUD
bool visible();
void setVisible(bool v);
void handleKeys();        // volume +/- : minimize / expand / tutup
void shutdown();

// Tombol "Keluar" di menu: minta loop utama berhenti lalu tool nge-unload
// dirinya sendiri (surface, touch, detach) dan prosesnya balik ke shell.
void requestExit();
bool exitRequested();

// Halaman sidebar aktif (1=Info ... 7=Config). Dipakai `--page N` dan
// `--ui-cycle`: buat ngecek tiap halaman render bener tanpa harus nge-tap
// (tap dari `adb shell input` nggak nyampe ke overlay ini -- dia baca
// perangkat sentuh langsung).
void setPage(int n);
int  page();
void setCycle(bool on, float seconds = 2.0f);

}  // namespace ui

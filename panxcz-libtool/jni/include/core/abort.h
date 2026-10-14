// ============================================================================
//  abort.h - BATAL BERSAMA + DENYUT PROGRES
//
//  KENAPA
//  ------
//  Tombol Stop dulu cuma nge-set flag di dumper. Padahal kerja beratnya nggak
//  cuma di situ: pindai pool MethodInfo (metapool), kalibrasi layout
//  (explore), dan memory scanner punya loop sendiri -- jadi Stop kelihatan
//  "nggak ngefek" (dump/scan tetap jalan sampai selesai). Sekarang semua
//  modul nanya ke SATU flag ini, jadi satu tombol ngeberhentiin semuanya.
//
//  Denyut (beat) dipakai UI buat tahu suatu kerja masih HIDUP. Kalau kerja
//  berat nggak pernah nge-denyut lagi (thread-nya mati / macet), UI bisa lepas
//  kunci tombolnya -- jadi nggak ada lagi state "sedang dump" yang nyangkut.
// ============================================================================
#pragma once

namespace abortx {

// Minta SELURUH kerja berat berhenti (tombol Stop / keluar / panic stealth).
// `reason` cuma buat log (boleh nullptr).
void request(const char* reason = nullptr);
// Batalkan permintaan (dipanggil waktu mulai kerja baru).
void clear();
bool requested();
const char* reason();

// Denyut progres: loop berat manggil ini sesekali (bukan tiap iterasi).
// `frac` (0..1) dipakai buat PROGRESS BAR di UI; -1 = tahap ini nggak bisa
// diukur (UI nampilin bar jalan-jalan, bukan persentase palsu).
void   beat(const char* phase, float frac = -1.0f);
double beatAgeSec();            // detik sejak denyut terakhir (0 kalau belum ada)
const char* phase();            // fase terakhir yang dilaporkan

// "Slot" progres: dipakai kerja yang manggil kerja lain (dumper manggil pool
// method) biar progress bar-nya nyambung, bukan lompat bolak-balik.
// Contoh: span(0.62f, 0.24f) -> semua beat(frac) selama span itu dipetakan ke
// 62%..86% dari total.
void  span(float base, float len);
// progres terakhir 0..1 (-1 = belum ada / nggak terukur)
float progress();

// Waktu monotonik (detik sejak proses start) -- dipakai buat batas waktu
// ("budget") di loop panjang, biar nggak pernah jalan tanpa batas di device
// yang lebih lambat / layout-nya beda.
double nowSec();

}  // namespace abortx

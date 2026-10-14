// ============================================================================
//  log.h - ring-buffer log buat overlay + file
// ============================================================================
#pragma once

#include <cstddef>

namespace logx {

void add(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
void clear();

// akses buat UI
const char* line(int backIndex);   // backIndex 0 = paling baru
int         count();

// simpan ke file (opsional), contoh "/sdcard/panxcz/libtool.log"
void setFile(const char* path);
bool fileEnabled();

}  // namespace logx

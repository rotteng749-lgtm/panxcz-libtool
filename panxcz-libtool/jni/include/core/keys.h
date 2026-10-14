// ============================================================================
//  keys.h - baca tombol volume buat minimize / expand menu
// ============================================================================
#pragma once

namespace keys {

void init();          // mulai thread pembaca /dev/input/event*
void stop();

// jumlah penekanan sejak terakhir dibaca (auto-reset)
int takeVolUp();
int takeVolDown();

}  // namespace keys

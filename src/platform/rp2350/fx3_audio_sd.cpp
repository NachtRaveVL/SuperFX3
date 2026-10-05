/*
 * NR-RetroWorks SuperFX3 Firmware
 * Copyright (C) 2026 NR-RetroWorks
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 3 or later.
 */

#include "fx3_audio_sd.h"
#include "sd_audio_source.h"

#ifndef SUPERFX3_BOARD_REVISION
#define SUPERFX3_BOARD_REVISION 1
#endif

#if SUPERFX3_BOARD_REVISION >= 2
// Weak hooks retain the source override used by host/bench integrations.
extern "C" __attribute__((weak)) bool fx3_audio_sd_mount() { return sd_audio_mount(); }
extern "C" __attribute__((weak)) bool fx3_audio_sd_open_file(const char* path) { return sd_audio_open(path); }
extern "C" __attribute__((weak)) int32_t fx3_audio_sd_read_file(uint8_t* data, uint32_t size) {
    return sd_audio_read(data, size);
}
extern "C" __attribute__((weak)) void fx3_audio_sd_close_file() { sd_audio_close(); }

namespace {
bool open(void*, uint16_t asset_id) {
    static constexpr char HEX[] = "0123456789ABCDEF";
    char path[] = "/audio/0000.brr";
    path[7] = HEX[(asset_id >> 12) & 15u];
    path[8] = HEX[(asset_id >> 8) & 15u];
    path[9] = HEX[(asset_id >> 4) & 15u];
    path[10] = HEX[asset_id & 15u];
    return fx3_audio_sd_mount() && fx3_audio_sd_open_file(path);
}
int32_t read(void*, uint8_t* data, uint32_t size) {
    return fx3_audio_sd_read_file(data, size);
}
void close(void*) { fx3_audio_sd_close_file(); }
}
#endif

Fx3AudioSource fx3_audio_sd_source() {
#if SUPERFX3_BOARD_REVISION >= 2
    return {nullptr, open, read, close};
#else
    return {};
#endif
}

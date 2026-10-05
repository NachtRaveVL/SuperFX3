/*
 * NR-RetroWorks SuperFX3 Firmware
 * Copyright (C) 2026 NR-RetroWorks
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 3 or later.
 */

#include "fx3_video_sd.h"
#include "sd_audio_source.h"

#if SUPERFX3_AUDIO_SD
namespace {
bool open(void*, uint16_t asset_id) {
    static constexpr char HEX[] = "0123456789ABCDEF";
    char path[] = "/video/0000.fmv";
    for (uint8_t i = 0; i < 4; ++i)
        path[7u + i] = HEX[(asset_id >> ((3u - i) * 4u)) & 15u];
    return sd_video_mount() && sd_video_open(path);
}
int32_t read(void*, uint8_t* data, uint32_t size) { return sd_video_read(data, size); }
void close(void*) { sd_video_close(); }
}
#endif

Fx3AudioSource fx3_video_sd_source() {
#if SUPERFX3_AUDIO_SD
    return {nullptr, open, read, close};
#else
    return {};
#endif
}

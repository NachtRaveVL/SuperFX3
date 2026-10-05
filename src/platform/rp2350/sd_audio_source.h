/*
 * NR-RetroWorks SuperFX3 Firmware
 * Copyright (C) 2026 NR-RetroWorks
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 3 or later.
 */

#pragma once

#include <stdint.h>

// Core 1 only, read-only FAT16/FAT32 source for /audio/XXXX.brr.
bool sd_audio_mount();
bool sd_audio_open(const char* path);
int32_t sd_audio_read(uint8_t* data, uint32_t size);
void sd_audio_close();

// Independent video cursor; both readers share the same read-only mounted volume.
bool sd_video_open(const char* path);
bool sd_video_mount();
int32_t sd_video_read(uint8_t* data, uint32_t size);
void sd_video_close();

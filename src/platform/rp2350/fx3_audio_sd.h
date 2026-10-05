/*
 * NR-RetroWorks SuperFX3 Firmware
 * Copyright (C) 2026 NR-RetroWorks
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 3 or later.
 */

#pragma once

#include "audio/fx3_audio_stream.h"

// Rev B filesystem hooks use the bounded qspi_sd_command() shared-bus path.
// Defaults provide read-only FAT16/FAT32 access; missing cards fail safely.
extern "C" bool fx3_audio_sd_mount();
extern "C" bool fx3_audio_sd_open_file(const char* path);
extern "C" int32_t fx3_audio_sd_read_file(uint8_t* data, uint32_t size);
extern "C" void fx3_audio_sd_close_file();

/// Returns the Rev-B SD source. Rev A intentionally returns an unavailable source.
Fx3AudioSource fx3_audio_sd_source();

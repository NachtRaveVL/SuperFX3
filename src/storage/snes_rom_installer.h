/*
 * NR-RetroWorks SuperFX3 Firmware
 * Copyright (C) 2026 NR-RetroWorks
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 3 or later.
 */

#pragma once

#include <stddef.h>
#include <stdint.h>

#include "parallel_rom_programmer.h"
#include "snes_rom_layout.h"
#include "usb_rom_volume.h"

enum class SnesRomInstallStatus : uint8_t {
    Idle,
    Receiving,
    Ready,
    Programming,
    Complete,
    InvalidFile,
    UnsupportedFlash,
    FlashError,
    Aborted,
};

struct SnesRomInstallHooks {
    void* context;
    bool (*usb_mode)(void* context);
    void (*busy_irq)(void* context, bool asserted);
    void (*service)(void* context);
    bool (*program_fx)(void* context, uint32_t offset, const uint8_t* data, uint32_t size) = nullptr;
};

class SnesRomInstaller {
public:
    SnesRomInstaller(const ParallelRomBus& bus, const SnesRomInstallHooks& hooks);

    bool begin(UsbRomFileType type);
    bool stage(uint32_t file_offset, const uint8_t* data, size_t size);
    uint8_t read(uint32_t file_offset) const;
    void finish(uint32_t file_size);
    bool finish(UsbRomFileType type, uint32_t file_size,
                const uint16_t* pages, uint32_t page_count);
    bool flush();
    void abort();
    bool process();

    SnesRomInstallStatus status() const { return status_; }
    uint32_t file_size() const { return file_size_; }
    SnesRomMap installed_map() const {
        return type_ == UsbRomFileType::Raw ?
            snes_rom_installed_map({bus_.context, bus_.read}) : installed_info_.map;
    }

private:
    static constexpr uint32_t MIB = 1024u * 1024u;
    static constexpr uint32_t LOWER_TEMP_BASE = 8u * MIB;
    static constexpr uint32_t UPPER_TEMP_BASE = 12u * MIB;
    static constexpr uint32_t PAGE_SIZE = 4096u;
    static constexpr uint32_t MAX_SOURCE_PAGES = (8u * MIB) / PAGE_SIZE;

    static uint8_t read_source(void* context, uint32_t offset);
    bool active() const;
    bool load_staging_sector(uint32_t address);
    bool write_staged(uint32_t offset, const uint8_t* data, size_t size);
    uint8_t read_staged(uint32_t offset) const;
    bool materialize_upload();
    bool copy_to_temp(uint32_t source_offset, uint32_t size,
                      uint32_t destination, bool descending);
    uint8_t read_install_source(uint32_t offset, uint32_t destination_sector) const;
    bool program_sector(uint32_t address, const SnesRomInfo& info);
    bool install(const SnesRomInfo& info);
    bool verify_raw() const;
    void build_representatives(const SnesRomInfo& info);
    void set_busy(bool busy);
    void fail(SnesRomInstallStatus status);

    ParallelRomProgrammer parallel_rom_;
    ParallelRomBus bus_;
    SnesRomInstallHooks hooks_;
    SnesRomInstallStatus status_ = SnesRomInstallStatus::Idle;
    UsbRomFileType type_ = UsbRomFileType::Sfc;
    bool busy_ = false;
    uint32_t file_size_ = 0;
    SnesRomInfo installed_info_{};
    uint32_t representative_[MAX_SOURCE_PAGES]{};
    uint8_t erased_sectors_[ParallelRomProgrammer::CAPACITY /
                            ParallelRomProgrammer::SECTOR_SIZE / 8u]{};
    uint8_t copier_header_[512]{};
    uint8_t sector_buffer_[ParallelRomProgrammer::SECTOR_SIZE]{};
    uint8_t spill_[UsbRomVolume::STAGING_PAGES * PAGE_SIZE - ParallelRomProgrammer::CAPACITY]{};
    uint8_t swap_[2][PAGE_SIZE]{};
    uint16_t upload_pages_[UsbRomVolume::STAGING_PAGES]{};
    uint32_t upload_page_count_ = 0;
    uint32_t staging_sector_ = 0xFFFFFFFFu;
    bool staging_dirty_ = false;
};

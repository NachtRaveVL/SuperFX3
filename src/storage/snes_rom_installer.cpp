/*
 * NR-RetroWorks SuperFX3 Firmware
 * Copyright (C) 2026 NR-RetroWorks
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 3 or later.
 */

#include "snes_rom_installer.h"

#include <string.h>

namespace {
constexpr uint32_t INVALID_ADDRESS = 0xFFFFFFFFu;
}

SnesRomInstaller::SnesRomInstaller(const ParallelRomBus& bus,
                                   const SnesRomInstallHooks& hooks)
    : parallel_rom_(bus), bus_(bus), hooks_(hooks) {}

bool SnesRomInstaller::active() const {
    // Explicit USB disconnect/cancel must stop an in-progress operation even
    // when the cartridge remains powered in standalone mode.
    return busy_ && hooks_.usb_mode && hooks_.usb_mode(hooks_.context);
}

void SnesRomInstaller::set_busy(bool busy) {
    if (busy_ == busy)
        return;
    busy_ = busy;
    if (hooks_.busy_irq)
        hooks_.busy_irq(hooks_.context, busy);
}

void SnesRomInstaller::fail(SnesRomInstallStatus status) {
    status_ = status;
    parallel_rom_.reset();
    staging_sector_ = INVALID_ADDRESS;
    staging_dirty_ = false;
    set_busy(false);
}

void SnesRomInstaller::abort() {
    if (busy_ || status_ == SnesRomInstallStatus::Ready)
        fail(SnesRomInstallStatus::Aborted);
}

bool SnesRomInstaller::begin(UsbRomFileType type) {
    if (!hooks_.usb_mode || !hooks_.usb_mode(hooks_.context)) {
        abort();
        return false;
    }
    if (busy_)
        return false;
    status_ = SnesRomInstallStatus::Receiving;
    type_ = type;
    file_size_ = 0;
    installed_info_ = {};
    upload_page_count_ = 0;
    staging_sector_ = INVALID_ADDRESS;
    staging_dirty_ = false;
    memset(spill_, 0xFF, sizeof(spill_));
    memset(erased_sectors_, 0, sizeof(erased_sectors_));
    memset(copier_header_, 0xFF, sizeof(copier_header_));
    for (uint32_t& address : representative_)
        address = INVALID_ADDRESS;
    set_busy(true);
    if (!parallel_rom_.is_supported_device()) {
        fail(SnesRomInstallStatus::UnsupportedFlash);
        return false;
    }
    return true;
}

bool SnesRomInstaller::flush() {
    if (!active()) {
        abort();
        return false;
    }
    if (!staging_dirty_)
        return true;
    if (!parallel_rom_.erase_sector(staging_sector_)) {
        fail(SnesRomInstallStatus::FlashError);
        return false;
    }
    for (uint32_t index = 0; index < sizeof(sector_buffer_); ++index) {
        if (!active()) {
            abort();
            return false;
        }
        if (sector_buffer_[index] != 0xFF &&
            !parallel_rom_.program_byte(staging_sector_ + index, sector_buffer_[index])) {
            fail(SnesRomInstallStatus::FlashError);
            return false;
        }
        if (hooks_.service && (index & 0xFFu) == 0)
            hooks_.service(hooks_.context);
    }
    if (!parallel_rom_.verify(staging_sector_, sector_buffer_, sizeof(sector_buffer_))) {
        fail(SnesRomInstallStatus::FlashError);
        return false;
    }
    const uint32_t sector = staging_sector_ / ParallelRomProgrammer::SECTOR_SIZE;
    erased_sectors_[sector >> 3] |= static_cast<uint8_t>(1u << (sector & 7u));
    staging_dirty_ = false;
    return true;
}

bool SnesRomInstaller::load_staging_sector(uint32_t address) {
    const uint32_t base = address & ~(ParallelRomProgrammer::SECTOR_SIZE - 1u);
    if (base == staging_sector_)
        return true;
    if (!flush())
        return false;
    staging_sector_ = base;
    const uint32_t sector = base / ParallelRomProgrammer::SECTOR_SIZE;
    if (erased_sectors_[sector >> 3] & (1u << (sector & 7u))) {
        for (uint32_t i = 0; i < sizeof(sector_buffer_); ++i)
            sector_buffer_[i] = bus_.read(bus_.context, base + i);
    } else {
        memset(sector_buffer_, 0xFF, sizeof(sector_buffer_));
    }
    return true;
}

bool SnesRomInstaller::stage(uint32_t file_offset, const uint8_t* data, size_t size) {
    if (!active()) {
        abort();
        return false;
    }
    if (status_ != SnesRomInstallStatus::Receiving)
        return false;
    return write_staged(file_offset, data, size);
}

bool SnesRomInstaller::write_staged(uint32_t file_offset, const uint8_t* data, size_t size) {
    constexpr uint32_t limit = UsbRomVolume::STAGING_PAGES * PAGE_SIZE;
    if (!data || file_offset > limit || size > limit - file_offset) {
        fail(SnesRomInstallStatus::InvalidFile);
        return false;
    }
    for (size_t index = 0; index < size; ++index) {
        if (!active()) {
            fail(SnesRomInstallStatus::Aborted);
            return false;
        }
        const uint32_t address = file_offset + static_cast<uint32_t>(index);
        if (address >= ParallelRomProgrammer::CAPACITY) {
            spill_[address - ParallelRomProgrammer::CAPACITY] = data[index];
        } else {
            if (!load_staging_sector(address))
                return false;
            sector_buffer_[address - staging_sector_] = data[index];
            staging_dirty_ = true;
        }
    }
    return true;
}

uint8_t SnesRomInstaller::read(uint32_t file_offset) const {
    if (status_ == SnesRomInstallStatus::Receiving || status_ == SnesRomInstallStatus::Ready)
        return read_staged(file_offset);
    if (status_ != SnesRomInstallStatus::Complete || type_ == UsbRomFileType::Raw)
        return file_offset < ParallelRomProgrammer::CAPACITY ?
            bus_.read(bus_.context, file_offset) : 0xFF;

    const bool fx3_dump = installed_info_.map == SnesRomMap::Fx3 &&
                          installed_info_.data_offset >= 4u * MIB;
    const uint32_t header_size = installed_info_.data_offset - (fx3_dump ? 4u * MIB : 0u);
    if (file_offset < header_size)
        return copier_header_[file_offset];
    const uint32_t source = fx3_dump && file_offset < installed_info_.data_offset ?
        (((file_offset - header_size) >> 16) << 15) | ((file_offset - header_size) & 0x7FFFu) :
        file_offset - installed_info_.data_offset;
    if (source >= installed_info_.size)
        return 0xFF;
    const uint32_t page = source / PAGE_SIZE;
    const uint32_t address = representative_[page];
    return address == INVALID_ADDRESS ? 0xFF :
        bus_.read(bus_.context, address + source % PAGE_SIZE);
}

uint8_t SnesRomInstaller::read_staged(uint32_t offset) const {
    if (offset >= ParallelRomProgrammer::CAPACITY)
        return offset < ParallelRomProgrammer::CAPACITY + sizeof(spill_) ?
            spill_[offset - ParallelRomProgrammer::CAPACITY] : 0xFF;
    if (staging_sector_ != INVALID_ADDRESS && offset >= staging_sector_ &&
        offset - staging_sector_ < sizeof(sector_buffer_))
        return sector_buffer_[offset - staging_sector_];
    const uint32_t sector = offset / ParallelRomProgrammer::SECTOR_SIZE;
    return (erased_sectors_[sector >> 3] & (1u << (sector & 7u))) ?
        bus_.read(bus_.context, offset) : 0xFF;
}

void SnesRomInstaller::finish(uint32_t file_size) {
    if (status_ != SnesRomInstallStatus::Receiving)
        return;
    file_size_ = file_size;
    status_ = SnesRomInstallStatus::Ready;
}

bool SnesRomInstaller::finish(UsbRomFileType type, uint32_t file_size,
                              const uint16_t* pages, uint32_t page_count) {
    const uint32_t limit = type == UsbRomFileType::Raw ?
        ParallelRomProgrammer::CAPACITY : 8u * MIB + 512u;
    if (!active()) {
        abort();
        return false;
    }
    if (status_ != SnesRomInstallStatus::Receiving || !pages || !file_size ||
        file_size > limit || page_count > UsbRomVolume::STAGING_PAGES ||
        page_count != (file_size + PAGE_SIZE - 1u) / PAGE_SIZE)
        return false;
    for (uint32_t i = 0; i < page_count; ++i) {
        if (pages[i] >= UsbRomVolume::STAGING_PAGES)
            return false;
        for (uint32_t j = 0; j < i; ++j) {
            if (pages[i] == pages[j])
                return false;
        }
        upload_pages_[i] = pages[i];
    }
    upload_page_count_ = page_count;
    type_ = type;
    finish(file_size);
    return true;
}

bool SnesRomInstaller::materialize_upload() {
    // Put the finalized FAT chain into file order. Swap pages, updating every
    // displaced source reference, so fragmented or backwards chains cannot
    // overwrite unread source data. Contiguous uploads require no swaps.
    for (uint32_t destination = 0; destination < upload_page_count_; ++destination) {
        const uint32_t source = upload_pages_[destination];
        if (source == destination)
            continue;
        for (uint32_t i = 0; i < PAGE_SIZE; ++i) {
            swap_[0][i] = read_staged(destination * PAGE_SIZE + i);
            swap_[1][i] = read_staged(source * PAGE_SIZE + i);
        }
        if (!write_staged(destination * PAGE_SIZE, swap_[1], PAGE_SIZE) ||
            !write_staged(source * PAGE_SIZE, swap_[0], PAGE_SIZE))
            return false;
        for (uint32_t i = destination + 1u; i < upload_page_count_; ++i) {
            if (upload_pages_[i] == destination)
                upload_pages_[i] = static_cast<uint16_t>(source);
        }
    }
    if (!flush())
        return false;
    staging_sector_ = INVALID_ADDRESS;
    return true;
}

uint8_t SnesRomInstaller::read_source(void* context, uint32_t offset) {
    auto* installer = static_cast<SnesRomInstaller*>(context);
    return offset < installer->file_size_ ?
        installer->bus_.read(installer->bus_.context, offset) : 0xFF;
}

bool SnesRomInstaller::copy_to_temp(uint32_t source_offset, uint32_t size,
                                    uint32_t destination, bool descending) {
    if (!size)
        return true;
    const uint32_t first_sector = destination & ~(ParallelRomProgrammer::SECTOR_SIZE - 1u);
    const uint32_t end = destination + size;
    const uint32_t sector_count =
        (end - first_sector + ParallelRomProgrammer::SECTOR_SIZE - 1u) /
        ParallelRomProgrammer::SECTOR_SIZE;

    for (uint32_t step = 0; step < sector_count; ++step) {
        const uint32_t sector_index = descending ? sector_count - 1u - step : step;
        const uint32_t sector_address = first_sector +
            sector_index * ParallelRomProgrammer::SECTOR_SIZE;
        memset(sector_buffer_, 0xFF, sizeof(sector_buffer_));

        const uint32_t copy_begin = sector_address > destination ? sector_address : destination;
        const uint32_t sector_end = sector_address + ParallelRomProgrammer::SECTOR_SIZE;
        const uint32_t copy_end = sector_end < end ? sector_end : end;
        for (uint32_t address = copy_begin; address < copy_end; ++address) {
            const uint32_t source = source_offset + address - destination;
            sector_buffer_[address - sector_address] = bus_.read(bus_.context, source);
        }

        if (!parallel_rom_.erase_sector(sector_address))
            return false;
        const uint32_t sector = sector_address / ParallelRomProgrammer::SECTOR_SIZE;
        erased_sectors_[sector >> 3] |= static_cast<uint8_t>(1u << (sector & 7u));
        for (uint32_t index = 0; index < ParallelRomProgrammer::SECTOR_SIZE; ++index) {
            if (!active())
                return false;
            if (sector_buffer_[index] != 0xFF &&
                !parallel_rom_.program_byte(sector_address + index, sector_buffer_[index]))
                return false;
            if (hooks_.service && (index & 0xFFu) == 0)
                hooks_.service(hooks_.context);
        }
        if (!parallel_rom_.verify(sector_address, sector_buffer_, sizeof(sector_buffer_)))
            return false;
    }
    return true;
}

uint8_t SnesRomInstaller::read_install_source(uint32_t offset,
                                              uint32_t destination_sector) const {
    const uint32_t temporary = offset < 4u * MIB ?
        UPPER_TEMP_BASE + offset : LOWER_TEMP_BASE + offset - 4u * MIB;
    if (temporary >= destination_sector)
        return bus_.read(bus_.context, temporary);

    const uint32_t representative = representative_[offset / PAGE_SIZE];
    return representative == INVALID_ADDRESS ? 0xFF :
        bus_.read(bus_.context, representative + offset % PAGE_SIZE);
}

bool SnesRomInstaller::program_sector(uint32_t address, const SnesRomInfo& info) {
    for (uint32_t index = 0; index < ParallelRomProgrammer::SECTOR_SIZE; ++index) {
        uint32_t source = 0;
        sector_buffer_[index] = snes_rom_source_offset(info, address + index, source) ?
            read_install_source(source, address) : 0xFF;
    }

    if (!parallel_rom_.erase_sector(address))
        return false;
    const uint32_t sector = address / ParallelRomProgrammer::SECTOR_SIZE;
    erased_sectors_[sector >> 3] |= static_cast<uint8_t>(1u << (sector & 7u));
    for (uint32_t index = 0; index < ParallelRomProgrammer::SECTOR_SIZE; ++index) {
        if (!active())
            return false;
        if (sector_buffer_[index] != 0xFF &&
            !parallel_rom_.program_byte(address + index, sector_buffer_[index]))
            return false;
        if (hooks_.service && (index & 0xFFu) == 0)
            hooks_.service(hooks_.context);
    }
    if (!parallel_rom_.verify(address, sector_buffer_, sizeof(sector_buffer_)))
        return false;
    for (uint32_t page_address = address;
         page_address < address + ParallelRomProgrammer::SECTOR_SIZE;
         page_address += PAGE_SIZE) {
        uint32_t source = 0;
        if (snes_rom_source_offset(info, page_address, source))
            representative_[source / PAGE_SIZE] = page_address;
    }
    return true;
}

void SnesRomInstaller::build_representatives(const SnesRomInfo& info) {
    for (uint32_t& address : representative_)
        address = INVALID_ADDRESS;
    for (uint32_t address = 0; address < ParallelRomProgrammer::CAPACITY;
         address += PAGE_SIZE) {
        uint32_t source = 0;
        if (snes_rom_source_offset(info, address, source))
            representative_[source / PAGE_SIZE] = address;
    }
}

bool SnesRomInstaller::install(const SnesRomInfo& info) {
    if (info.map == SnesRomMap::Fx3 && !hooks_.program_fx)
        return false;
    if (info.data_offset) {
        for (uint32_t index = 0; index < sizeof(copier_header_); ++index)
            copier_header_[index] = bus_.read(bus_.context, index);
    }

    // Preserve the complete mapped source in the upper 8 MiB before replacing
    // the raw upload. The halves are swapped so lower final addresses can be
    // generated first without destroying a source half needed later.
    if (info.size > 4u * MIB &&
        !copy_to_temp(info.data_offset + 4u * MIB, info.size - 4u * MIB,
                      LOWER_TEMP_BASE, true))
        return false;
    const uint32_t lower_size = info.size < 4u * MIB ? info.size : 4u * MIB;
    if (!copy_to_temp(info.data_offset, lower_size, UPPER_TEMP_BASE, false))
        return false;

    if (info.map == SnesRomMap::Fx3) {
        // Materialize the GSU's 3 MiB window, mirroring smaller canonical ROMs.
        for (uint32_t offset = 0; offset < 3u * MIB; offset += PAGE_SIZE) {
            if (!active())
                return false;
            uint32_t source = 0;
            if (!snes_rom_source_offset(info, 0x400000u + offset, source))
                return false;
            for (uint32_t i = 0; i < PAGE_SIZE; ++i)
                sector_buffer_[i] = bus_.read(bus_.context, UPPER_TEMP_BASE + source + i);
            if (!hooks_.program_fx(hooks_.context, offset, sector_buffer_, PAGE_SIZE))
                return false;
            if (hooks_.service)
                hooks_.service(hooks_.context);
        }
    }

    // Ascending order is intentional. For extended maps, the lower 8 MiB is
    // completed before either temporary half is overwritten. Within the upper
    // 8 MiB, every source page is consumed or represented by an earlier final
    // sector before its temporary sector is erased.
    for (uint32_t address = 0; address < ParallelRomProgrammer::CAPACITY;
         address += ParallelRomProgrammer::SECTOR_SIZE) {
        if (!program_sector(address, info))
            return false;
    }
    build_representatives(info);
    // Commit the map descriptor only after all mapped bytes have verified.
    // $7E is SNES WRAM, so this sector was emitted erased and has no ROM source.
    uint8_t descriptor[SNES_ROM_DESCRIPTOR_SIZE];
    snes_rom_descriptor(info, descriptor);
    for (uint32_t step = 0; step < sizeof(descriptor); ++step) {
        const uint32_t index = (step + 4u) % sizeof(descriptor); // Magic last.
        if (!active() || !parallel_rom_.program_byte(SNES_ROM_DESCRIPTOR_ADDRESS + index,
                                                    descriptor[index]))
            return false;
    }
    if (!parallel_rom_.verify(SNES_ROM_DESCRIPTOR_ADDRESS, descriptor, sizeof(descriptor)))
        return false;
    return true;
}

bool SnesRomInstaller::verify_raw() const {
    if (file_size_ != ParallelRomProgrammer::CAPACITY)
        return false;
    for (uint32_t address = 0; address < file_size_; ++address) {
        (void)bus_.read(bus_.context, address);
        if (hooks_.service && (address & 0xFFu) == 0)
            hooks_.service(hooks_.context);
        if (!active())
            return false;
    }
    return true;
}

bool SnesRomInstaller::process() {
    if (status_ != SnesRomInstallStatus::Ready)
        return false;
    if (!active()) {
        fail(SnesRomInstallStatus::Aborted);
        return false;
    }
    status_ = SnesRomInstallStatus::Programming;

    if (!materialize_upload()) {
        fail(active() ? SnesRomInstallStatus::FlashError : SnesRomInstallStatus::Aborted);
        return false;
    }

    if (type_ == UsbRomFileType::Raw) {
        if (!verify_raw()) {
            fail(active() ? SnesRomInstallStatus::InvalidFile :
                            SnesRomInstallStatus::Aborted);
            return false;
        }
    } else {
        SnesRomInfo info{};
        if (!snes_rom_detect({this, read_source}, file_size_,
                             type_ == UsbRomFileType::Smc, info)) {
            fail(SnesRomInstallStatus::InvalidFile);
            return false;
        }
        if (!install(info)) {
            fail(active() ? SnesRomInstallStatus::FlashError :
                            SnesRomInstallStatus::Aborted);
            return false;
        }
        installed_info_ = info;
    }

    status_ = SnesRomInstallStatus::Complete;
    set_busy(false);
    return true;
}

/*
 * NR-RetroWorks SuperFX3 Firmware
 * Copyright (C) 2026 NR-RetroWorks
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "usb_rom_loader.h"

#include "platform/rp2350/parallel_rom_gpio.h"
#include "platform/rp2350/snes_bus.h"
#include "platform/rp2350/snes_pio.h"
#include "storage/snes_rom_installer.h"
#include "storage/usb_rom_volume.h"

#include "tusb.h"

#include <string.h>

namespace {
// SYNCHRONIZE CACHE (10) is not named in the bundled TinyUSB SCSI enum.
constexpr uint8_t SCSI_SYNCHRONIZE_CACHE_10 = 0x35;
bool g_initialized = false;
bool g_enabled = false;
bool g_in_msc_callback = false;

bool usb_mode(void*) { return snes_bus_usb_mode(); }
void busy_irq(void*, bool asserted) { snes_busy_irq_write(nullptr, asserted); }
void service_usb(void*) {
    if (g_initialized && g_enabled && !g_in_msc_callback)
        tud_task();
}

SnesRomInstaller& installer() {
    static ParallelRomBus bus = parallel_rom_gpio_bus();
    static SnesRomInstaller value(bus, {nullptr, usb_mode, busy_irq, service_usb});
    return value;
}

bool sink_begin(void*, UsbRomFileType type, uint32_t) { return installer().begin(type); }
bool sink_write(void*, uint32_t offset, const uint8_t* data, size_t size) {
    return installer().stage(offset, data, size);
}
uint8_t sink_read(void*, uint32_t offset) { return installer().read(offset); }
bool sink_complete(void*, UsbRomFileType type, uint32_t size,
                   const uint16_t* pages, uint32_t count) {
    return installer().finish(type, size, pages, count);
}
bool sink_flush(void*) { return installer().flush(); }

UsbRomVolume& volume() {
    static UsbRomVolume value({nullptr, sink_begin, sink_write, sink_read, sink_complete, sink_flush});
    return value;
}
}

void usb_rom_loader_init() {
    if (g_initialized)
        return;
    volume().reset();
    (void)tusb_init();
    g_initialized = true;
    tud_disconnect();
}

void usb_rom_loader_set_enabled(bool enabled) {
    if (!g_initialized || enabled == g_enabled)
        return;
    g_enabled = enabled;
    if (enabled) {
        volume().reset();
        tud_connect();
    } else {
        installer().abort();
        tud_disconnect();
    }
}

void usb_rom_loader_task() {
    if (!g_initialized || !g_enabled)
        return;
    tud_task();
    if (installer().status() == SnesRomInstallStatus::Ready && installer().process())
        snes_pio_set_rom_map(installer().installed_map());
}

extern "C" void tud_msc_inquiry_cb(uint8_t, uint8_t vendor_id[8],
                                    uint8_t product_id[16], uint8_t product_rev[4]) {
    memcpy(vendor_id, "NR-RETRO", 8);
    memcpy(product_id, "SUPERFX3 LOADER ", 16);
    memcpy(product_rev, "0100", 4);
}

extern "C" bool tud_msc_test_unit_ready_cb(uint8_t lun) {
    const bool ready = g_enabled && snes_bus_usb_mode() && !volume().ejected();
    if (!ready)
        tud_msc_set_sense(lun, SCSI_SENSE_NOT_READY, 0x3A, 0x00);
    return ready;
}

extern "C" void tud_msc_capacity_cb(uint8_t, uint32_t* block_count,
                                     uint16_t* block_size) {
    *block_count = UsbRomVolume::BLOCK_COUNT;
    *block_size = UsbRomVolume::BLOCK_SIZE;
}

extern "C" bool tud_msc_start_stop_cb(uint8_t, uint8_t, bool start, bool load_eject) {
    if (!g_enabled || !snes_bus_usb_mode())
        return false;
    if (!start && load_eject) {
        g_in_msc_callback = true;
        const bool ok = volume().eject();
        g_in_msc_callback = false;
        return ok;
    }
    return !volume().ejected();
}

extern "C" bool tud_msc_is_writable_cb(uint8_t) {
    return g_enabled && snes_bus_usb_mode() && !volume().ejected();
}

extern "C" void tud_umount_cb() { installer().abort(); }

extern "C" int32_t tud_msc_read10_cb(uint8_t, uint32_t lba, uint32_t offset,
                                      void* buffer, uint32_t size) {
    if (!g_enabled || !snes_bus_usb_mode() || volume().ejected() ||
        offset != 0 || size % UsbRomVolume::BLOCK_SIZE != 0)
        return -1;
    auto* output = static_cast<uint8_t*>(buffer);
    for (uint32_t done = 0; done < size; done += UsbRomVolume::BLOCK_SIZE) {
        if (!volume().read(lba + done / UsbRomVolume::BLOCK_SIZE, output + done,
                           UsbRomVolume::BLOCK_SIZE))
            return -1;
    }
    return static_cast<int32_t>(size);
}

extern "C" int32_t tud_msc_write10_cb(uint8_t, uint32_t lba, uint32_t offset,
                                       uint8_t* buffer, uint32_t size) {
    if (!g_enabled || volume().ejected() || offset != 0 ||
        size % UsbRomVolume::BLOCK_SIZE != 0 || !snes_bus_usb_mode())
        return -1;
    g_in_msc_callback = true;
    bool ok = true;
    for (uint32_t done = 0; done < size; done += UsbRomVolume::BLOCK_SIZE) {
        if (!volume().write(lba + done / UsbRomVolume::BLOCK_SIZE, buffer + done,
                            UsbRomVolume::BLOCK_SIZE)) {
            ok = false;
            break;
        }
    }
    g_in_msc_callback = false;
    return ok ? static_cast<int32_t>(size) : -1;
}

extern "C" int32_t tud_msc_scsi_cb(uint8_t lun, uint8_t const scsi_cmd[16],
                                    void*, uint16_t) {
    switch (scsi_cmd[0]) {
    case SCSI_CMD_PREVENT_ALLOW_MEDIUM_REMOVAL:
        return 0;
    case SCSI_SYNCHRONIZE_CACHE_10:
        // A cache flush is not a file close. Hosts also flush growing files.
        g_in_msc_callback = true;
        {
            const bool ok = g_enabled && snes_bus_usb_mode() && volume().flush();
            g_in_msc_callback = false;
            return ok ? 0 : -1;
        }
    default:
        tud_msc_set_sense(lun, SCSI_SENSE_ILLEGAL_REQUEST, 0x20, 0x00);
        return -1;
    }
}

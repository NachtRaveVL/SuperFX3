/*
 * NR-RetroWorks SuperFX3 Firmware
 * Copyright (C) 2026 NR-RetroWorks
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 3 or later.
 */

#include "fx3_video_stream.h"
#include "pico.h"
#include "pico/sync.h"

namespace {
std::atomic<uint8_t> g_asset_low {0}, g_asset_high {0}, g_status {0}, g_error {0};
std::atomic<bool> g_locked {false};
#if SUPERFX3_AUDIO_SD
enum class PageState : uint8_t { Free, Building, Ready, Active };
struct Page {
    std::atomic<PageState> state {PageState::Free};
    uint32_t pts = 0, duration = 0, number = 0;
    uint16_t tiles = 0, bytes = 0;
} g_pages[2];
Fx3AudioSource g_source {};
std::atomic<uint8_t>* g_ram = nullptr;
bool (*g_acquire)() = nullptr;
void (*g_release)() = nullptr;
critical_section_t g_gate;
bool g_initialized = false, g_open = false;
std::atomic<uint32_t> g_command {0};
std::atomic<bool> g_reset {false};
std::atomic<uint8_t> g_active {fx3_video::NO_PAGE}, g_flags {0};
std::atomic<uint32_t> g_duration {0}, g_frame_count {0};
uint8_t g_header[32] {}, g_input[512] {};
uint32_t g_header_used = 0, g_input_used = 0, g_input_size = 0;
uint32_t g_frames = 0, g_previous_end = 0;
uint32_t g_stored = 0, g_decoded = 0, g_written = 0, g_crc = 0, g_expected_crc = 0;
uint16_t g_run = 0;
uint8_t g_building = fx3_video::NO_PAGE, g_codec = 0, g_repeat_byte = 0;
bool g_repeat = false, g_need_repeat = false, g_file_header = true;

uint16_t le16(const uint8_t* p) {
    return static_cast<uint16_t>(p[0] | (static_cast<uint16_t>(p[1]) << 8));
}
uint32_t le32(const uint8_t* p) {
    return p[0] | (static_cast<uint32_t>(p[1]) << 8) |
        (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}
uint8_t __not_in_flash_func(offered)() {
    uint8_t result = fx3_video::NO_PAGE;
    for (uint8_t i = 0; i < 2; ++i)
        if (g_pages[i].state.load(std::memory_order_acquire) == PageState::Ready &&
            (result == fx3_video::NO_PAGE || g_pages[i].number < g_pages[result].number))
            result = i;
    return result;
}
void close() {
    if (g_open && g_source.close) g_source.close(g_source.context);
    g_open = false;
}
void stop() {
    close();
    critical_section_enter_blocking(&g_gate);
    for (Page& page : g_pages)
        if (page.state.load(std::memory_order_relaxed) != PageState::Active)
            page.state.store(PageState::Free, std::memory_order_release);
    critical_section_exit(&g_gate);
    g_building = fx3_video::NO_PAGE;
    g_header_used = g_input_used = g_input_size = g_run = 0;
    g_status.store(static_cast<uint8_t>(Fx3VideoStatus::Idle), std::memory_order_release);
}
void release() {
    stop();
    critical_section_enter_blocking(&g_gate);
    g_active.store(fx3_video::NO_PAGE, std::memory_order_relaxed);
    for (Page& page : g_pages) page.state.store(PageState::Free, std::memory_order_release);
    critical_section_exit(&g_gate);
    if (g_locked.exchange(false, std::memory_order_acq_rel) && g_release) g_release();
}
void fail(Fx3VideoError error) {
    stop();
    g_error.store(static_cast<uint8_t>(error), std::memory_order_release);
    g_status.store(static_cast<uint8_t>(Fx3VideoStatus::Error), std::memory_order_release);
}
void play(uint16_t asset) {
    if (g_locked.load(std::memory_order_acquire)) {
        g_error.store(static_cast<uint8_t>(Fx3VideoError::Busy), std::memory_order_release);
        return;
    }
    g_error.store(0, std::memory_order_release);
    if (!g_ram || !g_source.open || !g_source.read) { fail(Fx3VideoError::Unavailable); return; }
    if (!g_acquire || !g_acquire()) { fail(Fx3VideoError::Busy); return; }
    g_locked.store(true, std::memory_order_release);
    g_status.store(static_cast<uint8_t>(Fx3VideoStatus::Opening), std::memory_order_release);
    if (!g_source.open(g_source.context, asset)) { fail(Fx3VideoError::OpenFailed); return; }
    g_open = true;
    g_file_header = true;
    g_frames = g_previous_end = g_header_used = g_input_used = g_input_size = 0;
    g_flags.store(0, std::memory_order_release);
    g_duration.store(0, std::memory_order_release);
    g_frame_count.store(0, std::memory_order_release);
}
// At most one 512-byte SD read per service. Retry never consumes input or output.
bool byte(uint8_t& value) {
    if (g_input_used == g_input_size) return false;
    value = g_input[g_input_used++];
    return true;
}
void output(uint8_t value) {
    g_ram[fx3_video::RAM_OFFSET + g_building * fx3_video::PAGE_SIZE + g_written++].store(
        value, std::memory_order_relaxed);
    g_crc ^= value;
    for (uint8_t bit = 0; bit < 8; ++bit)
        g_crc = (g_crc >> 1) ^ ((g_crc & 1u) ? 0xEDB88320u : 0u);
}
bool header() {
    const uint32_t size = g_file_header ? 32u : 24u;
    uint8_t value = 0;
    while (g_header_used < size && byte(value)) g_header[g_header_used++] = value;
    if (g_header_used != size) return false;
    g_header_used = 0;
    if (g_file_header) {
        if (le32(g_header) != 0x56584653u || g_header[4] != 1 || g_header[5] > 1 ||
            le16(g_header + 6) != 256 || le16(g_header + 8) != 224 ||
            le16(g_header + 10) != 32 || !le32(g_header + 12) || !le32(g_header + 16) ||
            le32(g_header + 20) != (g_header[5] ? 16000u : 0u) ||
            le32(g_header + 24) || le32(g_header + 28)) {
            fail(Fx3VideoError::InvalidFormat); return false;
        }
        g_flags.store(g_header[5], std::memory_order_release);
        g_frame_count.store(le32(g_header + 12), std::memory_order_release);
        g_duration.store(le32(g_header + 16), std::memory_order_release);
        g_file_header = false;
        g_status.store(static_cast<uint8_t>(Fx3VideoStatus::Streaming), std::memory_order_release);
    } else {
        Page& page = g_pages[g_building];
        page.pts = le32(g_header);
        page.tiles = le16(g_header + 4);
        g_codec = g_header[6];
        g_stored = le32(g_header + 8);
        g_decoded = le32(g_header + 12);
        g_expected_crc = le32(g_header + 16);
        page.duration = le32(g_header + 20);
        if (!page.tiles || page.tiles > fx3_video::MAX_TILES || g_codec > 1 || g_header[7] ||
            g_decoded != page.tiles * 32u + fx3_video::MAP_SIZE + fx3_video::PALETTE_SIZE ||
            !g_stored || g_stored > fx3_video::PAGE_SIZE * 2u ||
            (!g_codec && g_stored != g_decoded) || !page.duration ||
            page.pts != g_previous_end || page.pts >= g_duration.load(std::memory_order_acquire) ||
            page.duration > g_duration.load(std::memory_order_acquire) - page.pts) {
            fail(Fx3VideoError::InvalidFormat); return false;
        }
        page.bytes = static_cast<uint16_t>(g_decoded);
        page.number = g_frames;
        g_written = g_run = 0;
        g_crc = UINT32_MAX;
        g_need_repeat = false;
    }
    return true;
}
bool validate_frame() {
    // Reject map entries outside this frame's tile set and non-BG1 palette bits.
    const Page& page = g_pages[g_building];
    const uint32_t base = fx3_video::RAM_OFFSET + g_building * fx3_video::PAGE_SIZE;
    for (uint32_t i = page.tiles * 32u; i < g_decoded - 32u; i += 2) {
        const uint16_t entry = static_cast<uint16_t>(g_ram[base + i].load(std::memory_order_relaxed) |
            (static_cast<uint16_t>(g_ram[base + i + 1u].load(std::memory_order_relaxed)) << 8));
        if ((entry & 0x3FFu) >= page.tiles || (entry & 0x3C00u)) return false;
    }
    for (uint32_t i = g_decoded - 31u; i < g_decoded; i += 2)
        if (g_ram[base + i].load(std::memory_order_relaxed) & 0x80u) return false;
    return true;
}
}
#else
}
#endif

void fx3_video_init(const Fx3AudioSource& source, std::atomic<uint8_t>* ram,
                    bool (*acquire)(), void (*release_callback)()) {
    g_status.store(0, std::memory_order_relaxed);
    g_error.store(0, std::memory_order_relaxed);
#if SUPERFX3_AUDIO_SD
    if (!g_initialized) { critical_section_init(&g_gate); g_initialized = true; }
    release();
    g_source = source; g_ram = ram; g_acquire = acquire; g_release = release_callback;
    g_command.store(0, std::memory_order_relaxed);
    g_reset.store(false, std::memory_order_relaxed);
#else
    (void)source; (void)ram; (void)acquire; (void)release_callback;
#endif
}

void fx3_video_task() {
#if SUPERFX3_AUDIO_SD
    if (g_reset.exchange(false, std::memory_order_acq_rel)) {
        g_command.store(0, std::memory_order_release); release();
    }
    const uint32_t command = g_command.exchange(0, std::memory_order_acq_rel);
    if (command) {
        const uint8_t operation = static_cast<uint8_t>(command - 1u);
        if (operation == fx3_video::Stop) stop();
        else if (operation == fx3_video::Release) release();
        else play(static_cast<uint16_t>(command >> 8));
    }
    if (!g_open) return;
    if (!g_file_header && g_building == fx3_video::NO_PAGE) {
        critical_section_enter_blocking(&g_gate);
        for (uint8_t i = 0; i < 2; ++i)
            if (g_pages[i].state.load(std::memory_order_acquire) == PageState::Free) {
                g_pages[i].state.store(PageState::Building, std::memory_order_release);
                g_building = i; break;
            }
        critical_section_exit(&g_gate);
        if (g_building == fx3_video::NO_PAGE) return;
        g_decoded = 0;
    }
    if (g_input_used == g_input_size) {
        const int32_t read = g_source.read(g_source.context, g_input, sizeof(g_input));
        if (read == -2) return;
        if (read <= 0 || read > static_cast<int32_t>(sizeof(g_input))) {
            fail(Fx3VideoError::ReadFailed); return;
        }
        g_input_size = static_cast<uint32_t>(read); g_input_used = 0;
    }
    if (g_file_header) { header(); return; }
    if (!g_decoded && !header()) return;
    if (!g_open) return;
    // PackBits: 0..127 literal length + 1, 129..255 repeated length 257 - tag;
    // 128 is forbidden. Limit decoded work as well as SD work per service.
    uint8_t value = 0;
    for (uint32_t work = 0; work < 4096 && g_written < g_decoded; ++work) {
        if (!g_codec) {
            if (!g_stored || !byte(value)) break;
            --g_stored; output(value); continue;
        }
        if (!g_run) {
            if (!g_stored || !byte(value)) break;
            --g_stored;
            if (value == 128) { fail(Fx3VideoError::InvalidFormat); return; }
            g_repeat = value > 128;
            g_need_repeat = g_repeat;
            g_run = static_cast<uint16_t>(g_repeat ? 257u - value : value + 1u);
            if (g_run > g_decoded - g_written) { fail(Fx3VideoError::InvalidFormat); return; }
        }
        if (g_need_repeat) {
            if (!g_stored || !byte(g_repeat_byte)) break;
            --g_stored; g_need_repeat = false;
        }
        if (g_repeat) value = g_repeat_byte;
        else {
            if (!g_stored || !byte(value)) break;
            --g_stored;
        }
        output(value); --g_run;
    }
    if (g_written != g_decoded) {
        if (!g_stored && (!g_run || !g_repeat || g_need_repeat)) fail(Fx3VideoError::InvalidFormat);
        return;
    }
    if (g_stored || g_run) { fail(Fx3VideoError::InvalidFormat); return; }
    if ((g_crc ^ UINT32_MAX) != g_expected_crc) { fail(Fx3VideoError::BadCrc); return; }
    if (!validate_frame()) { fail(Fx3VideoError::InvalidFormat); return; }
    const Page& page = g_pages[g_building];
    g_previous_end = page.pts + page.duration;
    g_pages[g_building].state.store(PageState::Ready, std::memory_order_release);
    g_building = fx3_video::NO_PAGE;
    if (++g_frames == g_frame_count.load(std::memory_order_acquire)) {
        // The host still owns completion/display timing; EOF is not permission to reuse RAM.
        close();
        if (g_previous_end != g_duration.load(std::memory_order_acquire)) {
            fail(Fx3VideoError::InvalidFormat); return;
        }
        g_status.store(static_cast<uint8_t>(Fx3VideoStatus::Eof), std::memory_order_release);
    }
#endif
}

void __not_in_flash_func(fx3_video_request_reset)() {
#if SUPERFX3_AUDIO_SD
    g_reset.store(true, std::memory_order_release);
#endif
}
bool __not_in_flash_func(fx3_video_gsu_locked)() {
    return g_locked.load(std::memory_order_acquire);
}
bool __not_in_flash_func(fx3_video_mmio_address)(uint32_t address) {
    const uint16_t offset = static_cast<uint16_t>(address);
    return offset >= fx3_video::MMIO_BASE && offset <= fx3_video::MMIO_END;
}
uint8_t __not_in_flash_func(fx3_video_host_read)(uint16_t address) {
    const uint8_t offset = static_cast<uint8_t>(address - fx3_video::MMIO_BASE);
    if (offset == fx3_video::AssetIdLow) return g_asset_low.load(std::memory_order_acquire);
    if (offset == fx3_video::AssetIdHigh) return g_asset_high.load(std::memory_order_acquire);
    if (offset == fx3_video::Status) return g_status.load(std::memory_order_acquire);
    if (offset == fx3_video::Error) return g_error.load(std::memory_order_acquire);
    if (offset == fx3_video::Version) return 1;
#if SUPERFX3_AUDIO_SD
    if (offset == fx3_video::Command) {
        const uint32_t pending = g_command.load(std::memory_order_acquire);
        return pending ? static_cast<uint8_t>(pending - 1u) : 0xFF;
    }
    if (offset == fx3_video::Flags) return static_cast<uint8_t>(g_flags.load(std::memory_order_acquire) |
        (g_locked.load(std::memory_order_acquire) ? 0x80u : 0u));
    if (offset >= fx3_video::Duration && offset < fx3_video::Version) {
        const uint32_t value = offset < fx3_video::FrameCount ?
            g_duration.load(std::memory_order_acquire) : g_frame_count.load(std::memory_order_acquire);
        return static_cast<uint8_t>(value >> ((offset & 3u) * 8u));
    }
    critical_section_enter_blocking(&g_gate);
    const uint8_t active = g_active.load(std::memory_order_acquire);
    const uint8_t ready = offered();
    const uint8_t selected = active < 2 ? active : ready;
    uint8_t result = 0xFF;
    if (offset == fx3_video::Page) result = ready;
    else if (offset == fx3_video::ActivePage) result = active;
    else if (selected < 2) {
        const Page& page = g_pages[selected];
        if (offset >= fx3_video::Pts && offset < fx3_video::TileBytes)
            result = static_cast<uint8_t>(page.pts >> ((offset & 3u) * 8u));
        else if (offset >= fx3_video::FrameDuration && offset < fx3_video::FrameDuration + 4)
            result = static_cast<uint8_t>(page.duration >> ((offset & 3u) * 8u));
        else if (offset >= fx3_video::TileBytes && offset < fx3_video::Duration) {
            const uint16_t value = offset < fx3_video::FrameBytes ?
                static_cast<uint16_t>(page.tiles * 32u) : page.bytes;
            result = static_cast<uint8_t>(value >> ((offset & 1u) * 8u));
        }
    }
    critical_section_exit(&g_gate);
    return result;
#else
    return 0xFF;
#endif
}
void __not_in_flash_func(fx3_video_host_write)(uint16_t address, uint8_t value) {
    const uint8_t offset = static_cast<uint8_t>(address - fx3_video::MMIO_BASE);
    if (offset == fx3_video::AssetIdLow) g_asset_low.store(value, std::memory_order_release);
    else if (offset == fx3_video::AssetIdHigh) g_asset_high.store(value, std::memory_order_release);
    else if (offset == fx3_video::Command) {
#if SUPERFX3_AUDIO_SD
        uint32_t empty = 0;
        const uint32_t command = (static_cast<uint32_t>(value) + 1u) |
            (static_cast<uint32_t>(g_asset_low.load(std::memory_order_relaxed)) << 8) |
            (static_cast<uint32_t>(g_asset_high.load(std::memory_order_relaxed)) << 16);
        if (value > fx3_video::Release ||
            !g_command.compare_exchange_strong(empty, command, std::memory_order_release))
            g_error.store(static_cast<uint8_t>(Fx3VideoError::Busy), std::memory_order_release);
#else
        if (value == fx3_video::Play) {
            g_error.store(static_cast<uint8_t>(Fx3VideoError::Unavailable), std::memory_order_release);
            g_status.store(static_cast<uint8_t>(Fx3VideoStatus::Error), std::memory_order_release);
        } else g_status.store(0, std::memory_order_release);
#endif
    }
#if SUPERFX3_AUDIO_SD
    else if (offset == fx3_video::Page) {
        critical_section_enter_blocking(&g_gate);
        // A page claim replaces the previous ACTIVE only when the offered page matches.
        if (value == fx3_video::NO_PAGE || (value < 2 && offered() == value)) {
            const uint8_t active = g_active.exchange(value, std::memory_order_acq_rel);
            if (active < 2) g_pages[active].state.store(PageState::Free, std::memory_order_release);
            if (value < 2) g_pages[value].state.store(PageState::Active, std::memory_order_release);
        }
        critical_section_exit(&g_gate);
    }
#endif
}

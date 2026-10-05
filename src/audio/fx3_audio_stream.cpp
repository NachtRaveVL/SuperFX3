/*
 * NR-RetroWorks SuperFX3 Firmware
 * Copyright (C) 2026 NR-RetroWorks
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 3 or later.
 */

#include "fx3_audio_stream.h"

#include <atomic>

#include "pico.h"
#include "pico/sync.h"

namespace {
std::atomic<uint8_t> g_asset_low {0};
std::atomic<uint8_t> g_asset_high {0};
std::atomic<uint8_t> g_flags {0};
std::atomic<uint8_t> g_status {static_cast<uint8_t>(Fx3AudioStatus::Idle)};
std::atomic<uint8_t> g_error {static_cast<uint8_t>(Fx3AudioError::None)};
std::atomic<uint8_t> g_spc_level {0};

#if SUPERFX3_AUDIO_SD
constexpr uint32_t FIFO_MASK = fx3_audio::FIFO_SIZE - 1u;
constexpr uint32_t BRR_BLOCK_SIZE = 9;
constexpr uint32_t PACKET_SIZE = 3;
constexpr uint32_t VISIBLE_LINES = 224;
constexpr uint8_t PRIMING_SEGMENTS = 4;
constexpr uint32_t COMMAND_COUNT = 16;
constexpr uint32_t SPC_SEGMENT_BYTES = 252;

struct AudioCommand {
    Fx3AudioCommand command;
    uint16_t asset_id;
};
AudioCommand g_commands[COMMAND_COUNT] {};
std::atomic<uint32_t> g_command_read {0};
std::atomic<uint32_t> g_command_write {0};
std::atomic<bool> g_reset_requested {false};
critical_section_t g_page_gate;
bool g_page_gate_initialized = false;
uint32_t g_spc_credit = 7u * SPC_SEGMENT_BYTES;

static_assert((fx3_audio::FIFO_SIZE & FIFO_MASK) == 0, "Audio FIFO must be a power of two.");
static_assert(fx3_audio::HDMA_PAGE_SIZE >= 63u * 5u + 1u,
              "Audio HDMA page cannot hold one frame table.");

enum class PageState : uint8_t {
    Free,
    Building,
    Ready,
    Active,
};

struct HdmaPage {
    alignas(4) uint8_t data[fx3_audio::HDMA_PAGE_SIZE];
    std::atomic<uint8_t> state {static_cast<uint8_t>(PageState::Free)};
    uint8_t first_sequence = 0;
    uint8_t packets = 0;
    uint32_t generation = 0;
};

alignas(4) uint8_t g_fifo[fx3_audio::FIFO_SIZE];
HdmaPage g_pages[fx3_audio::HDMA_PAGE_COUNT];
Fx3AudioSource g_source {};
std::atomic<uint8_t> g_offered_page {fx3_audio::NO_PAGE};
std::atomic<uint8_t> g_active_page {fx3_audio::NO_PAGE};
std::atomic<uint32_t> g_counters[fx3_audio::CounterCount] {};
uint32_t g_fifo_read = 0;
uint32_t g_fifo_write = 0;
uint32_t g_fifo_count = 0;
uint32_t g_page_generation = 0;
uint8_t g_packet_sequence = 0;
uint8_t g_rate_phase = 0;
bool g_source_open = false;
bool g_source_eof = false;
bool g_paused = false;
bool g_refilling = false;
bool g_playback_started = false;
bool g_fifo_starved = false;

bool __not_in_flash_func(queue_command)(uint8_t value) {
    const uint32_t write = g_command_write.load(std::memory_order_relaxed);
    const uint32_t next = (write + 1u) % COMMAND_COUNT;
    if (next == g_command_read.load(std::memory_order_acquire))
        return false;
    g_commands[write] = {static_cast<Fx3AudioCommand>(value),
        static_cast<uint16_t>(g_asset_low.load(std::memory_order_relaxed) |
            static_cast<uint16_t>(g_asset_high.load(std::memory_order_relaxed)) << 8)};
    g_command_write.store(next, std::memory_order_release);
    return true;
}

void close_source() {
    if (g_source_open && g_source.close)
        g_source.close(g_source.context);
    g_source_open = false;
}

void clear_pages() {
    critical_section_enter_blocking(&g_page_gate);
    g_offered_page.store(fx3_audio::NO_PAGE, std::memory_order_release);
    // ACTIVE stays immutable until the host releases it at VBlank, even on error.
    for (HdmaPage& page : g_pages) {
        if (page.state.load(std::memory_order_relaxed) !=
                static_cast<uint8_t>(PageState::Active))
            page.state.store(static_cast<uint8_t>(PageState::Free), std::memory_order_release);
    }
    critical_section_exit(&g_page_gate);
}

void reset_stream() {
    close_source();
    g_fifo_read = 0;
    g_fifo_write = 0;
    g_fifo_count = 0;
    g_page_generation = 0;
    g_packet_sequence = 0;
    g_rate_phase = 0;
    g_source_eof = false;
    g_paused = false;
    g_refilling = false;
    g_playback_started = false;
    g_fifo_starved = false;
    clear_pages();
}

void fail(Fx3AudioError error) {
    reset_stream();
    g_error.store(static_cast<uint8_t>(error), std::memory_order_release);
    g_status.store(static_cast<uint8_t>(Fx3AudioStatus::Error), std::memory_order_release);
}

void stop() {
    reset_stream();
    g_error.store(static_cast<uint8_t>(Fx3AudioError::None), std::memory_order_release);
    g_status.store(static_cast<uint8_t>(Fx3AudioStatus::Idle), std::memory_order_release);
}

void refresh_offer() {
    critical_section_enter_blocking(&g_page_gate);
    if (g_offered_page.load(std::memory_order_acquire) != fx3_audio::NO_PAGE) {
        critical_section_exit(&g_page_gate);
        return;
    }

    uint8_t selected = fx3_audio::NO_PAGE;
    uint32_t generation = UINT32_MAX;
    for (uint8_t i = 0; i < fx3_audio::HDMA_PAGE_COUNT; ++i) {
        if (g_pages[i].state.load(std::memory_order_acquire) ==
                static_cast<uint8_t>(PageState::Ready) &&
            g_pages[i].generation < generation) {
            selected = i;
            generation = g_pages[i].generation;
        }
    }

    uint8_t expected = fx3_audio::NO_PAGE;
    if (selected != fx3_audio::NO_PAGE)
        g_offered_page.compare_exchange_strong(expected, selected,
                                                std::memory_order_release,
                                                std::memory_order_relaxed);
    critical_section_exit(&g_page_gate);
}

void __not_in_flash_func(release_active_page)() {
    const uint8_t active = g_active_page.exchange(fx3_audio::NO_PAGE,
                                                   std::memory_order_acq_rel);
    if (active < fx3_audio::HDMA_PAGE_COUNT)
        g_pages[active].state.store(static_cast<uint8_t>(PageState::Free),
                                    std::memory_order_release);
}

uint8_t __not_in_flash_func(offered_page)() {
    const uint8_t page = g_offered_page.load(std::memory_order_acquire);
    const Fx3AudioStatus status = static_cast<Fx3AudioStatus>(
        g_status.load(std::memory_order_acquire));
    if (page >= fx3_audio::HDMA_PAGE_COUNT ||
        (status != Fx3AudioStatus::Priming && status != Fx3AudioStatus::Playing &&
         status != Fx3AudioStatus::Draining && status != Fx3AudioStatus::Underrun) ||
        g_pages[page].state.load(std::memory_order_acquire) !=
            static_cast<uint8_t>(PageState::Ready) ||
        g_spc_credit < static_cast<uint32_t>(g_pages[page].packets) * PACKET_SIZE)
        return fx3_audio::NO_PAGE;
    return page;
}

bool __not_in_flash_func(claim_page)(uint8_t page_index) {
    critical_section_enter_blocking(&g_page_gate);
    if (page_index == fx3_audio::NO_PAGE) {
        release_active_page();
        critical_section_exit(&g_page_gate);
        return true;
    }
    if (offered_page() != page_index) {
        critical_section_exit(&g_page_gate);
        return false;
    }

    uint8_t expected_state = static_cast<uint8_t>(PageState::Ready);
    if (!g_pages[page_index].state.compare_exchange_strong(
            expected_state, static_cast<uint8_t>(PageState::Active),
            std::memory_order_acq_rel, std::memory_order_relaxed)) {
        critical_section_exit(&g_page_gate);
        return false;
    }

    uint8_t expected_page = page_index;
    g_offered_page.compare_exchange_strong(expected_page, fx3_audio::NO_PAGE,
                                            std::memory_order_acq_rel,
                                            std::memory_order_relaxed);
    const uint8_t previous = g_active_page.exchange(page_index, std::memory_order_acq_rel);
    if (previous < fx3_audio::HDMA_PAGE_COUNT && previous != page_index)
        g_pages[previous].state.store(static_cast<uint8_t>(PageState::Free),
                                      std::memory_order_release);
    g_counters[fx3_audio::StreamPacketsSent].fetch_add(
        g_pages[page_index].packets, std::memory_order_relaxed);
    g_spc_credit -= static_cast<uint32_t>(g_pages[page_index].packets) * PACKET_SIZE;
    critical_section_exit(&g_page_gate);
    return true;
}

void fifo_pop(uint8_t* data, uint32_t size) {
    for (uint32_t i = 0; i < size; ++i) {
        data[i] = g_fifo[g_fifo_read];
        g_fifo_read = (g_fifo_read + 1u) & FIFO_MASK;
    }
    g_fifo_count -= size;
    uint32_t low = g_counters[fx3_audio::PicoFifoLowWater].load(std::memory_order_relaxed);
    while (g_fifo_count < low &&
           !g_counters[fx3_audio::PicoFifoLowWater].compare_exchange_weak(
               low, g_fifo_count, std::memory_order_relaxed)) {}
}

bool refill_fifo() {
    if (g_source_eof || !g_source_open)
        return true;
    if (g_fifo_count < fx3_audio::FIFO_LOW_WATER)
        g_refilling = true;
    if (!g_refilling)
        return true;
    // One bounded burst per service lets the GSU and control queue make progress.
    if (g_fifo_count < fx3_audio::FIFO_HIGH_WATER) {
        uint32_t size = fx3_audio::FIFO_SIZE - g_fifo_write;
        const uint32_t free = fx3_audio::FIFO_SIZE - g_fifo_count;
        const uint32_t wanted = fx3_audio::FIFO_HIGH_WATER - g_fifo_count;
        if (size > free) size = free;
        if (size > wanted) size = wanted;
        if (size > 4096u) size = 4096u;

        const int32_t read = g_source.read(g_source.context, &g_fifo[g_fifo_write], size);
        g_counters[fx3_audio::SdReads].fetch_add(1, std::memory_order_relaxed);
        if (read == -2)
            return true;
        if (read < 0 || static_cast<uint32_t>(read) > size) {
            fail(Fx3AudioError::ReadFailed);
            return false;
        }
        if (!read) {
            close_source();
            g_source_eof = true;
            g_refilling = false;
            return true;
        }

        const uint32_t bytes = static_cast<uint32_t>(read);
        g_fifo_write = (g_fifo_write + bytes) & FIFO_MASK;
        g_fifo_count += bytes;
        g_counters[fx3_audio::SdBytes].fetch_add(bytes, std::memory_order_relaxed);
    }
    if (g_fifo_count >= fx3_audio::FIFO_HIGH_WATER)
        g_refilling = false;
    return true;
}

uint8_t packets_per_page() {
    const uint8_t level = g_spc_level.load(std::memory_order_acquire);
    if (g_flags.load(std::memory_order_acquire) & fx3_audio::PAL_RATE)
        return level <= 2 ? 63 : level >= 6 ? 57 : 60;
    if (level <= 2)
        return 51;
    if (level >= 6)
        return 48;

    const uint8_t packets = g_rate_phase == 2 ? 48 : 51;
    g_rate_phase = static_cast<uint8_t>((g_rate_phase + 1u) % 3u);
    return packets;
}

bool build_page() {
    uint8_t selected = fx3_audio::NO_PAGE;
    for (uint8_t i = 0; i < fx3_audio::HDMA_PAGE_COUNT; ++i) {
        uint8_t expected = static_cast<uint8_t>(PageState::Free);
        if (g_pages[i].state.compare_exchange_strong(
                expected, static_cast<uint8_t>(PageState::Building),
                std::memory_order_acq_rel, std::memory_order_relaxed)) {
            selected = i;
            break;
        }
    }
    if (selected == fx3_audio::NO_PAGE)
        return false;

    uint8_t packets = packets_per_page();
    bool silence = false;
    if (g_source_eof) {
        const uint32_t complete_blocks = g_fifo_count / BRR_BLOCK_SIZE;
        if (!complete_blocks) {
            if (g_fifo_count) {
                g_fifo_count = 0;
                g_fifo_read = g_fifo_write;
                g_error.store(static_cast<uint8_t>(Fx3AudioError::PartialBlock),
                              std::memory_order_release);
            }
            g_pages[selected].state.store(static_cast<uint8_t>(PageState::Free),
                                           std::memory_order_release);
            return false;
        }
        const uint32_t available_packets = complete_blocks * 3u;
        if (available_packets < packets)
            packets = static_cast<uint8_t>(available_packets);
    } else if (g_fifo_count < static_cast<uint32_t>(packets) * PACKET_SIZE) {
        bool ready = false;
        for (const HdmaPage& page : g_pages)
            ready |= page.state.load(std::memory_order_acquire) ==
                static_cast<uint8_t>(PageState::Ready);
        if (!g_playback_started || ready) {
            g_pages[selected].state.store(static_cast<uint8_t>(PageState::Free),
                                           std::memory_order_release);
            return false;
        }
        // Keep partial source blocks queued; insert only one READY silence frame.
        silence = true;
        g_fifo_starved = true;
        g_error.store(static_cast<uint8_t>(Fx3AudioError::FifoUnderrun),
                      std::memory_order_release);
    }

    HdmaPage& page = g_pages[selected];
    page.first_sequence = g_packet_sequence;
    page.packets = packets;
    page.generation = g_page_generation++;
    if (!silence)
        g_fifo_starved = false;

    const uint8_t base_lines = static_cast<uint8_t>(VISIBLE_LINES / packets);
    const uint8_t extra_lines = static_cast<uint8_t>(VISIBLE_LINES % packets);
    uint16_t line_accumulator = 0;
    uint32_t offset = 0;
    for (uint8_t packet = 0; packet < packets; ++packet) {
        uint8_t lines = base_lines;
        line_accumulator = static_cast<uint16_t>(line_accumulator + extra_lines);
        if (line_accumulator >= packets) {
            line_accumulator = static_cast<uint16_t>(line_accumulator - packets);
            ++lines;
        }
        page.data[offset++] = lines;
        if (silence) {
            page.data[offset] = 0;
            page.data[offset + 1u] = 0;
            page.data[offset + 2u] = 0;
        } else {
            fifo_pop(&page.data[offset], PACKET_SIZE);
        }
        offset += PACKET_SIZE;
        page.data[offset++] = g_packet_sequence++;
    }
    page.data[offset] = 0;
    page.state.store(static_cast<uint8_t>(PageState::Ready), std::memory_order_release);
    g_counters[fx3_audio::HdmaPagesBuilt].fetch_add(1, std::memory_order_relaxed);
    refresh_offer();
    return true;
}

bool pages_pending() {
    for (const HdmaPage& page : g_pages) {
        if (page.state.load(std::memory_order_acquire) !=
            static_cast<uint8_t>(PageState::Free))
            return true;
    }
    return false;
}

void update_status() {
    if (g_paused) {
        g_status.store(static_cast<uint8_t>(Fx3AudioStatus::Paused),
                       std::memory_order_release);
        return;
    }
    if (g_source_eof) {
        if (!g_fifo_count && !pages_pending() &&
            !g_spc_level.load(std::memory_order_acquire))
            g_status.store(static_cast<uint8_t>(Fx3AudioStatus::Eof),
                           std::memory_order_release);
        else
            g_status.store(static_cast<uint8_t>(Fx3AudioStatus::Draining),
                           std::memory_order_release);
        return;
    }

    const Fx3AudioStatus status = !g_playback_started ? Fx3AudioStatus::Priming :
        (g_fifo_starved ? Fx3AudioStatus::Underrun : Fx3AudioStatus::Playing);
    g_status.store(static_cast<uint8_t>(status), std::memory_order_release);
}

void play(uint16_t asset_id) {
    const Fx3AudioStatus status = static_cast<Fx3AudioStatus>(
        g_status.load(std::memory_order_acquire));
    if (status != Fx3AudioStatus::Idle && status != Fx3AudioStatus::Eof &&
        status != Fx3AudioStatus::Error) {
        g_error.store(static_cast<uint8_t>(Fx3AudioError::Busy), std::memory_order_release);
        return;
    }

    reset_stream();
    g_error.store(static_cast<uint8_t>(Fx3AudioError::None), std::memory_order_release);
    g_status.store(static_cast<uint8_t>(Fx3AudioStatus::Opening), std::memory_order_release);
    if (!g_source.open || !g_source.read) {
        fail(Fx3AudioError::Unavailable);
        return;
    }

    if (!g_source.open(g_source.context, asset_id)) {
        fail(Fx3AudioError::OpenFailed);
        return;
    }
    g_source_open = true;
    g_counters[fx3_audio::PicoFifoLowWater].store(fx3_audio::FIFO_SIZE,
                                                   std::memory_order_relaxed);
    g_status.store(static_cast<uint8_t>(Fx3AudioStatus::Priming),
                   std::memory_order_release);
}

void process_command() {
    if (g_reset_requested.exchange(false, std::memory_order_acq_rel)) {
        g_command_read.store(g_command_write.load(std::memory_order_acquire),
                             std::memory_order_release);
        stop();
    }
    for (uint32_t i = 0; i < COMMAND_COUNT; ++i) {
        const uint32_t read = g_command_read.load(std::memory_order_relaxed);
        if (read == g_command_write.load(std::memory_order_acquire))
            break;
        const AudioCommand command = g_commands[read];
        g_command_read.store((read + 1u) % COMMAND_COUNT, std::memory_order_release);

        switch (command.command) {
            case Fx3AudioCommand::Stop:
                stop();
                break;
            case Fx3AudioCommand::Play:
                play(command.asset_id);
                break;
            case Fx3AudioCommand::Pause:
                if (g_source_open || (g_source_eof &&
                        g_status.load(std::memory_order_acquire) !=
                            static_cast<uint8_t>(Fx3AudioStatus::Eof))) {
                    g_paused = true;
                    g_status.store(static_cast<uint8_t>(Fx3AudioStatus::Paused),
                                   std::memory_order_release);
                }
                break;
            case Fx3AudioCommand::Resume:
                g_paused = false;
                break;
        }
    }
}

uint8_t __not_in_flash_func(counter_read)(uint8_t offset) {
    const uint8_t relative = static_cast<uint8_t>(offset - fx3_audio::CounterBase);
    const uint8_t counter = static_cast<uint8_t>(relative >> 2);
    const uint8_t byte = static_cast<uint8_t>(relative & 3u);
    if (counter >= fx3_audio::CounterCount)
        return 0xFF;
    return static_cast<uint8_t>(
        g_counters[counter].load(std::memory_order_relaxed) >> (byte * 8u));
}

void __not_in_flash_func(report_event)(uint8_t value) {
    switch (static_cast<Fx3AudioEvent>(value)) {
        case Fx3AudioEvent::PageMissed:
            g_counters[fx3_audio::HdmaPagesMissed].fetch_add(1, std::memory_order_relaxed);
            break;
        case Fx3AudioEvent::PacketMissed:
            g_counters[fx3_audio::StreamPacketsMissed].fetch_add(1,
                                                                 std::memory_order_relaxed);
            break;
        case Fx3AudioEvent::SpcUnderrun:
            g_counters[fx3_audio::SpcUnderruns].fetch_add(1, std::memory_order_relaxed);
            break;
        case Fx3AudioEvent::SpcOverrun:
            g_counters[fx3_audio::SpcOverruns].fetch_add(1, std::memory_order_relaxed);
            break;
        default:
            break;
    }
}
#endif
}

void fx3_audio_init(const Fx3AudioSource& source) {
    g_asset_low.store(0, std::memory_order_relaxed);
    g_asset_high.store(0, std::memory_order_relaxed);
    g_flags.store(0, std::memory_order_relaxed);
    g_status.store(static_cast<uint8_t>(Fx3AudioStatus::Idle), std::memory_order_relaxed);
    g_error.store(static_cast<uint8_t>(Fx3AudioError::None), std::memory_order_relaxed);
    g_spc_level.store(0, std::memory_order_relaxed);
#if SUPERFX3_AUDIO_SD
    if (!g_page_gate_initialized) {
        critical_section_init(&g_page_gate);
        g_page_gate_initialized = true;
    }
    g_command_read.store(0, std::memory_order_relaxed);
    g_command_write.store(0, std::memory_order_relaxed);
    g_reset_requested.store(false, std::memory_order_relaxed);
    g_active_page.store(fx3_audio::NO_PAGE, std::memory_order_relaxed);
    g_spc_credit = 7u * SPC_SEGMENT_BYTES;
    close_source();
    g_source = source;
    for (HdmaPage& page : g_pages)
        page.state.store(static_cast<uint8_t>(PageState::Free), std::memory_order_relaxed);
    for (auto& counter : g_counters)
        counter.store(0, std::memory_order_relaxed);
    reset_stream();
#else
    (void)source;
#endif
}

void fx3_audio_task() {
#if SUPERFX3_AUDIO_SD
    process_command();
    const Fx3AudioStatus status = static_cast<Fx3AudioStatus>(
        g_status.load(std::memory_order_acquire));
    if (status == Fx3AudioStatus::Idle || status == Fx3AudioStatus::Opening ||
        status == Fx3AudioStatus::Error || status == Fx3AudioStatus::Eof || g_paused)
        return;

    if (!refill_fifo())
        return;
    if (g_spc_level.load(std::memory_order_acquire) >= PRIMING_SEGMENTS)
        g_playback_started = true;
    while (build_page()) {}
    refresh_offer();
    update_status();
#endif
}

void __not_in_flash_func(fx3_audio_request_reset)() {
#if SUPERFX3_AUDIO_SD
    g_reset_requested.store(true, std::memory_order_release);
#else
    g_error.store(static_cast<uint8_t>(Fx3AudioError::None), std::memory_order_release);
    g_status.store(static_cast<uint8_t>(Fx3AudioStatus::Idle), std::memory_order_release);
#endif
}

bool __not_in_flash_func(fx3_audio_mmio_address)(uint32_t address) {
    const uint16_t offset = static_cast<uint16_t>(address);
    return offset >= fx3_audio::MMIO_BASE && offset <= fx3_audio::MMIO_END;
}

bool __not_in_flash_func(fx3_audio_hdma_address)(uint32_t address) {
    const uint16_t offset = static_cast<uint16_t>(address);
    return offset >= fx3_audio::HDMA_BASE && offset <= fx3_audio::HDMA_END;
}

uint8_t __not_in_flash_func(fx3_audio_host_read)(uint16_t address) {
    const uint8_t offset = static_cast<uint8_t>(address - fx3_audio::MMIO_BASE);
    switch (offset) {
        case fx3_audio::AssetIdLow:
            return g_asset_low.load(std::memory_order_acquire);
        case fx3_audio::AssetIdHigh:
            return g_asset_high.load(std::memory_order_acquire);
        case fx3_audio::Flags:
            return g_flags.load(std::memory_order_acquire);
        case fx3_audio::Status:
            return g_status.load(std::memory_order_acquire);
        case fx3_audio::Error:
            return g_error.load(std::memory_order_acquire);
        case fx3_audio::SpcLevel:
            return g_spc_level.load(std::memory_order_acquire);
#if SUPERFX3_AUDIO_SD
        case fx3_audio::HdmaPage: {
            critical_section_enter_blocking(&g_page_gate);
            const uint8_t page = offered_page();
            critical_section_exit(&g_page_gate);
            return page;
        }
        case fx3_audio::HdmaSequence:
        case fx3_audio::HdmaLastSequence: {
            critical_section_enter_blocking(&g_page_gate);
            const uint8_t page = offered_page();
            const uint8_t sequence = page < fx3_audio::HDMA_PAGE_COUNT ?
                static_cast<uint8_t>(g_pages[page].first_sequence +
                    (offset == fx3_audio::HdmaLastSequence ? g_pages[page].packets - 1u : 0u)) :
                fx3_audio::NO_PAGE;
            critical_section_exit(&g_page_gate);
            return sequence;
        }
#else
        case fx3_audio::HdmaPage:
        case fx3_audio::HdmaSequence:
        case fx3_audio::HdmaLastSequence:
            return fx3_audio::NO_PAGE;
#endif
        default:
#if SUPERFX3_AUDIO_SD
            if (offset >= fx3_audio::CounterBase)
                return counter_read(offset);
#endif
            break;
    }
    return 0xFF;
}

void __not_in_flash_func(fx3_audio_host_write)(uint16_t address, uint8_t value) {
    const uint8_t offset = static_cast<uint8_t>(address - fx3_audio::MMIO_BASE);
    switch (offset) {
        case fx3_audio::Command:
            if (value > static_cast<uint8_t>(Fx3AudioCommand::Resume)) {
                g_error.store(static_cast<uint8_t>(Fx3AudioError::InvalidCommand),
                              std::memory_order_release);
                break;
            }
#if SUPERFX3_AUDIO_SD
            if (!queue_command(value))
                g_error.store(static_cast<uint8_t>(Fx3AudioError::CommandQueueFull),
                              std::memory_order_release);
#else
            if (value == static_cast<uint8_t>(Fx3AudioCommand::Stop)) {
                g_error.store(static_cast<uint8_t>(Fx3AudioError::None),
                              std::memory_order_release);
                g_status.store(static_cast<uint8_t>(Fx3AudioStatus::Idle),
                               std::memory_order_release);
            } else if (value == static_cast<uint8_t>(Fx3AudioCommand::Play)) {
                g_error.store(static_cast<uint8_t>(Fx3AudioError::Unavailable),
                              std::memory_order_release);
                g_status.store(static_cast<uint8_t>(Fx3AudioStatus::Error),
                               std::memory_order_release);
            }
#endif
            break;
        case fx3_audio::AssetIdLow:
            g_asset_low.store(value, std::memory_order_release);
            break;
        case fx3_audio::AssetIdHigh:
            g_asset_high.store(value, std::memory_order_release);
            break;
        case fx3_audio::Flags:
            g_flags.store(value, std::memory_order_release);
            break;
        case fx3_audio::Error:
            if (!value)
                g_error.store(static_cast<uint8_t>(Fx3AudioError::None),
                              std::memory_order_release);
            break;
        case fx3_audio::SpcLevel:
            g_spc_level.store(value > 8 ? 8 : value, std::memory_order_release);
#if SUPERFX3_AUDIO_SD
            critical_section_enter_blocking(&g_page_gate);
            // Leave one segment free for a partially written/decoding block.
            g_spc_credit = value >= 7 ? 0u : (7u - value) * SPC_SEGMENT_BYTES;
            critical_section_exit(&g_page_gate);
#endif
            break;
#if SUPERFX3_AUDIO_SD
        case fx3_audio::HdmaPage:
            if (!claim_page(value)) {
                g_counters[fx3_audio::HdmaPagesMissed].fetch_add(1,
                                                                 std::memory_order_relaxed);
            }
            break;
        case fx3_audio::Event:
            report_event(value);
            break;
#endif
        default:
            break;
    }
}

uint8_t __not_in_flash_func(fx3_audio_hdma_read)(uint16_t address) {
    if (address < fx3_audio::HDMA_BASE || address >= fx3_audio::HDMA_EMPTY)
        return 0;
#if SUPERFX3_AUDIO_SD
    const uint16_t relative = static_cast<uint16_t>(address - fx3_audio::HDMA_BASE);
    const uint8_t page_index = static_cast<uint8_t>(relative / fx3_audio::HDMA_PAGE_SIZE);
    const uint16_t page_offset = static_cast<uint16_t>(relative % fx3_audio::HDMA_PAGE_SIZE);
    critical_section_enter_blocking(&g_page_gate);
    const uint8_t state = g_pages[page_index].state.load(std::memory_order_acquire);
    uint8_t value = 0;
    if (state == static_cast<uint8_t>(PageState::Ready) ||
        state == static_cast<uint8_t>(PageState::Active))
        value = g_pages[page_index].data[page_offset];
    critical_section_exit(&g_page_gate);
    return value;
#else
    (void)address;
#endif
    return 0;
}

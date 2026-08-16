/*
 * NR-RetroWorks SuperFX3 Firmware
 * Copyright (C) 2026 NR-RetroWorks
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 3 or later.
 */

#include <atomic>

#include "fx_sync.h"
#include "snes_pio.h"

#include "pico.h"
#include "pico/sync.h"

static constexpr uint32_t FX_SYNC_COMMAND_COUNT = 256; ///< Entries in the cross-core command ring.
static constexpr uint32_t FX_SYNC_COMMAND_MASK = FX_SYNC_COMMAND_COUNT - 1; ///< Wrap mask for command-ring indices.

static constexpr uint32_t FX_SYNC_ACCESS_ROM = 1u << 0; ///< Published SNES ROM-access permission bit.
static constexpr uint32_t FX_SYNC_ACCESS_RAM = 1u << 1; ///< Published SNES RAM-access permission bit.

enum class FxSyncCommandType : uint8_t {
    CpuWrite,                          // Forward a captured SNES CPU write to the FX core.
    Reset                              // Reset the FX core state.
};

struct FxSyncCommand {
    uint16_t addr;                     // SNES register/address associated with the command.
    uint8_t value;                     // Data byte written by the SNES CPU.
    FxSyncCommandType type;            // Operation core 1 should perform.
};

static_assert((FX_SYNC_COMMAND_COUNT & FX_SYNC_COMMAND_MASK) == 0);
static_assert(std::atomic<uint32_t>::is_always_lock_free,
              "Cross-core snapshots require lock-free 32-bit atomics on RP2350.");
static_assert(std::atomic<bool>::is_always_lock_free,
              "Cross-core ownership flags require lock-free bool atomics on RP2350.");

static SuperFx* g_fx = nullptr;
static FxBackend g_backend {};

static FxSyncCommand g_commands[FX_SYNC_COMMAND_COUNT] {};
static std::atomic<uint32_t> g_command_write {0};
static std::atomic<uint32_t> g_command_read {0};
static std::atomic<bool> g_command_overflow {false};

static critical_section_t g_state_gate;
static std::atomic<bool> g_core1_owns_fx {false};
static std::atomic<bool> g_irq_clear_pending {false};
static std::atomic<uint32_t> g_runtime_snapshot {0};
static std::atomic<uint32_t> g_access_snapshot {FX_SYNC_ACCESS_ROM | FX_SYNC_ACCESS_RAM};

// Enters the short cross-core gate used when ownership of FxState can change.
static inline void fx_sync_lock_state() {
    critical_section_enter_blocking(&g_state_gate);
}

// Leaves the short cross-core FxState ownership gate.
static inline void fx_sync_unlock_state() {
    critical_section_exit(&g_state_gate);
}

// Packs the low SFR byte for the lock-free runtime snapshot.
static inline uint8_t fx_sync_flags_low(const FxState& state) {
    return static_cast<uint8_t>((state.flags.zero << 1) |
                                (state.flags.carry << 2) |
                                (state.flags.sign << 3) |
                                (state.flags.overflow << 4) |
                                (state.flags.running << 5) |
                                (state.flags.rom_read_pending << 6));
}

// Packs the high SFR byte for the lock-free runtime snapshot.
static inline uint8_t fx_sync_flags_high(const FxState& state) {
    return static_cast<uint8_t>((state.flags.alt1 << 0) |
                                (state.flags.alt2 << 1) |
                                (state.flags.imm_low << 2) |
                                (state.flags.imm_high << 3) |
                                (state.flags.prefix << 4) |
                                (state.flags.irq << 7));
}

// Builds the current SNES ROM/RAM ownership snapshot.
static inline uint32_t fx_sync_access_state(const SuperFx& fx) {
    const FxState& state = fx.state();
    uint32_t access = 0;

    if (fx.config().chip == FxChip::FX3 || !state.flags.running || !state.gsu_rom_access)
        access |= FX_SYNC_ACCESS_ROM;
    if (fx.config().chip == FxChip::FX3 || !state.flags.running || !state.gsu_ram_access)
        access |= FX_SYNC_ACCESS_RAM;

    return access;
}

// Publishes R15, status flags, and memory ownership for core-0 reads.
static void __not_in_flash_func(fx_sync_publish_state)() {
    const FxState& state = g_fx->state();

    uint8_t flags_high = fx_sync_flags_high(state);
    if (g_irq_clear_pending.load(std::memory_order_acquire))
        flags_high &= 0x7F;

    const uint32_t snapshot =
        static_cast<uint32_t>(state.r[15]) |
        (static_cast<uint32_t>(fx_sync_flags_low(state)) << 16) |
        (static_cast<uint32_t>(flags_high) << 24);

    const uint32_t access = fx_sync_access_state(*g_fx);
    const uint32_t previous_access = g_access_snapshot.exchange(access, std::memory_order_release);

    // FIXME: Publish GSU ownership and runtime status as one versioned snapshot.
    // They are separate atomics today, so core 0 can observe new access ownership with old
    // status, or the reverse. Add a generation counter or another linearization mechanism.

    // ROM ownership is also consumed directly by the PIO read state machine. Notify it only
    // when that bit changes so core 1 does not poke PIO on every instruction.
    if ((previous_access ^ access) & FX_SYNC_ACCESS_ROM)
        snes_pio_request_rom_ownership((access & FX_SYNC_ACCESS_ROM) == 0);

    g_runtime_snapshot.store(snapshot, std::memory_order_release);
}

// Queues one SNES-side state mutation for core 1 at an instruction boundary.
// FIXME: Define a bounded failure-free policy for a full cross-core command ring.
// The PIO write IRQ currently spins until core 1 creates space. Measure the required queue
// depth and remove indefinite blocking if saturation is reachable; legacy GSU1/2 also retains
// the physical-ROM circular-wait risk.
static bool __not_in_flash_func(fx_sync_queue_command)(FxSyncCommandType type, uint16_t addr = 0, uint8_t value = 0) {
    const uint32_t write = g_command_write.load(std::memory_order_relaxed);
    const uint32_t next = (write + 1) & FX_SYNC_COMMAND_MASK;

    if (next == g_command_read.load(std::memory_order_acquire)) {
        g_command_overflow.store(true, std::memory_order_release);
        return false;
    } else {
        g_commands[write] = {addr, value, type};
        g_command_write.store(next, std::memory_order_release);
        return true;
    }
}

// Removes the next pending cross-core command from the single-producer queue.
static bool fx_sync_pop_command(FxSyncCommand& command) {
    const uint32_t read = g_command_read.load(std::memory_order_relaxed);

    if (read == g_command_write.load(std::memory_order_acquire))
        return false;
    else {
        command = g_commands[read];
        g_command_read.store((read + 1) & FX_SYNC_COMMAND_MASK, std::memory_order_release);
        return true;
    }
}

void fx_sync_init(SuperFx& fx, const FxBackend& backend) {
    g_fx = &fx;
    g_backend = backend;

    g_command_write.store(0, std::memory_order_relaxed);
    g_command_read.store(0, std::memory_order_relaxed);
    g_command_overflow.store(false, std::memory_order_relaxed);

    critical_section_init(&g_state_gate);

    g_core1_owns_fx.store(false, std::memory_order_relaxed);
    g_irq_clear_pending.store(false, std::memory_order_relaxed);

    fx_sync_publish_state();
}

bool fx_sync_core1_service() {
    if (!g_core1_owns_fx.load(std::memory_order_acquire))
        return false;

    // SFR-high reads clear IRQ at the next core-1 instruction boundary without consuming queue space.
    if (g_irq_clear_pending.exchange(false, std::memory_order_acq_rel))
        g_fx->cpu_read(0x3031);

    FxSyncCommand command {};

    // FIXME: Verify CPU-write ordering relative to GSU instruction boundaries.
    // Core 1 drains the entire command ring before running the next GSU instruction, which is
    // deterministic but may collapse several real bus writes into one boundary. Test STOP/GO
    // and ownership-sensitive sequences against hardware or trusted traces.
    while (fx_sync_pop_command(command)) {
        switch (command.type) {
            case FxSyncCommandType::CpuWrite:
                g_fx->cpu_write(command.addr, command.value);
                break;

            case FxSyncCommandType::Reset:
                g_fx->reset();
                g_irq_clear_pending.store(false, std::memory_order_release);
                break;

        }
    }

    // FIXME: Add real SNES clock timing before treating FX1/FX2 execution as cycle-accurate.
    // The RP2350 service loop has no master-clock timestamp to feed run_accurate(). FX3 is
    // intentionally using Unlimited timing, but legacy timing modes are not validated yet.
    if (g_fx->running())
        g_fx->run_unlimited(1);

    fx_sync_publish_state();

    if (!g_fx->running()) {
        // The gate closes the ownership handoff race: core 0 cannot decide to
        // queue a command at the same time core 1 decides to release state_.
        fx_sync_lock_state();

        const bool commands_pending =
            g_command_read.load(std::memory_order_acquire) !=
            g_command_write.load(std::memory_order_acquire);
        const bool irq_clear_pending =
            g_irq_clear_pending.load(std::memory_order_acquire);

        if (!commands_pending && !irq_clear_pending)
            g_core1_owns_fx.store(false, std::memory_order_release);

        fx_sync_unlock_state();
    }

    return true;
}

uint8_t __not_in_flash_func(fx_sync_cpu_read)(uint16_t addr) {
    // Note: Keep the SNES-facing service path in SRAM so an XIP cache miss cannot stretch a PIO transaction.
    // NOTE: Open-bus reads mirror the core's Nintendo-style 0xFF policy instead of MesenCE's 0x00 fallback.

    // Preserve the external address for SuperFx::cpu_read(), which owns FX3's
    // mirrored $7000-$7FFF decode. A canonical copy is used only for snapshots.
    const uint16_t external_addr = addr;
    if (g_fx->config().chip == FxChip::FX3 && (addr & 0xF000) == 0x7000) {
        if ((addr & 0x0300) == 0x0300) return 0xFF;
        addr = static_cast<uint16_t>(0x3000 | (addr & 0x03FF));
    } else
        addr &= 0x33FF;

    fx_sync_lock_state();

    if (!g_core1_owns_fx.load(std::memory_order_relaxed)) {
        const uint8_t value = g_fx->cpu_read(external_addr);
        fx_sync_publish_state();
        fx_sync_unlock_state();
        return value;
    }

    fx_sync_unlock_state();

    const uint32_t snapshot =
        g_runtime_snapshot.load(std::memory_order_acquire);

    const uint16_t r15 =
        static_cast<uint16_t>(snapshot);
    const uint8_t sfr_low =
        static_cast<uint8_t>(snapshot >> 16);
    const uint8_t sfr_high =
        static_cast<uint8_t>(snapshot >> 24);

    // These reads remain valid while core 1 owns FxState. Handle them before
    // looking at the GO/running bit: after STOP there is a short handoff window
    // where the published snapshot is stopped but core 1 has not released state yet.
    if (g_fx->config().chip == FxChip::FX3) {
        if (addr == 0x301E)
            return static_cast<uint8_t>(r15);
        if (addr == 0x301F)
            return static_cast<uint8_t>(r15 >> 8);
    }

    switch (addr) {
        case 0x3030:
            return sfr_low;

        case 0x3031:
            // Only clear an IRQ that this published snapshot says was already pending.
            // Arming a deferred clear from an IRQ=0 read could erase a later STOP IRQ
            // that occurs after the CPU's read, which is not the hardware ordering.

            // NOTE: Restarting the GSU while an older IRQ is still pending can let a later
            // STOP race this deferred clear, so the clear still lacks event-level identity.
            if (!(sfr_high & 0x80))
                return sfr_high;

            g_irq_clear_pending.store(true, std::memory_order_release);

            if (g_backend.set_irq)
                g_backend.set_irq(g_backend.context, false);

            fx_sync_lock_state();

            if (!g_core1_owns_fx.load(std::memory_order_relaxed)) {
                g_fx->cpu_read(0x3031);
                g_irq_clear_pending.store(false, std::memory_order_release);
                fx_sync_publish_state();
            }

            fx_sync_unlock_state();
            return sfr_high;

        case 0x303B:
            return g_fx->config().chip == FxChip::FX3 ? 0x52 : 0x04;

        default:
            break;
    }

    if (!(sfr_low & 0x20)) {
        // Core 1 may have released ownership since the first check above. Retry
        // once so ordinary stopped-state registers do not unnecessarily appear as open bus.
        fx_sync_lock_state();

        if (!g_core1_owns_fx.load(std::memory_order_relaxed)) {
            const uint8_t value = g_fx->cpu_read(external_addr);
            fx_sync_publish_state();

            fx_sync_unlock_state();
            return value;
        }

        fx_sync_unlock_state();
    }

    // While core 1 still owns a running (or handoff-pending) state, only the
    // snapshot-safe registers above are exposed to the SNES CPU.
    return 0xFF;
}

bool __not_in_flash_func(fx_sync_cpu_write)(uint16_t addr, uint8_t value) {
    fx_sync_lock_state();

    if (g_core1_owns_fx.load(std::memory_order_relaxed)) {
        // Queue every register write while core 1 owns state_. SuperFx::cpu_write()
        // applies the architectural running-state restrictions when the command
        // reaches the instruction boundary. This avoids making that decision
        // from a potentially one-instruction-old snapshot.
        const bool queued = fx_sync_queue_command(FxSyncCommandType::CpuWrite, addr, value);

        fx_sync_unlock_state();
        return queued;
    }

    g_fx->cpu_write(addr, value);
    fx_sync_publish_state();

    if (g_fx->running())
        g_core1_owns_fx.store(true, std::memory_order_release);

    fx_sync_unlock_state();
    return true;
}

uint8_t __not_in_flash_func(fx_sync_cpu_ram_read)(uint32_t addr) {
    fx_sync_lock_state();

    if (!g_core1_owns_fx.load(std::memory_order_relaxed)) {
        const uint8_t value = g_fx->cpu_ram_read(addr);

        fx_sync_unlock_state();
        return value;
    }

    fx_sync_unlock_state();

    if (!(g_access_snapshot.load(std::memory_order_acquire) & FX_SYNC_ACCESS_RAM))
        return 0xFF;

    if (!g_backend.ram_read)
        return 0xFF;

    return g_backend.ram_read(g_backend.context, addr);
}

void __not_in_flash_func(fx_sync_cpu_ram_write)(uint32_t addr, uint8_t value) {
    fx_sync_lock_state();

    if (!g_core1_owns_fx.load(std::memory_order_relaxed)) {
        g_fx->cpu_ram_write(addr, value);

        fx_sync_unlock_state();
        return;
    }

    fx_sync_unlock_state();

    if (!(g_access_snapshot.load(std::memory_order_acquire) & FX_SYNC_ACCESS_RAM))
        return;

    if (g_backend.ram_write)
        g_backend.ram_write(g_backend.context, addr, value);
}

bool __not_in_flash_func(fx_sync_rom_access_allowed)() {
    if (g_core1_owns_fx.load(std::memory_order_acquire))
        return (g_access_snapshot.load(std::memory_order_acquire) & FX_SYNC_ACCESS_ROM) != 0;

    fx_sync_lock_state();

    const bool allowed = (fx_sync_access_state(*g_fx) & FX_SYNC_ACCESS_ROM) != 0;

    fx_sync_unlock_state();
    return allowed;
}

bool __not_in_flash_func(fx_sync_ram_access_allowed)() {
    if (g_core1_owns_fx.load(std::memory_order_acquire))
        return (g_access_snapshot.load(std::memory_order_acquire) & FX_SYNC_ACCESS_RAM) != 0;

    fx_sync_lock_state();

    const bool allowed = (fx_sync_access_state(*g_fx) & FX_SYNC_ACCESS_RAM) != 0;

    fx_sync_unlock_state();
    return allowed;
}

// Nintendo documents this blocked-ROM dummy pattern and the independent
// host sanity test checks all 16 low-address combinations.
uint8_t __not_in_flash_func(fx_sync_blocked_rom_value)(uint32_t addr) {
    if (addr & 0x01)
        return 0x01;

    switch (addr & 0x0E) {
        case 0x04:
            return 0x04;
        case 0x0A:
            return 0x08;
        case 0x0E:
            return 0x0C;
        default:
            return 0x00;
    }
}

bool fx_sync_reset() {
    fx_sync_lock_state();

    if (g_core1_owns_fx.load(std::memory_order_relaxed)) {
        g_irq_clear_pending.store(true, std::memory_order_release);
        if (g_backend.set_irq)
            g_backend.set_irq(g_backend.context, false);

        const bool queued = fx_sync_queue_command(FxSyncCommandType::Reset);

        fx_sync_unlock_state();
        return queued;
    }

    g_fx->reset();
    g_irq_clear_pending.store(false, std::memory_order_release);
    fx_sync_publish_state();

    fx_sync_unlock_state();
    return true;
}

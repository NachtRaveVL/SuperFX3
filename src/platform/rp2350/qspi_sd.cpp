/*
 * NR-RetroWorks SuperFX3 Firmware
 * Copyright (C) 2026 NR-RetroWorks
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 3 or later.
 */

#include "qspi_sd.h"
#include "qspi_bus.h"

#include "hardware/gpio.h"
#include "hardware/sync.h"
#include "pico.h"
#include "pico/stdlib.h"

#if SUPERFX3_AUDIO_SD
#include "hardware/structs/io_qspi.h"
#include "hardware/structs/pads_qspi.h"
#include "hardware/structs/qmi.h"
#include "hardware/structs/sio.h"
#include "hardware/structs/timer.h"
#include "hardware/structs/m33_eppb.h"
#include "hardware/regs/addressmap.h"

namespace {
constexpr uint32_t SCK = 1u << 26;
constexpr uint32_t MOSI = 1u << 28;
constexpr uint32_t MISO = 1u << 29;
constexpr uint32_t SPI_MASK = SCK | MOSI | MISO;

bool sram_buffer(const void* buffer, uint32_t size) {
#ifdef SUPERFX3_TEST
    (void)buffer;
    (void)size;
    return true;
#else
    const uintptr_t address = reinterpret_cast<uintptr_t>(buffer);
    return !buffer || (address >= SRAM_BASE && address < SRAM_END && size <= SRAM_END - address);
#endif
}

static __force_inline uint8_t spi_byte(uint8_t tx, uint32_t half_cycles) {
    uint8_t rx = 0;
    for (uint8_t bit = 0; bit < 8; ++bit) {
        if (tx & 0x80)
            sio_hw->gpio_hi_set = MOSI;
        else
            sio_hw->gpio_hi_clr = MOSI;
        busy_wait_at_least_cycles(half_cycles);
        sio_hw->gpio_hi_set = SCK;
        rx = static_cast<uint8_t>((rx << 1) | ((sio_hw->gpio_hi_in & MISO) ? 1u : 0u));
        busy_wait_at_least_cycles(half_cycles);
        sio_hw->gpio_hi_clr = SCK;
        tx = static_cast<uint8_t>(tx << 1);
    }
    return rx;
}

static __force_inline QspiSdResult checked_byte(uint8_t tx, uint8_t& rx,
                                                uint32_t half_cycles, uint32_t start) {
    if (qspi_bus_audio_cancelled())
        return QspiSdResult::Cancelled;
    if (static_cast<uint32_t>(timer0_hw->timerawl - start) >= 25000u)
        return QspiSdResult::Timeout;
    rx = spi_byte(tx, half_cycles);
    return QspiSdResult::Ok;
}

QspiSdResult __not_in_flash_func(sd_command_sram)(const uint8_t* command,
        uint8_t* response, uint32_t response_size, uint8_t* block,
        uint32_t block_size, uint32_t half_cycles, uint32_t start) {
    uint8_t input = 0xFF;
    QspiSdResult result = checked_byte(0xFF, input, half_cycles, start);
    for (uint32_t i = 0; i < 6 && result == QspiSdResult::Ok; ++i)
        result = checked_byte(command[i], input, half_cycles, start);
    if (result != QspiSdResult::Ok)
        return result;
    for (uint32_t i = 0; i < 8; ++i) {
        result = checked_byte(0xFF, input, half_cycles, start);
        if (result != QspiSdResult::Ok)
            return result;
        if (!(input & 0x80u))
            break;
    }
    if (input & 0x80u)
        return QspiSdResult::Timeout;
    response[0] = input;
    for (uint32_t i = 1; i < response_size; ++i) {
        result = checked_byte(0xFF, response[i], half_cycles, start);
        if (result != QspiSdResult::Ok)
            return result;
    }
    if (!block_size || response[0])
        return QspiSdResult::Ok;
    do {
        result = checked_byte(0xFF, input, half_cycles, start);
        if (result != QspiSdResult::Ok)
            return result;
    } while (input == 0xFF);
    if (input != 0xFE)
        return QspiSdResult::ProtocolError;
    for (uint32_t i = 0; i < block_size + 2u; ++i) {
        result = checked_byte(0xFF, block[i], half_cycles, start);
        if (result != QspiSdResult::Ok)
            return result;
    }
    return QspiSdResult::Ok;
}

QspiSdResult __no_inline_not_in_flash_func(qspi_sd_transfer_sram)(
        const uint8_t* tx, uint8_t* rx, uint32_t size, uint32_t half_cycles, bool select,
        bool command = false, uint8_t* block = nullptr, uint32_t block_size = 0) {
    const uint32_t interrupts = save_and_disable_interrupts();
    if (qspi_bus_audio_cancelled()) {
        restore_interrupts(interrupts);
        return QspiSdResult::Cancelled;
    }
    const uint32_t saved_csr = qmi_hw->direct_csr;
    const uint32_t start = timer0_hw->timerawl;
    if (saved_csr & (QMI_DIRECT_CSR_EN_BITS | QMI_DIRECT_CSR_ASSERT_CS0N_BITS |
                     QMI_DIRECT_CSR_ASSERT_CS1N_BITS)) {
        restore_interrupts(interrupts);
        return QspiSdResult::Busy;
    }
    // No flash command is issued: its continuous-read state and QMI formats stay intact.
    qmi_hw->direct_csr = QMI_DIRECT_CSR_EN_BITS;
    while (qmi_hw->direct_csr & QMI_DIRECT_CSR_BUSY_BITS) {
        if (qspi_bus_audio_cancelled() ||
            static_cast<uint32_t>(timer0_hw->timerawl - start) >= 25000u) {
            qmi_hw->direct_csr = saved_csr;
            __dsb();
            __isb();
            restore_interrupts(interrupts);
            return QspiSdResult::Timeout;
        }
    }

    const uint32_t saved_clock = io_qspi_hw->io[0].ctrl;
    const uint32_t saved_cs = io_qspi_hw->io[1].ctrl;
    const uint32_t saved_mosi = io_qspi_hw->io[2].ctrl;
    const uint32_t saved_miso = io_qspi_hw->io[3].ctrl;
    const uint32_t saved_pad_clock = pads_qspi_hw->io[0];
    const uint32_t saved_pad_mosi = pads_qspi_hw->io[2];
    const uint32_t saved_pad_miso = pads_qspi_hw->io[3];
    const uint32_t saved_output = sio_hw->gpio_hi_out & SPI_MASK;
    const uint32_t saved_oe = sio_hw->gpio_hi_oe & SPI_MASK;
    io_qspi_hw->io[1].ctrl = (saved_cs & ~IO_QSPI_GPIO_QSPI_SS_CTRL_OUTOVER_BITS) |
        (IO_QSPI_GPIO_QSPI_SS_CTRL_OUTOVER_VALUE_HIGH << IO_QSPI_GPIO_QSPI_SS_CTRL_OUTOVER_LSB);
    sio_hw->gpio_hi_oe_clr = SPI_MASK;
    sio_hw->gpio_hi_clr = SCK;
    sio_hw->gpio_hi_set = MOSI;
    sio_hw->gpio_hi_oe_set = SCK | MOSI;
    pads_qspi_hw->io[0] = saved_pad_clock &
        ~(PADS_QSPI_GPIO_QSPI_SCLK_ISO_BITS | PADS_QSPI_GPIO_QSPI_SCLK_OD_BITS);
    pads_qspi_hw->io[2] = saved_pad_mosi &
        ~(PADS_QSPI_GPIO_QSPI_SD0_ISO_BITS | PADS_QSPI_GPIO_QSPI_SD0_OD_BITS);
    pads_qspi_hw->io[3] = (saved_pad_miso & ~PADS_QSPI_GPIO_QSPI_SD1_ISO_BITS) |
        PADS_QSPI_GPIO_QSPI_SD1_IE_BITS;
    io_qspi_hw->io[0].ctrl = GPIO_FUNC1_SIO;
    io_qspi_hw->io[2].ctrl = GPIO_FUNC1_SIO;
    io_qspi_hw->io[3].ctrl = GPIO_FUNC1_SIO;
    if (select)
        gpio_put(SNES_SD_CS_N_PIN, 0);

    QspiSdResult result = QspiSdResult::Ok;
    if (command)
        result = sd_command_sram(tx, rx, size, block, block_size, half_cycles, start);
    for (uint32_t i = 0; !command && i < size; ++i) {
        if (qspi_bus_audio_cancelled()) {
            result = QspiSdResult::Cancelled;
            break;
        }
        if (static_cast<uint32_t>(timer0_hw->timerawl - start) >= 25000u) {
            result = QspiSdResult::Timeout;
            break;
        }
        const uint8_t input = spi_byte(tx ? tx[i] : 0xFF, half_cycles);
        if (rx)
            rx[i] = input;
    }
    gpio_put(SNES_SD_CS_N_PIN, 1);
    // Give the card eight deselected clocks to release MISO before flash resumes.
    spi_byte(0xFF, half_cycles);
    sio_hw->gpio_hi_oe_clr = SPI_MASK;
    sio_hw->gpio_hi_set = saved_output;
    sio_hw->gpio_hi_clr = SPI_MASK & ~saved_output;
    sio_hw->gpio_hi_oe_set = saved_oe;
    pads_qspi_hw->io[0] = saved_pad_clock;
    pads_qspi_hw->io[2] = saved_pad_mosi;
    pads_qspi_hw->io[3] = saved_pad_miso;
    io_qspi_hw->io[0].ctrl = saved_clock;
    io_qspi_hw->io[2].ctrl = saved_mosi;
    io_qspi_hw->io[3].ctrl = saved_miso;
    io_qspi_hw->io[1].ctrl = saved_cs;
    __dsb();
    qmi_hw->direct_csr = saved_csr;
    __dsb();
    __isb();
    restore_interrupts(interrupts);
    return result;
}

}
#endif

QspiSdResult qspi_sd_command(const uint8_t* command, uint8_t* response,
                            uint32_t response_size, uint8_t* block,
                            uint32_t block_size, uint32_t clock_hz) {
#if SUPERFX3_AUDIO_SD
    if (sio_hw->cpuid != 1 || !command || !response ||
        (response_size != 1 && response_size != 5) ||
        (block_size && (block_size != 16 && block_size != 512)) ||
        (block_size && !block) || clock_hz < 250000u || clock_hz > 4000000u ||
        !sram_buffer(command, 6) || !sram_buffer(response, response_size) ||
        !sram_buffer(block, block_size + 2u) ||
        m33_eppb_hw->nmi_mask[0] || m33_eppb_hw->nmi_mask[1])
        return QspiSdResult::Unavailable;
    if (!qspi_bus_audio_begin())
        return QspiSdResult::Busy;
    const uint32_t half_cycles = (150000000u + 2u * clock_hz - 1u) / (2u * clock_hz);
    const QspiSdResult result = qspi_sd_transfer_sram(command, response, response_size,
                                                    half_cycles, true, true, block, block_size);
    qspi_bus_audio_end();
    return result;
#else
    (void)command; (void)response; (void)response_size; (void)block;
    (void)block_size; (void)clock_hz;
    return QspiSdResult::Unavailable;
#endif
}

QspiSdResult qspi_sd_exchange(const uint8_t* tx, uint8_t* rx, uint32_t size,
                             uint32_t clock_hz, bool select) {
#if SUPERFX3_AUDIO_SD
    if (sio_hw->cpuid != 1 || !size || size > 4096u || clock_hz < 250000u ||
        clock_hz > 4000000u || !sram_buffer(tx, size) || !sram_buffer(rx, size))
        return QspiSdResult::Unavailable;
    if (m33_eppb_hw->nmi_mask[0] || m33_eppb_hw->nmi_mask[1])
        return QspiSdResult::Unavailable;
    const uint32_t half_cycles = (150000000u + 2u * clock_hz - 1u) / (2u * clock_hz);
    if (!qspi_bus_audio_begin())
        return QspiSdResult::Busy;
    const QspiSdResult result = qspi_sd_transfer_sram(tx, rx, size, half_cycles, select);
    qspi_bus_audio_end();
    return result;
#else
    (void)tx; (void)rx; (void)size; (void)clock_hz; (void)select;
    return QspiSdResult::Unavailable;
#endif
}

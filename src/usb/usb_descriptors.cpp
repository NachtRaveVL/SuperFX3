/*
 * NR-RetroWorks SuperFX3 Firmware
 * Copyright (C) 2026 NR-RetroWorks
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 3 or later.
 */

#include "tusb.h"

#include <string.h>

namespace {
constexpr uint8_t ITF_NUM_MSC = 0;
constexpr uint8_t ITF_NUM_TOTAL = 1;
constexpr uint8_t EP_MSC_OUT = 0x01;
constexpr uint8_t EP_MSC_IN = 0x81;
constexpr uint16_t CONFIG_TOTAL_LEN = TUD_CONFIG_DESC_LEN + TUD_MSC_DESC_LEN;

tusb_desc_device_t const DEVICE_DESCRIPTOR = {
    sizeof(tusb_desc_device_t), TUSB_DESC_DEVICE, 0x0200,
    0x00, 0x00, 0x00, CFG_TUD_ENDPOINT0_SIZE,
    0x2E8A, 0x000A, 0x0101,
    0x01, 0x02, 0x03, 0x01,
};

uint8_t const CONFIG_DESCRIPTOR[] = {
    TUD_CONFIG_DESCRIPTOR(1, ITF_NUM_TOTAL, 0, CONFIG_TOTAL_LEN,
                          TUSB_DESC_CONFIG_ATT_REMOTE_WAKEUP, 100),
    TUD_MSC_DESCRIPTOR(ITF_NUM_MSC, 4, EP_MSC_OUT, EP_MSC_IN, 64),
};

char const* const STRINGS[] = {
    nullptr,
    "NR-RetroWorks",
    "SuperFX3 ROM Loader",
    "SFX3-0001",
    "ROM programming drive",
};

uint16_t string_descriptor[32];
}

extern "C" uint8_t const* tud_descriptor_device_cb() {
    return reinterpret_cast<uint8_t const*>(&DEVICE_DESCRIPTOR);
}

extern "C" uint8_t const* tud_descriptor_configuration_cb(uint8_t) {
    return CONFIG_DESCRIPTOR;
}

extern "C" uint16_t const* tud_descriptor_string_cb(uint8_t index, uint16_t) {
    uint8_t count = 0;
    if (index == 0) {
        string_descriptor[1] = 0x0409;
        count = 1;
    } else {
        if (index >= sizeof(STRINGS) / sizeof(STRINGS[0]))
            return nullptr;
        const char* value = STRINGS[index];
        count = static_cast<uint8_t>(strlen(value));
        if (count > 31)
            count = 31;
        for (uint8_t i = 0; i < count; ++i)
            string_descriptor[1 + i] = static_cast<uint8_t>(value[i]);
    }
    string_descriptor[0] = static_cast<uint16_t>((TUSB_DESC_STRING << 8) |
                                                   (2u * count + 2u));
    return string_descriptor;
}

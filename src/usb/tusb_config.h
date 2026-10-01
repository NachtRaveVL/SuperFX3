/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#define CFG_TUSB_OS OPT_OS_PICO
#define CFG_TUD_ENABLED 1
#define CFG_TUSB_RHPORT0_MODE OPT_MODE_DEVICE
#define CFG_TUD_ENDPOINT0_SIZE 64

#define CFG_TUD_CDC 0
#define CFG_TUD_MSC 1
#define CFG_TUD_HID 0
#define CFG_TUD_MIDI 0
#define CFG_TUD_VENDOR 0

#define CFG_TUD_MSC_EP_BUFSIZE 512
#define CFG_TUSB_MEM_ALIGN __attribute__((aligned(4)))

# Routed FX3 Board Firmware Port

The production PCB routing is implemented by `boards/snes_fx3.h`, the routed-bit helpers in `snes_bus_layout.h`, and the three-PIO/DMA front end in `snes_bus.pio` and `snes_pio.cpp`. This document records the hardware contract that those files and their tests enforce.

## Routed RP2350B pin map

GPIO / function | Package pin | Signal
--- | ---: | ---
GPIO0 | 77 | `/SNES_PRES`
GPIO1 | 78 | `/RD`
GPIO2 | 79 | `/WR`
GPIO3 | 80 | `/ROM_CE`
GPIO4 | 1 | `/A_OE`
GPIO5 | 2 | `/C_OE`
GPIO6 | 3 | `D_DIR`
GPIO7 | 4 | `/D_OE`
GPIO8 | 5 | `I_A12`
GPIO9 | 7 | `I_A11`
GPIO10 | 8 | `I_A13`
GPIO11 | 9 | `I_A10`
GPIO12 | 11 | `I_A14`
GPIO13 | 12 | `I_A9`
GPIO14 | 13 | `I_A15`
GPIO15 | 14 | `I_A8`
GPIO16 | 16 | `I_A16`
GPIO17 | 17 | `I_A7`
GPIO18 | 18 | `I_A17`
GPIO19 | 19 | `I_A6`
GPIO20 | 20 | `I_A18`
GPIO21 | 21 | `I_A5`
GPIO22 | 22 | `I_A19`
GPIO23 | 23 | `I_A4`
GPIO24 | 25 | `I_A20`
GPIO25 | 26 | `I_A3`
GPIO26 | 27 | `I_A21`
GPIO27 | 28 | `I_A2`
GPIO28 | 36 | `I_A22`
GPIO29 | 37 | `I_A1`
GPIO30 | 38 | `I_A23`
GPIO31 | 39 | `I_A0`
GPIO32 | 40 | `/I_IRQ`
GPIO33 | 42 | `/I_CART`
GPIO34 | 43 | `/I_RD`
GPIO35 | 44 | `/I_WR`
GPIO36 | 45 | `/I_RST`
GPIO37 | 46 | `I_CLK`
GPIO38 | 47 | `/O_IRQ`
GPIO39 | 48 | `/O_RST`
GPIO40 / ADC0 | 49 | `I_D4`
GPIO41 / ADC1 | 52 | `I_D0`
GPIO42 / ADC2 | 53 | `I_D5`
GPIO43 / ADC3 | 54 | `I_D1`
GPIO44 / ADC4 | 55 | `I_D6`
GPIO45 / ADC5 | 56 | `I_D2`
GPIO46 / ADC6 | 57 | `I_D7`
GPIO47 / ADC7 | 58 | `I_D3`
RUN | 35 | `/B_RST`

## Implemented bus architecture

1. GPIO8-GPIO31 are captured as one raw 24-bit word and converted to logical A0-A23 with explicit pack/unpack helpers.
2. GPIO40-GPIO47 are captured/driven in their physical order and converted to/from logical D0-D7.
3. PIO0 owns the lower control outputs and address capture. PIO1 watches `/I_WR`, captures data, and watches `/I_RST`. PIO2 routes `/I_RD` cycles and drives read data.
4. Three paced DMA bridges carry read-control words, write triggers, and captured addresses between PIO windows without treating the routed pins as contiguous logical bits.
5. `/I_RD` and `/I_WR` are input-only console observations. `/RD` and `/WR` on GPIO1/GPIO2 are outputs to the single parallel ROM, with GPIO3 as its only `/ROM_CE`.
6. Host tests exhaust routed data values, sample the 24-bit address round trip, verify direction ownership, and lock the exact production pin map.

## Console-absent ownership (USB mode)

`/SNES_PRES` is active low and is derived from the raw console 5V rail via Q1. When no powered console is present `/SNES_PRES` will be high and the Pico will:

* disable `/C_OE` before changing any translated-control GPIO direction;
* keep `/I_IRQ`, `/I_CART`, `/I_RD`, `/I_WR`, and `I_CLK` as inputs; in particular, `/I_RD` and `/I_WR` are never repurposed as ROM strobes;
* drive `/I_RST` locally, with a deliberate assertion/deassertion sequence, because the normal `/O_RST` path through the 74LVC2G07 cannot provide the translated reset input while the control translator is disabled;
* keep `/O_IRQ` and `/O_RST` as the RP2350-controlled open-drain output side;
* enable USB ROM programming only in this console-absent (USB mode) state.
* when a powered console appears, restores `/I_RST` to input mode before enabling `/C_OE`.

## SNES and USB mode selection

`/SNES_PRES` is the only mode-select input:

`/SNES_PRES` | Mode | Bus state
--- | --- | ---
Low | SNES mode | PIO/DMA active; translated console inputs enabled; parallel ROM available to SNES
High | USB mode | PIO/DMA stopped; address/control/data translators disabled; `/I_RST` locally driven; TinyUSB MSC connected

USB mode exposes a generated FAT16 volume without allocating a full disk image in SRAM. Incoming data is staged by cluster in parallel NOR, with a 128 KiB sector cache and 16 KiB spill area. Directory/FAT updates may arrive before or after data. Safe eject validates the final chain and queues installation; neither an intermediate size nor SYNCHRONIZE CACHE closes a file. Keep USB power until `/O_IRQ` releases. Mapped `.sfc`/`.smc` files support LoROM, HiROM, ExLoROM, and ExHiROM through 8 MiB; `.rom`/`.bin` accepts a complete 128-Mbit/16-MiB physical image (utilizing all 24 address lines). Headered 32/64 KiB ROMs are also supported. Copier headers are detected by size remainder and stripped. Fragmented chains are first reordered without overwriting unread pages, then mapped files are expanded in overlap-safe order. Aborted uploads require a new upload.

The installed map descriptor occupies 16 otherwise-unused bytes at parallel address `0x7E:0000` (normal access being routed to internal SNES WRAM). Extended modes use mapper-specific SRAM windows instead of hiding ROM behind full-bank `$70/$71` SRAM. The default/raw FX3 map keeps those full banks. See README for exact windows and the framebuffer tradeoff.

The IS29GL128 is operated in x8 mode. Byte program unlocks using `AAA:AA`, `555:55`, `AAA:A0`, then the program address/data. Sector erase uses the corresponding six-cycle `AAA/555` unlock and `30` sector command. The implementation polls DQ7, checks DQ5 on timeout, resets the read array after failure, and verifies programmed bytes. The loader supports the full 128-Mbit device: mapped source files may be up to 8 MiB and raw physical images are exactly 16 MiB. The offline packer remains available for SuperFX-extended layouts and deterministic manufacturing images.

## QSPI save indication

The 4 MiB W25Q32 QSPI device has exactly three partitions:

Purpose | Offset | Size
--- | --- | ---
RP2350 firmware | `0x000000-0x07FFFF` | 512 KiB
SRAM save journal | `0x080000-0x0FFFFF` | 512 KiB
Private FX code/data | `0x100000-0x3FFFFF` | 3 MiB

The external 128-Mbit parallel NOR contains only the SNES game/program image. It is not used for save records, FX3 execution code, or firmware storage. During USB programming mode, the game-image installer may use uncommitted areas of that same parallel NOR as temporary overlap-safe staging; this is not persistent save storage and is discarded as the final game image is built.

The QSPI save-journal write interface requires a busy callback with the same signature as `snes_irq_write`. It asserts `/O_IRQ` immediately before the first possible QSPI erase/program operation, holds it through payload and commit-header programming, and releases it through an RAII guard on all success and failure exits. Tests reject any erase or program callback that runs without the busy IRQ asserted and reject any storage-layer crossover between the QSPI journal and parallel-ROM installer.

`qspi_save.cpp` wires this to the SDK flash-safe API, boot restore, and reset/disconnect saves. Core 1 initializes the SDK lockout victim before saves are enabled. Storage busy is ORed with the core IRQ so acknowledging one does not release the other. On wrap, only the next 132 KiB slot is erased, retaining the newest committed snapshot. A save can occur only while console reset is already asserted or the console is isolated and power remains. Sudden loss of both power sources is not a save trigger.

## Completion criteria

The source port, host tests, and PIO instruction-memory checks are complete. Hardware closure still requires logic-analyzer traces for read/write timing, translator direction changes, and console-present transitions on the real board.

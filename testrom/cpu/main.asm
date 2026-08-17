; NR-RetroWorks SuperFX3 diagnostic ROM
.setcpu "65816"
.smart

.include "snes.inc"
.include "fx3.inc"
.include "test_abi.inc"
.include "fx_entries.inc"
.include "test_registry.inc"

.include "vars.asm"
.include "text.asm"
.include "ppu.asm"
.include "background.asm"
.include "input.asm"
.include "runner.asm"
.include "ui.asm"
.include "startup.asm"

.segment "RODATA"

StrTitle:        .byte "NR-RETROWORKS FX3 TEST SUITE", 0
StrMenuHelpA:    .byte "A RUN TEST", 0
StrMenuHelpStart:.byte "START RUN ALL", 0
StrASelect:      .byte "A RUN SELECTED TEST", 0
StrStartAll:     .byte "START RUN ALL TESTS", 0
StrRunning:      .byte "RUNNING", 0
StrResult:       .byte "RESULT", 0
StrPass:         .byte "PASS", 0
StrFail:         .byte "FAIL", 0
StrTimeout:      .byte "TIMEOUT", 0
StrExpected:     .byte "EXPECTED 0X", 0
StrActual:       .byte "ACTUAL   0X", 0
StrAddress:      .byte "ADDRESS  0X", 0
StrARunAgain:    .byte "A RUN AGAIN", 0
StrBBack:        .byte "B BACK", 0
StrRunAll:       .byte "RUN ALL", 0
StrPassCount:    .byte "PASS     0X", 0
StrFailCount:    .byte "FAIL     0X", 0
StrTimeoutCount: .byte "TIMEOUT  0X", 0
StrLastFailure:  .byte "LAST FAILURE", 0
StrAllPassed:    .byte "ALL TESTS PASSED", 0

; Column stride is 20 tiles * 64 bytes = $0500.
Fx3ColumnOffsets:
.repeat 27, I
    .word I * $0500
.endrepeat

; Exact pattern used by the firmware's FX3 CLEAR commands.
Fx3ClearPattern:
    .byte $FF,$00,$FF,$00,$FF,$00,$FF,$00,$FF,$00,$FF,$00,$FF,$00,$FF,$00
    .byte $00,$00,$00,$00,$00,$00,$00,$00,$00,$00,$00,$00,$00,$00,$00,$00
    .byte $00,$00,$00,$00,$00,$00,$00,$00,$00,$00,$00,$00,$00,$00,$00,$00
    .byte $00,$FF,$00,$FF,$00,$FF,$00,$FF,$00,$FF,$00,$FF,$00,$FF,$00,$FF

; One 8bpp pixel at x=0, y=0 with color $5A. The GSU planar cache maps
; the pixel into bit 7 of planes 1, 3, 4, and 6.
PlotExpectedTile:
    .byte $00,$80,$00,$00,$00,$00,$00,$00,$00,$00,$00,$00,$00,$00,$00,$00
    .byte $00,$80,$00,$00,$00,$00,$00,$00,$00,$00,$00,$00,$00,$00,$00,$00
    .byte $80,$00,$00,$00,$00,$00,$00,$00,$00,$00,$00,$00,$00,$00,$00,$00
    .byte $80,$00,$00,$00,$00,$00,$00,$00,$00,$00,$00,$00,$00,$00,$00,$00

PlotExpectedPattern:
    .incbin "generated/plot_pattern8bpp.bin"
PlotExpectedPatternEnd:

; Tile 1 on BG1 is reserved as an always-transparent 8bpp tile. The BG1 map
; points here everywhere except the single visual-result slot.
Blank8bppTile:
.repeat 64
    .byte $00
.endrepeat
Blank8bppTileEnd:

.segment "FONT"
Font4bpp:
    .incbin "generated/font4bpp.bin"
Font4bppEnd:

DiagnosticPalette:
    .incbin "generated/palette.bin"
DiagnosticPaletteEnd:

.segment "HEADER"
    .byte "NR FX3 DIAGNOSTIC    "
    .byte $20               ; LoROM, slow ROM
    .byte $00               ; ROM only in the standard SNES header
    .byte $05               ; 32 KiB source ROM
    .byte $00               ; Standard header RAM size is unused
    .byte $01               ; North America
    .byte $00               ; Developer ID
    .byte $00               ; Version
    .word $0000             ; Checksum complement, patched by build.py
    .word $0000             ; Checksum, patched by build.py

.segment "VECTORS"
    .word $0000             ; $FFE0 unused
    .word $0000             ; $FFE2 unused
    .word IrqHandler        ; $FFE4 COP
    .word IrqHandler        ; $FFE6 BRK
    .word IrqHandler        ; $FFE8 ABORT
    .word NmiHandler        ; $FFEA NMI
    .word $0000             ; $FFEC unused
    .word IrqHandler        ; $FFEE IRQ
    .word $0000             ; $FFF0 unused
    .word $0000             ; $FFF2 unused
    .word IrqHandler        ; $FFF4 COP
    .word $0000             ; $FFF6 unused
    .word IrqHandler        ; $FFF8 ABORT
    .word NmiHandler        ; $FFFA NMI
    .word Reset             ; $FFFC RESET
    .word IrqHandler        ; $FFFE IRQ/BRK

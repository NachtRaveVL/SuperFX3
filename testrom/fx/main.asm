; NR-RetroWorks SuperFX3 diagnostic kernels
.setcpu "6502"
.include "gsu.inc"
.include "test_abi.inc"

.segment "FXCODE"

.export FxKernel_Stop
.export FxKernel_RamMagic
.export FxKernel_AluAdd
.export FxKernel_RomBuffer
.export FxKernel_PipelineMix
.export FxKernel_PlotPixel
.export FxKernel_PlotTilePattern
.export FxKernel_RpixRoundTrip
.export FxKernel_ClearA
.export FxKernel_ClearB
.export FxKernel_ClearC
.export FxKernel_C2pA
.export FxKernel_C2pB
.export FxKernel_C2pC
.export FxKernel_MergeLegacy
.export FxKernel_Plot4bppScbr
.export FxKernel_RomFullRange
.export FxKernel_SaveAndStop

FxKernel_Stop:
    gsu_stop
    gsu_nop

FxKernel_RamMagic:
    gsu_iwt r0, $A55A
    gsu_sm TEST_RAM_MAGIC, r0
    gsu_stop
    gsu_nop

FxKernel_AluAdd:
    gsu_iwt r1, $1234
    gsu_iwt r2, $0011
    gsu_add r3, r1, r2
    gsu_sm TEST_RAM_ALU, r3
    gsu_stop
    gsu_nop

FxKernel_RomBuffer:
    gsu_iwt r0, $01          ; Kernels occupy canonical offset $008000 (GSU bank $01).
    gsu_romb r0
    gsu_iwt r14, FxRomProbeByte
    gsu_getb r0
    gsu_sm TEST_RAM_ROM, r0
    gsu_stop
    gsu_nop

; Cross-boundary pipeline test: FX ROM buffering feeds an ALU operation,
; which feeds a delayed shared-RAM store immediately before STOP.
FxKernel_PipelineMix:
    gsu_iwt r0, $01
    gsu_romb r0
    gsu_iwt r14, FxRomProbeByte
    gsu_getb r2
    gsu_iwt r3, $0011
    gsu_add r4, r2, r3
    gsu_sm TEST_RAM_PIPE, r4
    gsu_stop
    gsu_nop

; PLOT increments R1. RPIX is used afterward so the retained pixel cache is
; forced to RAM before STOP and can be checked by the 65816.
FxKernel_PlotPixel:
    gsu_iwt r1, $0000
    gsu_iwt r2, $0000
    gsu_iwt r0, $005A
    gsu_color r0
    gsu_plot
    gsu_iwt r1, $0000
    gsu_iwt r2, $0000
    gsu_rpix r3
    gsu_stop
    gsu_nop

; Draw a complete 8x8 tile. Each row relies on PLOT to advance R1,
; exercising repeated cache handoffs rather than validating one isolated pixel.
FxKernel_PlotTilePattern:
.repeat 8, RowIndex
    gsu_iwt r1, $0000
    gsu_iwt r2, RowIndex
.repeat 8, ColumnIndex
    gsu_iwt r0, (RowIndex * 8) + ColumnIndex + 1
    gsu_color r0
    gsu_plot
.endrepeat
.endrepeat
    ; RPIX flushes the final retained cache line before STOP.
    gsu_iwt r1, $0000
    gsu_iwt r2, $0000
    gsu_rpix r3
    gsu_stop
    gsu_nop

FxKernel_RpixRoundTrip:
    gsu_iwt r1, $0000
    gsu_iwt r2, $0000
    gsu_iwt r0, $005A
    gsu_color r0
    gsu_plot
    gsu_iwt r1, $0000
    gsu_iwt r2, $0000
    gsu_rpix r3
    gsu_sm TEST_RAM_RPIX, r3
    gsu_stop
    gsu_nop

FxKernel_ClearA:
    gsu_iwt r0, $0003
    gsu_merge
    gsu_stop
    gsu_nop

FxKernel_ClearB:
    gsu_iwt r0, $0004
    gsu_merge
    gsu_stop
    gsu_nop

FxKernel_ClearC:
    gsu_iwt r0, $0005
    gsu_merge
    gsu_stop
    gsu_nop

FxKernel_C2pA:
    gsu_iwt r0, $0000
    gsu_merge
    gsu_stop
    gsu_nop

FxKernel_C2pB:
    gsu_iwt r0, $0001
    gsu_merge
    gsu_stop
    gsu_nop

FxKernel_C2pC:
    gsu_iwt r0, $0002
    gsu_merge
    gsu_stop
    gsu_nop

; Only exact ALT1 selects the original GSU MERGE operation in FX3 mode. The
; following IWT also proves the prefix was consumed by MERGE's common cleanup.
FxKernel_MergeLegacy:
    gsu_iwt r7, $12AB
    gsu_iwt r8, $CD34
    gsu_alt1
    gsu_merge
    gsu_iwt r1, $BEEF
    gsu_sm TEST_RAM_MERGE, r0
    gsu_sm TEST_RAM_PREFIX, r1
    gsu_stop
    gsu_nop

; UltraStarFox2-compatible legacy 4bpp PLOT/RPIX at programmable SCBR=$08.
FxKernel_Plot4bppScbr:
    gsu_iwt r1, $0000
    gsu_iwt r2, $0000
.repeat 8, PixelIndex
    .if PixelIndex = 0
        gsu_iwt r0, $0001
    .elseif PixelIndex = 1
        gsu_iwt r0, $0002
    .elseif PixelIndex = 2
        gsu_iwt r0, $0004
    .elseif PixelIndex = 3
        gsu_iwt r0, $0008
    .elseif PixelIndex = 4
        gsu_iwt r0, $000F
    .elseif PixelIndex = 5
        gsu_iwt r0, $0003
    .elseif PixelIndex = 6
        gsu_iwt r0, $0005
    .else
        gsu_iwt r0, $000A
    .endif
    gsu_color r0
    gsu_plot
.endrepeat
    gsu_iwt r1, $0000
    gsu_iwt r2, $0000
    gsu_rpix r3
    gsu_sm TEST_RAM_RPIX4, r3
    gsu_stop
    gsu_nop

; Exercise all three architectural landmarks in the 3 MiB FX-visible ROM window.
FxKernel_RomFullRange:
    gsu_iwt r0, $005F
    gsu_romb r0
    gsu_iwt r14, $FFFF
    gsu_getb r1
    gsu_sm TEST_RAM_ROM_5F, r1

    gsu_iwt r0, $0060
    gsu_romb r0
    gsu_iwt r14, $0000
    gsu_getb r1
    gsu_sm TEST_RAM_ROM_60, r1

    gsu_iwt r0, $006F
    gsu_romb r0
    gsu_iwt r14, $FFFF
    gsu_getb r1
    gsu_sm TEST_RAM_ROM_6F, r1
    gsu_stop
    gsu_nop

FxKernel_SaveAndStop:
    gsu_iwt r0, TEST_SAVE_COOKIE_VALUE
    gsu_sm TEST_RAM_SAVE_COOKIE, r0
    gsu_iwt r0, TEST_SAVE_GUARD_VALUE
    gsu_sm TEST_RAM_SAVE_GUARD, r0
    gsu_alt3
    gsu_stop
    gsu_nop

FxRomProbeByte:
    .byte $C7

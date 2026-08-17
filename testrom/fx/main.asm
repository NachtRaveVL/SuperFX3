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
    gsu_iwt r14, FxRomProbeByte
    gsu_getb r0
    gsu_sm TEST_RAM_ROM, r0
    gsu_stop
    gsu_nop

; Cross-boundary pipeline test: private ROM buffering feeds an ALU operation,
; which feeds a delayed shared-RAM store immediately before STOP.
FxKernel_PipelineMix:
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

FxRomProbeByte:
    .byte $C7

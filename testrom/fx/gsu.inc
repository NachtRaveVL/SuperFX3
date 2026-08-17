; Small GSU macro layer for the diagnostic kernels.
; This intentionally covers only instructions used by the test ROM. Keeping it
; local means the diagnostic build has no DOS-era assembler dependency. Opcode
; encodings and WITH optimizations are cross-checked against ARM9/casfx.

.define r0  0
.define r1  1
.define r2  2
.define r3  3
.define r4  4
.define r5  5
.define r6  6
.define r7  7
.define r8  8
.define r9  9
.define r10 10
.define r11 11
.define r12 12
.define r13 13
.define r14 14
.define r15 15

.macro gsu_stop
    .byte $00
.endmacro

.macro gsu_nop
    .byte $01
.endmacro

.macro gsu_to reg
    .assert reg >= r0 && reg <= r15, error, "GSU TO register out of range"
    .byte $10 + reg
.endmacro

.macro gsu_with reg
    .assert reg >= r0 && reg <= r15, error, "GSU WITH register out of range"
    .byte $20 + reg
.endmacro

.macro gsu_from reg
    .assert reg >= r0 && reg <= r15, error, "GSU FROM register out of range"
    .byte $B0 + reg
.endmacro

.macro gsu_iwt reg, value
    .assert reg >= r0 && reg <= r15, error, "GSU IWT register out of range"
    .byte $F0 + reg
    .word .loword(value)
.endmacro

.macro gsu_sm address, reg
    .assert reg >= r0 && reg <= r15, error, "GSU SM register out of range"
    .byte $3E
    .byte $F0 + reg
    .word .loword(address)
.endmacro

.macro gsu_add dreg, sreg, operand
    .assert dreg >= r0 && dreg <= r15, error, "GSU ADD destination out of range"
    .assert sreg >= r0 && sreg <= r15, error, "GSU ADD source out of range"
    .assert operand >= r0 && operand <= r15, error, "GSU ADD operand out of range"
    .if dreg = sreg && dreg <> r0
        gsu_with dreg
    .else
        .if dreg <> r0
            gsu_to dreg
        .endif
        .if sreg <> r0
            gsu_from sreg
        .endif
    .endif
    .byte $50 + operand
.endmacro

.macro gsu_color reg
    .if reg <> r0
        gsu_with reg
    .endif
    .byte $4E
.endmacro

.macro gsu_plot
    .byte $4C
.endmacro

.macro gsu_rpix dreg
    .if dreg <> r0
        gsu_with dreg
    .endif
    .byte $3D, $4C
.endmacro

.macro gsu_getb dreg
    .if dreg <> r0
        gsu_with dreg
    .endif
    .byte $EF
.endmacro

.macro gsu_merge
    .byte $70
.endmacro

.segment "CODE"

RunCurrentTest:
    lda current_test
    asl
    asl
    asl
    asl
    sta current_desc

    lda #TEST_RESULT_PASS
    sta test_result
    stz last_expected
    stz last_actual
    stz last_address

    jsr ResetGsuForTest
    jsr SetupCurrentTest

    stz current_flags
    ldy current_desc
    sep #$20
    .a8
    lda TestRegistry+TD_FLAGS,y
    sta current_flags
    and #TEST_FLAG_CPU_ONLY
    bne @validate
    rep #$20
    .a16

    lda #$0001
    sta repeat_count
    sep #$20
    .a8
    lda current_flags
    and #TEST_FLAG_REPEAT
    beq @run
    rep #$20
    .a16
    ldy current_desc
    lda TestRegistry+TD_PARAM0,y
    sta repeat_count

@run:
    rep #$20
    .a16
    jsr StartAndWaitKernel
    lda test_result
    cmp #TEST_RESULT_TIMEOUT
    beq @done
    dec repeat_count
    bne @run

@validate:
    rep #$20
    .a16
    jsr ValidateCurrentTest
@done:
    rep #$20
    .a16
    rts

ResetGsuForTest:
    sep #$20
    .a8
    stz FX_SFR
    rep #$20
    .a16

    ; R14 is deliberately skipped because committing its high byte starts a ROM
    ; buffer fetch. R15 is the execution trigger and is also left alone here.
    lda #$0000
    ldx #$0000
@clear_regs:
    sta FX_R0,x
    inx
    inx
    cpx #$001C
    bne @clear_regs

    sep #$20
    .a8
    stz FX_PBR
    lda #FX3_CFGR_TEST
    sta FX_CFGR
    lda #FX3_DEFAULT_SCBR
    sta FX_SCBR
    lda #FX3_CLSR_FAST
    sta FX_CLSR
    stz FX_SCMR
    rep #$20
    .a16
    rts

StartAndWaitKernel:
    ldy current_desc
    sep #$20
    .a8
    lda TestRegistry+TD_BANK,y
    sta FX_PBR
    rep #$20
    .a16
    lda TestRegistry+TD_KERNEL,y
    sta FX_R15
    lda TestRegistry+TD_TIMEOUT,y
    sta timeout_count

@wait:
    jsr WaitFrame
    lda FX_R15
    beq @complete
    dec timeout_count
    bne @wait

    sta last_actual
    stz last_expected
    lda #FX_R15
    sta last_address
    lda #TEST_RESULT_TIMEOUT
    sta test_result

    sep #$20
    .a8
    stz FX_SFR
    rep #$20
    .a16
@complete:
    rts

SetupCurrentTest:
    ldy current_desc
    sep #$20
    .a8
    lda TestRegistry+TD_SETUP,y
    cmp #SETUP_NONE
    beq @none
    cmp #SETUP_CPU_REG
    beq @cpu_reg
    cmp #SETUP_RAM_MAGIC
    beq @ram_magic
    cmp #SETUP_PLOT
    beq @plot
    cmp #SETUP_CLEAR
    beq @clear
    cmp #SETUP_C2P_NOOP
    beq @c2p
@none:
    rep #$20
    .a16
    rts
@cpu_reg:
    rep #$20
    .a16
    lda #$BEEF
    sta FX_R5
    rts
@ram_magic:
    rep #$20
    .a16
    lda #$A5A5
    sta $7001FE
    sta $700200
    sta $700202
    sta $700204
    sta $700206
    sta $700208
    sta $70020A
    sta $70020C
    rts
@plot:
    rep #$20
    .a16
    lda #$0000
    ldx #$0000
@plot_clear:
    sta $710000,x
    inx
    inx
    cpx #$0040
    bne @plot_clear
    lda #$A5A5
    sta $700206
    sep #$20
    .a8
    lda #FX3_DEFAULT_SCBR
    sta FX_SCBR
    lda #FX3_SCMR_8BPP
    sta FX_SCMR
    rep #$20
    .a16
    rts
@clear:
    rep #$20
    .a16
    lda #$A5A5
    ldx #$0000
@clear_fill:
    sta $710000,x
    inx
    inx
    cpx #$8700
    bne @clear_fill
    rts
@c2p:
    rep #$20
    .a16
    lda #$5A5A
    ldx #$0000
@c2p_fill:
    sta $710000,x
    inx
    inx
    cpx #$0100
    bne @c2p_fill
    rts

ValidateCurrentTest:
    ldy current_desc
    sep #$20
    .a8
    lda TestRegistry+TD_VALIDATOR,y
    cmp #VALIDATE_VCR
    beq @vcr
    cmp #VALIDATE_CPU_REG
    beq @cpu_reg
    cmp #VALIDATE_COMPLETE
    beq @complete
    cmp #VALIDATE_RAM_MAGIC
    beq @ram
    cmp #VALIDATE_ALU
    beq @alu
    cmp #VALIDATE_ROM
    beq @rom
    cmp #VALIDATE_PLOT
    beq @plot
    cmp #VALIDATE_RPIX
    beq @rpix
    cmp #VALIDATE_CLEAR
    beq @clear
    cmp #VALIDATE_C2P_NOOP
    beq @c2p
    cmp #VALIDATE_PIPELINE
    beq @pipeline
    cmp #VALIDATE_PLOT_PATTERN
    beq @plot_pattern
@complete:
    rep #$20
    .a16
    rts
@vcr:
    jsr ValidateVcr
    rts
@cpu_reg:
    rep #$20
    .a16
    jsr ValidateCpuRegister
    rts
@ram:
    rep #$20
    .a16
    jsr ValidateRamMagic
    rts
@alu:
    rep #$20
    .a16
    jsr ValidateAlu
    rts
@rom:
    rep #$20
    .a16
    jsr ValidateRomBuffer
    rts
@plot:
    rep #$20
    .a16
    jsr ValidatePlot
    rts
@rpix:
    rep #$20
    .a16
    jsr ValidateRpix
    rts
@clear:
    rep #$20
    .a16
    jsr ValidateClear
    rts
@c2p:
    rep #$20
    .a16
    jsr ValidateC2pNoop
    rts
@pipeline:
    rep #$20
    .a16
    jsr ValidatePipeline
    rts
@plot_pattern:
    rep #$20
    .a16
    jsr ValidatePlotPattern
    rts

ValidateVcr:
    stz last_actual
    sep #$20
    .a8
    lda FX_VCR
    sta last_actual
    cmp #FX3_VCR_VALUE
    beq @pass
    rep #$20
    .a16
    lda #FX3_VCR_VALUE
    sta last_expected
    lda #FX_VCR
    sta last_address
    lda #TEST_RESULT_FAIL
    sta test_result
    rts
@pass:
    rep #$20
    .a16
    rts

ValidateCpuRegister:
    lda FX_R5
    cmp #$BEEF
    beq @pass
    sta last_actual
    lda #$BEEF
    sta last_expected
    lda #FX_R5
    sta last_address
    lda #TEST_RESULT_FAIL
    sta test_result
@pass:
    rts

ValidateRamMagic:
    lda $700200
    cmp #$A55A
    bne @magic_fail
    lda $7001FE
    cmp #$A5A5
    bne @guard_lo_fail
    lda $700208
    cmp #$A5A5
    bne @guard_hi_fail
    rts
@magic_fail:
    sta last_actual
    lda #$A55A
    sta last_expected
    lda #TEST_RAM_MAGIC
    jmp RecordFailure
@guard_lo_fail:
    sta last_actual
    lda #$A5A5
    sta last_expected
    lda #TEST_RAM_GUARD_LO
    jmp RecordFailure
@guard_hi_fail:
    sta last_actual
    lda #$A5A5
    sta last_expected
    lda #TEST_RAM_GUARD_HI
    jmp RecordFailure

ValidateAlu:
    lda $700202
    cmp #$1245
    beq @pass
    sta last_actual
    lda #$1245
    sta last_expected
    lda #TEST_RAM_ALU
    jmp RecordFailure
@pass:
    rts

ValidateRomBuffer:
    lda $700204
    cmp #$00C7
    beq @pass
    sta last_actual
    lda #$00C7
    sta last_expected
    lda #TEST_RAM_ROM
    jmp RecordFailure
@pass:
    rts

ValidateRpix:
    lda $700206
    cmp #$005A
    beq @pass
    sta last_actual
    lda #$005A
    sta last_expected
    lda #TEST_RAM_RPIX
    jmp RecordFailure
@pass:
    rts

ValidatePlot:
    stz last_actual
    stz last_expected
    ldx #$0000
    ldy #$0000
@next:
    sep #$20
    .a8
    lda $710000,x
    cmp PlotExpectedTile,y
    bne @fail
    rep #$20
    .a16
    inx
    iny
    cpx #$0040
    bne @next
    rts
@fail:
    sta last_actual
    lda PlotExpectedTile,y
    sta last_expected
    rep #$20
    .a16
    txa
    sta last_address
    lda #TEST_RESULT_FAIL
    sta test_result
    rts

ValidatePipeline:
    lda $70020A
    cmp #$00D8
    bne @value_fail
    lda $70020C
    cmp #$A5A5
    bne @guard_fail
    rts
@value_fail:
    sta last_actual
    lda #$00D8
    sta last_expected
    lda #TEST_RAM_PIPE
    jmp RecordFailure
@guard_fail:
    sta last_actual
    lda #$A5A5
    sta last_expected
    lda #TEST_RAM_PIPE_GUARD
    jmp RecordFailure

ValidatePlotPattern:
    stz last_actual
    stz last_expected
    ldx #$0000
    ldy #$0000
@next:
    sep #$20
    .a8
    lda $710000,x
    cmp PlotExpectedPattern,y
    bne @fail
    rep #$20
    .a16
    inx
    iny
    cpx #$0040
    bne @next
    rts
@fail:
    sta last_actual
    lda PlotExpectedPattern,y
    sta last_expected
    rep #$20
    .a16
    txa
    sta last_address
    lda #TEST_RESULT_FAIL
    sta test_result
    rts

ValidateC2pNoop:
    stz last_actual
    stz last_expected
    ldx #$0000
@next:
    sep #$20
    .a8
    lda $710000,x
    cmp #$5A
    bne @fail
    rep #$20
    .a16
    inx
    cpx #$0100
    bne @next
    rts
@fail:
    sta last_actual
    lda #$5A
    sta last_expected
    rep #$20
    .a16
    txa
    sta last_address
    lda #TEST_RESULT_FAIL
    sta test_result
    rts

ValidateClear:
    ldy current_desc
    lda TestRegistry+TD_PARAM0,y
    sta clear_first
    sta clear_col
    lda TestRegistry+TD_PARAM1,y
    sta clear_last

@column_loop:
    lda clear_col
    asl
    tax
    lda Fx3ColumnOffsets,x
    sta clear_tile_addr
    stz clear_row

@row_loop:
    ldx clear_tile_addr
    ldy #$0000
@pattern_loop:
    sep #$20
    .a8
    lda $710000,x
    cmp Fx3ClearPattern,y
    bne @pattern_fail
    rep #$20
    .a16
    inx
    iny
    cpy #$0040
    bne @pattern_loop

    lda clear_tile_addr
    clc
    adc #$0040
    sta clear_tile_addr
    inc clear_row
    lda clear_row
    cmp #$0012
    bne @row_loop

    ; Rows 18 and 19 are padding in the 20-tile column stride. CLEAR must not
    ; spill into either row.
    ldx clear_tile_addr
    ldy #$0080
@padding_loop:
    sep #$20
    .a8
    lda $710000,x
    cmp #FX3_CLEAR_SENTINEL
    bne @sentinel_fail
    rep #$20
    .a16
    inx
    dey
    bne @padding_loop

    inc clear_col
    lda clear_col
    cmp clear_last
    beq @column_loop
    bcc @column_loop

    ; Also guard the neighboring columns. This catches a command using the
    ; wrong block range even if every tile inside the requested range is right.
    lda clear_first
    beq @right_guard
    dec
    sta clear_col
    jsr ValidateSentinelColumn
    lda test_result
    cmp #TEST_RESULT_PASS
    bne @done

@right_guard:
    lda clear_last
    cmp #$001A
    beq @done
    inc
    sta clear_col
    jsr ValidateSentinelColumn
@done:
    rts

@pattern_fail:
    sta last_actual
    lda Fx3ClearPattern,y
    sta last_expected
    rep #$20
    .a16
    txa
    sta last_address
    lda #TEST_RESULT_FAIL
    sta test_result
    rts

@sentinel_fail:
    sta last_actual
    lda #FX3_CLEAR_SENTINEL
    sta last_expected
    rep #$20
    .a16
    txa
    sta last_address
    lda #TEST_RESULT_FAIL
    sta test_result
    rts

ValidateSentinelColumn:
    lda clear_col
    asl
    tax
    lda Fx3ColumnOffsets,x
    tax
    ldy #$0500
@loop:
    sep #$20
    .a8
    lda $710000,x
    cmp #FX3_CLEAR_SENTINEL
    bne @fail
    rep #$20
    .a16
    inx
    dey
    bne @loop
    rts
@fail:
    sta last_actual
    lda #FX3_CLEAR_SENTINEL
    sta last_expected
    rep #$20
    .a16
    txa
    sta last_address
    lda #TEST_RESULT_FAIL
    sta test_result
    rts

; A = CPU-visible 16-bit address or shared-RAM offset associated with failure.
RecordFailure:
    sta last_address
    lda #TEST_RESULT_FAIL
    sta test_result
    rts

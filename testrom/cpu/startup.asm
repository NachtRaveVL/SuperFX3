.segment "CODE"

Reset:
    sei
    clc
    xce
    rep #$38
    .a16
    .i16

    ldx #$1FFF
    txs
    lda #$0000
    tcd
    phk
    plb

    stz joy_current
    stz joy_previous
    stz joy_pressed
    stz menu_index
    stz menu_top
    stz current_test
    stz irq_seen

    jsr PpuInit
    jsr InputInit
    jsr RenderMenu
    cli

MainLoop:
    jsr WaitFrame
    jsr PpuUploadTextMap
    jsr InputPoll

    lda joy_pressed
    bit #JOY_UP
    beq :+
    jsr MenuPrevious
    jsr RenderMenu
:
    lda joy_pressed
    bit #JOY_DOWN
    beq :+
    jsr MenuNext
    jsr RenderMenu
:
    lda joy_pressed
    bit #JOY_A
    beq :+
    lda menu_index
    sta current_test
    jsr RenderRunning
    jsr WaitFrame
    jsr PpuUploadTextMap
    jsr RunCurrentTest
    jsr ResultScreen
    jsr PpuHideVisual
    jsr RenderMenu
:
    lda joy_pressed
    bit #JOY_START
    beq :+
    jsr RunAllTests
    jsr SummaryScreen
    jsr PpuHideVisual
    jsr RenderMenu
:
    bra MainLoop

NmiHandler:
    php
    rep #$30
    pha
    phx
    phy
    sep #$20
    .a8
    lda RDNMI
    rep #$20
    .a16
    ply
    plx
    pla
    plp
    rti

IrqHandler:
    php
    rep #$30
    .a16
    .i16
    pha
    phx
    phy
    sep #$20
    .a8
    lda #$01
    sta irq_seen
    lda FX_SFR+1            ; Reading SFR high acknowledges the GSU IRQ.
    rep #$20
    .a16
    ply
    plx
    pla
    plp
    rti

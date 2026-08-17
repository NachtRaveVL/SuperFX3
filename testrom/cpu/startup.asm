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
    stz current_test

    jsr PpuInit
    jsr InputInit
    jsr RenderMenu

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
    rti

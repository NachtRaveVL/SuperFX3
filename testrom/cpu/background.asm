.segment "CODE"

; The diagnostic background is the black backdrop plus a fixed color. Only the
; backdrop participates in color math, so BG2 font pixels and BG1 test graphics
; retain their palette colors exactly.
BackgroundInit:
    sep #$20
    .a8

    ; Never clip, never prevent color math, and use fixed color rather than the
    ; subscreen as the second color source.
    stz CGWSEL
    lda #$20               ; Enable addition for the backdrop only.
    sta CGADSUB

    ; HDMA channels 5-7 independently update the red, green, and blue planes of
    ; COLDATA. Channel 0 remains free for the normal VRAM DMA routines.
    stz DMAP5
    stz DMAP6
    stz DMAP7
    lda #$32               ; COLDATA = $2132.
    sta BBAD5
    sta BBAD6
    sta BBAD7

    lda #^BgNormalRed
    sta A1B5
    lda #^BgNormalGreen
    sta A1B6
    lda #^BgNormalBlue
    sta A1B7

    ; Establish a sensible color immediately. HDMA takes over at scanline 0 of
    ; the next frame and then walks the full 224-line gradient.
    lda #$21
    sta COLDATA
    lda #$44
    sta COLDATA
    lda #$89
    sta COLDATA

    rep #$20
    .a16
    jsr BackgroundSetNormal

    sep #$20
    .a8
    lda #$E0               ; Enable HDMA channels 5, 6, and 7.
    sta HDMAEN
    rep #$20
    .a16
    rts

; A/X/Y are restored to 16-bit on return. Changing A1Tx updates the source used
; when HDMA initializes at the next frame boundary.
BackgroundSetNormal:
    rep #$30
    .a16
    .i16
    lda #BgNormalRed
    ldx #BgNormalGreen
    ldy #BgNormalBlue
    bra BackgroundSetTables

BackgroundSetPass:
    rep #$30
    .a16
    .i16
    lda #BgPassRed
    ldx #BgPassGreen
    ldy #BgPassBlue
    bra BackgroundSetTables

BackgroundSetFail:
    rep #$30
    .a16
    .i16
    lda #BgFailRed
    ldx #BgFailGreen
    ldy #BgFailBlue
    bra BackgroundSetTables

BackgroundSetTimeout:
    rep #$30
    .a16
    .i16
    lda #BgTimeoutRed
    ldx #BgTimeoutGreen
    ldy #BgTimeoutBlue

BackgroundSetTables:
    sta A1T5L
    stx A1T6L
    sty A1T7L
    rts

BackgroundSetForResult:
    lda test_result
    cmp #TEST_RESULT_PASS
    beq BackgroundSetPass
    cmp #TEST_RESULT_TIMEOUT
    beq BackgroundSetTimeout
    bra BackgroundSetFail

BackgroundSetForSummary:
    lda fail_count
    bne BackgroundSetFail
    lda timeout_total
    bne BackgroundSetTimeout
    bra BackgroundSetPass

.segment "RODATA"
.include "background.inc"

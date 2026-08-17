.segment "CODE"

PpuInit:
    sep #$20
    .a8
    lda #$80
    sta INIDISP
    stz NMITIMEN
    stz MDMAEN
    stz TM
    stz TS

    ; Show the palette exactly as written. $30 means never clip the main
    ; screen and always prevent color math, so no fixed/subscreen color can
    ; tint the diagnostic UI.
    lda #$30
    sta CGWSEL
    stz CGADSUB

    lda #$03
    sta BGMODE
    stz BG1SC
    lda #$08
    sta BG2SC
    lda #$41
    sta BG12NBA

    stz BG1HOFS
    stz BG1HOFS
    stz BG1VOFS
    stz BG1VOFS
    stz BG2HOFS
    stz BG2HOFS
    stz BG2VOFS
    stz BG2VOFS

    lda #$80
    sta VMAIN

    jsr PpuInitPalette
    rep #$20
    .a16
    jsr TextClearMap
    jsr PpuClearBg1Map
    jsr PpuUploadFont
    jsr PpuUploadTextMap
    jsr PpuUploadBg1Map

    sep #$20
    .a8
    lda #$02
    sta TM
    lda #$0F
    sta INIDISP
    lda #$81
    sta NMITIMEN
    rep #$20
    .a16
    rts

PpuInitPalette:
    ; Upload CGRAM directly from the CPU while forced blank is active. The
    ; palette is only 512 bytes, and avoiding DMA here keeps the diagnostic
    ; text path independent of DMA setup while we bring the ROM up.
    sep #$20
    .a8
    rep #$10
    .i16
    stz CGADD
    ldx #$0000
@loop:
    lda DiagnosticPalette,x
    sta CGDATA
    inx
    cpx #(DiagnosticPaletteEnd-DiagnosticPalette)
    bne @loop
    rts

PpuClearBg1Map:
    rep #$30
    .a16
    .i16
    lda #$0000
    ldx #$0000
@loop:
    sta BG1_MAP_WRAM,x
    inx
    inx
    cpx #BG1_MAP_BYTES
    bne @loop
    rts

PpuUploadFont:
    sep #$20
    .a8
    lda #$80
    sta VMAIN
    lda #<BG2_FONT_VRAM
    sta VMADDL
    lda #>BG2_FONT_VRAM
    sta VMADDL+1
    lda #$01
    sta DMAP0
    lda #$18
    sta BBAD0
    lda #<Font4bpp
    sta A1T0L
    lda #>Font4bpp
    sta A1T0L+1
    lda #^Font4bpp
    sta A1B0
    lda #<(Font4bppEnd-Font4bpp)
    sta DAS0L
    lda #>(Font4bppEnd-Font4bpp)
    sta DAS0L+1
    lda #$01
    sta MDMAEN
    rep #$20
    .a16
    rts

PpuUploadTextMap:
    sep #$20
    .a8
    lda #$80
    sta VMAIN
    lda #<BG2_MAP_VRAM
    sta VMADDL
    lda #>BG2_MAP_VRAM
    sta VMADDL+1
    lda #$01
    sta DMAP0
    lda #$18
    sta BBAD0
    lda #<TEXT_MAP_WRAM
    sta A1T0L
    lda #>TEXT_MAP_WRAM
    sta A1T0L+1
    lda #^TEXT_MAP_WRAM
    sta A1B0
    lda #<TEXT_MAP_BYTES
    sta DAS0L
    lda #>TEXT_MAP_BYTES
    sta DAS0L+1
    lda #$01
    sta MDMAEN
    rep #$20
    .a16
    rts

PpuUploadBg1Map:
    sep #$20
    .a8
    lda #$80
    sta VMAIN
    lda #<BG1_MAP_VRAM
    sta VMADDL
    lda #>BG1_MAP_VRAM
    sta VMADDL+1
    lda #$01
    sta DMAP0
    lda #$18
    sta BBAD0
    lda #<BG1_MAP_WRAM
    sta A1T0L
    lda #>BG1_MAP_WRAM
    sta A1T0L+1
    lda #^BG1_MAP_WRAM
    sta A1B0
    lda #<BG1_MAP_BYTES
    sta DAS0L
    lda #>BG1_MAP_BYTES
    sta DAS0L+1
    lda #$01
    sta MDMAEN
    rep #$20
    .a16
    rts

; Show one 8bpp tile from the first column associated with the current test.
; PLOT/RPIX use column 0. CLEAR uses param0 as its first affected column.
PpuShowCurrentVisual:
    ldy current_desc
    lda TestRegistry+TD_PARAM0,y
    asl
    tax
    lda Fx3ColumnOffsets,x
    sta visual_source

    sep #$20
    .a8
    lda #$80
    sta INIDISP            ; VRAM DMA must not depend on how long validation took.
    sta VMAIN
    lda #<BG1_TILE_VRAM
    sta VMADDL
    lda #>BG1_TILE_VRAM
    sta VMADDL+1
    lda #$01
    sta DMAP0
    lda #$18
    sta BBAD0
    lda visual_source
    sta A1T0L
    lda visual_source+1
    sta A1T0L+1
    lda #FX3_PLANAR_BANK
    sta A1B0
    lda #$40
    sta DAS0L
    stz DAS0L+1
    lda #$01
    sta MDMAEN
    lda #$03
    sta TM
    lda #$0F
    sta INIDISP
    rep #$20
    .a16
    rts

PpuHideVisual:
    sep #$20
    .a8
    lda #$02
    sta TM
    rep #$20
    .a16
    rts

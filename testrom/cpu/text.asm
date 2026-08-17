.segment "CODE"

TextClearMap:
    rep #$30
    .a16
    .i16
    lda #$0000
    ldx #$0000
@loop:
    sta TEXT_MAP_WRAM,x
    inx
    inx
    cpx #TEXT_MAP_BYTES
    bne @loop
    stz text_cursor
    stz text_palette
    rts

; A = byte offset into the 32x32 BG2 tilemap.
TextSetCursor:
    sta text_cursor
    rts

; A = ASCII character. The generated font starts at ASCII 32.
TextPutChar:
    and #$00FF
    cmp #$0020
    bcc @space
    cmp #$0080
    bcc :+
@space:
    lda #$0020
:
    sec
    sbc #$0020
    and #$00FF
    ora text_palette
    ora #$2000             ; Keep the diagnostic text above the 8bpp result layer.
    ldx text_cursor
    sta TEXT_MAP_WRAM,x
    inx
    inx
    stx text_cursor
    rts

; X = zero-terminated string in bank 00.
TextPrint:
    stx text_ptr
    ldy #$0000
@next:
    sep #$20
    .a8
    lda (text_ptr),y
    beq @done
    rep #$20
    .a16
    and #$00FF
    phy
    jsr TextPutChar
    ply
    iny
    bra @next
@done:
    rep #$20
    .a16
    rts

TextPrintHexNibble:
    and #$000F
    cmp #$000A
    bcc @digit
    clc
    adc #('A' - 10)
    bra TextPutChar
@digit:
    clc
    adc #'0'
    bra TextPutChar

; A = byte value in low eight bits.
TextPrintHex8:
    pha
    and #$00F0
    lsr
    lsr
    lsr
    lsr
    jsr TextPrintHexNibble
    pla
    and #$000F
    jmp TextPrintHexNibble

; A = 16-bit value.
TextPrintHex16:
    sta temp_word
    xba
    and #$00FF
    jsr TextPrintHex8
    lda temp_word
    and #$00FF
    jmp TextPrintHex8

.segment "CODE"

InputInit:
    stz joy_current
    stz joy_previous
    stz joy_pressed
    rts

; WAI gives the main loop a stable once-per-frame cadence. Auto joypad reading
; starts at VBlank, so wait for it to finish before consuming JOY1.
WaitFrame:
    wai
    sep #$20
    .a8
@joy_busy:
    lda HVBJOY
    and #$01
    bne @joy_busy
    rep #$20
    .a16
    rts

InputPoll:
    lda joy_current
    sta joy_previous
    lda JOY1L
    sta joy_current
    lda joy_previous
    eor #$FFFF
    and joy_current
    sta joy_pressed
    rts

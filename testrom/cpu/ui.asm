.segment "CODE"

.macro TEXT_AT column, row, label
    lda #(((row) * 32 + (column)) * 2)
    jsr TextSetCursor
    ldx #label
    jsr TextPrint
.endmacro

MenuPrevious:
    lda menu_index
    beq @wrap
    dec menu_index
    rts
@wrap:
    lda #TEST_COUNT - 1
    sta menu_index
    rts

MenuNext:
    inc menu_index
    lda menu_index
    cmp #TEST_COUNT
    bcc @done
    stz menu_index
@done:
    rts

RenderMenu:
    jsr TextClearMap
    TEXT_AT 1, 1, StrTitle
    TEXT_AT 1, 3, StrMenuHelpA
    TEXT_AT 16, 3, StrMenuHelpStart

    stz ui_index
    lda #((5 * 32 + 1) * 2)
    sta ui_line
@loop:
    lda ui_line
    jsr TextSetCursor
    lda ui_index
    cmp menu_index
    bne @not_selected
    lda #'>'
    bra @marker
@not_selected:
    lda #' '
@marker:
    jsr TextPutChar

    lda ui_line
    clc
    adc #$0004
    jsr TextSetCursor
    lda ui_index
    asl
    asl
    asl
    asl
    tay
    ldx TestRegistry+TD_NAME,y
    jsr TextPrint

    lda ui_line
    clc
    adc #$0040
    sta ui_line
    inc ui_index
    lda ui_index
    cmp #TEST_COUNT
    bcc @loop

    TEXT_AT 1, 22, StrASelect
    TEXT_AT 1, 23, StrStartAll
    rts

RenderRunning:
    jsr PpuHideVisual
    jsr TextClearMap
    TEXT_AT 1, 1, StrTitle
    TEXT_AT 1, 4, StrRunning
    lda current_test
    asl
    asl
    asl
    asl
    tay
    lda #((6 * 32 + 1) * 2)
    jsr TextSetCursor
    ldx TestRegistry+TD_NAME,y
    jsr TextPrint
    rts

RenderResult:
    jsr TextClearMap
    TEXT_AT 1, 1, StrTitle
    lda #((4 * 32 + 1) * 2)
    jsr TextSetCursor
    ldy current_desc
    ldx TestRegistry+TD_NAME,y
    jsr TextPrint

    TEXT_AT 1, 7, StrResult
    lda test_result
    cmp #TEST_RESULT_PASS
    beq @pass
    cmp #TEST_RESULT_TIMEOUT
    beq @timeout
    ldx #StrFail
    bra @print_result
@pass:
    ldx #StrPass
    bra @print_result
@timeout:
    ldx #StrTimeout
@print_result:
    lda #((7 * 32 + 10) * 2)
    jsr TextSetCursor
    jsr TextPrint

    lda test_result
    cmp #TEST_RESULT_PASS
    beq @footer
    TEXT_AT 1, 10, StrExpected
    lda last_expected
    jsr TextPrintHex16
    TEXT_AT 1, 11, StrActual
    lda last_actual
    jsr TextPrintHex16
    TEXT_AT 1, 12, StrAddress
    lda last_address
    jsr TextPrintHex16

@footer:
    TEXT_AT 1, 22, StrARunAgain
    TEXT_AT 1, 23, StrBBack
    rts

ResultScreen:
    sep #$20
    .a8
    lda current_flags
    and #TEST_FLAG_VISUAL
    beq @no_visual
    rep #$20
    .a16
    jsr PpuShowCurrentVisual
    bra @render
@no_visual:
    rep #$20
    .a16
    jsr PpuHideVisual
@render:
    jsr RenderResult
@loop:
    jsr WaitFrame
    jsr PpuUploadTextMap
    jsr InputPoll
    lda joy_pressed
    bit #JOY_B
    bne @done
    bit #JOY_A
    beq @loop

    jsr RenderRunning
    jsr WaitFrame
    jsr PpuUploadTextMap
    jsr RunCurrentTest
    sep #$20
    .a8
    lda current_flags
    and #TEST_FLAG_VISUAL
    beq @rerun_no_visual
    rep #$20
    .a16
    jsr PpuShowCurrentVisual
    bra @rerender
@rerun_no_visual:
    rep #$20
    .a16
    jsr PpuHideVisual
@rerender:
    jsr RenderResult
    bra @loop
@done:
    rts

RunAllTests:
    stz pass_count
    stz fail_count
    stz timeout_total
    lda #$FFFF
    sta first_failure
    stz ui_index

@next:
    lda ui_index
    sta current_test
    jsr RenderRunning
    jsr WaitFrame
    jsr PpuUploadTextMap
    jsr RunCurrentTest

    lda test_result
    cmp #TEST_RESULT_PASS
    beq @passed
    cmp #TEST_RESULT_TIMEOUT
    beq @timed_out
    inc fail_count
    bra @record_failure
@timed_out:
    inc timeout_total
@record_failure:
    lda first_failure
    cmp #$FFFF
    bne @advance
    lda ui_index
    sta first_failure
    bra @advance
@passed:
    inc pass_count
@advance:
    inc ui_index
    lda ui_index
    cmp #TEST_COUNT
    bcc @next
    rts

RenderSummary:
    jsr PpuHideVisual
    jsr TextClearMap
    TEXT_AT 1, 1, StrTitle
    TEXT_AT 1, 4, StrRunAll

    TEXT_AT 1, 7, StrPassCount
    lda pass_count
    jsr TextPrintHex16
    TEXT_AT 1, 8, StrFailCount
    lda fail_count
    jsr TextPrintHex16
    TEXT_AT 1, 9, StrTimeoutCount
    lda timeout_total
    jsr TextPrintHex16

    lda first_failure
    cmp #$FFFF
    beq @all_passed
    TEXT_AT 1, 12, StrLastFailure
    lda first_failure
    asl
    asl
    asl
    asl
    tay
    lda #((14 * 32 + 1) * 2)
    jsr TextSetCursor
    ldx TestRegistry+TD_NAME,y
    jsr TextPrint
    bra @footer
@all_passed:
    TEXT_AT 1, 12, StrAllPassed
@footer:
    TEXT_AT 1, 23, StrBBack
    rts

SummaryScreen:
    jsr RenderSummary
@loop:
    jsr WaitFrame
    jsr PpuUploadTextMap
    jsr InputPoll
    lda joy_pressed
    bit #JOY_B
    beq @loop
    rts

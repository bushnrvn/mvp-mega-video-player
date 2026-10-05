; Calls into the MEGA65's hypervisor (its DOS, which reads files from the SD card).
;   hyppo(func): A = DOS function number; h_x / h_y are the X / Y inputs, and on return h_a, h_x, h_y hold the outputs and
;   h_c is 1 if the call succeeded (the hypervisor sets the carry flag for success).
; namebuf and dirbuf must start on a page boundary: the DOS takes only the high byte of their address.

        .export _hyppo, _h_a, _h_x, _h_y, _h_c, _namebuf, _dirbuf

        .segment "NAMEBUF"
_namebuf: .res 256
_dirbuf:  .res 256

        .bss
_h_a:   .res 1
_h_x:   .res 1
_h_y:   .res 1
_h_c:   .res 1
func:   .res 1

        .code
_hyppo:
        sta     func
        ldx     _h_x
        ldy     _h_y
        ldz     #0
        lda     func
        sta     $D640               ; the trap: the next instruction runs when the hypervisor returns
        nop
        sta     _h_a
        stx     _h_x
        sty     _h_y
        lda     #0
        rol     a                   ; carry -> bit 0
        sta     _h_c
        ldz     #0
        ldx     #0
        rts

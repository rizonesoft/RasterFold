; Mode 4: subtract-green, MED, context Rice. Bits are LSB-first.
; Model state persists across mode-4 blocks and is saved around a rejected trial.
include rf.inc

option casemap:none
option prologue:none
option epilogue:none

EXTERNDEF rf_scatter_rgb:PROC

R_PIX     equ 0
R_STRIDE  equ 8
R_WIDTH   equ 16
R_HEIGHT  equ 20
R_X0      equ 24
R_Y0      equ 28
R_BW      equ 32
R_BH      equ 36
R_LOCAL   equ 40
R_CH      equ 48
R_DST     equ 56
R_CAP     equ 64
R_LEN     equ 72
R_ACC     equ 80
R_NBIT    equ 84
R_OVER    equ 88
R_IP      equ 96
R_SIZE    equ 104
R_JOB     equ 112

S_X       equ 176
S_Y       equ 180
S_LE      equ 184
S_UE      equ 188
S_CTX     equ 192
S_ERR     equ 196
S_K       equ 200
S_PRED    equ 204
S_VAL     equ 208
S_NPIX    equ 212
S_PREV    equ 256

.data
g_rice_A  dd 192 dup(4)
g_rice_N  dd 192 dup(1)
g_rice_As dd 192 dup(0)
g_rice_Ns dd 192 dup(0)

.const
msg_rice db "invalid RFI: rice", 0
msg_rice_self db "internal self-test failed", 0
st_rice_pix db 10, 20, 30, 40, 50, 60, 70, 80, 90, 1, 2, 3

.code

; ecx = x, edx = y, r8d = channel, rbx = context. eax = sample 0..255.
; Clobbers rax, rcx, rdx, r8, r9, r10, r11.
rice_sample PROC
    cmp ecx, 0
    jl rsm_zero
    cmp edx, 0
    jl rsm_zero
    cmp ecx, dword ptr [rbx+R_WIDTH]
    jge rsm_zero
    cmp edx, dword ptr [rbx+R_HEIGHT]
    jge rsm_zero
    mov r9, qword ptr [rbx+R_LOCAL]
    test r9, r9
    jz rsm_image
    mov eax, dword ptr [rbx+R_X0]
    cmp ecx, eax
    jl rsm_image
    add eax, dword ptr [rbx+R_BW]
    cmp ecx, eax
    jge rsm_image
    mov eax, dword ptr [rbx+R_Y0]
    cmp edx, eax
    jl rsm_image
    add eax, dword ptr [rbx+R_BH]
    cmp edx, eax
    jge rsm_image
    mov eax, edx
    sub eax, dword ptr [rbx+R_Y0]
    imul eax, dword ptr [rbx+R_BW]
    mov r10d, ecx
    sub r10d, dword ptr [rbx+R_X0]
    add eax, r10d
    movzx eax, byte ptr [r9+rax]
    ret
rsm_image:
    movsxd rax, edx
    movsxd r10, dword ptr [rbx+R_STRIDE]
    imul rax, r10
    add rax, qword ptr [rbx+R_PIX]
    movsxd r10, ecx
    lea r10, [r10+r10*2]
    add rax, r10
    movzx r10d, byte ptr [rax]
    movzx r11d, byte ptr [rax+1]
    movzx eax, byte ptr [rax+2]
    cmp r8d, 0
    jne rsm_not_g
    mov eax, r11d
    ret
rsm_not_g:
    cmp r8d, 1
    jne rsm_b
    sub r10d, r11d
    movzx eax, r10b
    ret
rsm_b:
    sub eax, r11d
    movzx eax, al
    ret
rsm_zero:
    xor eax, eax
    ret
rice_sample ENDP

; ecx = left, edx = up, r8d = up-left. eax = clamped MED.
rice_med PROC
    mov r9d, ecx
    cmp edx, r9d
    cmovg r9d, edx
    mov r10d, ecx
    cmp edx, r10d
    cmovl r10d, edx
    cmp r8d, r9d
    jl rmed_not_max
    mov eax, r10d
    jmp rmed_clamp
rmed_not_max:
    cmp r8d, r10d
    jg rmed_grad
    mov eax, r9d
    jmp rmed_clamp
rmed_grad:
    mov eax, ecx
    add eax, edx
    sub eax, r8d
rmed_clamp:
    cmp eax, 0
    jge rmed_nonneg
    xor eax, eax
rmed_nonneg:
    cmp eax, 255
    jle rmed_ok
    mov eax, 255
rmed_ok:
    ret
rice_med ENDP

; ecx = value, edx = pred. eax = signed residual.
rice_delta PROC
    mov eax, ecx
    sub eax, edx
    and eax, 255
    cmp eax, 128
    jb rdel_ok
    sub eax, 256
rdel_ok:
    ret
rice_delta ENDP

; ecx = signed residual. eax = context bucket 0..7.
rice_qb PROC
    mov eax, ecx
    test eax, eax
    jns rqb_abs
    neg eax
rqb_abs:
    cmp eax, 1
    ja rqb_gt1
    ret
rqb_gt1:
    cmp eax, 2
    jne rqb_gt2
    mov eax, 2
    ret
rqb_gt2:
    cmp eax, 4
    ja rqb_gt4
    mov eax, 3
    ret
rqb_gt4:
    cmp eax, 8
    ja rqb_gt8
    mov eax, 4
    ret
rqb_gt8:
    cmp eax, 16
    ja rqb_gt16
    mov eax, 5
    ret
rqb_gt16:
    cmp eax, 32
    ja rqb_gt32
    mov eax, 6
    ret
rqb_gt32:
    mov eax, 7
    ret
rice_qb ENDP

; ecx = A, edx = N. eax = k in 0..8.
rice_k PROC
    mov r11d, ecx
    xor eax, eax
rk_loop:
    cmp eax, 8
    jae rk_done
    mov r10d, edx
    mov ecx, eax
    shl r10d, cl
    cmp r10d, r11d
    jae rk_done
    inc eax
    jmp rk_loop
rk_done:
    ret
rice_k ENDP

; eax = signed residual. ecx = mapped m.
rice_to_m PROC
    test eax, eax
    js rtm_neg
    shl eax, 1
    mov ecx, eax
    ret
rtm_neg:
    neg eax
    shl eax, 1
    dec eax
    mov ecx, eax
    ret
rice_to_m ENDP

; ecx = m. eax = signed residual.
rice_from_m PROC
    test ecx, 1
    jnz rfm_odd
    mov eax, ecx
    shr eax, 1
    ret
rfm_odd:
    mov eax, ecx
    inc eax
    shr eax, 1
    neg eax
    ret
rice_from_m ENDP

; cl = 0 or 1. rbx = context. Sets R_OVER when the byte count reaches the cap.
rice_emit_bit PROC
    cmp dword ptr [rbx+R_OVER], 0
    jne reb_ret
    movzx eax, cl
    test eax, eax
    jz reb_count
    mov eax, 1
    mov ecx, dword ptr [rbx+R_NBIT]
    shl eax, cl
    or dword ptr [rbx+R_ACC], eax
reb_count:
    inc dword ptr [rbx+R_NBIT]
    cmp dword ptr [rbx+R_NBIT], 8
    jb reb_ret
    mov rax, qword ptr [rbx+R_LEN]
    cmp rax, qword ptr [rbx+R_CAP]
    jae reb_over
    mov r10, qword ptr [rbx+R_DST]
    mov ecx, dword ptr [rbx+R_ACC]
    mov byte ptr [r10+rax], cl
    inc qword ptr [rbx+R_LEN]
    mov dword ptr [rbx+R_ACC], 0
    mov dword ptr [rbx+R_NBIT], 0
    mov rax, qword ptr [rbx+R_LEN]
    cmp rax, qword ptr [rbx+R_CAP]
    jae reb_over
reb_ret:
    ret
reb_over:
    mov dword ptr [rbx+R_OVER], 1
    ret
rice_emit_bit ENDP

; eax = bit. rbx = context. R_OVER is set if the stream ends.
rice_read_bit PROC
    cmp dword ptr [rbx+R_OVER], 0
    jne rrb_err
    cmp dword ptr [rbx+R_NBIT], 0
    jne rrb_have
    mov rax, qword ptr [rbx+R_IP]
    cmp rax, qword ptr [rbx+R_SIZE]
    jae rrb_err
    mov r10, qword ptr [rbx+R_DST]
    movzx ecx, byte ptr [r10+rax]
    mov dword ptr [rbx+R_ACC], ecx
    inc qword ptr [rbx+R_IP]
    mov dword ptr [rbx+R_NBIT], 8
rrb_have:
    mov eax, dword ptr [rbx+R_ACC]
    and eax, 1
    shr dword ptr [rbx+R_ACC], 1
    dec dword ptr [rbx+R_NBIT]
    ret
rrb_err:
    mov dword ptr [rbx+R_OVER], 1
    xor eax, eax
    ret
rice_read_bit ENDP

; eax = signed residual, ecx = context index 0..63. Uses R_CH.
rice_bump PROC
    mov r10d, eax
    test r10d, r10d
    jns rbp_abs
    neg r10d
rbp_abs:
    mov edx, dword ptr [rbx+R_CH]
    shl edx, 6
    add edx, ecx
    lea r11, g_rice_A
    add dword ptr [r11+rdx*4], r10d
    lea r11, g_rice_N
    inc dword ptr [r11+rdx*4]
    cmp dword ptr [r11+rdx*4], 32
    jb rbp_ret
    lea r8, g_rice_A
    shr dword ptr [r8+rdx*4], 1
    shr dword ptr [r11+rdx*4], 1
    cmp dword ptr [r11+rdx*4], 1
    jae rbp_ret
    mov dword ptr [r11+rdx*4], 1
rbp_ret:
    ret
rice_bump ENDP

; ecx = x, edx = y. eax = signed MED residual, or 0 when x or y is negative.
rice_residual PROC FRAME
    RF_ENTER 48
    cmp ecx, 0
    jl rrs_zero
    cmp edx, 0
    jl rrs_zero
    mov dword ptr [rsp+32], ecx
    mov dword ptr [rsp+36], edx
    mov r8d, dword ptr [rbx+R_CH]
    call rice_sample
    mov dword ptr [rsp+40], eax
    xor r12d, r12d
    cmp dword ptr [rsp+32], 0
    jle rrs_up
    mov ecx, dword ptr [rsp+32]
    dec ecx
    mov edx, dword ptr [rsp+36]
    mov r8d, dword ptr [rbx+R_CH]
    call rice_sample
    mov r12d, eax
rrs_up:
    xor r13d, r13d
    cmp dword ptr [rsp+36], 0
    jle rrs_ul
    mov ecx, dword ptr [rsp+32]
    mov edx, dword ptr [rsp+36]
    dec edx
    mov r8d, dword ptr [rbx+R_CH]
    call rice_sample
    mov r13d, eax
rrs_ul:
    xor r14d, r14d
    cmp dword ptr [rsp+32], 0
    jle rrs_med
    cmp dword ptr [rsp+36], 0
    jle rrs_med
    mov ecx, dword ptr [rsp+32]
    dec ecx
    mov edx, dword ptr [rsp+36]
    dec edx
    mov r8d, dword ptr [rbx+R_CH]
    call rice_sample
    mov r14d, eax
rrs_med:
    mov ecx, r12d
    mov edx, r13d
    mov r8d, r14d
    call rice_med
    mov edx, eax
    mov ecx, dword ptr [rsp+40]
    call rice_delta
    jmp rrs_leave
rrs_zero:
    xor eax, eax
rrs_leave:
    RF_LEAVE 48
rice_residual ENDP

rf_rice_reset PROC
    xor ecx, ecx
    lea r8, g_rice_A
    lea r9, g_rice_N
rrr_loop:
    cmp ecx, 192
    jae rrr_done
    mov dword ptr [r8+rcx*4], 4
    mov dword ptr [r9+rcx*4], 1
    inc ecx
    jmp rrr_loop
rrr_done:
    ret
rf_rice_reset ENDP

rf_rice_save PROC FRAME
    RF_ENTER 32
    lea rcx, g_rice_As
    lea rdx, g_rice_A
    mov r8d, 768
    call rf_memcpy
    lea rcx, g_rice_Ns
    lea rdx, g_rice_N
    mov r8d, 768
    call rf_memcpy
    RF_LEAVE 32
rf_rice_save ENDP

rf_rice_restore PROC FRAME
    RF_ENTER 32
    lea rcx, g_rice_A
    lea rdx, g_rice_As
    mov r8d, 768
    call rf_memcpy
    lea rcx, g_rice_N
    lea rdx, g_rice_Ns
    mov r8d, 768
    call rf_memcpy
    RF_LEAVE 32
rf_rice_restore ENDP

; rcx = EncJob, rdx = destination, r8 = cap. rax = body size, or -1 when not strictly smaller.
rf_rice_encode PROC FRAME
    RF_ENTER 640
    lea rbx, [rsp+32]
    mov qword ptr [rbx+R_JOB], rcx
    mov rax, qword ptr [rcx+JOB_PIX]
    mov qword ptr [rbx+R_PIX], rax
    mov eax, dword ptr [rcx+JOB_STRIDE]
    mov dword ptr [rbx+R_STRIDE], eax
    mov eax, dword ptr [rcx+JOB_WIDTH]
    mov dword ptr [rbx+R_WIDTH], eax
    mov eax, dword ptr [rcx+JOB_HEIGHT]
    mov dword ptr [rbx+R_HEIGHT], eax
    mov eax, dword ptr [rcx+JOB_X0]
    mov dword ptr [rbx+R_X0], eax
    mov eax, dword ptr [rcx+JOB_Y0]
    mov dword ptr [rbx+R_Y0], eax
    mov eax, dword ptr [rcx+JOB_BW]
    mov dword ptr [rbx+R_BW], eax
    mov eax, dword ptr [rcx+JOB_BH]
    mov dword ptr [rbx+R_BH], eax
    mov qword ptr [rbx+R_LOCAL], 0
    mov qword ptr [rbx+R_DST], rdx
    mov qword ptr [rbx+R_CAP], r8
    mov qword ptr [rbx+R_LEN], 0
    mov dword ptr [rbx+R_ACC], 0
    mov dword ptr [rbx+R_NBIT], 0
    mov dword ptr [rbx+R_OVER], 0
    mov dword ptr [rbx+R_CH], 0
    cmp qword ptr [rbx+R_CAP], 0
    je ren_fail
    cmp dword ptr [rbx+R_BW], 0
    jle ren_fail
    cmp dword ptr [rbx+R_BH], 0
    jle ren_fail
ren_ch:
    cmp dword ptr [rbx+R_CH], 3
    jae ren_finish
    xor esi, esi
ren_ly:
    cmp esi, dword ptr [rbx+R_BH]
    jae ren_ch_next
    xor edi, edi
ren_lx:
    cmp edi, dword ptr [rbx+R_BW]
    jae ren_ly_next
    mov eax, dword ptr [rbx+R_X0]
    add eax, edi
    mov dword ptr [rsp+S_X], eax
    mov eax, dword ptr [rbx+R_Y0]
    add eax, esi
    mov dword ptr [rsp+S_Y], eax
    cmp dword ptr [rsp+S_X], 0
    jg ren_left_has
    mov dword ptr [rsp+S_LE], 0
    jmp ren_left_set
ren_left_has:
    cmp edi, 0
    jne ren_left_set
    mov ecx, dword ptr [rsp+S_X]
    dec ecx
    mov edx, dword ptr [rsp+S_Y]
    call rice_residual
    mov dword ptr [rsp+S_LE], eax
ren_left_set:
    cmp dword ptr [rsp+S_Y], 0
    jg ren_up_has
    mov dword ptr [rsp+S_UE], 0
    jmp ren_up_set
ren_up_has:
    cmp esi, 0
    jne ren_up_cached
    mov ecx, dword ptr [rsp+S_X]
    mov edx, dword ptr [rsp+S_Y]
    dec edx
    call rice_residual
    mov dword ptr [rsp+S_UE], eax
    jmp ren_up_set
ren_up_cached:
    lea r10, [rsp+S_PREV]
    mov eax, dword ptr [r10+rdi*4]
    mov dword ptr [rsp+S_UE], eax
ren_up_set:
    mov ecx, dword ptr [rsp+S_LE]
    call rice_qb
    mov r12d, eax
    shl r12d, 3
    mov ecx, dword ptr [rsp+S_UE]
    call rice_qb
    add r12d, eax
    mov dword ptr [rsp+S_CTX], r12d
    mov ecx, dword ptr [rsp+S_X]
    mov edx, dword ptr [rsp+S_Y]
    mov r8d, dword ptr [rbx+R_CH]
    call rice_sample
    mov dword ptr [rsp+S_VAL], eax
    xor r12d, r12d
    cmp dword ptr [rsp+S_X], 0
    jle ren_have_l
    mov ecx, dword ptr [rsp+S_X]
    dec ecx
    mov edx, dword ptr [rsp+S_Y]
    mov r8d, dword ptr [rbx+R_CH]
    call rice_sample
    mov r12d, eax
ren_have_l:
    xor r13d, r13d
    cmp dword ptr [rsp+S_Y], 0
    jle ren_have_u
    mov ecx, dword ptr [rsp+S_X]
    mov edx, dword ptr [rsp+S_Y]
    dec edx
    mov r8d, dword ptr [rbx+R_CH]
    call rice_sample
    mov r13d, eax
ren_have_u:
    xor r14d, r14d
    cmp dword ptr [rsp+S_X], 0
    jle ren_have_ul
    cmp dword ptr [rsp+S_Y], 0
    jle ren_have_ul
    mov ecx, dword ptr [rsp+S_X]
    dec ecx
    mov edx, dword ptr [rsp+S_Y]
    dec edx
    mov r8d, dword ptr [rbx+R_CH]
    call rice_sample
    mov r14d, eax
ren_have_ul:
    mov ecx, r12d
    mov edx, r13d
    mov r8d, r14d
    call rice_med
    mov dword ptr [rsp+S_PRED], eax
    mov ecx, dword ptr [rsp+S_VAL]
    mov edx, eax
    call rice_delta
    mov dword ptr [rsp+S_ERR], eax
    mov r12d, dword ptr [rbx+R_CH]
    shl r12d, 6
    add r12d, dword ptr [rsp+S_CTX]
    lea r10, g_rice_A
    mov ecx, dword ptr [r10+r12*4]
    lea r10, g_rice_N
    mov edx, dword ptr [r10+r12*4]
    call rice_k
    mov dword ptr [rsp+S_K], eax
    mov eax, dword ptr [rsp+S_ERR]
    call rice_to_m
    mov r13d, ecx
    mov ecx, dword ptr [rsp+S_K]
    mov eax, 1
    shl eax, cl
    dec eax
    and eax, r13d
    mov r14d, eax
    mov eax, r13d
    mov ecx, dword ptr [rsp+S_K]
    shr eax, cl
    mov r13d, eax
ren_quot:
    cmp r13d, 0
    je ren_stop
    mov cl, 1
    call rice_emit_bit
    cmp dword ptr [rbx+R_OVER], 0
    jne ren_fail
    dec r13d
    jmp ren_quot
ren_stop:
    xor cl, cl
    call rice_emit_bit
    cmp dword ptr [rbx+R_OVER], 0
    jne ren_fail
    xor r13d, r13d
ren_rem:
    cmp r13d, dword ptr [rsp+S_K]
    jae ren_bumped
    mov eax, r14d
    bt eax, r13d
    setc cl
    call rice_emit_bit
    cmp dword ptr [rbx+R_OVER], 0
    jne ren_fail
    inc r13d
    jmp ren_rem
ren_bumped:
    mov eax, dword ptr [rsp+S_ERR]
    mov ecx, dword ptr [rsp+S_CTX]
    call rice_bump
    mov eax, dword ptr [rsp+S_ERR]
    mov dword ptr [rsp+S_LE], eax
    lea r10, [rsp+S_PREV]
    mov dword ptr [r10+rdi*4], eax
    inc edi
    jmp ren_lx
ren_ly_next:
    inc esi
    jmp ren_ly
ren_ch_next:
    inc dword ptr [rbx+R_CH]
    jmp ren_ch
ren_finish:
    cmp dword ptr [rbx+R_OVER], 0
    jne ren_fail
    cmp dword ptr [rbx+R_NBIT], 0
    je ren_no_flush
    mov rax, qword ptr [rbx+R_LEN]
    cmp rax, qword ptr [rbx+R_CAP]
    jae ren_fail
    mov r10, qword ptr [rbx+R_DST]
    mov ecx, dword ptr [rbx+R_ACC]
    mov byte ptr [r10+rax], cl
    inc qword ptr [rbx+R_LEN]
ren_no_flush:
    mov rax, qword ptr [rbx+R_LEN]
    cmp rax, qword ptr [rbx+R_CAP]
    jae ren_fail
    jmp ren_leave
ren_fail:
    mov rax, -1
ren_leave:
    RF_LEAVE 640
rf_rice_encode ENDP

; rcx = DecJob. eax = 0, or a format error. Scatters the block on success.
rf_rice_decode PROC FRAME
    RF_ENTER 640
    mov rsi, rcx
    lea rbx, [rsp+32]
    mov qword ptr [rbx+R_JOB], rsi
    mov rax, qword ptr [rsi+DEC_PIX]
    mov qword ptr [rbx+R_PIX], rax
    mov eax, dword ptr [rsi+DEC_STRIDE]
    mov dword ptr [rbx+R_STRIDE], eax
    mov eax, dword ptr [rsi+DEC_WIDTH]
    mov dword ptr [rbx+R_WIDTH], eax
    mov eax, dword ptr [rsi+DEC_HEIGHT]
    mov dword ptr [rbx+R_HEIGHT], eax
    mov eax, dword ptr [rsi+DEC_X0]
    mov dword ptr [rbx+R_X0], eax
    mov eax, dword ptr [rsi+DEC_Y0]
    mov dword ptr [rbx+R_Y0], eax
    mov eax, dword ptr [rsi+DEC_BW]
    mov dword ptr [rbx+R_BW], eax
    mov eax, dword ptr [rsi+DEC_BH]
    mov dword ptr [rbx+R_BH], eax
    mov rax, qword ptr [rsi+DEC_DATA]
    mov qword ptr [rbx+R_DST], rax
    mov rax, qword ptr [rsi+DEC_SIZE]
    mov qword ptr [rbx+R_SIZE], rax
    mov qword ptr [rbx+R_IP], 0
    mov dword ptr [rbx+R_ACC], 0
    mov dword ptr [rbx+R_NBIT], 0
    mov dword ptr [rbx+R_OVER], 0
    mov eax, dword ptr [rbx+R_BW]
    imul eax, dword ptr [rbx+R_BH]
    mov dword ptr [rsp+S_NPIX], eax
    test eax, eax
    jz rde_bad
    cmp eax, RF_BLOCK_PIX
    ja rde_bad
    mov rcx, qword ptr [rsi+DEC_WKS]
    add rcx, WKS_RES
    xor edx, edx
    mov r8d, dword ptr [rsp+S_NPIX]
    imul r8d, 3
    call rf_memset
    mov dword ptr [rbx+R_CH], 0
rde_ch:
    cmp dword ptr [rbx+R_CH], 3
    jae rde_finish
    mov rax, qword ptr [rsi+DEC_WKS]
    add rax, WKS_RES
    mov ecx, dword ptr [rbx+R_CH]
    imul ecx, dword ptr [rsp+S_NPIX]
    add rax, rcx
    mov qword ptr [rbx+R_LOCAL], rax
    xor edi, edi
rde_ly:
    cmp edi, dword ptr [rbx+R_BH]
    jae rde_ch_next
    xor r15d, r15d
rde_lx:
    cmp r15d, dword ptr [rbx+R_BW]
    jae rde_ly_next
    mov eax, dword ptr [rbx+R_X0]
    add eax, r15d
    mov dword ptr [rsp+S_X], eax
    mov eax, dword ptr [rbx+R_Y0]
    add eax, edi
    mov dword ptr [rsp+S_Y], eax
    cmp dword ptr [rsp+S_X], 0
    jg rde_left_has
    mov dword ptr [rsp+S_LE], 0
    jmp rde_left_set
rde_left_has:
    cmp r15d, 0
    jne rde_left_set
    mov ecx, dword ptr [rsp+S_X]
    dec ecx
    mov edx, dword ptr [rsp+S_Y]
    call rice_residual
    mov dword ptr [rsp+S_LE], eax
rde_left_set:
    cmp dword ptr [rsp+S_Y], 0
    jg rde_up_has
    mov dword ptr [rsp+S_UE], 0
    jmp rde_up_set
rde_up_has:
    cmp edi, 0
    jne rde_up_cached
    mov ecx, dword ptr [rsp+S_X]
    mov edx, dword ptr [rsp+S_Y]
    dec edx
    call rice_residual
    mov dword ptr [rsp+S_UE], eax
    jmp rde_up_set
rde_up_cached:
    lea r10, [rsp+S_PREV]
    mov eax, dword ptr [r10+r15*4]
    mov dword ptr [rsp+S_UE], eax
rde_up_set:
    mov ecx, dword ptr [rsp+S_LE]
    call rice_qb
    mov r12d, eax
    shl r12d, 3
    mov ecx, dword ptr [rsp+S_UE]
    call rice_qb
    add r12d, eax
    mov dword ptr [rsp+S_CTX], r12d
    xor r12d, r12d
    cmp dword ptr [rsp+S_X], 0
    jle rde_have_l
    mov ecx, dword ptr [rsp+S_X]
    dec ecx
    mov edx, dword ptr [rsp+S_Y]
    mov r8d, dword ptr [rbx+R_CH]
    call rice_sample
    mov r12d, eax
rde_have_l:
    xor r13d, r13d
    cmp dword ptr [rsp+S_Y], 0
    jle rde_have_u
    mov ecx, dword ptr [rsp+S_X]
    mov edx, dword ptr [rsp+S_Y]
    dec edx
    mov r8d, dword ptr [rbx+R_CH]
    call rice_sample
    mov r13d, eax
rde_have_u:
    xor r14d, r14d
    cmp dword ptr [rsp+S_X], 0
    jle rde_have_ul
    cmp dword ptr [rsp+S_Y], 0
    jle rde_have_ul
    mov ecx, dword ptr [rsp+S_X]
    dec ecx
    mov edx, dword ptr [rsp+S_Y]
    dec edx
    mov r8d, dword ptr [rbx+R_CH]
    call rice_sample
    mov r14d, eax
rde_have_ul:
    mov ecx, r12d
    mov edx, r13d
    mov r8d, r14d
    call rice_med
    mov dword ptr [rsp+S_PRED], eax
    mov r12d, dword ptr [rbx+R_CH]
    shl r12d, 6
    add r12d, dword ptr [rsp+S_CTX]
    lea r10, g_rice_A
    mov ecx, dword ptr [r10+r12*4]
    lea r10, g_rice_N
    mov edx, dword ptr [r10+r12*4]
    call rice_k
    mov dword ptr [rsp+S_K], eax
    xor r13d, r13d
rde_quot:
    call rice_read_bit
    cmp dword ptr [rbx+R_OVER], 0
    jne rde_bad
    test eax, eax
    jz rde_qdone
    inc r13d
    cmp r13d, 255
    ja rde_bad
    jmp rde_quot
rde_qdone:
    xor r14d, r14d
    xor r12d, r12d
rde_rem:
    cmp r12d, dword ptr [rsp+S_K]
    jae rde_mapped
    call rice_read_bit
    cmp dword ptr [rbx+R_OVER], 0
    jne rde_bad
    test eax, eax
    jz rde_rem_next
    mov eax, 1
    mov ecx, r12d
    shl eax, cl
    or r14d, eax
rde_rem_next:
    inc r12d
    jmp rde_rem
rde_mapped:
    mov eax, r13d
    mov ecx, dword ptr [rsp+S_K]
    shl eax, cl
    or eax, r14d
    cmp eax, 255
    ja rde_bad
    mov ecx, eax
    call rice_from_m
    mov dword ptr [rsp+S_ERR], eax
    mov ecx, dword ptr [rsp+S_CTX]
    call rice_bump
    mov eax, dword ptr [rsp+S_PRED]
    add eax, dword ptr [rsp+S_ERR]
    and eax, 255
    mov r10, qword ptr [rbx+R_LOCAL]
    mov ecx, edi
    imul ecx, dword ptr [rbx+R_BW]
    add ecx, r15d
    mov byte ptr [r10+rcx], al
    mov eax, dword ptr [rsp+S_ERR]
    mov dword ptr [rsp+S_LE], eax
    lea r10, [rsp+S_PREV]
    mov dword ptr [r10+r15*4], eax
    inc r15d
    jmp rde_lx
rde_ly_next:
    inc edi
    jmp rde_ly
rde_ch_next:
    inc dword ptr [rbx+R_CH]
    jmp rde_ch
rde_finish:
    cmp dword ptr [rbx+R_OVER], 0
    jne rde_bad
    cmp dword ptr [rbx+R_ACC], 0
    jne rde_bad
    mov rax, qword ptr [rbx+R_IP]
    cmp rax, qword ptr [rbx+R_SIZE]
    jne rde_bad
    mov r8, qword ptr [rsi+DEC_WKS]
    lea rdi, [r8+WKS_RAW]
    xor ecx, ecx
rde_pack:
    cmp ecx, dword ptr [rsp+S_NPIX]
    jae rde_scatter
    mov r9, qword ptr [rsi+DEC_WKS]
    add r9, WKS_RES
    movzx r8d, byte ptr [r9+rcx]
    mov eax, dword ptr [rsp+S_NPIX]
    mov r10, r9
    add r10, rax
    movzx edx, byte ptr [r10+rcx]
    add r10, rax
    movzx r12d, byte ptr [r10+rcx]
    mov eax, ecx
    lea r10, [rax+rax*2]
    mov r11d, edx
    add r11d, r8d
    mov byte ptr [rdi+r10], r11b
    mov byte ptr [rdi+r10+1], r8b
    add r12d, r8d
    mov byte ptr [rdi+r10+2], r12b
    inc ecx
    jmp rde_pack
rde_scatter:
    mov rcx, rsi
    mov rdx, rdi
    call rf_scatter_rgb
    xor eax, eax
    jmp rde_leave
rde_bad:
    SET_ERR RF_ERR_FORMAT, msg_rice
    mov eax, RF_ERR_FORMAT
rde_leave:
    RF_LEAVE 640
rf_rice_decode ENDP

rf_rice_selftest PROC FRAME
    RF_ENTER 320
    mov qword ptr [rsp+224], 0
    mov ecx, WKS_SIZE
    call rf_alloc
    test rax, rax
    jz rst_fail
    mov qword ptr [rsp+224], rax
    call rf_rice_reset
    lea rax, st_rice_pix
    mov qword ptr [rsp+32], rax
    mov dword ptr [rsp+40], 6
    mov dword ptr [rsp+44], 0
    mov dword ptr [rsp+48], 0
    mov dword ptr [rsp+52], 2
    mov dword ptr [rsp+56], 2
    mov rax, qword ptr [rsp+224]
    mov qword ptr [rsp+64], rax
    mov qword ptr [rsp+72], 0
    mov qword ptr [rsp+80], 0
    mov dword ptr [rsp+88], 2
    mov dword ptr [rsp+92], 2
    lea rcx, [rsp+32]
    lea rdx, [rsp+96]
    mov r8d, 96
    call rf_rice_encode
    cmp rax, 1
    jl rst_fail
    cmp rax, 96
    ja rst_fail
    mov qword ptr [rsp+216], rax
    call rf_rice_reset
    lea rax, [rsp+192]
    mov qword ptr [rsp+32], rax
    mov dword ptr [rsp+40], 6
    mov dword ptr [rsp+44], 0
    mov dword ptr [rsp+48], 0
    mov dword ptr [rsp+52], 2
    mov dword ptr [rsp+56], 2
    mov rax, qword ptr [rsp+224]
    mov qword ptr [rsp+64], rax
    lea rax, [rsp+96]
    mov qword ptr [rsp+72], rax
    mov rax, qword ptr [rsp+216]
    mov qword ptr [rsp+80], rax
    mov dword ptr [rsp+88], 2
    mov dword ptr [rsp+92], 2
    lea rdi, [rsp+192]
    xor eax, eax
    mov ecx, 12
    cld
    rep stosb
    lea rcx, [rsp+32]
    call rf_rice_decode
    test eax, eax
    jnz rst_fail
    lea rsi, st_rice_pix
    lea rdi, [rsp+192]
    mov ecx, 12
    cld
    repe cmpsb
    jne rst_fail
    mov rcx, qword ptr [rsp+224]
    call rf_free
    xor eax, eax
    jmp rst_leave
rst_fail:
    mov rcx, qword ptr [rsp+224]
    call rf_free
    SET_ERR RF_ERR_LIMIT, msg_rice_self
    mov eax, RF_ERR_LIMIT
rst_leave:
    RF_LEAVE 320
rf_rice_selftest ENDP

END

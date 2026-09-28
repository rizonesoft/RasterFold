; Mode 5: subtract-green, MED, JPEG-LS gradient context, binary rANS.
; One 16-bit probability per context node. Bits are MSB first.
; The above-right sample is zero when that pixel is not decoded yet.
include rf.inc

option casemap:none
option prologue:none
option epilogue:none

EXTERNDEF rf_memcpy:PROC
EXTERNDEF rice_sample:PROC
EXTERNDEF rice_med:PROC
EXTERNDEF rice_delta:PROC
EXTERNDEF rice_residual:PROC
EXTERNDEF rice_to_m:PROC
EXTERNDEF rice_from_m:PROC
EXTERNDEF rf_scatter_rgb:PROC
EXTERNDEF rf_memset:PROC

RANS_L      equ 8388608
RANS_ONE    equ 4096
RANS_INIT   equ 2816
RANS_RATE   equ 4
RANS_NCTX   equ 13851
RANS_CELLS  equ RANS_NCTX * 256
RANS_BYTES  equ RANS_CELLS * 2
RANS_SNAP   equ 98304
XMAX_SHIFT  equ 19

R_PIX       equ 0
R_STRIDE    equ 8
R_WIDTH     equ 16
R_HEIGHT    equ 20
R_X0        equ 24
R_Y0        equ 28
R_BW        equ 32
R_BH        equ 36
R_LOCAL     equ 40
R_CH        equ 48
R_DST       equ 56
R_CAP       equ 64
R_LEN       equ 72
R_JOB       equ 112

S_X         equ 176
S_Y         equ 180
S_CTX       equ 192
S_ERR       equ 196
S_PRED      equ 204
S_NPIX      equ 212
S_NODE      equ 216
S_MAP       equ 220

.data?
ALIGN 16
g_rans_p    dw RANS_CELLS dup(?)
g_rans_save dw RANS_CELLS dup(?)
g_rans_snap dw RANS_SNAP dup(?)

.const
msg_rans db "invalid RFI: rans", 0
msg_rans_self db "internal self-test failed", 0
st_rans_pix db 10, 20, 30, 40, 50, 60, 70, 80, 90, 1, 2, 3

.code

; ecx = signed delta. eax = quantizer -4..4.
; ecx = x, edx = y, rbx = context. eax = green sample, or 0 outside the image.
; Reads the image, not the block-local plane.
rans_green PROC
    cmp ecx, 0
    jl rg0
    cmp edx, 0
    jl rg0
    cmp ecx, dword ptr [rbx+R_WIDTH]
    jge rg0
    cmp edx, dword ptr [rbx+R_HEIGHT]
    jge rg0
    movsxd rax, edx
    movsxd r10, dword ptr [rbx+R_STRIDE]
    imul rax, r10
    add rax, qword ptr [rbx+R_PIX]
    movsxd r10, ecx
    lea r10, [r10+r10*2]
    movzx eax, byte ptr [rax+r10+1]
    ret
rg0:
    xor eax, eax
    ret
rans_green ENDP

rans_qjls PROC
    cmp ecx, -21
    jg rq_a
    mov eax, -4
    ret
rq_a:
    cmp ecx, -7
    jg rq_b
    mov eax, -3
    ret
rq_b:
    cmp ecx, -3
    jg rq_c
    mov eax, -2
    ret
rq_c:
    cmp ecx, 0
    jge rq_d
    mov eax, -1
    ret
rq_d:
    jne rq_e
    xor eax, eax
    ret
rq_e:
    cmp ecx, 3
    jge rq_f
    mov eax, 1
    ret
rq_f:
    cmp ecx, 7
    jge rq_g
    mov eax, 2
    ret
rq_g:
    cmp ecx, 21
    jge rq_h
    mov eax, 3
    ret
rq_h:
    mov eax, 4
    ret
rans_qjls ENDP

; ecx = x, edx = y. rbx = context. eax = context index.
; Clobbers rax, rcx, rdx, r8-r11. Preserves rbx and callee-saved registers.
rans_ctx PROC FRAME
    RF_ENTER 96
    mov dword ptr [rsp+32], ecx
    mov dword ptr [rsp+36], edx
    xor r12d, r12d
    cmp ecx, 0
    jle rc_have_w
    dec ecx
    mov r8d, dword ptr [rbx+R_CH]
    call rice_sample
    mov r12d, eax
    mov ecx, dword ptr [rsp+32]
    mov edx, dword ptr [rsp+36]
rc_have_w:
    mov dword ptr [rsp+40], r12d
    xor r13d, r13d
    cmp edx, 0
    jle rc_have_n
    mov ecx, dword ptr [rsp+32]
    mov edx, dword ptr [rsp+36]
    dec edx
    mov r8d, dword ptr [rbx+R_CH]
    call rice_sample
    mov r13d, eax
rc_have_n:
    mov dword ptr [rsp+44], r13d
    xor r14d, r14d
    cmp dword ptr [rsp+32], 0
    jle rc_have_nw
    cmp dword ptr [rsp+36], 0
    jle rc_have_nw
    mov ecx, dword ptr [rsp+32]
    dec ecx
    mov edx, dword ptr [rsp+36]
    dec edx
    mov r8d, dword ptr [rbx+R_CH]
    call rice_sample
    mov r14d, eax
rc_have_nw:
    mov dword ptr [rsp+48], r14d
    xor r15d, r15d
    cmp dword ptr [rsp+36], 0
    jle rc_have_ne
    mov eax, dword ptr [rsp+32]
    inc eax
    cmp eax, dword ptr [rbx+R_WIDTH]
    jge rc_have_ne
    mov eax, dword ptr [rsp+36]
    dec eax
    cmp eax, dword ptr [rbx+R_Y0]
    jl rc_ne_yes
    mov eax, dword ptr [rsp+32]
    inc eax
    mov r10d, dword ptr [rbx+R_X0]
    add r10d, dword ptr [rbx+R_BW]
    cmp eax, r10d
    jge rc_have_ne
rc_ne_yes:
    mov ecx, dword ptr [rsp+32]
    inc ecx
    mov edx, dword ptr [rsp+36]
    dec edx
    mov r8d, dword ptr [rbx+R_CH]
    call rice_sample
    mov r15d, eax
rc_have_ne:
    mov ecx, r15d
    sub ecx, dword ptr [rsp+44]
    call rans_qjls
    add eax, 4
    imul eax, 81
    mov r12d, eax
    mov ecx, dword ptr [rsp+44]
    sub ecx, dword ptr [rsp+48]
    call rans_qjls
    add eax, 4
    imul eax, 9
    add r12d, eax
    mov ecx, dword ptr [rsp+48]
    sub ecx, dword ptr [rsp+40]
    call rans_qjls
    add eax, 4
    add r12d, eax
    cmp dword ptr [rbx+R_CH], 0
    jne rc_chroma
    mov ecx, dword ptr [rsp+40]
    mov edx, dword ptr [rsp+44]
    mov r8d, dword ptr [rsp+48]
    call rice_med
    mov edx, eax
    mov eax, r12d
    jmp rc_leave
rc_chroma:
    mov eax, dword ptr [rbx+R_CH]
    mov dword ptr [rsp+52], eax
    mov rax, qword ptr [rbx+R_LOCAL]
    mov qword ptr [rsp+56], rax
    mov dword ptr [rbx+R_CH], 0
    mov qword ptr [rbx+R_LOCAL], 0
    mov ecx, dword ptr [rsp+32]
    mov edx, dword ptr [rsp+36]
    call rans_green
    mov dword ptr [rsp+64], eax
    xor r11d, r11d
    cmp dword ptr [rsp+32], 0
    jle rg_n
    mov ecx, dword ptr [rsp+32]
    dec ecx
    mov edx, dword ptr [rsp+36]
    call rans_green
    mov r11d, eax
rg_n:
    mov dword ptr [rsp+68], 0
    cmp dword ptr [rsp+36], 0
    jle rg_ul
    mov ecx, dword ptr [rsp+32]
    mov edx, dword ptr [rsp+36]
    dec edx
    call rans_green
    mov dword ptr [rsp+68], eax
rg_ul:
    xor r8d, r8d
    cmp dword ptr [rsp+32], 0
    jle rg_med
    cmp dword ptr [rsp+36], 0
    jle rg_med
    mov ecx, dword ptr [rsp+32]
    dec ecx
    mov edx, dword ptr [rsp+36]
    dec edx
    call rans_green
    mov r8d, eax
rg_med:
    mov ecx, r11d
    mov edx, dword ptr [rsp+68]
    call rice_med
    mov edx, eax
    mov ecx, dword ptr [rsp+64]
    call rice_delta
    mov ecx, eax
    call rans_qjls
    add eax, 4
    mov r13d, eax
    mov eax, dword ptr [rsp+52]
    mov dword ptr [rbx+R_CH], eax
    mov rax, qword ptr [rsp+56]
    mov qword ptr [rbx+R_LOCAL], rax
    mov eax, dword ptr [rbx+R_CH]
    dec eax
    imul eax, 6561
    add eax, 729
    imul r12d, 9
    add eax, r12d
    add eax, r13d
    mov r11d, eax
    mov ecx, dword ptr [rsp+40]
    mov edx, dword ptr [rsp+44]
    mov r8d, dword ptr [rsp+48]
    call rice_med
    mov edx, eax
    mov eax, r11d
rc_leave:
    RF_LEAVE 96
rans_ctx ENDP

rf_rans_reset PROC
    push rdi
    cld
    lea rdi, g_rans_p
    mov eax, RANS_INIT
    mov ecx, RANS_CELLS
    rep stosw
    pop rdi
    ret
rf_rans_reset ENDP

rf_rans_save PROC FRAME
    RF_ENTER 32
    lea rcx, g_rans_save
    lea rdx, g_rans_p
    mov r8d, RANS_BYTES
    call rf_memcpy
    RF_LEAVE 32
rf_rans_save ENDP

rf_rans_restore PROC FRAME
    RF_ENTER 32
    lea rcx, g_rans_p
    lea rdx, g_rans_save
    mov r8d, RANS_BYTES
    call rf_memcpy
    RF_LEAVE 32
rf_rans_restore ENDP

; rcx = EncJob, rdx = destination, r8 = cap. rax = body size, or -1.
rf_rans_encode PROC FRAME
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
    mov dword ptr [rbx+R_CH], 0
    cmp r8, 4
    jb re_fail
    cmp dword ptr [rbx+R_BW], 0
    jle re_fail
    cmp dword ptr [rbx+R_BH], 0
    jle re_fail
re_ch:
    cmp dword ptr [rbx+R_CH], 3
    jae re_back
    xor esi, esi
re_ly:
    cmp esi, dword ptr [rbx+R_BH]
    jae re_ch_next
    xor edi, edi
re_lx:
    cmp edi, dword ptr [rbx+R_BW]
    jae re_ly_next
    mov eax, dword ptr [rbx+R_X0]
    add eax, edi
    mov dword ptr [rsp+S_X], eax
    mov eax, dword ptr [rbx+R_Y0]
    add eax, esi
    mov dword ptr [rsp+S_Y], eax
    mov ecx, dword ptr [rsp+S_X]
    mov edx, dword ptr [rsp+S_Y]
    call rice_residual
    mov dword ptr [rsp+S_ERR], eax
    mov ecx, dword ptr [rsp+S_X]
    mov edx, dword ptr [rsp+S_Y]
    call rans_ctx
    mov dword ptr [rsp+S_CTX], eax
    mov eax, dword ptr [rsp+S_ERR]
    call rice_to_m
    mov dword ptr [rsp+S_MAP], ecx
    mov dword ptr [rsp+S_NODE], 1
    mov r15d, 7
re_bit:
    mov eax, dword ptr [rsp+S_MAP]
    mov ecx, r15d
    shr eax, cl
    and eax, 1
    mov r14d, eax
    mov eax, dword ptr [rsp+S_CTX]
    shl eax, 8
    add eax, dword ptr [rsp+S_NODE]
    lea r10, g_rans_p
    movzx eax, word ptr [r10+rax*2]
    mov r13d, eax
    mov eax, r14d
    shl eax, 15
    or eax, r13d
    mov r11, qword ptr [rbx+R_LEN]
    cmp r11, RANS_SNAP
    jae re_fail
    lea r10, g_rans_snap
    mov word ptr [r10+r11*2], ax
    inc qword ptr [rbx+R_LEN]
    mov eax, r13d
    test r14d, r14d
    jnz re_adapt1
    mov ecx, RANS_ONE
    sub ecx, eax
    shr ecx, RANS_RATE
    add eax, ecx
    cmp eax, RANS_ONE - 1
    jbe re_adapt_store
    mov eax, RANS_ONE - 1
    jmp re_adapt_store
re_adapt1:
    mov ecx, eax
    shr ecx, RANS_RATE
    sub eax, ecx
    cmp eax, 1
    jae re_adapt_store
    mov eax, 1
re_adapt_store:
    mov r13d, eax
    mov eax, dword ptr [rsp+S_CTX]
    shl eax, 8
    add eax, dword ptr [rsp+S_NODE]
    lea r10, g_rans_p
    mov word ptr [r10+rax*2], r13w
    mov eax, dword ptr [rsp+S_NODE]
    shl eax, 1
    add eax, r14d
    mov dword ptr [rsp+S_NODE], eax
    dec r15d
    jns re_bit
    inc edi
    jmp re_lx
re_ly_next:
    inc esi
    jmp re_ly
re_ch_next:
    inc dword ptr [rbx+R_CH]
    jmp re_ch
re_back:
    mov r15d, RANS_L
    mov r12, qword ptr [rbx+R_DST]
    add r12, qword ptr [rbx+R_CAP]
    mov r14, qword ptr [rbx+R_DST]
    mov r13, qword ptr [rbx+R_LEN]
    dec r13
re_snap:
    cmp r13, 0
    jl re_flush
    lea r10, g_rans_snap
    movzx eax, word ptr [r10+r13*2]
    mov r11d, eax
    and r11d, 0FFFh
    shr eax, 15
    mov r8d, r11d
    xor r9d, r9d
    mov edx, RANS_ONE
    sub edx, r11d
    test eax, eax
    jz re_freq
    mov r9d, r11d
    mov r8d, edx
re_freq:
    mov eax, r8d
    shl eax, XMAX_SHIFT
re_renorm:
    cmp r15d, eax
    jb re_update
    cmp r12, r14
    je re_fail
    dec r12
    mov ecx, r15d
    mov byte ptr [r12], cl
    shr r15d, 8
    jmp re_renorm
re_update:
    mov eax, r15d
    xor edx, edx
    div r8d
    shl eax, 12
    add eax, edx
    add eax, r9d
    mov r15d, eax
    dec r13
    jmp re_snap
re_flush:
    mov rax, r12
    sub rax, r14
    cmp rax, 4
    jb re_fail
    sub r12, 4
    mov eax, r15d
    mov byte ptr [r12], al
    mov ecx, eax
    shr ecx, 8
    mov byte ptr [r12+1], cl
    shr eax, 16
    mov byte ptr [r12+2], al
    mov ecx, eax
    shr ecx, 8
    mov byte ptr [r12+3], cl
    mov rcx, qword ptr [rbx+R_DST]
    add rcx, qword ptr [rbx+R_CAP]
    sub rcx, r12
    cmp rcx, qword ptr [rbx+R_CAP]
    jae re_fail
    mov rsi, r12
    mov rdi, qword ptr [rbx+R_DST]
    mov r8, rcx
    cld
    rep movsb
    mov rax, r8
    jmp re_leave
re_fail:
    mov rax, -1
re_leave:
    RF_LEAVE 640
rf_rans_encode ENDP

; rcx = DecJob. eax = 0, or a format error.
rf_rans_decode PROC FRAME
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
    mov qword ptr [rbx+R_CAP], rax
    mov qword ptr [rbx+R_LEN], 4
    cmp rax, 4
    jb rd_bad
    mov r10, qword ptr [rbx+R_DST]
    movzx eax, byte ptr [r10]
    movzx ecx, byte ptr [r10+1]
    shl ecx, 8
    or eax, ecx
    movzx ecx, byte ptr [r10+2]
    shl ecx, 16
    or eax, ecx
    movzx ecx, byte ptr [r10+3]
    shl ecx, 24
    or eax, ecx
    mov dword ptr [rsp+S_MAP], eax
    mov eax, dword ptr [rbx+R_BW]
    imul eax, dword ptr [rbx+R_BH]
    mov dword ptr [rsp+S_NPIX], eax
    test eax, eax
    jz rd_bad
    cmp eax, RF_BLOCK_PIX
    ja rd_bad
    mov rcx, qword ptr [rsi+DEC_WKS]
    add rcx, WKS_RES
    xor edx, edx
    mov r8d, dword ptr [rsp+S_NPIX]
    imul r8d, 3
    call rf_memset
    mov dword ptr [rbx+R_CH], 0
rd_ch:
    cmp dword ptr [rbx+R_CH], 3
    jae rd_finish
    mov rax, qword ptr [rsi+DEC_WKS]
    add rax, WKS_RES
    mov ecx, dword ptr [rbx+R_CH]
    imul ecx, dword ptr [rsp+S_NPIX]
    add rax, rcx
    mov qword ptr [rbx+R_LOCAL], rax
    xor edi, edi
rd_ly:
    cmp edi, dword ptr [rbx+R_BH]
    jae rd_ch_next
    xor r15d, r15d
rd_lx:
    cmp r15d, dword ptr [rbx+R_BW]
    jae rd_ly_next
    mov eax, dword ptr [rbx+R_X0]
    add eax, r15d
    mov dword ptr [rsp+S_X], eax
    mov eax, dword ptr [rbx+R_Y0]
    add eax, edi
    mov dword ptr [rsp+S_Y], eax
    mov ecx, dword ptr [rsp+S_X]
    mov edx, dword ptr [rsp+S_Y]
    call rans_ctx
    mov dword ptr [rsp+S_CTX], eax
    mov dword ptr [rsp+S_PRED], edx
    mov dword ptr [rsp+S_NODE], 1
    xor r14d, r14d
    mov r12d, 7
rd_bit:
    mov eax, dword ptr [rsp+S_CTX]
    shl eax, 8
    add eax, dword ptr [rsp+S_NODE]
    lea r10, g_rans_p
    movzx r11d, word ptr [r10+rax*2]
    mov eax, dword ptr [rsp+S_MAP]
    and eax, RANS_ONE - 1
    xor r8d, r8d
    cmp eax, r11d
    setae r8b
    mov r9d, r11d
    mov eax, RANS_ONE
    sub eax, r11d
    test r8d, r8d
    jz rd_freq0
    mov edx, r11d
    mov r9d, eax
    jmp rd_mix
rd_freq0:
    xor edx, edx
rd_mix:
    mov eax, dword ptr [rsp+S_MAP]
    shr eax, 12
    imul eax, r9d
    mov ecx, dword ptr [rsp+S_MAP]
    and ecx, RANS_ONE - 1
    add eax, ecx
    sub eax, edx
    mov dword ptr [rsp+S_MAP], eax
rd_renorm:
    cmp dword ptr [rsp+S_MAP], RANS_L
    jae rd_adapted
    mov rax, qword ptr [rbx+R_LEN]
    cmp rax, qword ptr [rbx+R_CAP]
    jae rd_bad
    mov r10, qword ptr [rbx+R_DST]
    movzx ecx, byte ptr [r10+rax]
    inc qword ptr [rbx+R_LEN]
    mov eax, dword ptr [rsp+S_MAP]
    shl eax, 8
    or eax, ecx
    mov dword ptr [rsp+S_MAP], eax
    jmp rd_renorm
rd_adapted:
    mov eax, r11d
    test r8d, r8d
    jnz rd_adapt1
    mov ecx, RANS_ONE
    sub ecx, eax
    shr ecx, RANS_RATE
    add eax, ecx
    cmp eax, RANS_ONE - 1
    jbe rd_storep
    mov eax, RANS_ONE - 1
    jmp rd_storep
rd_adapt1:
    mov ecx, eax
    shr ecx, RANS_RATE
    sub eax, ecx
    cmp eax, 1
    jae rd_storep
    mov eax, 1
rd_storep:
    mov r11d, eax
    mov eax, dword ptr [rsp+S_CTX]
    shl eax, 8
    add eax, dword ptr [rsp+S_NODE]
    lea r10, g_rans_p
    mov word ptr [r10+rax*2], r11w
    mov eax, dword ptr [rsp+S_NODE]
    shl eax, 1
    add eax, r8d
    mov dword ptr [rsp+S_NODE], eax
    shl r14d, 1
    or r14d, r8d
    dec r12d
    jns rd_bit
    mov ecx, r14d
    call rice_from_m
    add eax, dword ptr [rsp+S_PRED]
    and eax, 255
    mov r10, qword ptr [rbx+R_LOCAL]
    mov ecx, edi
    imul ecx, dword ptr [rbx+R_BW]
    add ecx, r15d
    mov byte ptr [r10+rcx], al
    inc r15d
    jmp rd_lx
rd_ly_next:
    inc edi
    jmp rd_ly
rd_ch_next:
    cmp dword ptr [rbx+R_CH], 0
    jne rd_no_paint
    xor edi, edi
rd_paint_y:
    cmp edi, dword ptr [rbx+R_BH]
    jae rd_no_paint
    xor r15d, r15d
rd_paint_x:
    cmp r15d, dword ptr [rbx+R_BW]
    jae rd_paint_yn
    mov ecx, edi
    imul ecx, dword ptr [rbx+R_BW]
    add ecx, r15d
    mov r10, qword ptr [rsi+DEC_WKS]
    add r10, WKS_RES
    movzx eax, byte ptr [r10+rcx]
    mov ecx, dword ptr [rbx+R_Y0]
    add ecx, edi
    movsxd rcx, ecx
    movsxd r11, dword ptr [rbx+R_STRIDE]
    imul rcx, r11
    mov edx, dword ptr [rbx+R_X0]
    add edx, r15d
    movsxd rdx, edx
    lea rdx, [rdx+rdx*2]
    add rcx, rdx
    add rcx, qword ptr [rbx+R_PIX]
    mov byte ptr [rcx+1], al
    inc r15d
    jmp rd_paint_x
rd_paint_yn:
    inc edi
    jmp rd_paint_y
rd_no_paint:
    inc dword ptr [rbx+R_CH]
    jmp rd_ch
rd_finish:
    cmp dword ptr [rsp+S_MAP], RANS_L
    jne rd_bad
    mov rax, qword ptr [rbx+R_LEN]
    cmp rax, qword ptr [rbx+R_CAP]
    jne rd_bad
    mov r8, qword ptr [rsi+DEC_WKS]
    lea rdi, [r8+WKS_RAW]
    xor ecx, ecx
rd_pack:
    cmp ecx, dword ptr [rsp+S_NPIX]
    jae rd_scatter
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
    jmp rd_pack
rd_scatter:
    mov rcx, rsi
    mov rdx, rdi
    call rf_scatter_rgb
    xor eax, eax
    jmp rd_leave
rd_bad:
    SET_ERR RF_ERR_FORMAT, msg_rans
    mov eax, RF_ERR_FORMAT
rd_leave:
    RF_LEAVE 640
rf_rans_decode ENDP

rf_rans_selftest PROC FRAME
    RF_ENTER 320
    mov qword ptr [rsp+224], 0
    mov ecx, WKS_SIZE
    call rf_alloc
    test rax, rax
    jz rs_fail
    mov qword ptr [rsp+224], rax
    call rf_rans_reset
    lea rax, st_rans_pix
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
    mov r8d, 80
    call rf_rans_encode
    cmp rax, 1
    jl rs_fail
    cmp rax, 160
    ja rs_fail
    mov qword ptr [rsp+216], rax
    call rf_rans_reset
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
    call rf_rans_decode
    test eax, eax
    jnz rs_fail
    lea rsi, st_rans_pix
    lea rdi, [rsp+192]
    mov ecx, 12
    cld
    repe cmpsb
    jne rs_fail
    mov rcx, qword ptr [rsp+224]
    call rf_free
    xor eax, eax
    jmp rs_leave
rs_fail:
    mov rcx, qword ptr [rsp+224]
    call rf_free
    SET_ERR RF_ERR_LIMIT, msg_rans_self
    mov eax, RF_ERR_LIMIT
rs_leave:
    RF_LEAVE 320
rf_rans_selftest ENDP

END

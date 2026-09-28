; Block codec: raw, PackBits, planar subtract-green + left residual, palette.
include rf.inc

option casemap:none
option prologue:none
option epilogue:none

.data
g_mode_count dd 6 dup(0)

.const
msg_self db "internal self-test failed", 0
msg_enc_ov db "internal encoder overflow", 0
msg_oom db "out of memory", 0
msg_limit db "image exceeds size limit", 0
msg_magic db "invalid RFI: magic", 0
msg_ver db "unsupported RFI: version", 0
msg_hs db "unsupported RFI: header size", 0
msg_bs db "unsupported RFI: block size", 0
msg_pf db "unsupported RFI: pixel format", 0
msg_hcrc db "invalid RFI: header checksum", 0
msg_dim db "invalid RFI: dimensions", 0
msg_plen db "invalid RFI: payload length", 0
msg_pcrc db "invalid RFI: payload checksum", 0
msg_btrunc db "invalid RFI: truncated block", 0
msg_bflags db "invalid RFI: block flags", 0
msg_bmode db "invalid RFI: block mode", 0
msg_blen db "invalid RFI: block length", 0
msg_raw db "invalid RFI: raw length", 0
msg_pb db "invalid RFI: packbits", 0
msg_pal db "invalid RFI: palette", 0
msg_pidx db "invalid RFI: palette index", 0
msg_trail db "invalid RFI: trailing data", 0
st_rgb db 10, 20, 30, 10, 20, 30, 10, 20, 30, 11, 20, 30
st_res db 0F6h, 00h, 0F6h, 01h, 14h, 00h, 14h, 00h, 0Ah, 00h, 0Ah, 00h

.code

; rcx = job, rdx = destination. Copies one block out to packed RGB.
rf_extract_rgb PROC FRAME
    RF_ENTER 64
    mov rbx, rcx
    mov rsi, rdx
    mov r12d, dword ptr [rbx+JOB_BH]
    mov r13d, dword ptr [rbx+JOB_Y0]
ex_row:
    test r12d, r12d
    jz ex_done
    mov eax, r13d
    mov r8d, dword ptr [rbx+JOB_STRIDE]
    imul rax, r8
    add rax, qword ptr [rbx+JOB_PIX]
    mov ecx, dword ptr [rbx+JOB_X0]
    imul ecx, 3
    add rax, rcx
    mov ecx, dword ptr [rbx+JOB_BW]
    imul ecx, 3
    mov r14d, ecx
    mov rcx, rsi
    mov rdx, rax
    mov r8d, r14d
    call rf_memcpy
    add rsi, r14
    inc r13d
    dec r12d
    jmp ex_row
ex_done:
    RF_LEAVE 64
rf_extract_rgb ENDP

; rcx = job, rdx = packed RGB source.
rf_scatter_rgb PROC FRAME
    RF_ENTER 64
    mov rbx, rcx
    mov rsi, rdx
    mov r12d, dword ptr [rbx+JOB_BH]
    mov r13d, dword ptr [rbx+JOB_Y0]
sc_row:
    test r12d, r12d
    jz sc_done
    mov eax, r13d
    mov r8d, dword ptr [rbx+JOB_STRIDE]
    imul rax, r8
    add rax, qword ptr [rbx+JOB_PIX]
    mov ecx, dword ptr [rbx+JOB_X0]
    imul ecx, 3
    add rax, rcx
    mov ecx, dword ptr [rbx+JOB_BW]
    imul ecx, 3
    mov r14d, ecx
    mov rcx, rax
    mov rdx, rsi
    mov r8d, r14d
    call rf_memcpy
    add rsi, r14
    inc r13d
    dec r12d
    jmp sc_row
sc_done:
    RF_LEAVE 64
rf_scatter_rgb ENDP

; rcx = interleaved RGB, edx = bw, r8d = bh, r9 = planar residual destination.
rf_predict PROC FRAME
    RF_ENTER 48
    mov rsi, rcx
    mov rdi, r9
    mov r12d, edx
    mov r13d, r8d
    mov eax, r12d
    imul eax, r13d
    mov r14d, eax
    xor r15d, r15d
    xor r10d, r10d
pr_row:
    cmp r15d, r13d
    jae pr_done
    xor ebx, ebx
    xor r11d, r11d
    mov dword ptr [rsp+36], 0
pr_col:
    mov eax, dword ptr [rsp+36]
    cmp eax, r12d
    jae pr_next
    movzx eax, byte ptr [rsi]
    movzx ecx, byte ptr [rsi+1]
    movzx edx, byte ptr [rsi+2]
    sub al, cl
    sub dl, cl
    mov byte ptr [rsp+32], al
    mov byte ptr [rsp+33], cl
    mov byte ptr [rsp+34], dl
    mov al, byte ptr [rsp+32]
    sub al, bl
    mov r8b, al
    mov al, byte ptr [rsp+33]
    sub al, bh
    mov r9b, al
    mov al, byte ptr [rsp+34]
    sub al, r11b
    mov byte ptr [rsp+35], al
    mov ecx, r10d
    mov edx, r14d
    lea rax, [rdi+rcx]
    mov byte ptr [rax], r8b
    lea rax, [rdi+rdx]
    add rax, rcx
    mov byte ptr [rax], r9b
    lea rax, [rdi+rdx*2]
    add rax, rcx
    mov dl, byte ptr [rsp+35]
    mov byte ptr [rax], dl
    mov bl, byte ptr [rsp+32]
    mov bh, byte ptr [rsp+33]
    mov r11b, byte ptr [rsp+34]
    add rsi, 3
    inc r10d
    inc dword ptr [rsp+36]
    jmp pr_col
pr_next:
    inc r15d
    jmp pr_row
pr_done:
    RF_LEAVE 48
rf_predict ENDP

; rcx = planar residuals, edx = bw, r8d = bh, r9 = interleaved RGB destination.
rf_unpredict PROC FRAME
    RF_ENTER 48
    mov rsi, rcx
    mov rdi, r9
    mov r12d, edx
    mov r13d, r8d
    mov eax, r12d
    imul eax, r13d
    mov r14d, eax
    xor r15d, r15d
    xor r10d, r10d
up_row:
    cmp r15d, r13d
    jae up_done
    mov byte ptr [rsp+40], 0
    mov byte ptr [rsp+41], 0
    mov byte ptr [rsp+42], 0
    mov dword ptr [rsp+36], 0
up_col:
    mov eax, dword ptr [rsp+36]
    cmp eax, r12d
    jae up_next
    mov ecx, r10d
    mov edx, r14d
    movzx eax, byte ptr [rsi+rcx]
    mov byte ptr [rsp+43], al
    lea rax, [rsi+rdx]
    movzx eax, byte ptr [rax+rcx]
    mov byte ptr [rsp+44], al
    lea rax, [rsi+rdx*2]
    movzx eax, byte ptr [rax+rcx]
    mov byte ptr [rsp+45], al
    mov al, byte ptr [rsp+43]
    add al, byte ptr [rsp+40]
    mov byte ptr [rsp+32], al
    mov al, byte ptr [rsp+44]
    add al, byte ptr [rsp+41]
    mov byte ptr [rsp+33], al
    mov al, byte ptr [rsp+45]
    add al, byte ptr [rsp+42]
    mov byte ptr [rsp+34], al
    mov al, byte ptr [rsp+32]
    add al, byte ptr [rsp+33]
    mov byte ptr [rsp+46], al
    mov al, byte ptr [rsp+34]
    add al, byte ptr [rsp+33]
    mov byte ptr [rsp+47], al
    mov ecx, r10d
    lea ecx, [rcx+rcx*2]
    mov al, byte ptr [rsp+46]
    mov byte ptr [rdi+rcx], al
    mov al, byte ptr [rsp+33]
    mov byte ptr [rdi+rcx+1], al
    mov al, byte ptr [rsp+47]
    mov byte ptr [rdi+rcx+2], al
    mov al, byte ptr [rsp+32]
    mov byte ptr [rsp+40], al
    mov al, byte ptr [rsp+33]
    mov byte ptr [rsp+41], al
    mov al, byte ptr [rsp+34]
    mov byte ptr [rsp+42], al
    inc r10d
    inc dword ptr [rsp+36]
    jmp up_col
up_next:
    inc r15d
    jmp up_row
up_done:
    RF_LEAVE 48
rf_unpredict ENDP

; rcx = RGB, edx = pixel count, r8 = palette, r9 = indexes.
; eax = colour count, or -1 when the block needs more than 256 colours.
rf_palette_build PROC FRAME
    RF_ENTER 32
    mov rsi, rcx
    mov r12d, edx
    mov rdi, r8
    mov r13, r9
    cmp r12d, RF_BLOCK_PIX
    ja pb_over
    xor r14d, r14d
    xor r15d, r15d
pb_pix:
    cmp r15d, r12d
    jae pb_done
    mov eax, r15d
    lea rax, [rax+rax*2]
    lea r10, [rsi+rax]
    xor ebx, ebx
pb_find:
    cmp ebx, r14d
    jae pb_add
    mov eax, ebx
    lea rax, [rax+rax*2]
    lea r11, [rdi+rax]
    mov al, byte ptr [r11]
    cmp al, byte ptr [r10]
    jne pb_next
    mov al, byte ptr [r11+1]
    cmp al, byte ptr [r10+1]
    jne pb_next
    mov al, byte ptr [r11+2]
    cmp al, byte ptr [r10+2]
    jne pb_next
    mov byte ptr [r13+r15], bl
    jmp pb_step
pb_next:
    inc ebx
    jmp pb_find
pb_add:
    cmp r14d, 256
    jae pb_over
    mov eax, r14d
    lea rax, [rax+rax*2]
    lea r11, [rdi+rax]
    mov al, byte ptr [r10]
    mov byte ptr [r11], al
    mov al, byte ptr [r10+1]
    mov byte ptr [r11+1], al
    mov al, byte ptr [r10+2]
    mov byte ptr [r11+2], al
    mov byte ptr [r13+r15], r14b
    inc r14d
pb_step:
    inc r15d
    jmp pb_pix
pb_done:
    mov eax, r14d
    jmp pb_leave
pb_over:
    mov eax, -1
pb_leave:
    RF_LEAVE 32
rf_palette_build ENDP

; rcx points at dst, palette, indexes, ncolors, npix.
rf_palette_map PROC FRAME
    RF_ENTER 32
    mov rdi, qword ptr [rcx]
    mov rsi, qword ptr [rcx+8]
    mov r12, qword ptr [rcx+16]
    mov r13d, dword ptr [rcx+24]
    mov r14d, dword ptr [rcx+28]
    xor r15d, r15d
pm_loop:
    cmp r15d, r14d
    jae pm_ok
    movzx eax, byte ptr [r12+r15]
    cmp eax, r13d
    jae pm_bad
    lea rax, [rax+rax*2]
    lea r10, [rsi+rax]
    mov eax, r15d
    lea rax, [rax+rax*2]
    mov dl, byte ptr [r10]
    mov byte ptr [rdi+rax], dl
    mov dl, byte ptr [r10+1]
    mov byte ptr [rdi+rax+1], dl
    mov dl, byte ptr [r10+2]
    mov byte ptr [rdi+rax+2], dl
    inc r15d
    jmp pm_loop
pm_ok:
    xor eax, eax
    jmp pm_leave
pm_bad:
    mov eax, RF_ERR_FORMAT
pm_leave:
    RF_LEAVE 32
rf_palette_map ENDP

; rcx = EncJob. rax = bytes written, or -1.
rf_encode_block PROC FRAME
    RF_ENTER 80
    mov rbx, rcx
    mov eax, dword ptr [rbx+JOB_BW]
    imul eax, dword ptr [rbx+JOB_BH]
    test eax, eax
    jz eb_ov
    cmp eax, RF_BLOCK_PIX
    ja eb_ov
    mov dword ptr [rsp+32], eax
    imul eax, 3
    cmp eax, RF_BLOCK_BYTES
    ja eb_ov
    mov r12d, eax
    mov r13d, MODE_RAW
    mov r14d, r12d
    mov dword ptr [rsp+44], r14d
    mov rcx, rbx
    mov r10, qword ptr [rbx+JOB_WKS]
    lea rdx, [r10+WKS_RAW]
    call rf_extract_rgb
    mov r10, qword ptr [rbx+JOB_WKS]
    lea rcx, [r10+WKS_BEST]
    lea rdx, [r10+WKS_RAW]
    mov r8d, r12d
    call rf_memcpy

    mov r10, qword ptr [rbx+JOB_WKS]
    lea rcx, [r10+WKS_RAW]
    mov edx, r12d
    lea r8, [r10+WKS_TRIAL]
    mov r9d, RF_TRIAL_CAP
    call rf_packbits_encode
    cmp rax, 0
    jl eb_after_raw_pb
    cmp rax, r14
    jae eb_after_raw_pb
    mov r15, rax
    mov r13d, MODE_PACKBITS
    mov r10, qword ptr [rbx+JOB_WKS]
    lea rcx, [r10+WKS_BEST]
    lea rdx, [r10+WKS_TRIAL]
    mov r8, r15
    call rf_memcpy
    mov r14, r15
eb_after_raw_pb:
    mov r10, qword ptr [rbx+JOB_WKS]
    lea rcx, [r10+WKS_RAW]
    mov edx, dword ptr [rbx+JOB_BW]
    mov r8d, dword ptr [rbx+JOB_BH]
    lea r9, [r10+WKS_RES]
    call rf_predict
    mov r10, qword ptr [rbx+JOB_WKS]
    lea rcx, [r10+WKS_RES]
    mov edx, r12d
    lea r8, [r10+WKS_TRIAL]
    mov r9d, RF_TRIAL_CAP
    call rf_packbits_encode
    cmp rax, 0
    jl eb_after_pred
    cmp rax, r14
    jae eb_after_pred
    mov r15, rax
    mov r13d, MODE_PREDICT
    mov r10, qword ptr [rbx+JOB_WKS]
    lea rcx, [r10+WKS_BEST]
    lea rdx, [r10+WKS_TRIAL]
    mov r8, r15
    call rf_memcpy
    mov r14, r15
eb_after_pred:
    mov r10, qword ptr [rbx+JOB_WKS]
    lea rcx, [r10+WKS_RAW]
    mov edx, dword ptr [rsp+32]
    lea r8, [r10+WKS_PAL]
    lea r9, [r10+WKS_IDX]
    call rf_palette_build
    cmp eax, 1
    jl eb_rice
    mov esi, eax
    mov r10, qword ptr [rbx+JOB_WKS]
    lea rdi, [r10+WKS_TRIAL]
    mov eax, esi
    dec eax
    mov byte ptr [rdi], al
    lea rcx, [rdi+1]
    lea rdx, [r10+WKS_PAL]
    mov eax, esi
    imul eax, 3
    mov r15d, eax
    mov r8d, eax
    call rf_memcpy
    mov eax, RF_TRIAL_CAP
    sub eax, 1
    sub eax, r15d
    jbe eb_rice
    mov r10, qword ptr [rbx+JOB_WKS]
    lea rcx, [r10+WKS_IDX]
    mov edx, dword ptr [rsp+32]
    lea r8, [r10+WKS_TRIAL+1]
    add r8, r15
    mov r9d, RF_TRIAL_CAP
    sub r9d, 1
    sub r9d, r15d
    call rf_packbits_encode
    cmp rax, 0
    jl eb_rice
    mov rcx, rax
    add rcx, r15
    jc eb_rice
    add rcx, 1
    jc eb_rice
    cmp rcx, r14
    jae eb_rice
    mov r13d, MODE_PALETTE
    mov r14, rcx
    mov r10, qword ptr [rbx+JOB_WKS]
    lea rcx, [r10+WKS_BEST]
    lea rdx, [r10+WKS_TRIAL]
    mov r8, r14
    call rf_memcpy
eb_rice:
    mov dword ptr [rsp+40], 0
    call rf_rice_save
    mov rcx, rbx
    mov r10, qword ptr [rbx+JOB_WKS]
    lea rdx, [r10+WKS_TRIAL]
    mov r8, r14
    call rf_rice_encode
    cmp rax, -1
    je eb_rice_drop
    cmp rax, r14
    jae eb_rice_drop
    mov r15, rax
    mov r13d, MODE_RICE
    mov dword ptr [rsp+40], 1
    mov r10, qword ptr [rbx+JOB_WKS]
    lea rcx, [r10+WKS_BEST]
    lea rdx, [r10+WKS_TRIAL]
    mov r8, r15
    call rf_memcpy
    mov r14, r15
    jmp eb_rans
eb_rice_drop:
    call rf_rice_restore
eb_rans:
    call rf_rans_save
    mov rcx, rbx
    mov r10, qword ptr [rbx+JOB_WKS]
    lea rdx, [r10+WKS_TRIAL]
    mov r8d, dword ptr [rsp+44]
    call rf_rans_encode
    cmp rax, -1
    je eb_rans_drop
    mov r15, rax
    cmp dword ptr [rsp+40], 0
    je eb_rans_keep
    call rf_rice_restore
eb_rans_keep:
    mov r13d, MODE_RANS
    mov r10, qword ptr [rbx+JOB_WKS]
    lea rcx, [r10+WKS_BEST]
    lea rdx, [r10+WKS_TRIAL]
    mov r8, r15
    call rf_memcpy
    mov r14, r15
    jmp eb_emit
eb_rans_drop:
    call rf_rans_restore
eb_emit:
    mov rax, qword ptr [rbx+JOB_OUTEND]
    sub rax, qword ptr [rbx+JOB_OUT]
    mov rcx, r14
    add rcx, 6
    cmp rax, rcx
    jb eb_ov
    mov rdi, qword ptr [rbx+JOB_OUT]
    mov eax, r13d
    mov byte ptr [rdi], al
    mov byte ptr [rdi+1], 0
    mov rax, r14
    mov dword ptr [rdi+2], eax
    mov r10, qword ptr [rbx+JOB_WKS]
    lea rcx, [rdi+6]
    lea rdx, [r10+WKS_BEST]
    mov r8, r14
    call rf_memcpy
    mov eax, r13d
    lea r10, g_mode_count
    inc dword ptr [r10+rax*4]
    mov rax, r14
    add rax, 6
    jmp eb_leave
eb_ov:
    SET_ERR RF_ERR_LIMIT, msg_enc_ov
    mov rax, -1
eb_leave:
    RF_LEAVE 80
rf_encode_block ENDP

; rcx = DecJob. eax = 0 or an error code.
rf_decode_block PROC FRAME
    RF_ENTER 96
    mov rbx, rcx
    mov eax, dword ptr [rbx+DEC_BW]
    imul eax, dword ptr [rbx+DEC_BH]
    test eax, eax
    jz db_bad_mode
    cmp eax, RF_BLOCK_PIX
    ja db_bad_mode
    mov r12d, eax
    imul eax, 3
    mov r13d, eax
    mov eax, dword ptr [rbx+DEC_MODE]
    cmp eax, MODE_RAW
    je db_raw
    cmp eax, MODE_PACKBITS
    je db_pack
    cmp eax, MODE_PREDICT
    je db_pred
    cmp eax, MODE_PALETTE
    je db_pal
    cmp eax, MODE_RICE
    je db_rice
    cmp eax, MODE_RANS
    je db_rans
    jmp db_bad_mode
db_raw:
    mov rax, qword ptr [rbx+DEC_SIZE]
    mov ecx, r13d
    cmp rax, rcx
    jne db_bad_raw
    mov rcx, rbx
    mov rdx, qword ptr [rbx+DEC_DATA]
    call rf_scatter_rgb
    jmp db_ok
db_pack:
    mov rcx, qword ptr [rbx+DEC_DATA]
    mov rdx, qword ptr [rbx+DEC_SIZE]
    mov r10, qword ptr [rbx+DEC_WKS]
    lea r8, [r10+WKS_RAW]
    mov r9d, r13d
    call rf_packbits_decode
    test eax, eax
    jnz db_bad_pb
    mov rcx, rbx
    mov r10, qword ptr [rbx+DEC_WKS]
    lea rdx, [r10+WKS_RAW]
    call rf_scatter_rgb
    jmp db_ok
db_pred:
    mov rcx, qword ptr [rbx+DEC_DATA]
    mov rdx, qword ptr [rbx+DEC_SIZE]
    mov r10, qword ptr [rbx+DEC_WKS]
    lea r8, [r10+WKS_RES]
    mov r9d, r13d
    call rf_packbits_decode
    test eax, eax
    jnz db_bad_pb
    mov r10, qword ptr [rbx+DEC_WKS]
    lea rcx, [r10+WKS_RES]
    mov edx, dword ptr [rbx+DEC_BW]
    mov r8d, dword ptr [rbx+DEC_BH]
    lea r9, [r10+WKS_RAW]
    call rf_unpredict
    mov rcx, rbx
    mov r10, qword ptr [rbx+DEC_WKS]
    lea rdx, [r10+WKS_RAW]
    call rf_scatter_rgb
    jmp db_ok
db_pal:
    mov rax, qword ptr [rbx+DEC_SIZE]
    cmp rax, 1
    jb db_bad_pal
    mov r10, qword ptr [rbx+DEC_DATA]
    movzx esi, byte ptr [r10]
    inc esi
    mov eax, esi
    imul eax, 3
    mov r14d, eax
    mov rax, qword ptr [rbx+DEC_SIZE]
    lea rcx, [r14+1]
    cmp rax, rcx
    jb db_bad_pal
    sub rax, rcx
    mov r15, rax
    mov r10, qword ptr [rbx+DEC_DATA]
    lea rcx, [r10+1]
    add rcx, r14
    mov rdx, r15
    mov r11, qword ptr [rbx+DEC_WKS]
    lea r8, [r11+WKS_IDX]
    mov r9d, r12d
    call rf_packbits_decode
    test eax, eax
    jnz db_bad_pb
    mov r10, qword ptr [rbx+DEC_WKS]
    lea rax, [r10+WKS_RAW]
    mov qword ptr [rsp+32], rax
    mov r11, qword ptr [rbx+DEC_DATA]
    lea rax, [r11+1]
    mov qword ptr [rsp+40], rax
    lea rax, [r10+WKS_IDX]
    mov qword ptr [rsp+48], rax
    mov dword ptr [rsp+56], esi
    mov dword ptr [rsp+60], r12d
    lea rcx, [rsp+32]
    call rf_palette_map
    test eax, eax
    jnz db_bad_idx
    mov rcx, rbx
    mov r10, qword ptr [rbx+DEC_WKS]
    lea rdx, [r10+WKS_RAW]
    call rf_scatter_rgb
    jmp db_ok
db_rice:
    mov rcx, rbx
    call rf_rice_decode
    test eax, eax
    jnz db_leave
    jmp db_ok
db_rans:
    mov rcx, rbx
    call rf_rans_decode
    test eax, eax
    jnz db_leave
    jmp db_ok
db_ok:
    mov eax, dword ptr [rbx+DEC_MODE]
    lea r10, g_mode_count
    inc dword ptr [r10+rax*4]
    xor eax, eax
    jmp db_leave
db_bad_mode:
    SET_ERR RF_ERR_FORMAT, msg_bmode
    mov eax, RF_ERR_FORMAT
    jmp db_leave
db_bad_raw:
    SET_ERR RF_ERR_FORMAT, msg_raw
    mov eax, RF_ERR_FORMAT
    jmp db_leave
db_bad_pb:
    SET_ERR RF_ERR_FORMAT, msg_pb
    mov eax, RF_ERR_FORMAT
    jmp db_leave
db_bad_pal:
    SET_ERR RF_ERR_FORMAT, msg_pal
    mov eax, RF_ERR_FORMAT
    jmp db_leave
db_bad_idx:
    SET_ERR RF_ERR_FORMAT, msg_pidx
    mov eax, RF_ERR_FORMAT
db_leave:
    RF_LEAVE 96
rf_decode_block ENDP

rf_clear_modes PROC
    xor eax, eax
    mov dword ptr g_mode_count, eax
    mov dword ptr [g_mode_count+4], eax
    mov dword ptr [g_mode_count+8], eax
    mov dword ptr [g_mode_count+12], eax
    mov dword ptr [g_mode_count+16], eax
    mov dword ptr [g_mode_count+20], eax
    ret
rf_clear_modes ENDP

; rcx = image, rdx = qword* file, r8 = qword* size.
rf_encode_image PROC FRAME
    RF_ENTER 320
    mov rbx, rcx
    mov qword ptr [rsp+64], rdx
    mov qword ptr [rsp+72], r8
    mov qword ptr [rdx], 0
    mov qword ptr [r8], 0
    mov qword ptr [rsp+80], 0
    mov qword ptr [rsp+96], 0
    mov qword ptr [rsp+104], 0
    call rf_clear_modes
    call rf_rice_reset
    call rf_rans_reset
    mov eax, dword ptr [rbx+IMG_WIDTH]
    mov dword ptr [rsp+112], eax
    mov eax, dword ptr [rbx+IMG_HEIGHT]
    mov dword ptr [rsp+116], eax
    mov eax, dword ptr [rbx+IMG_STRIDE]
    mov dword ptr [rsp+120], eax
    cmp dword ptr [rsp+112], 1
    jb ei_dim
    cmp dword ptr [rsp+112], RF_MAX_DIM
    ja ei_limit
    cmp dword ptr [rsp+116], 1
    jb ei_dim
    cmp dword ptr [rsp+116], RF_MAX_DIM
    ja ei_limit
    mov eax, dword ptr [rsp+112]
    mov ecx, dword ptr [rsp+116]
    imul rax, rcx
    cmp rax, RF_MAX_PIXELS
    ja ei_limit
    mov eax, dword ptr [rsp+112]
    add eax, RF_BLOCK - 1
    shr eax, 6
    mov dword ptr [rsp+124], eax
    mov eax, dword ptr [rsp+116]
    add eax, RF_BLOCK - 1
    shr eax, 6
    mov dword ptr [rsp+128], eax
    mov eax, dword ptr [rsp+112]
    mov ecx, dword ptr [rsp+116]
    imul rax, rcx
    imul rax, 3
    mov r15, rax
    mov eax, dword ptr [rsp+124]
    mov ecx, dword ptr [rsp+128]
    imul rax, rcx
    imul rax, 6
    add rax, r15
    jc ei_limit
    add rax, RF_HEADER_SIZE
    jc ei_limit
    cmp rax, RF_MAX_FILE
    ja ei_limit
    mov qword ptr [rsp+88], rax
    mov rcx, rax
    call rf_alloc
    test rax, rax
    jz ei_oom
    mov qword ptr [rsp+80], rax
    mov rcx, WKS_SIZE
    call rf_alloc
    test rax, rax
    jz ei_oom
    mov qword ptr [rsp+96], rax
    mov dword ptr [rsp+132], 0
ei_by:
    mov eax, dword ptr [rsp+132]
    cmp eax, dword ptr [rsp+128]
    jae ei_header
    mov dword ptr [rsp+136], 0
ei_bx:
    mov eax, dword ptr [rsp+136]
    cmp eax, dword ptr [rsp+124]
    jae ei_by_next
    mov eax, dword ptr [rsp+132]
    shl eax, 6
    mov r10d, eax
    mov eax, dword ptr [rsp+116]
    sub eax, r10d
    cmp eax, RF_BLOCK
    jbe ei_bh
    mov eax, RF_BLOCK
ei_bh:
    mov r11d, eax
    mov eax, dword ptr [rsp+136]
    shl eax, 6
    mov r8d, eax
    mov eax, dword ptr [rsp+112]
    sub eax, r8d
    cmp eax, RF_BLOCK
    jbe ei_bw
    mov eax, RF_BLOCK
ei_bw:
    mov r9d, eax
    lea rax, [rbx+IMG_PIXELS]
    mov qword ptr [rsp+144], rax
    mov eax, dword ptr [rsp+120]
    mov dword ptr [rsp+152], eax
    mov dword ptr [rsp+156], r8d
    mov dword ptr [rsp+160], r10d
    mov dword ptr [rsp+164], r9d
    mov dword ptr [rsp+168], r11d
    mov rax, qword ptr [rsp+96]
    mov qword ptr [rsp+176], rax
    mov rax, qword ptr [rsp+80]
    add rax, RF_HEADER_SIZE
    add rax, qword ptr [rsp+104]
    mov qword ptr [rsp+184], rax
    mov rax, qword ptr [rsp+80]
    add rax, qword ptr [rsp+88]
    mov qword ptr [rsp+192], rax
    mov eax, dword ptr [rsp+112]
    mov dword ptr [rsp+200], eax
    mov eax, dword ptr [rsp+116]
    mov dword ptr [rsp+204], eax
    lea rcx, [rsp+144]
    call rf_encode_block
    cmp rax, -1
    je ei_fail
    add qword ptr [rsp+104], rax
    inc dword ptr [rsp+136]
    jmp ei_bx
ei_by_next:
    inc dword ptr [rsp+132]
    jmp ei_by
ei_header:
    mov r10, qword ptr [rsp+80]
    mov dword ptr [r10], RF_MAGIC
    mov word ptr [r10+4], 1
    mov word ptr [r10+6], RF_HEADER_SIZE
    mov eax, dword ptr [rsp+112]
    mov dword ptr [r10+8], eax
    mov eax, dword ptr [rsp+116]
    mov dword ptr [r10+12], eax
    mov word ptr [r10+16], RF_BLOCK
    mov word ptr [r10+18], RF_FORMAT_RGB8
    mov rax, qword ptr [rsp+104]
    mov dword ptr [r10+20], eax
    lea rcx, [r10+RF_HEADER_SIZE]
    mov rdx, rax
    call rf_crc32
    mov r10, qword ptr [rsp+80]
    mov dword ptr [r10+24], eax
    mov rcx, r10
    mov edx, 28
    call rf_crc32
    mov r10, qword ptr [rsp+80]
    mov dword ptr [r10+28], eax
    mov rax, qword ptr [rsp+64]
    mov rcx, qword ptr [rsp+80]
    mov qword ptr [rax], rcx
    mov rax, qword ptr [rsp+72]
    mov rcx, qword ptr [rsp+104]
    add rcx, RF_HEADER_SIZE
    mov qword ptr [rax], rcx
    mov rcx, qword ptr [rsp+96]
    call rf_free
    xor eax, eax
    jmp ei_leave
ei_dim:
    SET_ERR RF_ERR_FORMAT, msg_dim
    mov eax, RF_ERR_FORMAT
    jmp ei_fail
ei_limit:
    SET_ERR RF_ERR_LIMIT, msg_limit
    mov eax, RF_ERR_LIMIT
    jmp ei_fail
ei_oom:
    SET_ERR RF_ERR_LIMIT, msg_oom
    mov eax, RF_ERR_LIMIT
ei_fail:
    mov eax, dword ptr g_err_code
    test eax, eax
    jnz ei_fail_have
    mov eax, RF_ERR_LIMIT
ei_fail_have:
    mov dword ptr [rsp+48], eax
    mov rcx, qword ptr [rsp+80]
    call rf_free
    mov rcx, qword ptr [rsp+96]
    call rf_free
    mov eax, dword ptr [rsp+48]
ei_leave:
    RF_LEAVE 320
rf_encode_image ENDP

; rcx = file, rdx = size, r8 = qword* image, r9 = stats or 0.
rf_decode_rfi PROC FRAME
    RF_ENTER 320
    mov rbx, rcx
    mov r12, rdx
    mov qword ptr [rsp+64], r8
    mov qword ptr [rsp+72], r9
    mov qword ptr [r8], 0
    mov qword ptr [rsp+80], 0
    mov qword ptr [rsp+88], 0
    mov dword ptr g_err_block, 0FFFFFFFFh
    call rf_clear_modes
    call rf_rice_reset
    call rf_rans_reset
    cmp r12, RF_HEADER_SIZE
    jb di_trunc
    cmp r12, RF_MAX_FILE
    ja di_limit
    mov eax, dword ptr [rbx]
    cmp eax, RF_MAGIC
    jne di_magic
    movzx eax, word ptr [rbx+4]
    cmp eax, 1
    jne di_ver
    movzx eax, word ptr [rbx+6]
    cmp eax, RF_HEADER_SIZE
    jne di_hs
    movzx eax, word ptr [rbx+16]
    cmp eax, RF_BLOCK
    jne di_bs
    movzx eax, word ptr [rbx+18]
    cmp eax, RF_FORMAT_RGB8
    jne di_pf
    mov rcx, rbx
    mov edx, 28
    call rf_crc32
    cmp eax, dword ptr [rbx+28]
    jne di_hcrc
    mov eax, dword ptr [rbx+8]
    mov dword ptr [rsp+96], eax
    mov eax, dword ptr [rbx+12]
    mov dword ptr [rsp+100], eax
    cmp dword ptr [rsp+96], 1
    jb di_dim
    cmp dword ptr [rsp+96], RF_MAX_DIM
    ja di_limit
    cmp dword ptr [rsp+100], 1
    jb di_dim
    cmp dword ptr [rsp+100], RF_MAX_DIM
    ja di_limit
    mov eax, dword ptr [rsp+96]
    mov ecx, dword ptr [rsp+100]
    imul rax, rcx
    cmp rax, RF_MAX_PIXELS
    ja di_limit
    mov eax, dword ptr [rbx+20]
    mov rcx, r12
    sub rcx, RF_HEADER_SIZE
    cmp rax, rcx
    jne di_plen
    mov qword ptr [rsp+112], rax
    lea rcx, [rbx+RF_HEADER_SIZE]
    mov rdx, rax
    call rf_crc32
    cmp eax, dword ptr [rbx+24]
    jne di_pcrc
    mov eax, dword ptr [rsp+96]
    add eax, RF_BLOCK - 1
    shr eax, 6
    mov dword ptr [rsp+104], eax
    mov eax, dword ptr [rsp+100]
    add eax, RF_BLOCK - 1
    shr eax, 6
    mov dword ptr [rsp+108], eax
    mov eax, dword ptr [rsp+96]
    mov ecx, dword ptr [rsp+100]
    imul rax, rcx
    imul rax, 3
    add rax, IMG_PIXELS
    mov rcx, rax
    call rf_alloc
    test rax, rax
    jz di_oom
    mov qword ptr [rsp+80], rax
    mov ecx, dword ptr [rsp+96]
    mov dword ptr [rax+IMG_WIDTH], ecx
    mov ecx, dword ptr [rsp+100]
    mov dword ptr [rax+IMG_HEIGHT], ecx
    mov ecx, dword ptr [rsp+96]
    imul ecx, 3
    mov dword ptr [rax+IMG_STRIDE], ecx
    mov dword ptr [rax+IMG_FLAGS], 0
    mov rcx, WKS_SIZE
    call rf_alloc
    test rax, rax
    jz di_oom
    mov qword ptr [rsp+88], rax
    mov qword ptr [rsp+120], 0
    mov dword ptr [rsp+128], 0
    mov dword ptr [rsp+132], 0
di_by:
    mov eax, dword ptr [rsp+132]
    cmp eax, dword ptr [rsp+108]
    jae di_end
    mov dword ptr [rsp+136], 0
di_bx:
    mov eax, dword ptr [rsp+136]
    cmp eax, dword ptr [rsp+104]
    jae di_by_next
    mov eax, dword ptr [rsp+128]
    mov dword ptr g_err_block, eax
    mov rax, qword ptr [rsp+112]
    sub rax, qword ptr [rsp+120]
    cmp rax, 6
    jb di_btrunc
    mov r15, qword ptr [rsp+120]
    movzx r8d, byte ptr [rbx+r15+32]
    movzx ecx, byte ptr [rbx+r15+33]
    mov edx, dword ptr [rbx+r15+34]
    test ecx, ecx
    jnz di_bflags
    mov r10, rdx
    add r10, 6
    jc di_blen
    cmp r10, rax
    ja di_blen
    mov qword ptr [rsp+144], r10
    mov eax, dword ptr [rsp+132]
    shl eax, 6
    mov r11d, eax
    mov eax, dword ptr [rsp+100]
    sub eax, r11d
    cmp eax, RF_BLOCK
    jbe di_bh
    mov eax, RF_BLOCK
di_bh:
    mov dword ptr [rsp+184], eax
    mov eax, dword ptr [rsp+136]
    shl eax, 6
    mov r9d, eax
    mov eax, dword ptr [rsp+96]
    sub eax, r9d
    cmp eax, RF_BLOCK
    jbe di_bw
    mov eax, RF_BLOCK
di_bw:
    mov dword ptr [rsp+180], eax
    mov rax, qword ptr [rsp+80]
    add rax, IMG_PIXELS
    mov qword ptr [rsp+160], rax
    mov rax, qword ptr [rsp+80]
    mov eax, dword ptr [rax+IMG_STRIDE]
    mov dword ptr [rsp+168], eax
    mov dword ptr [rsp+172], r9d
    mov dword ptr [rsp+176], r11d
    mov dword ptr [rsp+188], r8d
    mov rax, qword ptr [rsp+88]
    mov qword ptr [rsp+192], rax
    lea rax, [rbx+r15+38]
    mov qword ptr [rsp+200], rax
    mov dword ptr [rsp+208], edx
    mov qword ptr [rsp+208], rdx
    mov eax, dword ptr [rsp+96]
    mov dword ptr [rsp+216], eax
    mov eax, dword ptr [rsp+100]
    mov dword ptr [rsp+220], eax
    lea rcx, [rsp+160]
    call rf_decode_block
    test eax, eax
    jnz di_fail
    mov rax, qword ptr [rsp+144]
    add qword ptr [rsp+120], rax
    inc dword ptr [rsp+128]
    inc dword ptr [rsp+136]
    jmp di_bx
di_by_next:
    inc dword ptr [rsp+132]
    jmp di_by
di_end:
    mov rax, qword ptr [rsp+120]
    cmp rax, qword ptr [rsp+112]
    jne di_trail
    mov dword ptr g_err_block, 0FFFFFFFFh
    mov rax, qword ptr [rsp+72]
    test rax, rax
    jz di_nostats
    mov ecx, dword ptr [rsp+104]
    mov dword ptr [rax+ST_BLOCKS_X], ecx
    mov ecx, dword ptr [rsp+108]
    mov dword ptr [rax+ST_BLOCKS_Y], ecx
    mov ecx, dword ptr [rsp+104]
    imul ecx, dword ptr [rsp+108]
    mov dword ptr [rax+ST_BLOCK_COUNT], ecx
    mov ecx, dword ptr g_mode_count
    mov dword ptr [rax+ST_RAW], ecx
    mov ecx, dword ptr [g_mode_count+4]
    mov dword ptr [rax+ST_PACKBITS], ecx
    mov ecx, dword ptr [g_mode_count+8]
    mov dword ptr [rax+ST_PREDICT], ecx
    mov ecx, dword ptr [g_mode_count+12]
    mov dword ptr [rax+ST_PALETTE], ecx
    mov ecx, dword ptr [g_mode_count+16]
    mov dword ptr [rax+ST_RICE], ecx
    mov ecx, dword ptr [g_mode_count+20]
    mov dword ptr [rax+ST_RANS], ecx
di_nostats:
    mov rcx, qword ptr [rsp+88]
    call rf_free
    mov rax, qword ptr [rsp+64]
    mov rcx, qword ptr [rsp+80]
    mov qword ptr [rax], rcx
    xor eax, eax
    jmp di_leave
di_magic:
    SET_ERR RF_ERR_FORMAT, msg_magic
    mov eax, RF_ERR_FORMAT
    jmp di_fail
di_ver:
    SET_ERR RF_ERR_UNSUPPORTED, msg_ver
    mov eax, RF_ERR_UNSUPPORTED
    jmp di_fail
di_hs:
    SET_ERR RF_ERR_UNSUPPORTED, msg_hs
    mov eax, RF_ERR_UNSUPPORTED
    jmp di_fail
di_bs:
    SET_ERR RF_ERR_UNSUPPORTED, msg_bs
    mov eax, RF_ERR_UNSUPPORTED
    jmp di_fail
di_pf:
    SET_ERR RF_ERR_UNSUPPORTED, msg_pf
    mov eax, RF_ERR_UNSUPPORTED
    jmp di_fail
di_hcrc:
    SET_ERR RF_ERR_FORMAT, msg_hcrc
    mov eax, RF_ERR_FORMAT
    jmp di_fail
di_dim:
    SET_ERR RF_ERR_FORMAT, msg_dim
    mov eax, RF_ERR_FORMAT
    jmp di_fail
di_limit:
    SET_ERR RF_ERR_LIMIT, msg_limit
    mov eax, RF_ERR_LIMIT
    jmp di_fail
di_plen:
    SET_ERR RF_ERR_FORMAT, msg_plen
    mov eax, RF_ERR_FORMAT
    jmp di_fail
di_pcrc:
    SET_ERR RF_ERR_FORMAT, msg_pcrc
    mov eax, RF_ERR_FORMAT
    jmp di_fail
di_trunc:
    SET_ERR RF_ERR_FORMAT, msg_btrunc
    mov eax, RF_ERR_FORMAT
    jmp di_fail
di_btrunc:
    SET_ERR RF_ERR_FORMAT, msg_btrunc
    mov eax, RF_ERR_FORMAT
    jmp di_fail
di_bflags:
    SET_ERR RF_ERR_FORMAT, msg_bflags
    mov eax, RF_ERR_FORMAT
    jmp di_fail
di_blen:
    SET_ERR RF_ERR_FORMAT, msg_blen
    mov eax, RF_ERR_FORMAT
    jmp di_fail
di_trail:
    SET_ERR RF_ERR_FORMAT, msg_trail
    mov eax, RF_ERR_FORMAT
    jmp di_fail
di_oom:
    SET_ERR RF_ERR_LIMIT, msg_oom
    mov eax, RF_ERR_LIMIT
di_fail:
    mov dword ptr [rsp+48], eax
    mov rcx, qword ptr [rsp+80]
    call rf_free
    mov rcx, qword ptr [rsp+88]
    call rf_free
    mov rax, qword ptr [rsp+64]
    mov qword ptr [rax], 0
    mov eax, dword ptr [rsp+48]
di_leave:
    RF_LEAVE 320
rf_decode_rfi ENDP

rf_predict_selftest PROC FRAME
    RF_ENTER 96
    lea rdi, [rsp+64]
    lea rsi, st_rgb
    mov ecx, 12
    cld
    rep movsb
    lea rcx, [rsp+64]
    mov edx, 2
    mov r8d, 2
    lea r9, [rsp+32]
    call rf_predict
    lea rsi, st_res
    lea rdi, [rsp+32]
    mov ecx, 12
    cld
    repe cmpsb
    jne pst_fail
    lea rcx, [rsp+32]
    mov edx, 2
    mov r8d, 2
    lea r9, [rsp+80]
    call rf_unpredict
    lea rsi, st_rgb
    lea rdi, [rsp+80]
    mov ecx, 12
    cld
    repe cmpsb
    jne pst_fail
    xor eax, eax
    jmp pst_leave
pst_fail:
    SET_ERR RF_ERR_LIMIT, msg_self
    mov eax, RF_ERR_LIMIT
pst_leave:
    RF_LEAVE 96
rf_predict_selftest ENDP

END

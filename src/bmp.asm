; Uncompressed 24-bit BMP subset. Pixels cross the boundary as packed top-down RGB.
include rf.inc

option casemap:none
option prologue:none
option epilogue:none

.const
bmp_sig db "invalid BMP: signature", 0
bmp_hdr db "unsupported BMP: header", 0
bmp_planes db "unsupported BMP: planes", 0
bmp_bits db "unsupported BMP: bit count", 0
bmp_comp db "unsupported BMP: compression", 0
bmp_dim db "invalid BMP: dimensions", 0
bmp_limit db "image exceeds size limit", 0
bmp_trunc db "invalid BMP: truncated", 0
bmp_fsize db "invalid BMP: file size", 0
bmp_isize db "invalid BMP: image size", 0
bmp_oom db "out of memory", 0
bmp_internal db "invalid BMP: internal stride", 0

.code

; rcx = file bytes, rdx = size, r8 = qword* image.
rf_bmp_load PROC FRAME
    RF_ENTER 64
    mov rbx, rcx
    mov r12, rdx
    mov rsi, r8
    mov qword ptr [rsi], 0
    cmp r12, 54
    jb bl_trunc
    movzx eax, word ptr [rbx]
    cmp eax, 4D42h
    jne bl_sig
    mov eax, dword ptr [rbx+14]
    cmp eax, 40
    jne bl_hdr
    mov eax, dword ptr [rbx+18]
    cmp eax, 1
    jl bl_dim
    cmp eax, RF_MAX_DIM
    ja bl_limit
    mov r13d, eax
    mov eax, dword ptr [rbx+22]
    cmp eax, 0
    je bl_dim
    cmp eax, 80000000h
    je bl_dim
    xor r14d, r14d
    test eax, eax
    jns bl_hpos
    mov r14d, 1
    neg eax
bl_hpos:
    cmp eax, 1
    jl bl_dim
    cmp eax, RF_MAX_DIM
    ja bl_limit
    mov r15d, eax
    movzx eax, word ptr [rbx+26]
    cmp eax, 1
    jne bl_planes
    movzx eax, word ptr [rbx+28]
    cmp eax, 24
    jne bl_bits
    mov eax, dword ptr [rbx+30]
    test eax, eax
    jnz bl_comp
    mov eax, r13d
    imul rax, r15
    cmp rax, RF_MAX_PIXELS
    ja bl_limit
    mov eax, dword ptr [rbx+10]
    cmp eax, 54
    jb bl_trunc
    mov r11d, eax
    mov eax, r13d
    imul eax, 3
    add eax, 3
    and eax, 0FFFFFFFCh
    mov ecx, eax
    imul rax, r15
    mov edx, r11d
    add rax, rdx
    jc bl_trunc
    cmp rax, r12
    ja bl_trunc
    mov r10, rax
    mov eax, dword ptr [rbx+2]
    test eax, eax
    jz bl_bf_ok
    cmp rax, r12
    ja bl_fsize
    cmp rax, r10
    jb bl_fsize
bl_bf_ok:
    mov eax, dword ptr [rbx+34]
    test eax, eax
    jz bl_is_ok
    mov ecx, r13d
    imul ecx, 3
    add ecx, 3
    and ecx, 0FFFFFFFCh
    imul rcx, r15
    cmp rax, rcx
    jb bl_isize
bl_is_ok:
    mov eax, r13d
    imul rax, r15
    imul rax, 3
    add rax, IMG_PIXELS
    jc bl_oom
    mov rcx, rax
    call rf_alloc
    test rax, rax
    jz bl_oom
    mov rdi, rax
    mov dword ptr [rdi+IMG_WIDTH], r13d
    mov dword ptr [rdi+IMG_HEIGHT], r15d
    mov eax, r13d
    imul eax, 3
    mov dword ptr [rdi+IMG_STRIDE], eax
    mov dword ptr [rdi+IMG_FLAGS], r14d
    xor r11d, r11d
bl_row:
    cmp r11d, r15d
    jae bl_ok
    mov eax, r11d
    cmp r14d, 0
    jne bl_y_top
    mov eax, r15d
    dec eax
    sub eax, r11d
bl_y_top:
    mov ecx, dword ptr [rdi+IMG_STRIDE]
    imul rax, rcx
    lea r8, [rdi+IMG_PIXELS]
    add r8, rax
    mov eax, r13d
    imul eax, 3
    add eax, 3
    and eax, 0FFFFFFFCh
    imul rax, r11
    mov ecx, dword ptr [rbx+10]
    add rax, rcx
    lea r9, [rbx+rax]
    xor ecx, ecx
bl_pix:
    cmp ecx, r13d
    jae bl_pix_done
    mov eax, ecx
    lea rax, [rax+rax*2]
    movzx edx, byte ptr [r9+rax+2]
    mov byte ptr [r8+rax], dl
    movzx edx, byte ptr [r9+rax+1]
    mov byte ptr [r8+rax+1], dl
    movzx edx, byte ptr [r9+rax]
    mov byte ptr [r8+rax+2], dl
    inc ecx
    jmp bl_pix
bl_pix_done:
    inc r11d
    jmp bl_row
bl_ok:
    mov qword ptr [rsi], rdi
    xor eax, eax
    jmp bl_leave
bl_sig:
    SET_ERR RF_ERR_FORMAT, bmp_sig
    mov eax, RF_ERR_FORMAT
    jmp bl_leave
bl_hdr:
    SET_ERR RF_ERR_UNSUPPORTED, bmp_hdr
    mov eax, RF_ERR_UNSUPPORTED
    jmp bl_leave
bl_planes:
    SET_ERR RF_ERR_UNSUPPORTED, bmp_planes
    mov eax, RF_ERR_UNSUPPORTED
    jmp bl_leave
bl_bits:
    SET_ERR RF_ERR_UNSUPPORTED, bmp_bits
    mov eax, RF_ERR_UNSUPPORTED
    jmp bl_leave
bl_comp:
    SET_ERR RF_ERR_UNSUPPORTED, bmp_comp
    mov eax, RF_ERR_UNSUPPORTED
    jmp bl_leave
bl_dim:
    SET_ERR RF_ERR_FORMAT, bmp_dim
    mov eax, RF_ERR_FORMAT
    jmp bl_leave
bl_limit:
    SET_ERR RF_ERR_LIMIT, bmp_limit
    mov eax, RF_ERR_LIMIT
    jmp bl_leave
bl_trunc:
    SET_ERR RF_ERR_FORMAT, bmp_trunc
    mov eax, RF_ERR_FORMAT
    jmp bl_leave
bl_fsize:
    SET_ERR RF_ERR_FORMAT, bmp_fsize
    mov eax, RF_ERR_FORMAT
    jmp bl_leave
bl_isize:
    SET_ERR RF_ERR_FORMAT, bmp_isize
    mov eax, RF_ERR_FORMAT
    jmp bl_leave
bl_oom:
    SET_ERR RF_ERR_LIMIT, bmp_oom
    mov eax, RF_ERR_LIMIT
bl_leave:
    RF_LEAVE 64
rf_bmp_load ENDP

; rcx = UTF-16 path, rdx = image.
rf_bmp_store PROC FRAME
    RF_ENTER 64
    mov rsi, rcx
    mov rbx, rdx
    mov eax, dword ptr [rbx+IMG_WIDTH]
    mov r12d, eax
    mov eax, dword ptr [rbx+IMG_HEIGHT]
    mov r13d, eax
    test r12d, r12d
    jz bs_int
    test r13d, r13d
    jz bs_int
    mov eax, r12d
    imul eax, 3
    cmp eax, dword ptr [rbx+IMG_STRIDE]
    jne bs_int
    mov dword ptr [rsp+48], eax
    add eax, 3
    and eax, 0FFFFFFFCh
    mov r15d, eax
    mov eax, r15d
    imul rax, r13
    jc bs_limit
    add rax, 54
    jc bs_limit
    cmp rax, RF_MAX_FILE
    ja bs_limit
    mov rdi, rax
    mov rcx, rax
    call rf_alloc
    test rax, rax
    jz bs_oom
    mov r14, rax
    mov rcx, rax
    xor edx, edx
    mov r8, rdi
    call rf_memset
    mov word ptr [r14], 4D42h
    mov eax, edi
    mov dword ptr [r14+2], eax
    mov dword ptr [r14+6], 0
    mov dword ptr [r14+10], 54
    mov dword ptr [r14+14], 40
    mov dword ptr [r14+18], r12d
    mov dword ptr [r14+22], r13d
    mov word ptr [r14+26], 1
    mov word ptr [r14+28], 24
    mov dword ptr [r14+30], 0
    mov rax, rdi
    sub rax, 54
    mov dword ptr [r14+34], eax
    mov dword ptr [r14+38], 3780
    mov dword ptr [r14+42], 3780
    mov dword ptr [r14+46], 0
    mov dword ptr [r14+50], 0
    xor r11d, r11d
bs_row:
    cmp r11d, r13d
    jae bs_write
    mov eax, r13d
    dec eax
    sub eax, r11d
    mov ecx, dword ptr [rsp+48]
    imul rax, rcx
    lea r8, [rbx+IMG_PIXELS]
    add r8, rax
    mov eax, r15d
    imul rax, r11
    lea r9, [r14+54]
    add r9, rax
    xor ecx, ecx
bs_pix:
    cmp ecx, r12d
    jae bs_next_row
    mov eax, ecx
    lea rax, [rax+rax*2]
    movzx edx, byte ptr [r8+rax]
    mov byte ptr [r9+rax+2], dl
    movzx edx, byte ptr [r8+rax+1]
    mov byte ptr [r9+rax+1], dl
    movzx edx, byte ptr [r8+rax+2]
    mov byte ptr [r9+rax], dl
    inc ecx
    jmp bs_pix
bs_next_row:
    inc r11d
    jmp bs_row
bs_write:
    mov rcx, rsi
    mov rdx, r14
    mov r8, rdi
    call rf_write_atomic
    mov ebx, eax
    mov rcx, r14
    call rf_free
    mov eax, ebx
    jmp bs_leave
bs_int:
    SET_ERR RF_ERR_FORMAT, bmp_internal
    mov eax, RF_ERR_FORMAT
    jmp bs_leave
bs_limit:
    SET_ERR RF_ERR_LIMIT, bmp_limit
    mov eax, RF_ERR_LIMIT
    jmp bs_leave
bs_oom:
    SET_ERR RF_ERR_LIMIT, bmp_oom
    mov eax, RF_ERR_LIMIT
bs_leave:
    RF_LEAVE 64
rf_bmp_store ENDP

END

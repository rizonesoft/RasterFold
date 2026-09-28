; Apple PackBits. Control 0x80 is rejected. See spec/FOLD-v1.md.
include rf.inc

option casemap:none
option prologue:none
option epilogue:none

.const
msg_pb_fail db "internal self-test failed", 0
pb_src5 db 01h, 01h, 01h, 02h, 03h
pb_exp5 db 0FEh, 01h, 01h, 02h, 03h
pb_src_lit db 01h, 02h, 03h, 03h, 03h
pb_exp_lit db 01h, 01h, 02h, 0FEh, 03h

.code

; rcx = src, rdx = src_len, r8 = dst, r9 = dst_cap.
; rax = encoded size, or -1 if the destination is too small.
rf_packbits_encode PROC FRAME
    RF_ENTER 32
    mov rsi, rcx
    mov r12, rdx
    mov rdi, r8
    mov r13, r9
    xor r14, r14
    xor r15, r15
pbe_outer:
    cmp r14, r12
    jae pbe_done
    mov ebx, 1
pbe_run:
    cmp ebx, 128
    jae pbe_have_run
    mov rax, r14
    mov ecx, ebx
    add rax, rcx
    jc pbe_have_run
    cmp rax, r12
    jae pbe_have_run
    movzx edx, byte ptr [rsi+r14]
    movzx ecx, byte ptr [rsi+rax]
    cmp dl, cl
    jne pbe_have_run
    inc ebx
    jmp pbe_run
pbe_have_run:
    cmp ebx, 2
    jb pbe_literal
    mov rax, r15
    add rax, 2
    jc pbe_fail
    cmp rax, r13
    ja pbe_fail
    mov eax, 257
    sub eax, ebx
    mov byte ptr [rdi+r15], al
    mov al, byte ptr [rsi+r14]
    mov byte ptr [rdi+r15+1], al
    add r15, 2
    mov eax, ebx
    add r14, rax
    jmp pbe_outer
pbe_literal:
    mov r10, r14
    xor ebx, ebx
pbe_lit:
    cmp r14, r12
    jae pbe_lit_emit
    cmp ebx, 128
    jae pbe_lit_emit
    mov rax, r14
    add rax, 2
    jc pbe_lit_take
    cmp rax, r12
    jae pbe_lit_take
    movzx edx, byte ptr [rsi+r14]
    movzx ecx, byte ptr [rsi+r14+1]
    cmp dl, cl
    jne pbe_lit_take
    movzx ecx, byte ptr [rsi+r14+2]
    cmp dl, cl
    jne pbe_lit_take
    jmp pbe_lit_emit
pbe_lit_take:
    inc r14
    inc ebx
    jmp pbe_lit
pbe_lit_emit:
    test ebx, ebx
    jz pbe_fail
    mov rax, r15
    add rax, 1
    jc pbe_fail
    mov ecx, ebx
    add rax, rcx
    jc pbe_fail
    cmp rax, r13
    ja pbe_fail
    mov eax, ebx
    dec eax
    mov byte ptr [rdi+r15], al
    inc r15
    xor ecx, ecx
pbe_copy:
    cmp ecx, ebx
    jae pbe_copy_done
    mov rax, r10
    add rax, rcx
    mov dl, byte ptr [rsi+rax]
    mov rax, r15
    add rax, rcx
    mov byte ptr [rdi+rax], dl
    inc ecx
    jmp pbe_copy
pbe_copy_done:
    mov eax, ebx
    add r15, rax
    jmp pbe_outer
pbe_done:
    mov rax, r15
    jmp pbe_leave
pbe_fail:
    mov rax, -1
pbe_leave:
    RF_LEAVE 32
rf_packbits_encode ENDP

; rcx = src, rdx = src_len, r8 = dst, r9 = expected output length.
; eax = 0, or RF_ERR_FORMAT.
rf_packbits_decode PROC FRAME
    RF_ENTER 32
    mov rsi, rcx
    mov r12, rdx
    mov rdi, r8
    mov r13, r9
    xor r14, r14
    xor r15, r15
pbd_loop:
    cmp r14, r12
    jae pbd_check
    movzx eax, byte ptr [rsi+r14]
    inc r14
    cmp al, 80h
    je pbd_fail
    cmp al, 80h
    ja pbd_rep
    movzx ebx, al
    inc ebx
    mov rax, r14
    mov ecx, ebx
    add rax, rcx
    jc pbd_fail
    cmp rax, r12
    ja pbd_fail
    mov rax, r15
    mov ecx, ebx
    add rax, rcx
    jc pbd_fail
    cmp rax, r13
    ja pbd_fail
    xor ecx, ecx
pbd_copy:
    cmp ecx, ebx
    jae pbd_copy_done
    mov rax, r14
    add rax, rcx
    mov dl, byte ptr [rsi+rax]
    mov rax, r15
    add rax, rcx
    mov byte ptr [rdi+rax], dl
    inc ecx
    jmp pbd_copy
pbd_copy_done:
    mov eax, ebx
    add r14, rax
    add r15, rax
    jmp pbd_loop
pbd_rep:
    movzx eax, al
    mov ebx, 257
    sub ebx, eax
    cmp ebx, 2
    jb pbd_fail
    cmp ebx, 128
    ja pbd_fail
    cmp r14, r12
    jae pbd_fail
    mov dl, byte ptr [rsi+r14]
    inc r14
    mov rax, r15
    mov ecx, ebx
    add rax, rcx
    jc pbd_fail
    cmp rax, r13
    ja pbd_fail
    xor ecx, ecx
pbd_fill:
    cmp ecx, ebx
    jae pbd_fill_done
    mov rax, r15
    add rax, rcx
    mov byte ptr [rdi+rax], dl
    inc ecx
    jmp pbd_fill
pbd_fill_done:
    mov eax, ebx
    add r15, rax
    jmp pbd_loop
pbd_check:
    cmp r14, r12
    jne pbd_fail
    cmp r15, r13
    jne pbd_fail
    xor eax, eax
    jmp pbd_leave
pbd_fail:
    mov eax, RF_ERR_FORMAT
pbd_leave:
    RF_LEAVE 32
rf_packbits_decode ENDP

rf_packbits_selftest PROC FRAME
    RF_ENTER 832
    cld
    ; raw rsp+64, enc rsp+320, dec rsp+576, each 256 bytes.
    lea rdi, [rsp+64]
    lea rsi, pb_src5
    mov ecx, 5
    rep movsb
    lea rcx, [rsp+64]
    mov edx, 5
    lea r8, [rsp+320]
    mov r9d, 256
    call rf_packbits_encode
    cmp rax, 5
    jne pbs_fail
    lea rsi, pb_exp5
    lea rdi, [rsp+320]
    mov ecx, 5
    repe cmpsb
    jne pbs_fail
    lea rcx, [rsp+320]
    mov edx, 5
    lea r8, [rsp+576]
    mov r9d, 5
    call rf_packbits_decode
    test eax, eax
    jnz pbs_fail
    lea rsi, pb_src5
    lea rdi, [rsp+576]
    mov ecx, 5
    repe cmpsb
    jne pbs_fail

    lea rdi, [rsp+64]
    lea rsi, pb_src_lit
    mov ecx, 5
    rep movsb
    lea rcx, [rsp+64]
    mov edx, 5
    lea r8, [rsp+320]
    mov r9d, 256
    call rf_packbits_encode
    cmp rax, 5
    jne pbs_fail
    lea rsi, pb_exp_lit
    lea rdi, [rsp+320]
    mov ecx, 5
    repe cmpsb
    jne pbs_fail

    lea rdi, [rsp+64]
    xor eax, eax
    mov ecx, 128
pbs_zero:
    mov byte ptr [rdi+rax], 0
    inc eax
    cmp eax, 128
    jb pbs_zero
    lea rcx, [rsp+64]
    mov edx, 128
    lea r8, [rsp+320]
    mov r9d, 256
    call rf_packbits_encode
    cmp rax, 2
    jne pbs_fail
    cmp byte ptr [rsp+320], 81h
    jne pbs_fail
    cmp byte ptr [rsp+321], 0
    jne pbs_fail
    lea rcx, [rsp+320]
    mov edx, 2
    lea r8, [rsp+576]
    mov r9d, 128
    call rf_packbits_decode
    test eax, eax
    jnz pbs_fail

    mov byte ptr [rsp+64], 0ABh
    lea rcx, [rsp+64]
    mov edx, 1
    lea r8, [rsp+320]
    mov r9d, 256
    call rf_packbits_encode
    cmp rax, 2
    jne pbs_fail
    cmp byte ptr [rsp+320], 0
    jne pbs_fail
    cmp byte ptr [rsp+321], 0ABh
    jne pbs_fail

    mov byte ptr [rsp+320], 80h
    lea rcx, [rsp+320]
    mov edx, 1
    lea r8, [rsp+576]
    mov r9d, 1
    call rf_packbits_decode
    test eax, eax
    jz pbs_fail

    lea rdi, [rsp+64]
    xor eax, eax
pbs_pat:
    mov byte ptr [rdi+rax], al
    inc eax
    cmp eax, 200
    jb pbs_pat
    lea rcx, [rsp+64]
    mov edx, 200
    lea r8, [rsp+320]
    mov r9d, 256
    call rf_packbits_encode
    cmp rax, 0
    jl pbs_fail
    cmp rax, 256
    ja pbs_fail
    mov r12, rax
    lea rcx, [rsp+320]
    mov rdx, r12
    lea r8, [rsp+576]
    mov r9d, 200
    call rf_packbits_decode
    test eax, eax
    jnz pbs_fail
    mov ecx, 200
    lea rsi, [rsp+64]
    lea rdi, [rsp+576]
    repe cmpsb
    jne pbs_fail
    xor eax, eax
    jmp pbs_leave
pbs_fail:
    SET_ERR RF_ERR_LIMIT, msg_pb_fail
    mov eax, RF_ERR_LIMIT
pbs_leave:
    RF_LEAVE 832
rf_packbits_selftest ENDP

END

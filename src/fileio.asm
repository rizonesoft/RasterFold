; Whole-file reads and atomic creates. A failed write never becomes the destination.
include rf.inc

option casemap:none
option prologue:none
option epilogue:none

.const
msg_read db "cannot read input", 0
msg_empty db "invalid input: empty file", 0
msg_toobig db "input exceeds size limit", 0
msg_write db "cannot write output", 0
msg_temp db "temporary output already exists", 0
msg_long db "path too long", 0
w_partial dw '.', 'p', 'a', 'r', 't', 'i', 'a', 'l', 0

.code

; rcx = UTF-16 path, rdx = qword* buffer, r8 = qword* size.
rf_read_file PROC FRAME
    RF_ENTER 96
    mov rbx, rcx
    mov rsi, rdx
    mov rdi, r8
    mov qword ptr [rsi], 0
    mov qword ptr [rdi], 0
    mov r12, 0
    mov r14, 0
    mov rcx, rbx
    mov edx, GENERIC_READ
    mov r8d, FILE_SHARE_READ
    xor r9d, r9d
    mov dword ptr [rsp+32], OPEN_EXISTING
    mov dword ptr [rsp+40], FILE_ATTRIBUTE_NORMAL
    mov qword ptr [rsp+48], 0
    call CreateFileW
    cmp rax, -1
    je rd_io
    mov r12, rax
    mov rcx, r12
    lea rdx, [rsp+56]
    call GetFileSizeEx
    test eax, eax
    jz rd_io
    mov r13, qword ptr [rsp+56]
    cmp r13, 0
    jle rd_empty
    cmp r13, RF_MAX_FILE
    ja rd_limit
    mov rcx, r13
    call rf_alloc
    test rax, rax
    jz rd_oom
    mov r14, rax
    xor r15, r15
rd_loop:
    cmp r15, r13
    jae rd_done
    mov rcx, r12
    lea rdx, [r14+r15]
    mov r8, r13
    sub r8, r15
    lea r9, [rsp+72]
    mov qword ptr [rsp+32], 0
    mov dword ptr [rsp+72], 0
    call ReadFile
    test eax, eax
    jz rd_io
    mov eax, dword ptr [rsp+72]
    test eax, eax
    jz rd_io
    add r15, rax
    jmp rd_loop
rd_done:
    mov rcx, r12
    call CloseHandle
    mov qword ptr [rsi], r14
    mov qword ptr [rdi], r13
    xor eax, eax
    jmp rd_leave
rd_empty:
    SET_ERR RF_ERR_FORMAT, msg_empty
    mov eax, RF_ERR_FORMAT
    jmp rd_close
rd_limit:
    SET_ERR RF_ERR_LIMIT, msg_toobig
    mov eax, RF_ERR_LIMIT
    jmp rd_close
rd_oom:
    SET_ERR RF_ERR_LIMIT, msg_read
    mov dword ptr g_err_code, RF_ERR_LIMIT
    lea rax, msg_oom_alias
    mov qword ptr g_err_msg, rax
    mov eax, RF_ERR_LIMIT
    jmp rd_close
rd_io:
    SET_ERR RF_ERR_IO, msg_read
    mov eax, RF_ERR_IO
rd_close:
    mov ebx, eax
    cmp r12, 0
    je rd_free
    mov rcx, r12
    call CloseHandle
rd_free:
    mov rcx, r14
    call rf_free
    mov eax, ebx
rd_leave:
    RF_LEAVE 96
rf_read_file ENDP

; rcx = UTF-16 destination, rdx = bytes, r8 = size.
rf_write_atomic PROC FRAME
    RF_ENTER 80
    mov rsi, rcx
    mov r14, rdx
    mov r15, r8
    mov qword ptr [rsp+64], 0
    mov dword ptr [rsp+72], 0
    mov r12, 0
    mov rcx, rsi
    call rf_path_blocked
    test eax, eax
    jnz wr_leave_eax
    mov rcx, 2080
    call rf_alloc
    test rax, rax
    jz wr_oom
    mov qword ptr [rsp+64], rax
    mov rbx, rax
    xor ecx, ecx
wr_copy:
    cmp ecx, 1024
    jae wr_long
    mov dx, word ptr [rsi+rcx*2]
    test dx, dx
    jz wr_copied
    mov word ptr [rbx+rcx*2], dx
    inc ecx
    jmp wr_copy
wr_copied:
    test ecx, ecx
    jz wr_io
    lea r10, w_partial
    xor edx, edx
wr_app:
    mov ax, word ptr [r10+rdx*2]
    mov word ptr [rbx+rcx*2], ax
    test ax, ax
    jz wr_app_done
    inc ecx
    inc edx
    jmp wr_app
wr_app_done:
    mov rcx, rbx
    call GetFileAttributesW
    cmp eax, INVALID_FILE_ATTRIBUTES
    jne wr_temp_exists
    mov rcx, rbx
    mov edx, GENERIC_WRITE
    xor r8d, r8d
    xor r9d, r9d
    mov dword ptr [rsp+32], CREATE_NEW
    mov dword ptr [rsp+40], FILE_ATTRIBUTE_NORMAL
    mov qword ptr [rsp+48], 0
    call CreateFileW
    cmp rax, -1
    je wr_io
    mov r12, rax
    mov dword ptr [rsp+72], 1
    xor r13, r13
wr_loop:
    cmp r13, r15
    jae wr_flush
    mov rcx, r12
    lea rdx, [r14+r13]
    mov r8, r15
    sub r8, r13
    cmp r8, 10000000h
    jbe wr_len
    mov r8, 10000000h
wr_len:
    lea r9, [rsp+56]
    mov qword ptr [rsp+32], 0
    mov dword ptr [rsp+56], 0
    call WriteFile
    test eax, eax
    jz wr_io
    mov eax, dword ptr [rsp+56]
    test eax, eax
    jz wr_io
    add r13, rax
    jmp wr_loop
wr_flush:
    mov rcx, r12
    call FlushFileBuffers
    test eax, eax
    jz wr_io
    mov rcx, r12
    call CloseHandle
    mov r12, 0
    mov rcx, rbx
    mov rdx, rsi
    call MoveFileW
    test eax, eax
    jz wr_io
    mov dword ptr [rsp+72], 0
    xor eax, eax
    jmp wr_cleanup
wr_long:
    SET_ERR RF_ERR_IO, msg_long
    mov eax, RF_ERR_IO
    jmp wr_cleanup
wr_temp_exists:
    SET_ERR RF_ERR_IO, msg_temp
    mov eax, RF_ERR_IO
    jmp wr_cleanup
wr_oom:
    SET_ERR RF_ERR_LIMIT, msg_oom_alias
    mov eax, RF_ERR_LIMIT
    jmp wr_leave_eax
wr_io:
    SET_ERR RF_ERR_IO, msg_write
    mov eax, RF_ERR_IO
wr_cleanup:
    mov ebx, eax
    cmp r12, 0
    je wr_del
    mov rcx, r12
    call CloseHandle
    mov r12, 0
wr_del:
    cmp dword ptr [rsp+72], 0
    je wr_free
    mov rcx, qword ptr [rsp+64]
    call DeleteFileW
wr_free:
    mov rcx, qword ptr [rsp+64]
    call rf_free
    mov eax, ebx
    jmp wr_leave_eax
wr_leave_eax:
    RF_LEAVE 80
rf_write_atomic ENDP

.const
msg_oom_alias db "out of memory", 0

END

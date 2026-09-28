; Heap, console output, timing, and small string helpers.
include rf.inc

option casemap:none
option prologue:none
option epilogue:none

.data
g_heap dq 0
g_stdout dq 0
g_stderr dq 0
g_alloc_current dq 0
g_alloc_peak dq 0
g_qpc_freq dq 0
g_err_code dd 0
g_err_block dd 0FFFFFFFFh
g_err_msg dq 0

.const
msg_oom db "out of memory", 0
msg_crlf db 13, 10, 0
msg_exists db "output file already exists", 0
msg_access db "cannot access output path", 0

.code

rf_init PROC FRAME
    RF_ENTER 48
    mov dword ptr g_err_code, 0
    mov dword ptr g_err_block, 0FFFFFFFFh
    mov qword ptr g_err_msg, 0
    mov ecx, STD_OUTPUT_HANDLE
    call GetStdHandle
    mov g_stdout, rax
    mov ecx, STD_ERROR_HANDLE
    call GetStdHandle
    mov g_stderr, rax
    xor ecx, ecx
    xor edx, edx
    xor r8d, r8d
    call HeapCreate
    test rax, rax
    jz init_oom
    mov g_heap, rax
    lea rcx, g_qpc_freq
    call QueryPerformanceFrequency
    test eax, eax
    jz init_oom
    cmp qword ptr g_qpc_freq, 0
    je init_oom
    xor eax, eax
    jmp init_done
init_oom:
    SET_ERR RF_ERR_LIMIT, msg_oom
    mov eax, RF_ERR_LIMIT
init_done:
    RF_LEAVE 48
rf_init ENDP

; rcx = user size. Returns a 16-byte-aligned pointer, or 0.
; Eight unused bytes and an 8-byte size sit in front of the pointer.
rf_alloc PROC FRAME
    RF_ENTER 48
    mov r12, rcx
    test r12, r12
    jz alloc_fail
    mov rax, r12
    add rax, 16
    jc alloc_fail
    cmp rax, RF_MAX_FILE
    ja alloc_fail
    mov rcx, g_heap
    xor edx, edx
    mov r8, rax
    call HeapAlloc
    test rax, rax
    jz alloc_fail
    mov qword ptr [rax], r12
    add rax, 16
    add g_alloc_current, r12
    add g_alloc_current, 16
    mov rdx, g_alloc_current
    cmp rdx, g_alloc_peak
    jbe alloc_ok
    mov g_alloc_peak, rdx
alloc_ok:
    RF_LEAVE 48
alloc_fail:
    xor eax, eax
    RF_LEAVE 48
rf_alloc ENDP

; rcx = user pointer, or 0.
rf_free PROC FRAME
    RF_ENTER 48
    test rcx, rcx
    jz free_done
    mov r12, rcx
    mov rax, qword ptr [r12-16]
    sub g_alloc_current, rax
    sub g_alloc_current, 16
    lea rcx, [r12-16]
    mov rdx, g_heap
    ; HeapFree(heap, flags, ptr)
    mov r8, rcx
    mov rcx, rdx
    xor edx, edx
    call HeapFree
free_done:
    RF_LEAVE 48
rf_free ENDP

rf_memcpy PROC FRAME
    push rsi
    .pushreg rsi
    push rdi
    .pushreg rdi
    .endprolog
    cld
    mov rdi, rcx
    mov rsi, rdx
    mov rcx, r8
    rep movsb
    pop rdi
    pop rsi
    ret
rf_memcpy ENDP

rf_memset PROC FRAME
    push rdi
    .pushreg rdi
    .endprolog
    cld
    mov rdi, rcx
    mov al, dl
    mov rcx, r8
    rep stosb
    pop rdi
    ret
rf_memset ENDP

; rcx = handle, rdx = buffer, r8 = length
rf_write_bytes PROC FRAME
    RF_ENTER 80
    mov rbx, rcx
    mov rsi, rdx
    mov rdi, r8
write_loop:
    test rdi, rdi
    jz write_ok
    mov rcx, rbx
    mov rdx, rsi
    mov r8, rdi
    cmp r8, 10000000h
    jbe write_len_ok
    mov r8, 10000000h
write_len_ok:
    lea r9, [rsp+48]
    mov qword ptr [rsp+32], 0
    mov dword ptr [rsp+48], 0
    call WriteFile
    test eax, eax
    jz write_ok
    mov eax, dword ptr [rsp+48]
    test eax, eax
    jz write_ok
    add rsi, rax
    sub rdi, rax
    jmp write_loop
write_ok:
    RF_LEAVE 80
rf_write_bytes ENDP

; rcx = handle, rdx = NUL-terminated string
rf_write_cstr PROC FRAME
    RF_ENTER 48
    mov rbx, rcx
    mov rsi, rdx
    xor edi, edi
cstr_len:
    cmp byte ptr [rsi+rdi], 0
    je cstr_go
    inc edi
    jmp cstr_len
cstr_go:
    mov rcx, rbx
    mov rdx, rsi
    mov r8, rdi
    call rf_write_bytes
    RF_LEAVE 48
rf_write_cstr ENDP

; rcx = handle, rdx = unsigned value
rf_write_u64 PROC FRAME
    RF_ENTER 96
    mov rbx, rcx
    mov rax, rdx
    lea rsi, [rsp+64]
    add rsi, 24
    xor edi, edi
    test rax, rax
    jnz u64_loop
    mov byte ptr [rsi-1], '0'
    dec rsi
    mov edi, 1
    jmp u64_emit
u64_loop:
    xor edx, edx
    mov r10, 10
    div r10
    add dl, '0'
    dec rsi
    mov byte ptr [rsi], dl
    inc edi
    test rax, rax
    jnz u64_loop
u64_emit:
    mov rcx, rbx
    mov rdx, rsi
    mov r8, rdi
    call rf_write_bytes
    RF_LEAVE 96
rf_write_u64 ENDP

; rcx = handle, edx = value. Eight lowercase hex digits, no prefix.
rf_write_hex32 PROC FRAME
    RF_ENTER 64
    mov rbx, rcx
    mov r12d, edx
    lea rsi, [rsp+40]
    mov edi, 8
hex_loop:
    mov eax, r12d
    shr eax, 28
    and eax, 0Fh
    cmp al, 10
    jb hex_digit
    add al, 'a' - 10
    jmp hex_store
hex_digit:
    add al, '0'
hex_store:
    mov byte ptr [rsi], al
    inc rsi
    shl r12d, 4
    dec edi
    jnz hex_loop
    mov rcx, rbx
    lea rdx, [rsp+40]
    mov r8d, 8
    call rf_write_bytes
    RF_LEAVE 64
rf_write_hex32 ENDP

; rcx = handle, rdx = label, r8 = value
rf_write_line_u64 PROC FRAME
    RF_ENTER 64
    mov rbx, rcx
    mov rsi, r8
    mov r12, rdx
    mov rcx, rbx
    mov rdx, r12
    call rf_write_cstr
    mov rcx, rbx
    mov rdx, rsi
    call rf_write_u64
    mov rcx, rbx
    lea rdx, msg_crlf
    call rf_write_cstr
    RF_LEAVE 64
rf_write_line_u64 ENDP

; rcx = handle, rdx = label (includes any "0x"), r8d = value
rf_write_line_hex PROC FRAME
    RF_ENTER 64
    mov rbx, rcx
    mov esi, r8d
    mov r12, rdx
    mov rcx, rbx
    mov rdx, r12
    call rf_write_cstr
    mov rcx, rbx
    mov edx, esi
    call rf_write_hex32
    mov rcx, rbx
    lea rdx, msg_crlf
    call rf_write_cstr
    RF_LEAVE 64
rf_write_line_hex ENDP

rf_qpc PROC FRAME
    RF_ENTER 48
    lea rcx, [rsp+32]
    call QueryPerformanceCounter
    mov rax, qword ptr [rsp+32]
    RF_LEAVE 48
rf_qpc ENDP

; rcx = start ticks, rdx = end ticks. Returns microseconds in rax.
rf_elapsed_us PROC
    mov rax, rdx
    sub rax, rcx
    mov rcx, 1000000
    mul rcx
    div qword ptr g_qpc_freq
    ret
rf_elapsed_us ENDP

; Returns the process peak working set, or 0 if the query fails.
rf_peak_working_set PROC FRAME
    RF_ENTER 128
    lea rdx, [rsp+32]
    mov dword ptr [rdx], 72
    mov rcx, -1
    mov r8d, 72
    call K32GetProcessMemoryInfo
    test eax, eax
    jz peak_fail
    mov rax, qword ptr [rsp+40]
    jmp peak_done
peak_fail:
    xor eax, eax
peak_done:
    RF_LEAVE 128
rf_peak_working_set ENDP

; rcx = UTF-16 string, rdx = ASCII string. eax = 1 when equal.
rf_wstr_eq_ascii PROC
wcmp_loop:
    movzx eax, word ptr [rcx]
    movzx r8d, byte ptr [rdx]
    cmp eax, r8d
    jne wcmp_no
    test eax, eax
    jz wcmp_yes
    add rcx, 2
    inc rdx
    jmp wcmp_loop
wcmp_yes:
    mov eax, 1
    ret
wcmp_no:
    xor eax, eax
    ret
rf_wstr_eq_ascii ENDP

; rcx = UTF-16 path. eax = 0 when the path is free to create.
rf_path_blocked PROC FRAME
    RF_ENTER 48
    call GetFileAttributesW
    cmp eax, INVALID_FILE_ATTRIBUTES
    jne path_exists
    call GetLastError
    cmp eax, ERROR_FILE_NOT_FOUND
    je path_free
    cmp eax, ERROR_PATH_NOT_FOUND
    je path_free
    SET_ERR RF_ERR_IO, msg_access
    mov eax, RF_ERR_IO
    jmp path_done
path_exists:
    SET_ERR RF_ERR_IO, msg_exists
    mov eax, RF_ERR_IO
    jmp path_done
path_free:
    xor eax, eax
path_done:
    RF_LEAVE 48
rf_path_blocked ENDP

END

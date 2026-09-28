; zlib/PNG CRC-32. Table is built at startup so the polynomial is visible.
include rf.inc

option casemap:none
option prologue:none
option epilogue:none

.data
crc_table dd 256 dup(0)

.const
crc_vector db "123456789"
msg_crc_fail db "internal self-test failed", 0

.code

rf_crc32_init PROC
    xor r8d, r8d
    lea r9, crc_table
crc_i:
    mov eax, r8d
    mov ecx, 8
crc_bit:
    test eax, 1
    jz crc_shr
    shr eax, 1
    xor eax, 0EDB88320h
    jmp crc_next
crc_shr:
    shr eax, 1
crc_next:
    dec ecx
    jnz crc_bit
    mov dword ptr [r9+r8*4], eax
    inc r8d
    cmp r8d, 256
    jb crc_i
    ret
rf_crc32_init ENDP

; rcx = bytes, rdx = length. eax = CRC-32.
rf_crc32 PROC
    mov r8, rcx
    mov r9, rdx
    mov eax, 0FFFFFFFFh
    lea r10, crc_table
    test r9, r9
    jz crc_finish
crc_loop:
    movzx r11d, byte ptr [r8]
    xor r11d, eax
    and r11d, 0FFh
    shr eax, 8
    xor eax, dword ptr [r10+r11*4]
    inc r8
    dec r9
    jnz crc_loop
crc_finish:
    xor eax, 0FFFFFFFFh
    ret
rf_crc32 ENDP

rf_crc32_selftest PROC FRAME
    RF_ENTER 32
    lea rcx, crc_vector
    mov edx, 9
    call rf_crc32
    cmp eax, 0CBF43926h
    je crc_ok
    SET_ERR RF_ERR_LIMIT, msg_crc_fail
    mov eax, RF_ERR_LIMIT
    jmp crc_ret
crc_ok:
    xor eax, eax
crc_ret:
    RF_LEAVE 32
rf_crc32_selftest ENDP

END

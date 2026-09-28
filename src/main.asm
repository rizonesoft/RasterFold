; Command line: encode, decode, inspect.
include rf.inc

option casemap:none
option prologue:none
option epilogue:none

.const
usage_text db "RasterFold experimental lossless codec", 13, 10
           db "usage:", 13, 10
           db "  rasterfold encode [--time] input.bmp output.fold", 13, 10
           db "  rasterfold decode [--time] input.fold output.bmp", 13, 10
           db "  rasterfold inspect [--time] input.fold", 13, 10
           db "exit codes: 0 ok, 1 usage, 2 io, 3 unsupported, 4 malformed, 5 limit", 13, 10, 0
prefix db "rasterfold: ", 0
unknown_err db "unknown error", 0
block_pre db " (block ", 0
block_post db ")", 13, 10, 0
crlf db 13, 10, 0
s_time db "--time", 0
s_encode db "encode", 0
s_decode db "decode", 0
s_inspect db "inspect", 0
i_title db "RasterFold RFI version 1", 13, 10, 0
l_width db "width: ", 0
l_height db "height: ", 0
l_block db "block_size: ", 0
l_format db "pixel_format: ", 0
l_bx db "blocks_x: ", 0
l_by db "blocks_y: ", 0
l_count db "block_count: ", 0
l_rawbytes db "uncompressed_rgb_bytes: ", 0
l_payload db "payload_size: ", 0
l_file db "file_size: ", 0
l_pcrc db "payload_crc32: 0x", 0
l_hcrc db "header_crc32: 0x", 0
l_mraw db "mode_raw: ", 0
l_mpb db "mode_packbits: ", 0
l_mpr db "mode_predict: ", 0
l_mpal db "mode_palette: ", 0
l_mrice db "mode_rice: ", 0
l_mrans db "mode_rans: ", 0
l_ok db "status: ok", 13, 10, 0
st_read db "rf_stat read_us=", 0
st_parse db "rf_stat parse_us=", 0
st_codec db "rf_stat codec_us=", 0
st_write db "rf_stat write_us=", 0
st_total db "rf_stat total_us=", 0
st_alloc db "rf_stat alloc_peak=", 0
st_ws db "rf_stat peak_working_set=", 0
st_out db "rf_stat output_bytes=", 0
st_mraw db "rf_stat mode_raw=", 0
st_mpb db "rf_stat mode_packbits=", 0
st_mpr db "rf_stat mode_predict=", 0
st_mpal db "rf_stat mode_palette=", 0
st_mrice db "rf_stat mode_rice=", 0
st_mrans db "rf_stat mode_rans=", 0

.code

main PROC FRAME
    RF_ENTER 320
    cld
    xor ebx, ebx
    mov qword ptr [rsp+80], 0
    mov qword ptr [rsp+88], 0
    mov qword ptr [rsp+96], 0
    mov qword ptr [rsp+104], 0
    mov qword ptr [rsp+152], 0
    mov qword ptr [rsp+168], 0
    mov qword ptr [rsp+176], 0
    mov dword ptr [rsp+68], 0
    mov dword ptr [rsp+76], 0
    call rf_init
    test eax, eax
    jnz main_fail
    call rf_crc32_init
    call rf_crc32_selftest
    test eax, eax
    jnz main_fail
    call rf_packbits_selftest
    test eax, eax
    jnz main_fail
    call rf_predict_selftest
    test eax, eax
    jnz main_fail
    call rf_rice_selftest
    test eax, eax
    jnz main_fail
    call rf_rans_selftest
    test eax, eax
    jnz main_fail
    call GetCommandLineW
    mov rcx, rax
    lea rdx, [rsp+64]
    call CommandLineToArgvW
    test rax, rax
    jz main_usage
    mov qword ptr [rsp+80], rax
    mov eax, dword ptr [rsp+64]
    cmp eax, 2
    jb main_usage
    mov r12d, 1
    mov dword ptr [rsp+68], 0
parse_loop:
    cmp r12d, dword ptr [rsp+64]
    jae parse_done
    mov rax, qword ptr [rsp+80]
    mov rcx, qword ptr [rax+r12*8]
    lea rdx, s_time
    call rf_wstr_eq_ascii
    test eax, eax
    jnz parse_time
    mov eax, dword ptr [rsp+68]
    cmp eax, 3
    jae main_usage
    mov r11, qword ptr [rsp+80]
    mov rcx, qword ptr [r11+r12*8]
    cmp dword ptr [rsp+68], 0
    jne parse_not0
    mov qword ptr [rsp+88], rcx
    jmp parse_stored
parse_not0:
    cmp dword ptr [rsp+68], 1
    jne parse_not1
    mov qword ptr [rsp+96], rcx
    jmp parse_stored
parse_not1:
    mov qword ptr [rsp+104], rcx
parse_stored:
    inc dword ptr [rsp+68]
    jmp parse_next
parse_time:
    mov dword ptr [rsp+76], 1
parse_next:
    inc r12d
    jmp parse_loop
parse_done:
    cmp dword ptr [rsp+68], 1
    jb main_usage
    mov rcx, qword ptr [rsp+88]
    lea rdx, s_encode
    call rf_wstr_eq_ascii
    test eax, eax
    jnz cmd_encode
    mov rcx, qword ptr [rsp+88]
    lea rdx, s_decode
    call rf_wstr_eq_ascii
    test eax, eax
    jnz cmd_decode
    mov rcx, qword ptr [rsp+88]
    lea rdx, s_inspect
    call rf_wstr_eq_ascii
    test eax, eax
    jnz cmd_inspect
    jmp main_usage

cmd_encode:
    cmp dword ptr [rsp+68], 3
    jne main_usage
    mov rcx, qword ptr [rsp+104]
    call rf_path_blocked
    test eax, eax
    jnz main_fail
    call rf_qpc
    mov qword ptr [rsp+112], rax
    mov rcx, qword ptr [rsp+96]
    lea rdx, [rsp+152]
    lea r8, [rsp+160]
    call rf_read_file
    test eax, eax
    jnz main_fail
    call rf_qpc
    mov qword ptr [rsp+120], rax
    mov rcx, qword ptr [rsp+152]
    mov rdx, qword ptr [rsp+160]
    lea r8, [rsp+168]
    call rf_bmp_load
    test eax, eax
    jnz main_fail
    call rf_qpc
    mov qword ptr [rsp+128], rax
    mov rcx, qword ptr [rsp+168]
    lea rdx, [rsp+176]
    lea r8, [rsp+184]
    call rf_encode_image
    test eax, eax
    jnz main_fail
    call rf_qpc
    mov qword ptr [rsp+136], rax
    mov rcx, qword ptr [rsp+104]
    mov rdx, qword ptr [rsp+176]
    mov r8, qword ptr [rsp+184]
    call rf_write_atomic
    test eax, eax
    jnz main_fail
    call rf_qpc
    mov qword ptr [rsp+144], rax
    mov r13, qword ptr [rsp+184]
    jmp main_ok

cmd_decode:
    cmp dword ptr [rsp+68], 3
    jne main_usage
    mov rcx, qword ptr [rsp+104]
    call rf_path_blocked
    test eax, eax
    jnz main_fail
    call rf_qpc
    mov qword ptr [rsp+112], rax
    mov rcx, qword ptr [rsp+96]
    lea rdx, [rsp+152]
    lea r8, [rsp+160]
    call rf_read_file
    test eax, eax
    jnz main_fail
    call rf_qpc
    mov qword ptr [rsp+120], rax
    mov qword ptr [rsp+128], rax
    mov rcx, qword ptr [rsp+152]
    mov rdx, qword ptr [rsp+160]
    lea r8, [rsp+168]
    xor r9d, r9d
    call rf_decode_rfi
    test eax, eax
    jnz main_fail
    call rf_qpc
    mov qword ptr [rsp+136], rax
    mov rcx, qword ptr [rsp+104]
    mov rdx, qword ptr [rsp+168]
    call rf_bmp_store
    test eax, eax
    jnz main_fail
    call rf_qpc
    mov qword ptr [rsp+144], rax
    mov rcx, qword ptr [rsp+104]
    call rf_file_size
    mov r13, rax
    jmp main_ok

cmd_inspect:
    cmp dword ptr [rsp+68], 2
    jne main_usage
    call rf_qpc
    mov qword ptr [rsp+112], rax
    mov rcx, qword ptr [rsp+96]
    lea rdx, [rsp+152]
    lea r8, [rsp+160]
    call rf_read_file
    test eax, eax
    jnz main_fail
    call rf_qpc
    mov qword ptr [rsp+120], rax
    mov qword ptr [rsp+128], rax
    mov rcx, qword ptr [rsp+152]
    mov rdx, qword ptr [rsp+160]
    lea r8, [rsp+168]
    lea r9, [rsp+200]
    call rf_decode_rfi
    test eax, eax
    jnz main_fail
    call rf_qpc
    mov qword ptr [rsp+136], rax
    mov qword ptr [rsp+144], rax
    mov r13, 0
    mov rcx, g_stdout
    lea rdx, i_title
    call rf_write_cstr
    mov rcx, g_stdout
    lea rdx, l_width
    mov r8d, dword ptr [rsp+152]
    mov r10, qword ptr [rsp+152]
    mov r8d, dword ptr [r10+8]
    call rf_write_line_u64
    mov rcx, g_stdout
    lea rdx, l_height
    mov r10, qword ptr [rsp+152]
    mov r8d, dword ptr [r10+12]
    call rf_write_line_u64
    mov rcx, g_stdout
    lea rdx, l_block
    mov r10, qword ptr [rsp+152]
    movzx r8d, word ptr [r10+16]
    call rf_write_line_u64
    mov rcx, g_stdout
    lea rdx, l_format
    mov r10, qword ptr [rsp+152]
    movzx r8d, word ptr [r10+18]
    call rf_write_line_u64
    mov rcx, g_stdout
    lea rdx, l_bx
    mov r8d, dword ptr [rsp+200]
    call rf_write_line_u64
    mov rcx, g_stdout
    lea rdx, l_by
    mov r8d, dword ptr [rsp+204]
    call rf_write_line_u64
    mov rcx, g_stdout
    lea rdx, l_count
    mov r8d, dword ptr [rsp+208]
    call rf_write_line_u64
    mov r10, qword ptr [rsp+152]
    mov eax, dword ptr [r10+8]
    mov ecx, dword ptr [r10+12]
    imul rax, rcx
    imul rax, 3
    mov rcx, g_stdout
    lea rdx, l_rawbytes
    mov r8, rax
    call rf_write_line_u64
    mov rcx, g_stdout
    lea rdx, l_payload
    mov r10, qword ptr [rsp+152]
    mov r8d, dword ptr [r10+20]
    call rf_write_line_u64
    mov rcx, g_stdout
    lea rdx, l_file
    mov r8, qword ptr [rsp+160]
    call rf_write_line_u64
    mov rcx, g_stdout
    lea rdx, l_pcrc
    mov r10, qword ptr [rsp+152]
    mov r8d, dword ptr [r10+24]
    call rf_write_line_hex
    mov rcx, g_stdout
    lea rdx, l_hcrc
    mov r10, qword ptr [rsp+152]
    mov r8d, dword ptr [r10+28]
    call rf_write_line_hex
    mov rcx, g_stdout
    lea rdx, l_mraw
    mov r8d, dword ptr [rsp+212]
    call rf_write_line_u64
    mov rcx, g_stdout
    lea rdx, l_mpb
    mov r8d, dword ptr [rsp+216]
    call rf_write_line_u64
    mov rcx, g_stdout
    lea rdx, l_mpr
    mov r8d, dword ptr [rsp+220]
    call rf_write_line_u64
    mov rcx, g_stdout
    lea rdx, l_mpal
    mov r8d, dword ptr [rsp+224]
    call rf_write_line_u64
    mov rcx, g_stdout
    lea rdx, l_mrice
    mov r8d, dword ptr [rsp+228]
    call rf_write_line_u64
    mov rcx, g_stdout
    lea rdx, l_mrans
    mov r8d, dword ptr [rsp+232]
    call rf_write_line_u64
    mov rcx, g_stdout
    lea rdx, l_ok
    call rf_write_cstr
    jmp main_ok

main_ok:
    cmp dword ptr [rsp+76], 0
    je main_success
    mov rcx, qword ptr [rsp+112]
    mov rdx, qword ptr [rsp+120]
    call rf_elapsed_us
    mov rsi, rax
    mov rcx, qword ptr [rsp+120]
    mov rdx, qword ptr [rsp+128]
    call rf_elapsed_us
    mov rdi, rax
    mov rcx, qword ptr [rsp+128]
    mov rdx, qword ptr [rsp+136]
    call rf_elapsed_us
    mov r12, rax
    mov rcx, qword ptr [rsp+136]
    mov rdx, qword ptr [rsp+144]
    call rf_elapsed_us
    mov r14, rax
    mov rcx, qword ptr [rsp+112]
    mov rdx, qword ptr [rsp+144]
    call rf_elapsed_us
    mov r15, rax
    mov rcx, g_stdout
    lea rdx, st_read
    mov r8, rsi
    call rf_write_line_u64
    mov rcx, g_stdout
    lea rdx, st_parse
    mov r8, rdi
    call rf_write_line_u64
    mov rcx, g_stdout
    lea rdx, st_codec
    mov r8, r12
    call rf_write_line_u64
    mov rcx, g_stdout
    lea rdx, st_write
    mov r8, r14
    call rf_write_line_u64
    mov rcx, g_stdout
    lea rdx, st_total
    mov r8, r15
    call rf_write_line_u64
    mov rcx, g_stdout
    lea rdx, st_alloc
    mov r8, g_alloc_peak
    call rf_write_line_u64
    call rf_peak_working_set
    mov rcx, g_stdout
    lea rdx, st_ws
    mov r8, rax
    call rf_write_line_u64
    mov rcx, g_stdout
    lea rdx, st_out
    mov r8, r13
    call rf_write_line_u64
    mov rcx, g_stdout
    lea rdx, st_mraw
    mov r8d, dword ptr g_mode_count
    call rf_write_line_u64
    mov rcx, g_stdout
    lea rdx, st_mpb
    mov r8d, dword ptr [g_mode_count+4]
    call rf_write_line_u64
    mov rcx, g_stdout
    lea rdx, st_mpr
    mov r8d, dword ptr [g_mode_count+8]
    call rf_write_line_u64
    mov rcx, g_stdout
    lea rdx, st_mpal
    mov r8d, dword ptr [g_mode_count+12]
    call rf_write_line_u64
    mov rcx, g_stdout
    lea rdx, st_mrice
    mov r8d, dword ptr [g_mode_count+16]
    call rf_write_line_u64
    mov rcx, g_stdout
    lea rdx, st_mrans
    mov r8d, dword ptr [g_mode_count+20]
    call rf_write_line_u64
main_success:
    xor ebx, ebx
    jmp main_cleanup

main_usage:
    mov rcx, g_stderr
    lea rdx, usage_text
    call rf_write_cstr
    mov ebx, RF_ERR_USAGE
    jmp main_cleanup

main_fail:
    mov ebx, dword ptr g_err_code
    test ebx, ebx
    jnz main_have_err
    mov ebx, RF_ERR_FORMAT
main_have_err:
    mov rcx, g_stderr
    lea rdx, prefix
    call rf_write_cstr
    mov rcx, qword ptr g_err_msg
    test rcx, rcx
    jnz main_have_msg
    lea rcx, unknown_err
main_have_msg:
    mov rdx, g_stderr
    xchg rcx, rdx
    call rf_write_cstr
    mov eax, dword ptr g_err_block
    cmp eax, 0FFFFFFFFh
    je main_err_nl
    mov rcx, g_stderr
    lea rdx, block_pre
    call rf_write_cstr
    mov rcx, g_stderr
    mov edx, dword ptr g_err_block
    call rf_write_u64
    mov rcx, g_stderr
    lea rdx, block_post
    call rf_write_cstr
    jmp main_cleanup
main_err_nl:
    mov rcx, g_stderr
    lea rdx, crlf
    call rf_write_cstr

main_cleanup:
    mov rcx, qword ptr [rsp+176]
    call rf_free
    mov rcx, qword ptr [rsp+168]
    call rf_free
    mov rcx, qword ptr [rsp+152]
    call rf_free
    mov rcx, qword ptr [rsp+80]
    call LocalFree
    mov ecx, ebx
    call ExitProcess
    RF_LEAVE 320
main ENDP

; rcx = UTF-16 path. rax = file size, or 0.
rf_file_size PROC FRAME
    RF_ENTER 80
    mov edx, GENERIC_READ
    mov r8d, FILE_SHARE_READ
    xor r9d, r9d
    mov dword ptr [rsp+32], OPEN_EXISTING
    mov dword ptr [rsp+40], FILE_ATTRIBUTE_NORMAL
    mov qword ptr [rsp+48], 0
    call CreateFileW
    cmp rax, -1
    je fs_zero
    mov rbx, rax
    mov rcx, rax
    lea rdx, [rsp+56]
    call GetFileSizeEx
    test eax, eax
    jz fs_close_zero
    mov rsi, qword ptr [rsp+56]
    mov rcx, rbx
    call CloseHandle
    mov rax, rsi
    jmp fs_leave
fs_close_zero:
    mov rcx, rbx
    call CloseHandle
fs_zero:
    xor eax, eax
fs_leave:
    RF_LEAVE 80
rf_file_size ENDP

END

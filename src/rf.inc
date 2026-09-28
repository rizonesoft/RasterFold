; RasterFold shared constants, unwind macros, and symbols.
; Microsoft x64 ABI. Every non-leaf PROC FRAME uses RF_ENTER / RF_LEAVE.

RF_OK            equ 0
RF_ERR_USAGE     equ 1
RF_ERR_IO        equ 2
RF_ERR_UNSUPPORTED equ 3
RF_ERR_FORMAT    equ 4
RF_ERR_LIMIT     equ 5

RF_MAX_DIM       equ 32768
RF_MAX_PIXELS    equ 33554432
RF_MAX_FILE      equ 268435456
RF_BLOCK         equ 64
RF_HEADER_SIZE   equ 32
RF_MAGIC         equ 01494652h
RF_FORMAT_RGB8   equ 1

RF_BLOCK_PIX     equ 4096
RF_BLOCK_BYTES   equ 12288
RF_TRIAL_CAP     equ 16384

; One workspace: raw | residual | indexes | palette | trial | best
WKS_RAW          equ 0
WKS_RES          equ 12288
WKS_IDX          equ 24576
WKS_PAL          equ 28672
WKS_TRIAL        equ 29440
WKS_BEST         equ 45824
WKS_SIZE         equ 62208

MODE_RAW         equ 0
MODE_PACKBITS    equ 1
MODE_PREDICT     equ 2
MODE_PALETTE     equ 3
MODE_RICE        equ 4

IMG_WIDTH        equ 0
IMG_HEIGHT       equ 4
IMG_STRIDE       equ 8
IMG_FLAGS        equ 12
IMG_PIXELS       equ 16

JOB_PIX          equ 0
JOB_STRIDE       equ 8
JOB_X0           equ 12
JOB_Y0           equ 16
JOB_BW           equ 20
JOB_BH           equ 24
JOB_WKS          equ 32
JOB_OUT          equ 40
JOB_OUTEND       equ 48
JOB_WIDTH        equ 56
JOB_HEIGHT       equ 60

DEC_PIX          equ 0
DEC_STRIDE       equ 8
DEC_X0           equ 12
DEC_Y0           equ 16
DEC_BW           equ 20
DEC_BH           equ 24
DEC_MODE         equ 28
DEC_WKS          equ 32
DEC_DATA         equ 40
DEC_SIZE         equ 48
DEC_WIDTH        equ 56
DEC_HEIGHT       equ 60

ST_BLOCKS_X      equ 0
ST_BLOCKS_Y      equ 4
ST_BLOCK_COUNT   equ 8
ST_RAW           equ 12
ST_PACKBITS      equ 16
ST_PREDICT       equ 20
ST_PALETTE       equ 24
ST_RICE          equ 28

GENERIC_READ     equ 80000000h
GENERIC_WRITE    equ 40000000h
FILE_SHARE_READ  equ 1
OPEN_EXISTING    equ 3
CREATE_NEW       equ 1
FILE_ATTRIBUTE_NORMAL equ 80h
INVALID_HANDLE_VALUE equ -1
INVALID_FILE_ATTRIBUTES equ -1
STD_OUTPUT_HANDLE equ -11
STD_ERROR_HANDLE equ -12
ERROR_FILE_NOT_FOUND equ 2
ERROR_PATH_NOT_FOUND equ 3

; Entry RSP is 8 (mod 16). Seven pushes leave RSP 0 (mod 16).
; stackbytes must be a multiple of 16 and at least 32 when the body calls out.
RF_ENTER MACRO stackbytes
    push rbx
    .pushreg rbx
    push rsi
    .pushreg rsi
    push rdi
    .pushreg rdi
    push r12
    .pushreg r12
    push r13
    .pushreg r13
    push r14
    .pushreg r14
    push r15
    .pushreg r15
    sub rsp, stackbytes
    .allocstack stackbytes
    .endprolog
ENDM

RF_LEAVE MACRO stackbytes
    add rsp, stackbytes
    pop r15
    pop r14
    pop r13
    pop r12
    pop rdi
    pop rsi
    pop rbx
    ret
ENDM

SET_ERR MACRO code, msglabel
    mov dword ptr g_err_code, code
    lea rax, msglabel
    mov qword ptr g_err_msg, rax
ENDM

EXTERNDEF g_heap:QWORD
EXTERNDEF g_stdout:QWORD
EXTERNDEF g_stderr:QWORD
EXTERNDEF g_alloc_current:QWORD
EXTERNDEF g_alloc_peak:QWORD
EXTERNDEF g_qpc_freq:QWORD
EXTERNDEF g_err_code:DWORD
EXTERNDEF g_err_block:DWORD
EXTERNDEF g_err_msg:QWORD
EXTERNDEF g_mode_count:DWORD

EXTERNDEF rf_init:PROC
EXTERNDEF rf_alloc:PROC
EXTERNDEF rf_free:PROC
EXTERNDEF rf_memcpy:PROC
EXTERNDEF rf_memset:PROC
EXTERNDEF rf_write_bytes:PROC
EXTERNDEF rf_write_cstr:PROC
EXTERNDEF rf_write_u64:PROC
EXTERNDEF rf_write_hex32:PROC
EXTERNDEF rf_write_line_u64:PROC
EXTERNDEF rf_write_line_hex:PROC
EXTERNDEF rf_qpc:PROC
EXTERNDEF rf_elapsed_us:PROC
EXTERNDEF rf_peak_working_set:PROC
EXTERNDEF rf_wstr_eq_ascii:PROC
EXTERNDEF rf_path_blocked:PROC

EXTERNDEF rf_crc32_init:PROC
EXTERNDEF rf_crc32:PROC
EXTERNDEF rf_crc32_selftest:PROC

EXTERNDEF rf_packbits_encode:PROC
EXTERNDEF rf_packbits_decode:PROC
EXTERNDEF rf_packbits_selftest:PROC

EXTERNDEF rf_read_file:PROC
EXTERNDEF rf_write_atomic:PROC

EXTERNDEF rf_bmp_load:PROC
EXTERNDEF rf_bmp_store:PROC

EXTERNDEF rf_encode_image:PROC
EXTERNDEF rf_decode_rfi:PROC
EXTERNDEF rf_predict_selftest:PROC
EXTERNDEF rf_rice_reset:PROC
EXTERNDEF rf_rice_save:PROC
EXTERNDEF rf_rice_restore:PROC
EXTERNDEF rf_rice_encode:PROC
EXTERNDEF rf_rice_decode:PROC
EXTERNDEF rf_rice_selftest:PROC

EXTERNDEF ExitProcess:PROC
EXTERNDEF GetStdHandle:PROC
EXTERNDEF WriteFile:PROC
EXTERNDEF ReadFile:PROC
EXTERNDEF CreateFileW:PROC
EXTERNDEF CloseHandle:PROC
EXTERNDEF GetFileSizeEx:PROC
EXTERNDEF FlushFileBuffers:PROC
EXTERNDEF DeleteFileW:PROC
EXTERNDEF MoveFileW:PROC
EXTERNDEF GetFileAttributesW:PROC
EXTERNDEF GetLastError:PROC
EXTERNDEF HeapCreate:PROC
EXTERNDEF HeapAlloc:PROC
EXTERNDEF HeapFree:PROC
EXTERNDEF GetCommandLineW:PROC
EXTERNDEF CommandLineToArgvW:PROC
EXTERNDEF LocalFree:PROC
EXTERNDEF QueryPerformanceCounter:PROC
EXTERNDEF QueryPerformanceFrequency:PROC
EXTERNDEF K32GetProcessMemoryInfo:PROC

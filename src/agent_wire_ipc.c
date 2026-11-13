/*
 * agent_wire_ipc - native 1:1 agent IPC over a Win32 named pipe (Ultra-Brain-Labs v1.1.0)
 *
 * One C11 file. No C runtime, no SDK header, no DLL other than KERNEL32.dll.
 * Two local agents exchange fixed 64-byte packets through the message-mode pipe
 * \\.\pipe\ultra_brain_agent_wire. The pipe layer uses exactly six KERNEL32
 * functions: CreateNamedPipeA, ConnectNamedPipe, CreateFileA, ReadFile,
 * WriteFile, CloseHandle. The rest of the import table is process scaffolding
 * (console output, command line, timing, exit) and is listed below.
 *
 * Build (LLVM clang for Windows, lld):
 *   clang -std=c11 -O2 -Wall -Wextra -pedantic -Werror -ffreestanding -nostdlib -fuse-ld=lld
 *         -Wl,/subsystem:console src/agent_wire_ipc.c -lkernel32 -o agent_wire_ipc.exe
 * Build (MSVC):
 *   cl /nologo /std:c11 /O2 /W4 /WX /utf-8 /GS- src\agent_wire_ipc.c
 *      /link /SUBSYSTEM:CONSOLE /NODEFAULTLIB kernel32.lib /OUT:agent_wire_ipc.exe
 *
 * Usage:
 *   agent_wire_ipc --server [sessions]   create the pipe, serve clients (default: forever)
 *   agent_wire_ipc --client [text]       send PING (or TASK text <= 40 bytes), print the receipt and RTT
 *   agent_wire_ipc --bench [count]       count PING round trips (default 1000), min/p50/avg/p99/max
 *   agent_wire_ipc --selftest            packet layout, FNV-1a vectors, validation, receipt (exit 0 = pass)
 *   agent_wire_ipc --version             prints 1.1.0
 *
 * Exit codes: 0 ok, 1 pipe/system error, 2 usage, 3 receipt rejected (bad status or checksum).
 */

#include <stddef.h>
#include <stdint.h>

#define AW_VERSION "1.1.0"
#define AW_PIPE_NAME "\\\\.\\pipe\\ultra_brain_agent_wire"
#define AW_PIPE_BUFFER 4096u
#define AW_PACKET_BYTES 64u
#define AW_PAYLOAD_BYTES 40u
#define AW_MAGIC 0x57495245u /* "WIRE" */
#define AW_WIRE_VERSION 0x0100u
#define AW_DEFAULT_PROJECT 0xB1605844u /* FNV-1a 32 of "ultra_brain_agent_wire" */
#define AW_SERVER_ID 1u
#define AW_CLIENT_ID 2u
#define AW_BENCH_DEFAULT 1000u
#define AW_BENCH_MAX 16384u
#define AW_CONNECT_RETRIES 3000u /* x Sleep(1) = about 3 s */

/* ---- The only external surface: KERNEL32.dll ------------------------------ */

typedef void *aw_handle;
typedef uint32_t aw_dword;
typedef int aw_bool;

typedef struct { /* PROCESS_MEMORY_COUNTERS */
    aw_dword cb;
    aw_dword page_fault_count;
    size_t peak_working_set;
    size_t working_set;
    size_t quota_peak_paged_pool;
    size_t quota_paged_pool;
    size_t quota_peak_non_paged_pool;
    size_t quota_non_paged_pool;
    size_t pagefile_usage;
    size_t peak_pagefile_usage;
} aw_memory_counters;

#define AW_IMPORT __declspec(dllimport)
#define AW_API __stdcall

/* pipe layer: the six functions */
AW_IMPORT aw_handle AW_API CreateNamedPipeA(const char *name, aw_dword open_mode, aw_dword pipe_mode,
                                            aw_dword max_instances, aw_dword out_buffer, aw_dword in_buffer,
                                            aw_dword default_timeout, void *security);
AW_IMPORT aw_bool AW_API ConnectNamedPipe(aw_handle pipe, void *overlapped);
AW_IMPORT aw_handle AW_API CreateFileA(const char *name, aw_dword access, aw_dword share, void *security,
                                       aw_dword disposition, aw_dword flags, aw_handle template_file);
AW_IMPORT aw_bool AW_API ReadFile(aw_handle file, void *data, aw_dword size, aw_dword *read, void *overlapped);
AW_IMPORT aw_bool AW_API WriteFile(aw_handle file, const void *data, aw_dword size, aw_dword *written,
                                   void *overlapped);
AW_IMPORT aw_bool AW_API CloseHandle(aw_handle object);
/* process scaffolding */
AW_IMPORT aw_handle AW_API GetStdHandle(aw_dword which);
AW_IMPORT const uint16_t *AW_API GetCommandLineW(void);
AW_IMPORT aw_dword AW_API GetLastError(void);
AW_IMPORT aw_bool AW_API QueryPerformanceCounter(int64_t *count);
AW_IMPORT aw_bool AW_API QueryPerformanceFrequency(int64_t *frequency);
AW_IMPORT void AW_API Sleep(aw_dword milliseconds);
AW_IMPORT aw_bool AW_API SetConsoleOutputCP(unsigned int code_page);
AW_IMPORT aw_handle AW_API GetCurrentProcess(void);
AW_IMPORT aw_bool AW_API K32GetProcessMemoryInfo(aw_handle process, aw_memory_counters *counters, aw_dword size);
AW_IMPORT __declspec(noreturn) void AW_API ExitProcess(unsigned int code);

#define AW_INVALID_HANDLE ((aw_handle)(intptr_t)-1)
#define AW_STD_OUTPUT ((aw_dword)-11)
#define AW_CP_UTF8 65001u
#define AW_PIPE_ACCESS_DUPLEX 0x00000003u
#define AW_PIPE_TYPE_MESSAGE 0x00000004u
#define AW_PIPE_READMODE_MESSAGE 0x00000002u
#define AW_PIPE_REJECT_REMOTE_CLIENTS 0x00000008u
#define AW_GENERIC_READ 0x80000000u
#define AW_GENERIC_WRITE 0x40000000u
#define AW_OPEN_EXISTING 3u
#define AW_ERROR_FILE_NOT_FOUND 2u
#define AW_ERROR_BROKEN_PIPE 109u
#define AW_ERROR_PIPE_BUSY 231u
#define AW_ERROR_NO_DATA 232u
#define AW_ERROR_MORE_DATA 234u
#define AW_ERROR_PIPE_CONNECTED 535u

/* A freestanding build still lets either compiler emit these two calls. The
 * volatile stores keep the optimizer from turning the loops back into calls. */
#if defined(_MSC_VER) && !defined(__clang__)
#pragma function(memset, memcpy)
#endif
void *memset(void *dest, int value, size_t count);
void *memcpy(void *dest, const void *src, size_t count);
void *memset(void *dest, int value, size_t count) {
    volatile unsigned char *d = dest;
    while (count--) *d++ = (unsigned char)value;
    return dest;
}
void *memcpy(void *dest, const void *src, size_t count) {
    volatile unsigned char *d = dest;
    const unsigned char *s = src;
    while (count--) *d++ = *s++;
    return dest;
}

/* ---- Wire format: one 64-byte packet per pipe message ----------------------- */

#pragma pack(push, 1)
typedef struct {
    uint32_t magic;       /* 0x57495245 ("WIRE") */
    uint16_t version;     /* 0x0100 (v1.0) */
    uint16_t opcode;      /* 0x0001 PING, 0x0002 TASK, 0x0003 STATUS, 0x0004 RECEIPT */
    uint32_t project_id;  /* project hash identifier */
    uint16_t sender_id;   /* sending agent */
    uint16_t receiver_id; /* receiving agent */
    uint32_t payload_len; /* bytes used in payload, 0..40 */
    uint32_t checksum;    /* FNV-1a 32 over the other 60 bytes */
    uint8_t payload[AW_PAYLOAD_BYTES];
} agent_packet_t;
#pragma pack(pop)

#define AW_STATIC_ASSERT(condition, name) typedef char name[(condition) ? 1 : -1]
AW_STATIC_ASSERT(sizeof(agent_packet_t) == AW_PACKET_BYTES, aw_packet_is_64_bytes);
AW_STATIC_ASSERT(offsetof(agent_packet_t, checksum) == 20u, aw_checksum_at_20);
AW_STATIC_ASSERT(offsetof(agent_packet_t, payload) == 24u, aw_payload_at_24);

enum {
    AW_OP_PING = 0x0001,
    AW_OP_TASK = 0x0002,
    AW_OP_STATUS = 0x0003,
    AW_OP_RECEIPT = 0x0004
};

/* Receipt payload (little endian): status u32 | request_checksum u32 | sequence u32 | request_opcode u16 */
enum {
    AW_ACK_OK = 0,
    AW_ACK_BAD_SIZE = 1,     /* pipe message was not exactly 64 bytes */
    AW_ACK_BAD_MAGIC = 2,
    AW_ACK_BAD_VERSION = 3,
    AW_ACK_BAD_OPCODE = 4,   /* server accepts PING, TASK, STATUS */
    AW_ACK_BAD_LENGTH = 5,   /* payload_len > 40 */
    AW_ACK_BAD_CHECKSUM = 6
};
#define AW_RECEIPT_PAYLOAD_LEN 14u

static const char *aw_ack_name(uint32_t status) {
    static const char *const names[] = { "ACK_OK", "BAD_SIZE", "BAD_MAGIC", "BAD_VERSION",
                                         "BAD_OPCODE", "BAD_LENGTH", "BAD_CHECKSUM" };
    return status < sizeof names / sizeof names[0] ? names[status] : "UNKNOWN";
}

static const char *aw_opcode_name(uint32_t opcode) {
    switch (opcode) {
    case AW_OP_PING: return "PING";
    case AW_OP_TASK: return "TASK";
    case AW_OP_STATUS: return "STATUS";
    case AW_OP_RECEIPT: return "RECEIPT";
    default: return "?";
    }
}

static uint32_t aw_fnv1a(const unsigned char *data, size_t length, uint32_t hash) {
    for (size_t i = 0; i < length; i++) hash = (hash ^ data[i]) * 0x01000193u;
    return hash;
}
#define AW_FNV_OFFSET 0x811C9DC5u

/* Checksum domain: bytes [0,20) and [24,64) of the packet, i.e. every byte but the checksum field. */
static uint32_t aw_packet_checksum(const agent_packet_t *packet) {
    const unsigned char *bytes = (const unsigned char *)packet;
    uint32_t hash = aw_fnv1a(bytes, 20u, AW_FNV_OFFSET);
    return aw_fnv1a(bytes + 24, AW_PACKET_BYTES - 24u, hash);
}

static void aw_put_u16(uint8_t *at, uint32_t value) {
    at[0] = (uint8_t)value;
    at[1] = (uint8_t)(value >> 8);
}
static void aw_put_u32(uint8_t *at, uint32_t value) {
    aw_put_u16(at, value & 0xFFFFu);
    aw_put_u16(at + 2, value >> 16);
}
static uint32_t aw_get_u16(const uint8_t *at) { return (uint32_t)at[0] | ((uint32_t)at[1] << 8); }
static uint32_t aw_get_u32(const uint8_t *at) { return aw_get_u16(at) | (aw_get_u16(at + 2) << 16); }

static void aw_packet_init(agent_packet_t *packet, uint32_t opcode, uint32_t project_id, uint32_t sender,
                           uint32_t receiver, const uint8_t *payload, uint32_t payload_len) {
    memset(packet, 0, sizeof *packet);
    packet->magic = AW_MAGIC;
    packet->version = (uint16_t)AW_WIRE_VERSION;
    packet->opcode = (uint16_t)opcode;
    packet->project_id = project_id;
    packet->sender_id = (uint16_t)sender;
    packet->receiver_id = (uint16_t)receiver;
    packet->payload_len = payload_len;
    if (payload != NULL && payload_len != 0u) memcpy(packet->payload, payload, payload_len);
    packet->checksum = aw_packet_checksum(packet);
}

/* Validate a request as the server sees it: `size` is the pipe message length. */
static uint32_t aw_packet_validate(const agent_packet_t *packet, size_t size, int receipt_allowed) {
    if (size != AW_PACKET_BYTES) return AW_ACK_BAD_SIZE;
    if (packet->magic != AW_MAGIC) return AW_ACK_BAD_MAGIC;
    if (packet->version != AW_WIRE_VERSION) return AW_ACK_BAD_VERSION;
    if (packet->opcode < AW_OP_PING || packet->opcode > (receipt_allowed ? AW_OP_RECEIPT : AW_OP_STATUS))
        return AW_ACK_BAD_OPCODE;
    if (packet->payload_len > AW_PAYLOAD_BYTES) return AW_ACK_BAD_LENGTH;
    if (packet->checksum != aw_packet_checksum(packet)) return AW_ACK_BAD_CHECKSUM;
    return AW_ACK_OK;
}

/* Build the RECEIPT for a request. `raw` may be shorter than a packet (BAD_SIZE): fields are then 0. */
static void aw_receipt_build(agent_packet_t *receipt, const agent_packet_t *raw, size_t size, uint32_t status,
                             uint32_t sequence) {
    uint8_t body[AW_RECEIPT_PAYLOAD_LEN];
    const int whole = size >= AW_PACKET_BYTES;
    aw_put_u32(body, status);
    aw_put_u32(body + 4, whole ? raw->checksum : 0u);
    aw_put_u32(body + 8, sequence);
    aw_put_u16(body + 12, whole ? raw->opcode : 0u);
    aw_packet_init(receipt, AW_OP_RECEIPT, whole ? raw->project_id : 0u, AW_SERVER_ID,
                   whole ? raw->sender_id : 0u, body, AW_RECEIPT_PAYLOAD_LEN);
}

typedef struct {
    uint32_t status;
    uint32_t request_checksum;
    uint32_t sequence;
    uint32_t request_opcode;
} aw_receipt_view;

/* Client-side check of a receipt against the request it answers. 0 = accepted, else an AW_ACK_* code,
 * or 100 + status when the server reported a non-OK status, or 200 when the receipt answers another request. */
static uint32_t aw_receipt_check(const agent_packet_t *receipt, size_t size, const agent_packet_t *request,
                                 aw_receipt_view *view) {
    const uint32_t valid = aw_packet_validate(receipt, size, 1);
    if (valid != AW_ACK_OK) return valid;
    if (receipt->opcode != AW_OP_RECEIPT || receipt->payload_len != AW_RECEIPT_PAYLOAD_LEN) return AW_ACK_BAD_OPCODE;
    view->status = aw_get_u32(receipt->payload);
    view->request_checksum = aw_get_u32(receipt->payload + 4);
    view->sequence = aw_get_u32(receipt->payload + 8);
    view->request_opcode = aw_get_u16(receipt->payload + 12);
    if (view->status != AW_ACK_OK) return 100u + view->status;
    if (view->request_checksum != request->checksum || view->request_opcode != request->opcode ||
        receipt->project_id != request->project_id || receipt->receiver_id != request->sender_id)
        return 200u;
    return 0u;
}

/* ---- Console output (UTF-8, no formatting library) -------------------------- */

static size_t aw_length(const char *text) {
    size_t n = 0;
    while (text[n] != '\0') n++;
    return n;
}

static void aw_write(const char *text) {
    aw_dword written = 0;
    (void)WriteFile(GetStdHandle(AW_STD_OUTPUT), text, (aw_dword)aw_length(text), &written, NULL);
}

/* Unsigned decimal into a caller buffer of at least 21 bytes. */
static const char *aw_decimal(uint64_t value, char buffer[21]) {
    char *p = buffer + 20;
    *p = '\0';
    do {
        *--p = (char)('0' + value % 10u);
        value /= 10u;
    } while (value != 0u);
    return p;
}

static void aw_write_decimal(uint64_t value) {
    char digits[21];
    aw_write(aw_decimal(value, digits));
}

static void aw_write_hex32(uint32_t value) {
    static const char hex[] = "0123456789ABCDEF";
    char text[11] = "0x";
    for (int i = 0; i < 8; i++) text[2 + i] = hex[(value >> (28 - 4 * i)) & 0xFu];
    text[10] = '\0';
    aw_write(text);
}

/* Microseconds as milliseconds with three decimals, e.g. 244 -> "0.244". Buffer >= 25 bytes. */
static const char *aw_millis(uint64_t micros, char buffer[25]) {
    char digits[21];
    const char *whole = aw_decimal(micros / 1000u, digits);
    size_t n = aw_length(whole);
    memcpy(buffer, whole, n);
    buffer[n++] = '.';
    buffer[n++] = (char)('0' + (micros / 100u) % 10u);
    buffer[n++] = (char)('0' + (micros / 10u) % 10u);
    buffer[n++] = (char)('0' + micros % 10u);
    buffer[n] = '\0';
    return buffer;
}

static void aw_write_latency(const char *label, uint64_t micros) {
    char text[25];
    aw_write(label);
    aw_write(aw_millis(micros, text));
    aw_write(" ms (");
    aw_write_decimal(micros);
    aw_write(" us)\n");
}

static void aw_write_last_error(const char *what) {
    aw_write("error: ");
    aw_write(what);
    aw_write(" failed, GetLastError=");
    aw_write_decimal(GetLastError());
    aw_write("\n");
}

static void aw_write_peak_working_set(void) {
    aw_memory_counters counters;
    counters.cb = (aw_dword)sizeof counters;
    aw_write("  peak working set  ");
    if (K32GetProcessMemoryInfo(GetCurrentProcess(), &counters, counters.cb) != 0)
        aw_write_decimal((uint64_t)counters.peak_working_set / 1024u);
    else
        aw_write("unknown");
    aw_write(" KiB\n");
}

/* ---- Timing ------------------------------------------------------------------ */

static int64_t aw_frequency;

static int64_t aw_now(void) {
    int64_t count = 0;
    (void)QueryPerformanceCounter(&count);
    return count;
}

static uint64_t aw_micros(int64_t ticks) {
    if (aw_frequency <= 0) return 0u;
    return (uint64_t)ticks * 1000000u / (uint64_t)aw_frequency;
}

/* ---- Command line: UTF-16 tokens to UTF-8 ------------------------------------ */

typedef struct {
    const uint16_t *cursor;
} aw_arguments;

static void aw_utf8_append(char *out, size_t *n, size_t cap, uint32_t code) {
    unsigned char bytes[4];
    size_t count;
    if (code < 0x80u) { bytes[0] = (unsigned char)code; count = 1; }
    else if (code < 0x800u) { bytes[0] = (unsigned char)(0xC0u | (code >> 6)); bytes[1] = (unsigned char)(0x80u | (code & 0x3Fu)); count = 2; }
    else if (code < 0x10000u) {
        bytes[0] = (unsigned char)(0xE0u | (code >> 12)); bytes[1] = (unsigned char)(0x80u | ((code >> 6) & 0x3Fu));
        bytes[2] = (unsigned char)(0x80u | (code & 0x3Fu)); count = 3;
    } else {
        bytes[0] = (unsigned char)(0xF0u | (code >> 18)); bytes[1] = (unsigned char)(0x80u | ((code >> 12) & 0x3Fu));
        bytes[2] = (unsigned char)(0x80u | ((code >> 6) & 0x3Fu)); bytes[3] = (unsigned char)(0x80u | (code & 0x3Fu)); count = 4;
    }
    for (size_t i = 0; i < count; i++)
        if (*n < cap) out[(*n)++] = (char)bytes[i];
        else (*n)++; /* keep counting so the caller sees the overflow */
}

/* Next whitespace-separated token (double quotes group), UTF-8 into out[cap]; returns byte length
 * (may exceed cap - 1 when truncated) or -1 when no token is left. */
static long aw_next_argument(aw_arguments *args, char *out, size_t cap) {
    const uint16_t *p = args->cursor;
    size_t n = 0;
    int quoted = 0;
    while (*p == ' ' || *p == '\t') p++;
    if (*p == 0) { args->cursor = p; return -1; }
    while (*p != 0 && (quoted || (*p != ' ' && *p != '\t'))) {
        uint32_t code = *p++;
        if (code == '"') { quoted = !quoted; continue; }
        if (code >= 0xD800u && code <= 0xDBFFu && *p >= 0xDC00u && *p <= 0xDFFFu)
            code = 0x10000u + ((code - 0xD800u) << 10) + (uint32_t)(*p++ - 0xDC00u);
        aw_utf8_append(out, &n, cap - 1u, code);
    }
    out[n < cap ? n : cap - 1u] = '\0';
    args->cursor = p;
    return (long)n;
}

static int aw_same(const char *a, const char *b) {
    size_t i = 0;
    while (a[i] != '\0' && a[i] == b[i]) i++;
    return a[i] == b[i];
}

/* Decimal to uint32; returns 0 on bad input (empty, non-digit, overflow). */
static int aw_parse_u32(const char *text, uint32_t *value) {
    uint64_t v = 0;
    if (text[0] == '\0') return 0;
    for (size_t i = 0; text[i] != '\0'; i++) {
        if (text[i] < '0' || text[i] > '9') return 0;
        v = v * 10u + (uint64_t)(text[i] - '0');
        if (v > 0xFFFFFFFFu) return 0;
    }
    *value = (uint32_t)v;
    return 1;
}

/* ---- Pipe layer: the six KERNEL32 calls ------------------------------------- */

static unsigned char aw_inbox[AW_PIPE_BUFFER];

/* Read one pipe message into aw_inbox. Returns 1 and the message size (capped at the buffer: a longer
 * message is drained and its full length reported), 0 when the client is gone or the read failed. */
static int aw_read_message(aw_handle pipe, size_t *size) {
    aw_dword got = 0;
    *size = 0;
    for (;;) {
        const aw_bool ok = ReadFile(pipe, aw_inbox, (aw_dword)sizeof aw_inbox, &got, NULL);
        *size += got;
        if (ok) return 1;
        if (GetLastError() != AW_ERROR_MORE_DATA) return 0;
        /* message longer than the buffer: keep draining, aw_inbox now holds the tail */
    }
}

static int aw_write_packet(aw_handle pipe, const agent_packet_t *packet) {
    aw_dword written = 0;
    return WriteFile(pipe, packet, AW_PACKET_BYTES, &written, NULL) != 0 && written == AW_PACKET_BYTES;
}

/* Client side reads in byte mode: accumulate exactly one 64-byte receipt. */
static int aw_read_packet(aw_handle pipe, agent_packet_t *packet) {
    unsigned char *at = (unsigned char *)packet;
    aw_dword total = 0;
    while (total < AW_PACKET_BYTES) {
        aw_dword got = 0;
        if (ReadFile(pipe, at + total, AW_PACKET_BYTES - total, &got, NULL) == 0 || got == 0u) return 0;
        total += got;
    }
    return 1;
}

static unsigned int aw_server(uint32_t session_limit) {
    uint32_t sessions = 0;
    aw_write("agent_wire_ipc " AW_VERSION " server\n  pipe              " AW_PIPE_NAME "\n");
    aw_write("  sessions          ");
    if (session_limit == 0u) aw_write("unlimited\n"); else { aw_write_decimal(session_limit); aw_write("\n"); }
    for (;;) {
        const aw_handle pipe = CreateNamedPipeA(AW_PIPE_NAME, AW_PIPE_ACCESS_DUPLEX,
                                                AW_PIPE_TYPE_MESSAGE | AW_PIPE_READMODE_MESSAGE |
                                                    AW_PIPE_REJECT_REMOTE_CLIENTS,
                                                1u, AW_PIPE_BUFFER, AW_PIPE_BUFFER, 0u, NULL);
        uint32_t sequence = 0, rejected = 0;
        if (pipe == AW_INVALID_HANDLE) { aw_write_last_error("CreateNamedPipeA"); return 1u; }
        if (ConnectNamedPipe(pipe, NULL) == 0 && GetLastError() != AW_ERROR_PIPE_CONNECTED) {
            aw_write_last_error("ConnectNamedPipe");
            (void)CloseHandle(pipe);
            return 1u;
        }
        sessions++;
        aw_write("  session ");
        aw_write_decimal(sessions);
        aw_write("          agent connected\n");
        for (;;) {
            agent_packet_t receipt;
            size_t size;
            uint32_t status;
            if (!aw_read_message(pipe, &size)) {
                const aw_dword err = GetLastError();
                if (err != AW_ERROR_BROKEN_PIPE && err != AW_ERROR_NO_DATA) aw_write_last_error("ReadFile");
                break;
            }
            sequence++;
            status = aw_packet_validate((const agent_packet_t *)aw_inbox, size, 0);
            if (status != AW_ACK_OK) rejected++;
            aw_receipt_build(&receipt, (const agent_packet_t *)aw_inbox, size, status, sequence);
            if (!aw_write_packet(pipe, &receipt)) { aw_write_last_error("WriteFile"); break; }
        }
        (void)CloseHandle(pipe);
        aw_write("  session ");
        aw_write_decimal(sessions);
        aw_write("          closed: ");
        aw_write_decimal(sequence);
        aw_write(" packets, ");
        aw_write_decimal(rejected);
        aw_write(" rejected\n");
        if (session_limit != 0u && sessions >= session_limit) return 0u;
    }
}

/* Connect to the server pipe, retrying while it does not exist yet or is busy with another client. */
static aw_handle aw_connect(void) {
    for (uint32_t attempt = 0; attempt < AW_CONNECT_RETRIES; attempt++) {
        const aw_handle pipe = CreateFileA(AW_PIPE_NAME, AW_GENERIC_READ | AW_GENERIC_WRITE, 0u, NULL,
                                           AW_OPEN_EXISTING, 0u, NULL);
        if (pipe != AW_INVALID_HANDLE) return pipe;
        const aw_dword err = GetLastError();
        if (err != AW_ERROR_FILE_NOT_FOUND && err != AW_ERROR_PIPE_BUSY) break;
        Sleep(1u);
    }
    aw_write_last_error("CreateFileA(" AW_PIPE_NAME ")");
    return AW_INVALID_HANDLE;
}

/* One request/receipt exchange, timed. Returns 0 accepted, else the aw_receipt_check code or 300 on I/O failure. */
static uint32_t aw_round_trip(aw_handle pipe, const agent_packet_t *request, agent_packet_t *receipt,
                              aw_receipt_view *view, uint64_t *micros) {
    const int64_t start = aw_now();
    if (!aw_write_packet(pipe, request) || !aw_read_packet(pipe, receipt)) return 300u;
    *micros = aw_micros(aw_now() - start);
    return aw_receipt_check(receipt, AW_PACKET_BYTES, request, view);
}

static void aw_write_rejection(uint32_t code) {
    aw_write("  result            REJECTED (");
    if (code == 300u) aw_write("pipe I/O failed");
    else if (code == 200u) aw_write("receipt answers a different request");
    else if (code >= 100u) { aw_write("server status "); aw_write(aw_ack_name(code - 100u)); }
    else { aw_write("receipt "); aw_write(aw_ack_name(code)); }
    aw_write(")\n");
}

static unsigned int aw_client(const char *text, long text_bytes) {
    agent_packet_t request, receipt;
    aw_receipt_view view = { 0u, 0u, 0u, 0u };
    uint64_t micros = 0;
    aw_handle pipe;
    uint32_t code;
    if (text_bytes > (long)AW_PAYLOAD_BYTES) {
        aw_write("error: TASK payload is limited to 40 bytes\n");
        return 2u;
    }
    if (text_bytes > 0)
        aw_packet_init(&request, AW_OP_TASK, AW_DEFAULT_PROJECT, AW_CLIENT_ID, AW_SERVER_ID, (const uint8_t *)text,
                       (uint32_t)text_bytes);
    else
        aw_packet_init(&request, AW_OP_PING, AW_DEFAULT_PROJECT, AW_CLIENT_ID, AW_SERVER_ID, NULL, 0u);
    aw_write("agent_wire_ipc " AW_VERSION " client\n  pipe              " AW_PIPE_NAME "\n");
    pipe = aw_connect();
    if (pipe == AW_INVALID_HANDLE) return 1u;
    code = aw_round_trip(pipe, &request, &receipt, &view, &micros);
    (void)CloseHandle(pipe);
    aw_write("  request           ");
    aw_write(aw_opcode_name(request.opcode));
    aw_write("  project ");
    aw_write_hex32(request.project_id);
    aw_write("  ");
    aw_write_decimal(request.sender_id);
    aw_write(" -> ");
    aw_write_decimal(request.receiver_id);
    aw_write("  payload ");
    aw_write_decimal(request.payload_len);
    aw_write(" B  checksum ");
    aw_write_hex32(request.checksum);
    aw_write("\n");
    if (code == 300u) { aw_write_rejection(code); return 1u; }
    if (code < 100u && code != 0u) { aw_write_rejection(code); return 3u; }
    aw_write("  receipt           status ");
    aw_write_decimal(view.status);
    aw_write(" (");
    aw_write(aw_ack_name(view.status));
    aw_write(")  sequence ");
    aw_write_decimal(view.sequence);
    aw_write("  ");
    aw_write_decimal(receipt.sender_id);
    aw_write(" -> ");
    aw_write_decimal(receipt.receiver_id);
    aw_write("  echoed checksum ");
    aw_write_hex32(view.request_checksum);
    aw_write("\n");
    aw_write_latency("  rtt               ", micros);
    if (code != 0u) { aw_write_rejection(code); return 3u; }
    aw_write("  result            ACCEPTED\n");
    return 0u;
}

/* ---- Benchmark ---------------------------------------------------------------- */

static uint64_t aw_samples[AW_BENCH_MAX];

static void aw_sift(uint64_t *a, size_t start, size_t end) { /* heapsort helper */
    size_t root = start;
    for (;;) {
        size_t child = 2 * root + 1, swap = root;
        if (child >= end) return;
        if (a[swap] < a[child]) swap = child;
        if (child + 1 < end && a[swap] < a[child + 1]) swap = child + 1;
        if (swap == root) return;
        const uint64_t t = a[root]; a[root] = a[swap]; a[swap] = t;
        root = swap;
    }
}

static void aw_sort(uint64_t *a, size_t n) {
    if (n < 2) return;
    for (size_t start = n / 2; start-- > 0;) aw_sift(a, start, n);
    for (size_t end = n; end-- > 1;) {
        const uint64_t t = a[0]; a[0] = a[end]; a[end] = t;
        aw_sift(a, 0, end);
    }
}

static unsigned int aw_bench(uint32_t count) {
    agent_packet_t request, receipt;
    aw_receipt_view view;
    aw_handle pipe;
    uint64_t sum = 0;
    int64_t started;
    if (count == 0u || count > AW_BENCH_MAX) {
        aw_write("error: --bench count must be 1..16384\n");
        return 2u;
    }
    aw_packet_init(&request, AW_OP_PING, AW_DEFAULT_PROJECT, AW_CLIENT_ID, AW_SERVER_ID, NULL, 0u);
    aw_write("agent_wire_ipc " AW_VERSION " bench\n  pipe              " AW_PIPE_NAME "\n  round trips       ");
    aw_write_decimal(count);
    aw_write(" x PING(64 B) -> RECEIPT(64 B), every receipt verified\n");
    pipe = aw_connect();
    if (pipe == AW_INVALID_HANDLE) return 1u;
    started = aw_now();
    for (uint32_t i = 0; i < count; i++) {
        const uint32_t code = aw_round_trip(pipe, &request, &receipt, &view, &aw_samples[i]);
        if (code != 0u) {
            (void)CloseHandle(pipe);
            aw_write("  failed at round trip ");
            aw_write_decimal(i + 1u);
            aw_write("\n");
            aw_write_rejection(code);
            return code == 300u ? 1u : 3u;
        }
        sum += aw_samples[i];
    }
    const uint64_t total = aw_micros(aw_now() - started);
    (void)CloseHandle(pipe);
    aw_sort(aw_samples, count);
    aw_write_latency("  min               ", aw_samples[0]);
    aw_write_latency("  p50               ", aw_samples[count / 2u]);
    aw_write_latency("  avg               ", sum / count);
    aw_write_latency("  p99               ", aw_samples[(count * 99u) / 100u]);
    aw_write_latency("  max               ", aw_samples[count - 1u]);
    aw_write("  total             ");
    aw_write_decimal(total / 1000u);
    aw_write(" ms, ");
    aw_write_decimal(total != 0u ? (uint64_t)count * 1000000u / total : 0u);
    aw_write(" round trips/s\n");
    aw_write_peak_working_set();
    aw_write("  result            ");
    aw_write_decimal(count);
    aw_write("/");
    aw_write_decimal(count);
    aw_write(" receipts accepted\n");
    return 0u;
}

/* ---- Self-test ------------------------------------------------------------------ */

static void aw_check(const char *name, int ok, int *passed) {
    aw_write(ok ? "  PASS  " : "  FAIL  ");
    aw_write(name);
    aw_write("\n");
    *passed += ok ? 1 : 0;
}

static unsigned int aw_selftest(void) {
    agent_packet_t packet, receipt, broken;
    aw_receipt_view view;
    char text[25];
    int passed = 0;
    static const uint8_t hello[5] = { 'h', 'e', 'l', 'l', 'o' };

    aw_write("agent_wire_ipc " AW_VERSION " self-test\n");

    aw_check("Layout: packet is 64 bytes, checksum at 20, payload at 24, payload 40 bytes",
             sizeof(agent_packet_t) == 64u && offsetof(agent_packet_t, checksum) == 20u &&
                 offsetof(agent_packet_t, payload) == 24u && sizeof packet.payload == 40u, &passed);

    aw_check("FNV-1a 32 vectors: \"\" = 0x811C9DC5, \"a\" = 0xE40C292C, pipe name tag = 0xB1605844",
             aw_fnv1a((const unsigned char *)"", 0u, AW_FNV_OFFSET) == 0x811C9DC5u &&
                 aw_fnv1a((const unsigned char *)"a", 1u, AW_FNV_OFFSET) == 0xE40C292Cu &&
                 aw_fnv1a((const unsigned char *)"ultra_brain_agent_wire", 22u, AW_FNV_OFFSET) == AW_DEFAULT_PROJECT,
             &passed);

    aw_packet_init(&packet, AW_OP_PING, AW_DEFAULT_PROJECT, AW_CLIENT_ID, AW_SERVER_ID, NULL, 0u);
    aw_check("Golden PING (project 0xB1605844, 2 -> 1): checksum 0x85BD5F54", packet.checksum == 0x85BD5F54u, &passed);

    aw_packet_init(&packet, AW_OP_TASK, AW_DEFAULT_PROJECT, AW_CLIENT_ID, AW_SERVER_ID, hello, 5u);
    aw_check("Golden TASK \"hello\": checksum 0x716CD786, validates ACK_OK",
             packet.checksum == 0x716CD786u && aw_packet_validate(&packet, 64u, 0) == AW_ACK_OK, &passed);

    {
        int ok = aw_packet_validate(&packet, 63u, 0) == AW_ACK_BAD_SIZE;
        broken = packet; broken.magic ^= 1u; ok = ok && aw_packet_validate(&broken, 64u, 0) == AW_ACK_BAD_MAGIC;
        broken = packet; broken.version = 0x0200u; ok = ok && aw_packet_validate(&broken, 64u, 0) == AW_ACK_BAD_VERSION;
        broken = packet; broken.opcode = AW_OP_RECEIPT; ok = ok && aw_packet_validate(&broken, 64u, 0) == AW_ACK_BAD_OPCODE;
        broken = packet; broken.opcode = 0u; ok = ok && aw_packet_validate(&broken, 64u, 0) == AW_ACK_BAD_OPCODE;
        broken = packet; broken.payload_len = 41u; ok = ok && aw_packet_validate(&broken, 64u, 0) == AW_ACK_BAD_LENGTH;
        broken = packet; broken.payload[3] ^= 0x20u; ok = ok && aw_packet_validate(&broken, 64u, 0) == AW_ACK_BAD_CHECKSUM;
        broken = packet; broken.payload[39] = 1u; ok = ok && aw_packet_validate(&broken, 64u, 0) == AW_ACK_BAD_CHECKSUM;
        aw_check("Validation: size, magic, version, opcode (0 and RECEIPT), length 41, payload bit, padding byte rejected",
                 ok, &passed);
    }

    aw_receipt_build(&receipt, &packet, 64u, AW_ACK_OK, 7u);
    aw_check("Receipt: RECEIPT 1 -> 2, 14-byte body, echoes checksum/opcode/project, sequence 7, client accepts it",
             receipt.opcode == AW_OP_RECEIPT && receipt.sender_id == AW_SERVER_ID && receipt.receiver_id == AW_CLIENT_ID &&
                 receipt.payload_len == AW_RECEIPT_PAYLOAD_LEN && aw_receipt_check(&receipt, 64u, &packet, &view) == 0u &&
                 view.sequence == 7u && view.request_opcode == AW_OP_TASK && view.request_checksum == packet.checksum,
             &passed);

    {
        aw_receipt_build(&receipt, &packet, 64u, AW_ACK_BAD_CHECKSUM, 8u);
        const int rejected = aw_receipt_check(&receipt, 64u, &packet, &view) == 106u && view.status == AW_ACK_BAD_CHECKSUM;
        broken = packet; broken.payload[0] ^= 1u; broken.checksum = aw_packet_checksum(&broken);
        aw_receipt_build(&receipt, &packet, 64u, AW_ACK_OK, 9u);
        aw_check("Receipt: server status BAD_CHECKSUM -> client code 106; receipt for another request -> 200",
                 rejected && aw_receipt_check(&receipt, 64u, &broken, &view) == 200u, &passed);
    }

    aw_receipt_build(&receipt, &packet, 10u, AW_ACK_BAD_SIZE, 1u);
    aw_check("Receipt for a short message: fields zero, status BAD_SIZE, still a valid 64-byte RECEIPT",
             receipt.project_id == 0u && receipt.receiver_id == 0u && aw_packet_validate(&receipt, 64u, 1) == AW_ACK_OK &&
                 aw_get_u32(receipt.payload) == AW_ACK_BAD_SIZE && aw_get_u32(receipt.payload + 4) == 0u, &passed);

    aw_check("Formatter: 244 us -> 0.244 ms, 1000000 us -> 1000.000 ms, 5 us -> 0.005 ms",
             aw_same(aw_millis(244u, text), "0.244") && aw_same(aw_millis(1000000u, text), "1000.000") &&
                 aw_same(aw_millis(5u, text), "0.005"), &passed);

    {
        uint32_t v = 0;
        aw_check("Argument parser: \"1000\" -> 1000, \"\", \"12a\", \"4294967296\" rejected",
                 aw_parse_u32("1000", &v) && v == 1000u && !aw_parse_u32("", &v) && !aw_parse_u32("12a", &v) &&
                     !aw_parse_u32("4294967296", &v), &passed);
    }

    {
        static const uint64_t unsorted[5] = { 5u, 1u, 4u, 2u, 3u };
        uint64_t sorted[5];
        memcpy(sorted, unsorted, sizeof sorted);
        aw_sort(sorted, 5u);
        aw_check("Heapsort: 5 1 4 2 3 -> 1 2 3 4 5",
                 sorted[0] == 1u && sorted[1] == 2u && sorted[2] == 3u && sorted[3] == 4u && sorted[4] == 5u, &passed);
    }

    aw_write(passed == 11 ? "SELFTEST PASS 11/11\n" : "SELFTEST FAIL\n");
    return passed == 11 ? 0u : 1u;
}

/* ---- Entry point (no C runtime: the linker's default console entry name) ------ */

static unsigned int aw_usage(void) {
    aw_write("usage: agent_wire_ipc --server [sessions] | --client [text] | --bench [count] | --selftest | --version\n");
    return 2u;
}

__declspec(noreturn) void AW_API mainCRTStartup(void);
void AW_API mainCRTStartup(void) {
    static char first[64], second[4096];
    aw_arguments args;
    long first_len, second_len;
    uint32_t count = 0;

    (void)SetConsoleOutputCP(AW_CP_UTF8);
    (void)QueryPerformanceFrequency(&aw_frequency);

    args.cursor = GetCommandLineW();
    (void)aw_next_argument(&args, first, sizeof first); /* program path */
    first_len = aw_next_argument(&args, first, sizeof first);
    second_len = aw_next_argument(&args, second, sizeof second);
    if (first_len < 0) ExitProcess(aw_usage());

    if (aw_same(first, "--version")) { aw_write(AW_VERSION "\n"); ExitProcess(0u); }
    if (aw_same(first, "--selftest")) ExitProcess(aw_selftest());
    if (aw_same(first, "--server")) {
        if (second_len >= 0 && !aw_parse_u32(second, &count)) ExitProcess(aw_usage());
        ExitProcess(aw_server(count));
    }
    if (aw_same(first, "--client")) ExitProcess(aw_client(second, second_len < 0 ? 0 : second_len));
    if (aw_same(first, "--bench")) {
        count = AW_BENCH_DEFAULT;
        if (second_len >= 0 && !aw_parse_u32(second, &count)) ExitProcess(aw_usage());
        ExitProcess(aw_bench(count));
    }
    ExitProcess(aw_usage());
}

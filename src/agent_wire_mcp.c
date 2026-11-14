/*
 * agent_wire_mcp - MCP stdio adapter for the agent_wire_ipc named pipe (Ultra-Brain-Labs v1.2.0)
 *
 * One C11 file. No C runtime, no SDK header, no DLL other than KERNEL32.dll, no JSON library.
 * It speaks the Model Context Protocol over stdio (JSON-RPC 2.0, one message per line) to an
 * MCP host such as Claude Desktop or Cursor, and the 64-byte agent wire protocol to the pipe
 * \\.\pipe\ultra_brain_agent_wire that `agent_wire_ipc --server` serves. agent_wire_ipc.c is
 * not modified: this file repeats the packet layout and checksum and is tested against the
 * unmodified server binary.
 *
 * MCP surface (handshake revisions 2024-11-05, 2025-03-26, 2025-06-18, 2025-11-25):
 *   initialize, notifications/initialized, ping, tools/list, tools/call
 * Tools:
 *   wire_status        connect if needed, one PING, state + round-trip time + counters
 *   wire_roundtrip     count (1..16384) PING/TASK/STATUS round trips, every receipt verified
 *   wire_server_start  start the sibling agent_wire_ipc.exe --server; it ends with this adapter
 *   wire_close         release the pipe so another client can connect
 *
 * Build (LLVM clang for Windows, lld):
 *   clang -std=c11 -O2 -Wall -Wextra -pedantic -Werror -ffreestanding -nostdlib -fuse-ld=lld
 *         -Wl,/subsystem:console src/agent_wire_mcp.c -lkernel32 -o agent_wire_mcp.exe
 * Build (MSVC):
 *   cl /nologo /std:c11 /O2 /W4 /WX /utf-8 /GS- src\agent_wire_mcp.c
 *      /link /SUBSYSTEM:CONSOLE /NODEFAULTLIB kernel32.lib /OUT:agent_wire_mcp.exe
 *
 * Usage:
 *   agent_wire_mcp              MCP server on stdin/stdout until stdin closes (exit 0)
 *   agent_wire_mcp --selftest   wire layout, JSON scanner, JSON-RPC dispatch (exit 0 = pass)
 *   agent_wire_mcp --version    prints 1.2.0
 *
 * The process has one thread, no heap use of its own and no timer: between messages it is
 * blocked in ReadFile on stdin. stdout carries MCP messages only; stderr gets one start line.
 */

#include <stddef.h>
#include <stdint.h>

#define MW_VERSION "1.2.0"
#define AW_PIPE_NAME "\\\\.\\pipe\\ultra_brain_agent_wire"
#define AW_PACKET_BYTES 64u
#define AW_PAYLOAD_BYTES 40u
#define AW_MAGIC 0x57495245u /* "WIRE" */
#define AW_WIRE_VERSION 0x0100u
#define AW_DEFAULT_PROJECT 0xB1605844u /* FNV-1a 32 of "ultra_brain_agent_wire" */
#define AW_SERVER_ID 1u
#define AW_CLIENT_ID 2u
#define MW_BATCH_MAX 16384u       /* round trips per wire_roundtrip call */
#define MW_CONNECT_WAIT_MS 3000u  /* wire_roundtrip waits this long for the pipe, as agent_wire_ipc --client does */
#define MW_STATUS_WAIT_MAX 5000u
#define MCP_LINE_MAX 65536u       /* longest accepted JSON-RPC message, bytes */
#define MCP_OUT_MAX 16384u
#define MCP_INNER_MAX 4096u
#define MCP_DEPTH_MAX 32u
#define MW_PATH_MAX 1024u

/* ---- The only external surface: KERNEL32.dll ------------------------------ */

typedef void *aw_handle;
typedef uint32_t aw_dword;
typedef int aw_bool;

typedef struct { /* SECURITY_ATTRIBUTES */
    aw_dword length;
    void *descriptor;
    aw_bool inherit;
} mw_security;

typedef struct { /* STARTUPINFOW */
    aw_dword cb;
    uint16_t *reserved;
    uint16_t *desktop;
    uint16_t *title;
    aw_dword x, y, x_size, y_size, x_chars, y_chars, fill, flags;
    uint16_t show_window;
    uint16_t reserved2_bytes;
    uint8_t *reserved2;
    aw_handle std_input;
    aw_handle std_output;
    aw_handle std_error;
} mw_startup_info;

typedef struct { /* PROCESS_INFORMATION */
    aw_handle process;
    aw_handle thread;
    aw_dword process_id;
    aw_dword thread_id;
} mw_process_info;

typedef struct { /* JOBOBJECT_EXTENDED_LIMIT_INFORMATION */
    int64_t per_process_user_time;
    int64_t per_job_user_time;
    aw_dword limit_flags;
    size_t minimum_working_set;
    size_t maximum_working_set;
    aw_dword active_process_limit;
    size_t affinity;
    aw_dword priority_class;
    aw_dword scheduling_class;
    uint64_t io_counters[6];
    size_t process_memory_limit;
    size_t job_memory_limit;
    size_t peak_process_memory;
    size_t peak_job_memory;
} mw_job_limits;

#define AW_IMPORT __declspec(dllimport)
#define AW_NOINLINE __declspec(noinline) /* shared helpers stay single copies: smaller code */
#define AW_API __stdcall

/* pipe client and stdio */
AW_IMPORT aw_handle AW_API CreateFileA(const char *name, aw_dword access, aw_dword share, void *security,
                                       aw_dword disposition, aw_dword flags, aw_handle template_file);
AW_IMPORT aw_bool AW_API ReadFile(aw_handle file, void *data, aw_dword size, aw_dword *read, void *overlapped);
AW_IMPORT aw_bool AW_API WriteFile(aw_handle file, const void *data, aw_dword size, aw_dword *written,
                                   void *overlapped);
AW_IMPORT aw_bool AW_API CloseHandle(aw_handle object);
AW_IMPORT aw_bool AW_API WaitNamedPipeA(const char *name, aw_dword timeout);
/* process scaffolding */
AW_IMPORT aw_handle AW_API GetStdHandle(aw_dword which);
AW_IMPORT const uint16_t *AW_API GetCommandLineW(void);
AW_IMPORT aw_dword AW_API GetLastError(void);
AW_IMPORT aw_bool AW_API QueryPerformanceCounter(int64_t *count);
AW_IMPORT aw_bool AW_API QueryPerformanceFrequency(int64_t *frequency);
AW_IMPORT void AW_API Sleep(aw_dword milliseconds);
AW_IMPORT aw_bool AW_API SetConsoleOutputCP(unsigned int code_page);
AW_IMPORT __declspec(noreturn) void AW_API ExitProcess(unsigned int code);
/* wire_server_start: one child process inside a kill-on-close job */
AW_IMPORT aw_dword AW_API GetModuleFileNameW(aw_handle module, uint16_t *path, aw_dword size);
AW_IMPORT aw_bool AW_API CreateProcessW(const uint16_t *application, uint16_t *command_line, void *process_security,
                                        void *thread_security, aw_bool inherit_handles, aw_dword flags,
                                        void *environment, const uint16_t *directory, mw_startup_info *startup,
                                        mw_process_info *info);
AW_IMPORT aw_handle AW_API CreateJobObjectW(void *security, const uint16_t *name);
AW_IMPORT aw_bool AW_API SetInformationJobObject(aw_handle job, int info_class, void *info, aw_dword size);
AW_IMPORT aw_bool AW_API AssignProcessToJobObject(aw_handle job, aw_handle process);
AW_IMPORT aw_dword AW_API ResumeThread(aw_handle thread);
AW_IMPORT aw_bool AW_API TerminateProcess(aw_handle process, unsigned int code);
AW_IMPORT aw_dword AW_API WaitForSingleObject(aw_handle object, aw_dword milliseconds);
AW_IMPORT aw_bool AW_API GetExitCodeProcess(aw_handle process, aw_dword *code);
AW_IMPORT aw_bool AW_API SetHandleInformation(aw_handle object, aw_dword mask, aw_dword flags);

#define AW_INVALID_HANDLE ((aw_handle)(intptr_t)-1)
#define AW_STD_INPUT ((aw_dword)-10)
#define AW_STD_OUTPUT ((aw_dword)-11)
#define AW_STD_ERROR ((aw_dword)-12)
#define AW_CP_UTF8 65001u
#define AW_GENERIC_READ 0x80000000u
#define AW_GENERIC_WRITE 0x40000000u
#define AW_FILE_SHARE_READ_WRITE 0x00000003u
#define AW_OPEN_EXISTING 3u
#define AW_ERROR_FILE_NOT_FOUND 2u
#define AW_ERROR_SEM_TIMEOUT 121u
#define AW_ERROR_PIPE_BUSY 231u
#define AW_WAIT_TIMEOUT 258u
#define AW_HANDLE_FLAG_INHERIT 1u
#define AW_STARTF_USESTDHANDLES 0x00000100u
#define AW_CREATE_SUSPENDED 0x00000004u
#define AW_CREATE_NO_WINDOW 0x08000000u
#define AW_JOB_EXTENDED_LIMIT_INFORMATION 9
#define AW_JOB_LIMIT_KILL_ON_JOB_CLOSE 0x00002000u

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

/* ---- Wire format: the 64-byte packet of agent_wire_ipc.c, unchanged ---------- */

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
AW_STATIC_ASSERT(sizeof(void *) != 8u || sizeof(mw_job_limits) == 144u, mw_job_limits_is_144_bytes_on_x64);
AW_STATIC_ASSERT(sizeof(void *) != 8u || sizeof(mw_startup_info) == 104u, mw_startup_info_is_104_bytes_on_x64);

enum {
    AW_OP_PING = 0x0001,
    AW_OP_TASK = 0x0002,
    AW_OP_STATUS = 0x0003,
    AW_OP_RECEIPT = 0x0004
};

enum {
    AW_ACK_OK = 0,
    AW_ACK_BAD_SIZE = 1,
    AW_ACK_BAD_MAGIC = 2,
    AW_ACK_BAD_VERSION = 3,
    AW_ACK_BAD_OPCODE = 4,
    AW_ACK_BAD_LENGTH = 5,
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
    default: return "?";
    }
}

static uint32_t aw_fnv1a(const unsigned char *data, size_t length, uint32_t hash) {
    for (size_t i = 0; i < length; i++) hash = (hash ^ data[i]) * 0x01000193u;
    return hash;
}
#define AW_FNV_OFFSET 0x811C9DC5u

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

static AW_NOINLINE void aw_packet_init(agent_packet_t *packet, uint32_t opcode, uint32_t project_id, uint32_t sender,
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

typedef struct {
    uint32_t status;
    uint32_t request_checksum;
    uint32_t sequence;
    uint32_t request_opcode;
} aw_receipt_view;

/* Client-side check of a receipt against the request it answers. 0 = accepted, else an AW_ACK_* code,
 * or 100 + status when the server reported a non-OK status, or 200 when the receipt answers another request. */
static uint32_t aw_receipt_check(const agent_packet_t *receipt, const agent_packet_t *request, aw_receipt_view *view) {
    if (receipt->magic != AW_MAGIC) return AW_ACK_BAD_MAGIC;
    if (receipt->version != AW_WIRE_VERSION) return AW_ACK_BAD_VERSION;
    if (receipt->payload_len > AW_PAYLOAD_BYTES) return AW_ACK_BAD_LENGTH;
    if (receipt->checksum != aw_packet_checksum(receipt)) return AW_ACK_BAD_CHECKSUM;
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

/* ---- Small text helpers ------------------------------------------------------- */

static size_t aw_length(const char *text) {
    size_t n = 0;
    while (text[n] != '\0') n++;
    return n;
}

static AW_NOINLINE int aw_equal(const char *a, size_t a_len, const char *b) {
    size_t i = 0;
    while (i < a_len && b[i] != '\0' && a[i] == b[i]) i++;
    return i == a_len && b[i] == '\0';
}

static int aw_same(const char *a, const char *b) { return aw_equal(a, aw_length(a), b); }

/* First position of needle in text[0..n), or NULL. */
static AW_NOINLINE const char *aw_find(const char *text, size_t n, const char *needle) {
    const size_t m = aw_length(needle);
    if (m == 0u || m > n) return NULL;
    for (size_t i = 0; i + m <= n; i++) {
        size_t k = 0;
        while (k < m && text[i + k] == needle[k]) k++;
        if (k == m) return text + i;
    }
    return NULL;
}

/* Append one code point as UTF-8; returns 0 when it does not fit in cap bytes. */
static int aw_utf8_append(char *out, size_t *n, size_t cap, uint32_t code) {
    unsigned char bytes[4];
    size_t count;
    if (code < 0x80u) { bytes[0] = (unsigned char)code; count = 1; }
    else if (code < 0x800u) {
        bytes[0] = (unsigned char)(0xC0u | (code >> 6)); bytes[1] = (unsigned char)(0x80u | (code & 0x3Fu)); count = 2;
    } else if (code < 0x10000u) {
        bytes[0] = (unsigned char)(0xE0u | (code >> 12)); bytes[1] = (unsigned char)(0x80u | ((code >> 6) & 0x3Fu));
        bytes[2] = (unsigned char)(0x80u | (code & 0x3Fu)); count = 3;
    } else {
        bytes[0] = (unsigned char)(0xF0u | (code >> 18)); bytes[1] = (unsigned char)(0x80u | ((code >> 12) & 0x3Fu));
        bytes[2] = (unsigned char)(0x80u | ((code >> 6) & 0x3Fu)); bytes[3] = (unsigned char)(0x80u | (code & 0x3Fu));
        count = 4;
    }
    if (*n + count > cap) return 0;
    for (size_t i = 0; i < count; i++) out[(*n)++] = (char)bytes[i];
    return 1;
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

/* ---- JSON scanner: validates one RFC 8259 document, no allocation ------------- */

typedef struct {
    const char *at;  /* first byte of the value */
    const char *end; /* one past its last byte */
} mj_span;

static const char *mj_ws(const char *p, const char *end) {
    while (p < end && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) p++;
    return p;
}

static int mj_hex(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static uint32_t mj_hex4(const char *p) {
    return ((uint32_t)mj_hex(p[0]) << 12) | ((uint32_t)mj_hex(p[1]) << 8) | ((uint32_t)mj_hex(p[2]) << 4) |
           (uint32_t)mj_hex(p[3]);
}

/* p is at the opening quote. Returns one past the closing quote, or NULL for a control character,
 * a bad escape, ill-formed UTF-8 (overlong, surrogate, above U+10FFFF) or a missing closing quote. */
static AW_NOINLINE const char *mj_string(const char *p, const char *end) {
    p++;
    while (p < end) {
        const unsigned char c = (unsigned char)*p;
        size_t need;
        uint32_t code, low;
        if (c == '"') return p + 1;
        if (c < 0x20u) return NULL;
        if (c == '\\') {
            if (end - p < 2) return NULL;
            if (p[1] == 'u') {
                if (end - p < 6) return NULL;
                for (int i = 2; i < 6; i++)
                    if (mj_hex(p[i]) < 0) return NULL;
                p += 6;
            } else if (p[1] == '"' || p[1] == '\\' || p[1] == '/' || p[1] == 'b' || p[1] == 'f' || p[1] == 'n' ||
                       p[1] == 'r' || p[1] == 't') {
                p += 2;
            } else {
                return NULL;
            }
            continue;
        }
        if (c < 0x80u) { p++; continue; }
        if (c >= 0xC2u && c <= 0xDFu) { need = 1; code = c & 0x1Fu; low = 0x80u; }
        else if (c >= 0xE0u && c <= 0xEFu) { need = 2; code = c & 0x0Fu; low = 0x800u; }
        else if (c >= 0xF0u && c <= 0xF4u) { need = 3; code = c & 0x07u; low = 0x10000u; }
        else return NULL;
        if ((size_t)(end - p) <= need) return NULL;
        for (size_t i = 1; i <= need; i++) {
            const unsigned char cc = (unsigned char)p[i];
            if ((cc & 0xC0u) != 0x80u) return NULL;
            code = (code << 6) | (cc & 0x3Fu);
        }
        if (code < low || code > 0x10FFFFu || (code >= 0xD800u && code <= 0xDFFFu)) return NULL;
        p += need + 1u;
    }
    return NULL;
}

static int mj_digit(const char *p, const char *end) { return p < end && *p >= '0' && *p <= '9'; }

static const char *mj_number(const char *p, const char *end) {
    if (p < end && *p == '-') p++;
    if (!mj_digit(p, end)) return NULL;
    if (*p == '0') p++;
    else while (mj_digit(p, end)) p++;
    if (p < end && *p == '.') {
        p++;
        if (!mj_digit(p, end)) return NULL;
        while (mj_digit(p, end)) p++;
    }
    if (p < end && (*p == 'e' || *p == 'E')) {
        p++;
        if (p < end && (*p == '+' || *p == '-')) p++;
        if (!mj_digit(p, end)) return NULL;
        while (mj_digit(p, end)) p++;
    }
    return p;
}

static const char *mj_literal(const char *p, const char *end, const char *word) {
    const size_t n = aw_length(word);
    if ((size_t)(end - p) < n || !aw_equal(p, n, word)) return NULL;
    return p + n;
}

/* Validates the value that starts at or after p (leading whitespace allowed). Returns one past the
 * value, or NULL. Objects and arrays nest at most MCP_DEPTH_MAX deep. */
static const char *mj_value(const char *p, const char *end, unsigned depth) {
    p = mj_ws(p, end);
    if (p >= end) return NULL;
    switch (*p) {
    case '"':
        return mj_string(p, end);
    case '{':
        if (depth >= MCP_DEPTH_MAX) return NULL;
        p = mj_ws(p + 1, end);
        if (p < end && *p == '}') return p + 1;
        for (;;) {
            p = mj_ws(p, end);
            if (p >= end || *p != '"') return NULL;
            p = mj_string(p, end);
            if (p == NULL) return NULL;
            p = mj_ws(p, end);
            if (p >= end || *p != ':') return NULL;
            p = mj_value(p + 1, end, depth + 1u);
            if (p == NULL) return NULL;
            p = mj_ws(p, end);
            if (p >= end) return NULL;
            if (*p == '}') return p + 1;
            if (*p != ',') return NULL;
            p++;
        }
    case '[':
        if (depth >= MCP_DEPTH_MAX) return NULL;
        p = mj_ws(p + 1, end);
        if (p < end && *p == ']') return p + 1;
        for (;;) {
            p = mj_value(p, end, depth + 1u);
            if (p == NULL) return NULL;
            p = mj_ws(p, end);
            if (p >= end) return NULL;
            if (*p == ']') return p + 1;
            if (*p != ',') return NULL;
            p++;
        }
    case 't':
        return mj_literal(p, end, "true");
    case 'f':
        return mj_literal(p, end, "false");
    case 'n':
        return mj_literal(p, end, "null");
    default:
        return mj_number(p, end);
    }
}

/* Decode a validated string token to UTF-8 bytes. Returns the byte count, or -1 when it exceeds cap.
 * An unpaired \u surrogate becomes U+FFFD. */
static AW_NOINLINE long mj_decode(mj_span string, char *out, size_t cap) {
    const char *p = string.at + 1;
    const char *const end = string.end - 1;
    size_t n = 0;
    while (p < end) {
        uint32_t code;
        if (*p != '\\') {
            if (n >= cap) return -1;
            out[n++] = *p++;
            continue;
        }
        switch (p[1]) {
        case 'b': code = 8u; p += 2; break;
        case 'f': code = 12u; p += 2; break;
        case 'n': code = 10u; p += 2; break;
        case 'r': code = 13u; p += 2; break;
        case 't': code = 9u; p += 2; break;
        case 'u':
            code = mj_hex4(p + 2);
            p += 6;
            if (code >= 0xD800u && code <= 0xDBFFu && end - p >= 6 && p[0] == '\\' && p[1] == 'u') {
                const uint32_t low = mj_hex4(p + 2);
                if (low >= 0xDC00u && low <= 0xDFFFu) {
                    code = 0x10000u + ((code - 0xD800u) << 10) + (low - 0xDC00u);
                    p += 6;
                }
            }
            if (code >= 0xD800u && code <= 0xDFFFu) code = 0xFFFDu;
            break;
        default: /* quote, backslash, slash */
            code = (unsigned char)p[1];
            p += 2;
            break;
        }
        if (!aw_utf8_append(out, &n, cap, code)) return -1;
    }
    return (long)n;
}

static char mj_scratch[64];

/* Does the validated string token decode to exactly `text`? (text shorter than 64 bytes) */
static AW_NOINLINE int mj_string_is(mj_span string, const char *text) {
    const long n = mj_decode(string, mj_scratch, sizeof mj_scratch);
    return n >= 0 && aw_equal(mj_scratch, (size_t)n, text);
}

/* Find a member of a validated object by key; the first match wins. */
static AW_NOINLINE int mj_member(mj_span object, const char *key, mj_span *value) {
    const char *p = mj_ws(object.at + 1, object.end);
    if (*p == '}') return 0;
    for (;;) {
        mj_span name;
        p = mj_ws(p, object.end);
        name.at = p;
        name.end = mj_string(p, object.end);
        p = mj_ws(name.end, object.end) + 1; /* past the colon */
        p = mj_ws(p, object.end);
        value->at = p;
        value->end = mj_value(p, object.end, 0u);
        if (mj_string_is(name, key)) return 1;
        p = mj_ws(value->end, object.end);
        if (*p == '}') return 0;
        p++; /* comma */
    }
}

/* Non-negative integer literal (digits only) that fits in 32 bits. */
static int mj_u32(mj_span number, uint32_t *value) {
    uint64_t v = 0;
    if (number.at >= number.end) return 0;
    for (const char *p = number.at; p < number.end; p++) {
        if (*p < '0' || *p > '9') return 0;
        v = v * 10u + (uint64_t)(*p - '0');
        if (v > 0xFFFFFFFFu) return 0;
    }
    *value = (uint32_t)v;
    return 1;
}

/* ---- JSON writer: bounded buffer, overflow is counted not written -------------- */

typedef struct {
    char *at;
    size_t n; /* bytes produced; exceeds cap when the content did not fit */
    size_t cap;
} mo_buf;

static AW_NOINLINE void mo_bytes(mo_buf *o, const char *data, size_t length) {
    for (size_t i = 0; i < length; i++) {
        if (o->n < o->cap) o->at[o->n] = data[i];
        o->n++;
    }
}

static AW_NOINLINE void mo_text(mo_buf *o, const char *text) { mo_bytes(o, text, aw_length(text)); }

static AW_NOINLINE void mo_u64(mo_buf *o, uint64_t value) {
    char digits[21];
    char *p = digits + 20;
    *p = '\0';
    do {
        *--p = (char)('0' + value % 10u);
        value /= 10u;
    } while (value != 0u);
    mo_text(o, p);
}

static const char mo_hex_digits[] = "0123456789ABCDEF";

/* "0x85BD5F54" including the quotes */
static void mo_hex32(mo_buf *o, uint32_t value) {
    char text[13];
    text[0] = '"'; text[1] = '0'; text[2] = 'x';
    for (int i = 0; i < 8; i++) text[3 + i] = mo_hex_digits[(value >> (28 - 4 * i)) & 0xFu];
    text[11] = '"';
    text[12] = '\0';
    mo_text(o, text);
}

/* A JSON string token for UTF-8 bytes: quotes, backslashes and control characters escaped. */
static AW_NOINLINE void mo_quoted(mo_buf *o, const char *data, size_t length) {
    mo_bytes(o, "\"", 1u);
    for (size_t i = 0; i < length; i++) {
        const unsigned char c = (unsigned char)data[i];
        if (c == '"') mo_bytes(o, "\\\"", 2u);
        else if (c == '\\') mo_bytes(o, "\\\\", 2u);
        else if (c < 0x20u) {
            char escape[6];
            escape[0] = '\\'; escape[1] = 'u'; escape[2] = '0'; escape[3] = '0';
            escape[4] = mo_hex_digits[c >> 4];
            escape[5] = mo_hex_digits[c & 0xFu];
            mo_bytes(o, escape, 6u);
        } else {
            mo_bytes(o, data + i, 1u);
        }
    }
    mo_bytes(o, "\"", 1u);
}

static void mo_quoted_text(mo_buf *o, const char *text) { mo_quoted(o, text, aw_length(text)); }

/* ,"key":  (or "key": for the first member) */
static AW_NOINLINE void mo_key(mo_buf *o, const char *key) {
    if (o->n != 0u && o->n <= o->cap && o->at[o->n - 1u] != '{') mo_bytes(o, ",", 1u);
    mo_bytes(o, "\"", 1u);
    mo_text(o, key);
    mo_bytes(o, "\":", 2u);
}

static void mo_member_u64(mo_buf *o, const char *key, uint64_t value) {
    mo_key(o, key);
    mo_u64(o, value);
}

static void mo_member_text(mo_buf *o, const char *key, const char *value) {
    mo_key(o, key);
    mo_quoted_text(o, value);
}

/* ---- Pipe client: one connection kept open across tool calls ------------------- */

enum { MW_CONNECTED = 0, MW_NO_SERVER = 1, MW_BUSY = 2, MW_ERROR = 3 };

static aw_handle mw_pipe;
static int mw_connected;
static aw_dword mw_last_error;
static uint64_t mw_connects;    /* successful pipe connections */
static uint64_t mw_round_trips; /* accepted receipts */
static uint64_t mw_tool_calls;  /* tools/call requests that named a known tool */
static uint64_t mw_samples[MW_BATCH_MAX];

static const char *mw_state_name(unsigned state) {
    static const char *const names[] = { "connected", "no_server", "busy", "error" };
    return names[state < 4u ? state : 3u];
}

/* Open the pipe unless it is open already. While it does not exist or is busy, retry every
 * millisecond until wait_ms has passed (0 = one attempt). */
static unsigned mw_connect(uint32_t wait_ms) {
    const int64_t start = aw_now();
    if (mw_connected) return MW_CONNECTED;
    for (;;) {
        unsigned state;
        const aw_handle pipe = CreateFileA(AW_PIPE_NAME, AW_GENERIC_READ | AW_GENERIC_WRITE, 0u, NULL,
                                           AW_OPEN_EXISTING, 0u, NULL);
        if (pipe != AW_INVALID_HANDLE) {
            mw_pipe = pipe;
            mw_connected = 1;
            mw_connects++;
            return MW_CONNECTED;
        }
        mw_last_error = GetLastError();
        if (mw_last_error == AW_ERROR_FILE_NOT_FOUND) state = MW_NO_SERVER;
        else if (mw_last_error == AW_ERROR_PIPE_BUSY) state = MW_BUSY;
        else return MW_ERROR;
        if (aw_micros(aw_now() - start) >= (uint64_t)wait_ms * 1000u) return state;
        Sleep(1u);
    }
}

static void mw_disconnect(void) {
    if (mw_connected) (void)CloseHandle(mw_pipe);
    mw_connected = 0;
}

static int aw_write_packet(aw_handle pipe, const agent_packet_t *packet) {
    aw_dword written = 0;
    return WriteFile(pipe, packet, AW_PACKET_BYTES, &written, NULL) != 0 && written == AW_PACKET_BYTES;
}

/* The client end reads in byte mode: accumulate exactly one 64-byte receipt. */
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

/* One request/receipt exchange, timed. Returns 0 accepted, else the aw_receipt_check code or 300 on I/O failure. */
static AW_NOINLINE uint32_t aw_round_trip(const agent_packet_t *request, aw_receipt_view *view, uint64_t *micros) {
    agent_packet_t receipt;
    const int64_t start = aw_now();
    if (!aw_write_packet(mw_pipe, request) || !aw_read_packet(mw_pipe, &receipt)) {
        mw_last_error = GetLastError();
        return 300u;
    }
    *micros = aw_micros(aw_now() - start);
    return aw_receipt_check(&receipt, request, view);
}

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

/* ---- Tools ------------------------------------------------------------------- */

/* Every tool writes one JSON object into `o` and returns 1 when it is an error result. */

static AW_NOINLINE int mw_fail(mo_buf *o, const char *message) {
    o->n = 0;
    mo_text(o, "{");
    mo_member_text(o, "error", message);
    mo_text(o, "}");
    return 1;
}

static void mw_rejection_text(mo_buf *o, const char *key, uint32_t code) {
    mo_key(o, key);
    if (code == 300u) mo_quoted_text(o, "pipe I/O failed");
    else if (code == 200u) mo_quoted_text(o, "receipt answers a different request");
    else if (code >= 100u) {
        mo_text(o, "\"server status ");
        mo_text(o, aw_ack_name(code - 100u));
        mo_text(o, "\"");
    } else {
        mo_text(o, "\"receipt ");
        mo_text(o, aw_ack_name(code));
        mo_text(o, "\"");
    }
}

static void mw_not_connected(mo_buf *o, unsigned state) {
    mo_member_text(o, "state", mw_state_name(state));
    mo_member_u64(o, "win32_error", mw_last_error);
    mo_member_text(o, "hint",
                   state == MW_BUSY
                       ? "the single pipe instance is held by another client; retry after it disconnects"
                       : state == MW_NO_SERVER
                             ? "no pipe server; run agent_wire_ipc --server or call wire_server_start"
                             : "CreateFile on the pipe failed; see win32_error");
}

static aw_handle mw_child;
static int mw_child_live;
static aw_dword mw_child_pid;

static int mw_tool_status(mj_span args, int has_args, mo_buf *o) {
    agent_packet_t request;
    aw_receipt_view view;
    mj_span v;
    uint32_t wait_ms = 0, code = 0;
    uint64_t micros = 0;
    unsigned state;
    memset(&view, 0, sizeof view);
    if (has_args && mj_member(args, "wait_ms", &v) && (!mj_u32(v, &wait_ms) || wait_ms > MW_STATUS_WAIT_MAX))
        return mw_fail(o, "wait_ms must be an integer from 0 to 5000");
    state = mw_connect(wait_ms);
    if (state == MW_CONNECTED) {
        aw_packet_init(&request, AW_OP_PING, AW_DEFAULT_PROJECT, AW_CLIENT_ID, AW_SERVER_ID, NULL, 0u);
        code = aw_round_trip(&request, &view, &micros);
        if (code == 0u) mw_round_trips++;
        else { mw_disconnect(); state = MW_ERROR; }
    }
    mo_text(o, "{");
    mo_member_text(o, "adapter", "agent_wire_mcp");
    mo_member_text(o, "version", MW_VERSION);
    mo_member_text(o, "pipe", AW_PIPE_NAME);
    if (state == MW_CONNECTED) {
        mo_member_text(o, "state", "connected");
        mo_member_u64(o, "rtt_us", micros);
        mo_member_u64(o, "sequence", view.sequence);
    } else if (code != 0u) {
        mo_member_text(o, "state", "error");
        mw_rejection_text(o, "detail", code);
        mo_member_u64(o, "win32_error", code == 300u ? mw_last_error : 0u);
    } else {
        mw_not_connected(o, state);
    }
    mo_member_u64(o, "connects", mw_connects);
    mo_member_u64(o, "round_trips", mw_round_trips);
    mo_member_u64(o, "tool_calls", mw_tool_calls);
    mo_member_u64(o, "server_child_pid", mw_child_live ? mw_child_pid : 0u);
    mo_text(o, "}");
    return 0;
}

static int mw_tool_roundtrip(mj_span args, int has_args, mo_buf *o) {
    static char text[AW_PAYLOAD_BYTES];
    agent_packet_t request;
    aw_receipt_view view;
    mj_span v;
    uint32_t count = 1, opcode = 0, first_sequence = 0;
    uint64_t sum = 0, first_micros = 0, total;
    long text_len = 0;
    unsigned state;
    int64_t started;
    memset(&view, 0, sizeof view);
    if (has_args && mj_member(args, "count", &v) && (!mj_u32(v, &count) || count == 0u || count > MW_BATCH_MAX))
        return mw_fail(o, "count must be an integer from 1 to 16384");
    if (has_args && mj_member(args, "text", &v)) {
        text_len = *v.at == '"' ? mj_decode(v, text, sizeof text) : -1;
        if (text_len < 0) return mw_fail(o, "text must be a string of at most 40 UTF-8 bytes");
    }
    if (has_args && mj_member(args, "opcode", &v)) {
        if (*v.at == '"' && mj_string_is(v, "PING")) opcode = AW_OP_PING;
        else if (*v.at == '"' && mj_string_is(v, "TASK")) opcode = AW_OP_TASK;
        else if (*v.at == '"' && mj_string_is(v, "STATUS")) opcode = AW_OP_STATUS;
        else return mw_fail(o, "opcode must be \"PING\", \"TASK\" or \"STATUS\"");
    } else {
        opcode = text_len > 0 ? AW_OP_TASK : AW_OP_PING;
    }
    state = mw_connect(MW_CONNECT_WAIT_MS);
    if (state != MW_CONNECTED) {
        mo_text(o, "{");
        mo_member_text(o, "error", "not connected to the agent wire pipe");
        mw_not_connected(o, state);
        mo_text(o, "}");
        return 1;
    }
    aw_packet_init(&request, opcode, AW_DEFAULT_PROJECT, AW_CLIENT_ID, AW_SERVER_ID, (const uint8_t *)text,
                   (uint32_t)text_len);
    started = aw_now();
    for (uint32_t i = 0; i < count; i++) {
        const uint32_t code = aw_round_trip(&request, &view, &mw_samples[i]);
        if (code != 0u) {
            mw_disconnect(); /* the session state is unknown: the next call reconnects */
            mw_round_trips += i;
            mo_text(o, "{");
            mw_rejection_text(o, "error", code);
            mo_member_u64(o, "failed_at", (uint64_t)i + 1u);
            mo_member_u64(o, "accepted", i);
            mo_member_u64(o, "count", count);
            mo_member_u64(o, "win32_error", code == 300u ? mw_last_error : 0u);
            mo_text(o, "}");
            return 1;
        }
        if (i == 0u) { first_sequence = view.sequence; first_micros = mw_samples[0]; }
        sum += mw_samples[i];
    }
    total = aw_micros(aw_now() - started);
    mw_round_trips += count;
    aw_sort(mw_samples, count);
    mo_text(o, "{");
    mo_member_text(o, "opcode", aw_opcode_name(opcode));
    mo_member_u64(o, "count", count);
    mo_member_u64(o, "accepted", count);
    mo_member_u64(o, "payload_bytes", (uint64_t)text_len);
    mo_key(o, "request_checksum");
    mo_hex32(o, request.checksum);
    mo_member_u64(o, "first_sequence", first_sequence);
    mo_member_u64(o, "last_sequence", view.sequence);
    mo_key(o, "rtt_us");
    mo_text(o, "{");
    mo_member_u64(o, "first", first_micros);
    mo_member_u64(o, "min", mw_samples[0]);
    mo_member_u64(o, "p50", mw_samples[count / 2u]);
    mo_member_u64(o, "avg", sum / count);
    mo_member_u64(o, "p99", mw_samples[(count * 99u) / 100u]);
    mo_member_u64(o, "max", mw_samples[count - 1u]);
    mo_text(o, "}");
    mo_member_u64(o, "total_us", total);
    mo_text(o, "}");
    return 0;
}

static int mw_tool_close(mo_buf *o) {
    mo_text(o, "{");
    mo_member_text(o, "state", "closed");
    mo_key(o, "was_connected");
    mo_text(o, mw_connected ? "true" : "false");
    mo_text(o, "}");
    mw_disconnect();
    return 0;
}

/* ---- wire_server_start: the sibling agent_wire_ipc.exe, bound to this process -- */

static aw_handle mw_job;
static int mw_job_ready;
static uint16_t mw_exe_wide[MW_PATH_MAX];
static uint16_t mw_command[MW_PATH_MAX + 32u];
static char mw_exe_utf8[MW_PATH_MAX * 3u];

static void mw_wide_append(uint16_t *out, size_t *n, const char *ascii) {
    while (*ascii != '\0') out[(*n)++] = (uint16_t)(unsigned char)*ascii++;
}

/* Is the server this adapter started still running? Reaps the handle when it has exited. */
static int mw_child_running(aw_dword *exit_code) {
    if (!mw_child_live) return 0;
    if (WaitForSingleObject(mw_child, 0u) == AW_WAIT_TIMEOUT) return 1;
    if (exit_code != NULL) (void)GetExitCodeProcess(mw_child, exit_code);
    (void)CloseHandle(mw_child);
    mw_child_live = 0;
    return 0;
}

/* Does any process serve the pipe right now? WaitNamedPipe does not connect, so it does not
 * consume one of the server's sessions. */
static int mw_pipe_exists(void) {
    if (mw_connected) return 1;
    if (WaitNamedPipeA(AW_PIPE_NAME, 1u) != 0) return 1;
    return GetLastError() == AW_ERROR_SEM_TIMEOUT; /* exists, its instance is busy */
}

/* Fill mw_exe_wide / mw_exe_utf8 with <directory of this exe>\agent_wire_ipc.exe. */
static int mw_sibling_path(void) {
    const aw_dword length = GetModuleFileNameW(NULL, mw_exe_wide, MW_PATH_MAX);
    size_t cut = 0, n, utf8 = 0;
    if (length == 0u || length >= MW_PATH_MAX - 20u) return 0;
    for (size_t i = 0; i < length; i++)
        if (mw_exe_wide[i] == '\\' || mw_exe_wide[i] == '/') cut = i + 1u;
    n = cut;
    mw_wide_append(mw_exe_wide, &n, "agent_wire_ipc.exe");
    mw_exe_wide[n] = 0;
    for (size_t i = 0; i < n; i++) {
        uint32_t code = mw_exe_wide[i];
        if (code >= 0xD800u && code <= 0xDBFFu && i + 1u < n && mw_exe_wide[i + 1u] >= 0xDC00u &&
            mw_exe_wide[i + 1u] <= 0xDFFFu) {
            code = 0x10000u + ((code - 0xD800u) << 10) + (uint32_t)(mw_exe_wide[i + 1u] - 0xDC00u);
            i++;
        } else if (code >= 0xD800u && code <= 0xDFFFu) {
            code = 0xFFFDu;
        }
        (void)aw_utf8_append(mw_exe_utf8, &utf8, sizeof mw_exe_utf8 - 1u, code);
    }
    mw_exe_utf8[utf8] = '\0';
    return 1;
}

static int mw_spawn_failed(mo_buf *o, const char *what, aw_dword error) {
    o->n = 0;
    mo_text(o, "{");
    mo_member_text(o, "error", what);
    mo_member_u64(o, "win32_error", error);
    mo_member_text(o, "exe", mw_exe_utf8);
    mo_text(o, "}");
    return 1;
}

static int mw_tool_server_start(mj_span args, int has_args, mo_buf *o) {
    mw_security inheritable;
    mw_startup_info startup;
    mw_process_info info;
    mw_job_limits limits;
    mj_span v;
    aw_handle nul;
    aw_dword error, exit_code = 0;
    uint32_t sessions = 0;
    size_t n = 0;
    char digits[21];
    char *digit = digits + 20;
    int ready = 0;

    if (has_args && mj_member(args, "sessions", &v) && !mj_u32(v, &sessions))
        return mw_fail(o, "sessions must be an integer from 0 to 4294967295 (0 = until this adapter exits)");

    if (mw_child_running(NULL) || mw_pipe_exists()) {
        mo_text(o, "{");
        mo_member_text(o, "state", "already_running");
        mo_member_text(o, "owner", mw_child_live ? "this adapter" : "another process");
        mo_member_u64(o, "pid", mw_child_live ? mw_child_pid : 0u);
        mo_text(o, "}");
        return 0;
    }

    if (!mw_sibling_path()) return mw_fail(o, "the adapter path is too long to locate agent_wire_ipc.exe");
    mw_command[n++] = '"';
    for (size_t i = 0; mw_exe_wide[i] != 0; i++) mw_command[n++] = mw_exe_wide[i];
    mw_wide_append(mw_command, &n, "\" --server ");
    *digit = '\0';
    do {
        *--digit = (char)('0' + sessions % 10u);
        sessions /= 10u;
    } while (sessions != 0u);
    mw_wide_append(mw_command, &n, digit);
    mw_command[n] = 0;

    if (!mw_job_ready) {
        mw_job = CreateJobObjectW(NULL, NULL);
        if (mw_job == NULL) return mw_spawn_failed(o, "CreateJobObject failed", GetLastError());
        memset(&limits, 0, sizeof limits);
        limits.limit_flags = AW_JOB_LIMIT_KILL_ON_JOB_CLOSE;
        if (SetInformationJobObject(mw_job, AW_JOB_EXTENDED_LIMIT_INFORMATION, &limits, (aw_dword)sizeof limits) == 0) {
            error = GetLastError();
            (void)CloseHandle(mw_job);
            return mw_spawn_failed(o, "SetInformationJobObject failed", error);
        }
        mw_job_ready = 1;
    }

    /* The child must never share the MCP channel: this process's standard handles stop being
     * inheritable and the child's three standard handles are the NUL device. */
    (void)SetHandleInformation(GetStdHandle(AW_STD_INPUT), AW_HANDLE_FLAG_INHERIT, 0u);
    (void)SetHandleInformation(GetStdHandle(AW_STD_OUTPUT), AW_HANDLE_FLAG_INHERIT, 0u);
    (void)SetHandleInformation(GetStdHandle(AW_STD_ERROR), AW_HANDLE_FLAG_INHERIT, 0u);
    memset(&inheritable, 0, sizeof inheritable);
    inheritable.length = (aw_dword)sizeof inheritable;
    inheritable.inherit = 1;
    nul = CreateFileA("NUL", AW_GENERIC_READ | AW_GENERIC_WRITE, AW_FILE_SHARE_READ_WRITE, &inheritable,
                      AW_OPEN_EXISTING, 0u, NULL);
    if (nul == AW_INVALID_HANDLE) return mw_spawn_failed(o, "opening NUL failed", GetLastError());

    memset(&startup, 0, sizeof startup);
    memset(&info, 0, sizeof info);
    startup.cb = (aw_dword)sizeof startup;
    startup.flags = AW_STARTF_USESTDHANDLES;
    startup.std_input = nul;
    startup.std_output = nul;
    startup.std_error = nul;
    if (CreateProcessW(mw_exe_wide, mw_command, NULL, NULL, 1, AW_CREATE_NO_WINDOW | AW_CREATE_SUSPENDED, NULL, NULL,
                       &startup, &info) == 0) {
        error = GetLastError();
        (void)CloseHandle(nul);
        return mw_spawn_failed(o, "CreateProcess failed for the sibling agent_wire_ipc.exe", error);
    }
    (void)CloseHandle(nul);
    if (AssignProcessToJobObject(mw_job, info.process) == 0) {
        error = GetLastError();
        (void)TerminateProcess(info.process, 1u); /* never leave a server that would outlive the adapter */
        (void)CloseHandle(info.thread);
        (void)CloseHandle(info.process);
        return mw_spawn_failed(o, "AssignProcessToJobObject failed; the server was not started", error);
    }
    (void)ResumeThread(info.thread);
    (void)CloseHandle(info.thread);
    mw_child = info.process;
    mw_child_pid = info.process_id;
    mw_child_live = 1;

    for (const int64_t start = aw_now(); aw_micros(aw_now() - start) < (uint64_t)MW_CONNECT_WAIT_MS * 1000u;) {
        if (mw_pipe_exists()) { ready = 1; break; }
        if (!mw_child_running(&exit_code)) break;
        Sleep(1u);
    }
    if (ready && !mw_child_running(&exit_code)) ready = 0; /* the pipe belongs to someone else */
    if (!mw_child_live) {
        mo_text(o, "{");
        mo_member_text(o, "error", "agent_wire_ipc --server exited before serving the pipe");
        mo_member_u64(o, "exit_code", exit_code);
        mo_member_text(o, "exe", mw_exe_utf8);
        mo_text(o, "}");
        return 1;
    }
    mo_text(o, "{");
    mo_member_text(o, "state", "started");
    mo_member_u64(o, "pid", mw_child_pid);
    mo_key(o, "ready");
    mo_text(o, ready ? "true" : "false");
    mo_member_text(o, "exe", mw_exe_utf8);
    mo_member_text(o, "lifetime", "job object with kill-on-close: the server ends when this adapter exits");
    mo_text(o, "}");
    return 0;
}

/* ---- MCP: JSON-RPC 2.0 dispatch ------------------------------------------------ */

static const char *const mcp_versions[] = { "2025-11-25", "2025-06-18", "2025-03-26", "2024-11-05" };
#define MCP_VERSION_COUNT (sizeof mcp_versions / sizeof mcp_versions[0])

/* tools/list entries. Each literal stays under the 4,095 characters ISO C guarantees. */
static const char *const mcp_tools[] = {
    "{\"name\":\"wire_status\",\"title\":\"Agent wire status\","
    "\"description\":\"Check the agent_wire_ipc named pipe. Connects if this adapter is not connected yet "
    "(waiting up to wait_ms for the pipe), sends one 64-byte PING and reports the state (connected, no_server, "
    "busy or error), the measured round-trip time in microseconds, the server sequence number and the adapter "
    "counters. The connection stays open for later calls; wire_close releases it.\","
    "\"inputSchema\":{\"type\":\"object\",\"properties\":{\"wait_ms\":{\"type\":\"integer\",\"minimum\":0,"
    "\"maximum\":5000,\"description\":\"How long to wait while the pipe does not exist or is busy. Default 0.\"}},"
    "\"additionalProperties\":false},"
    "\"annotations\":{\"readOnlyHint\":true,\"destructiveHint\":false,\"idempotentHint\":true,"
    "\"openWorldHint\":false}}",

    "{\"name\":\"wire_roundtrip\",\"title\":\"Agent wire round trip\","
    "\"description\":\"Send count 64-byte packets through the agent_wire_ipc named pipe and verify the RECEIPT of "
    "each one (magic, version, FNV-1a checksum, echoed request checksum, opcode, project and receiver). Returns "
    "the accepted count, the server sequence numbers and the round-trip time in microseconds (first, min, p50, "
    "avg, p99, max) measured with QueryPerformanceCounter around WriteFile and ReadFile. Waits up to 3 seconds "
    "for the pipe when no server is ready.\","
    "\"inputSchema\":{\"type\":\"object\",\"properties\":{"
    "\"count\":{\"type\":\"integer\",\"minimum\":1,\"maximum\":16384,\"description\":\"Round trips. Default 1.\"},"
    "\"opcode\":{\"type\":\"string\",\"enum\":[\"PING\",\"TASK\",\"STATUS\"],"
    "\"description\":\"Packet opcode. Default TASK when text is given, otherwise PING.\"},"
    "\"text\":{\"type\":\"string\",\"description\":\"Payload, at most 40 bytes of UTF-8.\"}},"
    "\"additionalProperties\":false},"
    "\"annotations\":{\"readOnlyHint\":false,\"destructiveHint\":false,\"idempotentHint\":false,"
    "\"openWorldHint\":false}}",

    "{\"name\":\"wire_server_start\",\"title\":\"Start the agent wire server\","
    "\"description\":\"Start agent_wire_ipc.exe --server from the directory of this adapter, unless the pipe is "
    "already served. Nothing else can be launched: the executable name and its arguments are fixed. The server "
    "has no window, its standard handles are the NUL device, and it runs in a job object that ends it when "
    "this adapter exits. There is no stop tool.\","
    "\"inputSchema\":{\"type\":\"object\",\"properties\":{\"sessions\":{\"type\":\"integer\",\"minimum\":0,"
    "\"maximum\":4294967295,\"description\":\"Client sessions to serve before the server exits. Default 0: until "
    "this adapter exits.\"}},\"additionalProperties\":false},"
    "\"annotations\":{\"readOnlyHint\":false,\"destructiveHint\":false,\"idempotentHint\":true,"
    "\"openWorldHint\":false}}",

    "{\"name\":\"wire_close\",\"title\":\"Release the agent wire pipe\","
    "\"description\":\"Close this adapter's pipe connection so that another client can use the single pipe "
    "instance. The next wire_status or wire_roundtrip call connects again as a new server session.\","
    "\"inputSchema\":{\"type\":\"object\",\"properties\":{},\"additionalProperties\":false},"
    "\"annotations\":{\"readOnlyHint\":false,\"destructiveHint\":false,\"idempotentHint\":true,"
    "\"openWorldHint\":false}}"
};
#define MCP_TOOL_COUNT (sizeof mcp_tools / sizeof mcp_tools[0])

static int mcp_initialized; /* notifications/initialized seen */
static char mcp_inner[MCP_INNER_MAX];
static char mcp_method[64];
static char mcp_name[64];

/* {"jsonrpc":"2.0","id":<id or null>, */
static AW_NOINLINE void mcp_open(mo_buf *o, const mj_span *id) {
    o->n = 0;
    mo_text(o, "{\"jsonrpc\":\"2.0\",\"id\":");
    if (id != NULL) mo_bytes(o, id->at, (size_t)(id->end - id->at));
    else mo_text(o, "null");
    mo_text(o, ",");
}

static AW_NOINLINE void mcp_error(mo_buf *o, const mj_span *id, const char *code, const char *message) {
    mcp_open(o, id);
    mo_text(o, "\"error\":{\"code\":");
    mo_text(o, code);
    mo_text(o, ",\"message\":");
    mo_quoted_text(o, message);
    mo_text(o, "}}\n");
}

static void mcp_initialize(mo_buf *o, const mj_span *id, mj_span params, int has_params) {
    mj_span requested;
    const char *version = mcp_versions[0];
    if (!has_params || !mj_member(params, "protocolVersion", &requested) || *requested.at != '"') {
        mcp_error(o, id, "-32602", "Invalid params: initialize requires a protocolVersion string");
        return;
    }
    for (size_t i = 0; i < MCP_VERSION_COUNT; i++)
        if (mj_string_is(requested, mcp_versions[i])) version = mcp_versions[i];
    mcp_initialized = 0;
    mcp_open(o, id);
    mo_text(o, "\"result\":{\"protocolVersion\":\"");
    mo_text(o, version);
    mo_text(o, "\",\"capabilities\":{\"tools\":{\"listChanged\":false}},"
               "\"serverInfo\":{\"name\":\"agent_wire_mcp\",\"title\":\"Ultra-Brain Agent Wire\",\"version\":\""
               MW_VERSION "\"},"
               "\"instructions\":\"Local Windows named-pipe agent IPC. Call wire_status first. If its state is "
               "no_server, call wire_server_start (or run agent_wire_ipc --server yourself), then wire_roundtrip. "
               "Payloads are limited to 40 bytes; times are microseconds measured inside this adapter.\"}}\n");
}

static void mcp_tools_list(mo_buf *o, const mj_span *id) {
    mcp_open(o, id);
    mo_text(o, "\"result\":{\"tools\":[");
    for (size_t i = 0; i < MCP_TOOL_COUNT; i++) {
        if (i != 0u) mo_text(o, ",");
        mo_text(o, mcp_tools[i]);
    }
    mo_text(o, "]}}\n");
}

static void mcp_tools_call(mo_buf *o, const mj_span *id, mj_span params, int has_params) {
    mo_buf inner;
    mj_span name, args;
    long name_len;
    int has_args, is_error, tool;
    args.at = NULL;
    args.end = NULL;
    if (!has_params || !mj_member(params, "name", &name) || *name.at != '"') {
        mcp_error(o, id, "-32602", "Invalid params: tools/call requires a name string");
        return;
    }
    has_args = mj_member(params, "arguments", &args);
    if (has_args && *args.at == 'n') has_args = 0; /* null: no arguments */
    if (has_args && *args.at != '{') {
        mcp_error(o, id, "-32602", "Invalid params: arguments must be an object");
        return;
    }
    name_len = mj_decode(name, mcp_name, sizeof mcp_name);
    inner.at = mcp_inner;
    inner.n = 0;
    inner.cap = sizeof mcp_inner;
    if (name_len < 0) name_len = 0;
    if (aw_equal(mcp_name, (size_t)name_len, "wire_status")) tool = 1;
    else if (aw_equal(mcp_name, (size_t)name_len, "wire_roundtrip")) tool = 2;
    else if (aw_equal(mcp_name, (size_t)name_len, "wire_server_start")) tool = 3;
    else if (aw_equal(mcp_name, (size_t)name_len, "wire_close")) tool = 4;
    else {
        mcp_error(o, id, "-32602", "Unknown tool");
        return;
    }
    mw_tool_calls++; /* counted before it runs: wire_status reports the call it is answering */
    if (tool == 1) is_error = mw_tool_status(args, has_args, &inner);
    else if (tool == 2) is_error = mw_tool_roundtrip(args, has_args, &inner);
    else if (tool == 3) is_error = mw_tool_server_start(args, has_args, &inner);
    else is_error = mw_tool_close(&inner);
    if (inner.n > inner.cap) is_error = mw_fail(&inner, "internal: tool result exceeds the result buffer");
    mcp_open(o, id);
    mo_text(o, "\"result\":{\"content\":[{\"type\":\"text\",\"text\":");
    mo_quoted(o, inner.at, inner.n);
    mo_text(o, "}],\"structuredContent\":");
    mo_bytes(o, inner.at, inner.n);
    mo_text(o, is_error ? ",\"isError\":true}}\n" : ",\"isError\":false}}\n");
}

/* Handle one line. Writes at most one response (ending in a newline) and returns its length; 0 means
 * nothing is sent: a notification, a response from the host, or a blank line. */
static AW_NOINLINE size_t mcp_handle(const char *line, size_t length, char *out, size_t cap) {
    const char *const end = line + length;
    const char *p = mj_ws(line, end);
    const char *q;
    mj_span root, id, value, params;
    const mj_span *reply_id = NULL;
    mo_buf o;
    long method_len;
    int has_id, has_params;

    o.at = out;
    o.n = 0;
    o.cap = cap;
    params.at = NULL;
    params.end = NULL;
    if (p == end) return 0;

    q = mj_value(p, end, 0u);
    if (q == NULL || mj_ws(q, end) != end) {
        mcp_error(&o, NULL, "-32700", "Parse error");
    } else if (*p == '[') {
        mcp_error(&o, NULL, "-32600", "Invalid Request: JSON-RPC batch arrays are not supported");
    } else if (*p != '{') {
        mcp_error(&o, NULL, "-32600", "Invalid Request: a JSON-RPC message is an object");
    } else {
        root.at = p;
        root.end = q;
        has_id = mj_member(root, "id", &id);
        if (has_id && (*id.at == '"' || *id.at == '-' || (*id.at >= '0' && *id.at <= '9'))) reply_id = &id;
        if (has_id && reply_id == NULL) {
            mcp_error(&o, NULL, "-32600", "Invalid Request: id must be a string or a number");
        } else if (!mj_member(root, "jsonrpc", &value) || *value.at != '"' || !mj_string_is(value, "2.0")) {
            mcp_error(&o, reply_id, "-32600", "Invalid Request: jsonrpc must be \"2.0\"");
        } else if (!mj_member(root, "method", &value)) {
            /* a response to a request: this server sends none, so there is nothing to match */
            if (mj_member(root, "result", &value) || mj_member(root, "error", &value)) return 0;
            mcp_error(&o, reply_id, "-32600", "Invalid Request: method is missing");
        } else if (*value.at != '"') {
            mcp_error(&o, reply_id, "-32600", "Invalid Request: method must be a string");
        } else {
            method_len = mj_decode(value, mcp_method, sizeof mcp_method);
            if (method_len < 0) method_len = 0;
            has_params = mj_member(root, "params", &params) && *params.at == '{';
            if (!has_id) {
                if (aw_equal(mcp_method, (size_t)method_len, "notifications/initialized")) mcp_initialized = 1;
                return 0; /* notifications are never answered */
            }
            if (aw_equal(mcp_method, (size_t)method_len, "initialize")) mcp_initialize(&o, reply_id, params, has_params);
            else if (aw_equal(mcp_method, (size_t)method_len, "ping")) {
                mcp_open(&o, reply_id);
                mo_text(&o, "\"result\":{}}\n");
            } else if (aw_equal(mcp_method, (size_t)method_len, "tools/list")) mcp_tools_list(&o, reply_id);
            else if (aw_equal(mcp_method, (size_t)method_len, "tools/call")) mcp_tools_call(&o, reply_id, params, has_params);
            else mcp_error(&o, reply_id, "-32601", "Method not found");
        }
    }
    if (o.n > o.cap) mcp_error(&o, NULL, "-32603", "Internal error: the response exceeds the output buffer");
    return o.n;
}

/* ---- stdio loop ---------------------------------------------------------------- */

static char mcp_in[MCP_LINE_MAX];
static char mcp_out[MCP_OUT_MAX];

static int mcp_send(aw_handle out, const char *data, size_t length) {
    while (length != 0u) {
        aw_dword written = 0;
        if (WriteFile(out, data, (aw_dword)length, &written, NULL) == 0 || written == 0u) return 0;
        data += written;
        length -= written;
    }
    return 1;
}

static unsigned int mcp_serve(void) {
    const aw_handle in = GetStdHandle(AW_STD_INPUT), out = GetStdHandle(AW_STD_OUTPUT);
    size_t used = 0;
    int discarding = 0;
    unsigned int exit_code = 0u;
    static const char ready[] = "agent_wire_mcp " MW_VERSION " ready: MCP over stdio, pipe " AW_PIPE_NAME "\n";
    aw_dword ignored = 0;
    (void)WriteFile(GetStdHandle(AW_STD_ERROR), ready, (aw_dword)(sizeof ready - 1u), &ignored, NULL);
    for (;;) {
        aw_dword got = 0;
        size_t start = 0, scan;
        if (used == sizeof mcp_in) { /* a line longer than the buffer: drop it up to its newline */
            discarding = 1;
            used = 0;
        }
        scan = used;
        if (ReadFile(in, mcp_in + used, (aw_dword)(sizeof mcp_in - used), &got, NULL) == 0 || got == 0u) break;
        used += got;
        for (size_t i = scan; i < used; i++) {
            size_t reply;
            if (mcp_in[i] != '\n') continue;
            if (discarding) {
                mo_buf o;
                o.at = mcp_out;
                o.n = 0;
                o.cap = sizeof mcp_out;
                mcp_error(&o, NULL, "-32600", "Invalid Request: the message exceeds the 65536-byte line buffer");
                reply = o.n;
                discarding = 0;
            } else {
                size_t line_end = i;
                if (line_end > start && mcp_in[line_end - 1u] == '\r') line_end--;
                reply = mcp_handle(mcp_in + start, line_end - start, mcp_out, sizeof mcp_out);
            }
            start = i + 1u;
            if (reply != 0u && !mcp_send(out, mcp_out, reply)) { exit_code = 1u; break; }
        }
        if (exit_code != 0u) break;
        if (start != 0u) {
            for (size_t i = start; i < used; i++) mcp_in[i - start] = mcp_in[i];
            used -= start;
        }
    }
    mw_disconnect();
    return exit_code; /* process exit closes the job handle, which ends a server started by wire_server_start */
}

/* ---- Self-test ------------------------------------------------------------------ */

static void st_write(const char *text) {
    aw_dword written = 0;
    (void)WriteFile(GetStdHandle(AW_STD_OUTPUT), text, (aw_dword)aw_length(text), &written, NULL);
}

static AW_NOINLINE void st_check(const char *name, int ok, int *passed) {
    st_write(ok ? "  PASS  " : "  FAIL  ");
    st_write(name);
    st_write("\n");
    *passed += ok ? 1 : 0;
}

static size_t st_reply; /* length of the last response in mcp_out */

static AW_NOINLINE void st_call(const char *line) { st_reply = mcp_handle(line, aw_length(line), mcp_out, sizeof mcp_out); }

static AW_NOINLINE int st_has(const char *needle) { return aw_find(mcp_out, st_reply, needle) != NULL; }

/* The last response is exactly one valid JSON value followed by one newline. */
static AW_NOINLINE int st_valid(void) {
    if (st_reply < 2u || mcp_out[st_reply - 1u] != '\n') return 0;
    for (size_t i = 0; i + 1u < st_reply; i++)
        if (mcp_out[i] == '\n') return 0;
    return mj_value(mcp_out, mcp_out + st_reply - 1u, 0u) == mcp_out + st_reply - 1u;
}

static AW_NOINLINE int st_json(const char *text) {
    const char *const end = text + aw_length(text);
    const char *const q = mj_value(text, end, 0u);
    return q != NULL && mj_ws(q, end) == end;
}

static unsigned int mcp_selftest(void) {
    agent_packet_t packet, receipt;
    aw_receipt_view view;
    uint8_t body[AW_RECEIPT_PAYLOAD_LEN];
    char text[64];
    mj_span span, found;
    mo_buf o;
    uint32_t number = 0;
    long n;
    int passed = 0, ok;
    static const uint8_t hello[5] = { 'h', 'e', 'l', 'l', 'o' };
    static const char utf8_bad_overlong[] = "\"\xC0\xAF\"";
    static const char utf8_bad_surrogate[] = "\"\xED\xA0\x80\"";
    static const char utf8_ok[] = "\"\xED\x95\x9C\""; /* U+D55C */
    static const char escapes[] = "\"a\\n\\u00e9\\ud83d\\ude00\\\"\\\\\"";
    static const char decoded[] = "a\n\xC3\xA9\xF0\x9F\x98\x80\"\\"; /* a, LF, U+00E9, U+1F600, quote, backslash */
    static const char deep33[] = "[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[1]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]";
    static const char deep32[] = "[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[1]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]";

    st_write("agent_wire_mcp " MW_VERSION " self-test\n");

    aw_packet_init(&packet, AW_OP_PING, AW_DEFAULT_PROJECT, AW_CLIENT_ID, AW_SERVER_ID, NULL, 0u);
    ok = packet.checksum == 0x85BD5F54u;
    aw_packet_init(&packet, AW_OP_TASK, AW_DEFAULT_PROJECT, AW_CLIENT_ID, AW_SERVER_ID, hello, 5u);
    st_check("Wire: 64-byte packet, golden PING 0x85BD5F54 and TASK \"hello\" 0x716CD786 as in agent_wire_ipc",
             ok && packet.checksum == 0x716CD786u && sizeof(agent_packet_t) == 64u, &passed);

    aw_put_u32(body, AW_ACK_OK);
    aw_put_u32(body + 4, packet.checksum);
    aw_put_u32(body + 8, 7u);
    aw_put_u16(body + 12, AW_OP_TASK);
    aw_packet_init(&receipt, AW_OP_RECEIPT, AW_DEFAULT_PROJECT, AW_SERVER_ID, AW_CLIENT_ID, body, AW_RECEIPT_PAYLOAD_LEN);
    ok = aw_receipt_check(&receipt, &packet, &view) == 0u && view.sequence == 7u;
    aw_put_u32(body, AW_ACK_BAD_CHECKSUM);
    aw_packet_init(&receipt, AW_OP_RECEIPT, AW_DEFAULT_PROJECT, AW_SERVER_ID, AW_CLIENT_ID, body, AW_RECEIPT_PAYLOAD_LEN);
    ok = ok && aw_receipt_check(&receipt, &packet, &view) == 106u;
    receipt.payload[20] ^= 1u;
    st_check("Receipt: accepted with sequence 7; server status BAD_CHECKSUM -> 106; damaged receipt -> BAD_CHECKSUM",
             ok && aw_receipt_check(&receipt, &packet, &view) == AW_ACK_BAD_CHECKSUM, &passed);

    st_check("JSON accepts: nested object, numbers -0 1.5e-3, literals, empty containers, depth 32",
             st_json("{\"a\":[1,-0,1.5e-3,true,false,null,{}],\"b\":{\"c\":[]}}") && st_json(" [ ] ") && st_json(deep32),
             &passed);

    st_check("JSON rejects: trailing comma, 01, 1., bare word, unterminated string, raw newline, \\x, depth 33, two values",
             !st_json("{\"a\":1,}") && !st_json("01") && !st_json("1.") && !st_json("nul") && !st_json("\"abc") &&
                 !st_json("\"a\nb\"") && !st_json("\"\\x41\"") && !st_json(deep33) && !st_json("1 2") &&
                 !st_json("{\"a\" 1}") && !st_json("[1 2]") && !st_json(""),
             &passed);

    st_check("JSON strings: UTF-8 U+D55C accepted, overlong C0 AF and surrogate ED A0 80 rejected",
             st_json(utf8_ok) && !st_json(utf8_bad_overlong) && !st_json(utf8_bad_surrogate), &passed);

    span.at = escapes;
    span.end = escapes + sizeof escapes - 1u;
    n = mj_decode(span, text, sizeof text);
    ok = n == 10 && aw_equal(text, 10u, decoded) && mj_decode(span, text, 9u) == -1;
    span.at = "{\"x\":{\"count\":1},\"\\u0063ount\":4096,\"count\":7}";
    span.end = span.at + aw_length(span.at);
    found.at = NULL;
    found.end = NULL;
    ok = ok && mj_member(span, "count", &found) && mj_u32(found, &number) && number == 4096u &&
         !mj_member(span, "missing", &found);
    span.at = "4294967296";
    span.end = span.at + 10;
    st_check("JSON decode: escapes and a surrogate pair to UTF-8, bounded; member lookup by decoded key; u32 overflow",
             ok && !mj_u32(span, &number), &passed);

    o.at = text;
    o.n = 0;
    o.cap = sizeof text;
    mo_quoted(&o, "a\"b\\c\n", 6u);
    mo_hex32(&o, 0x85BD5F54u);
    mo_u64(&o, 18446744073709551615u);
    st_check("JSON writer: quote, backslash and control escapes; hex32; u64 maximum",
             o.n == 47u && aw_equal(text, 47u,"\"a\\\"b\\\\c\\u000A\"\"0x85BD5F54\"18446744073709551615"), &passed);

    st_call("{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"initialize\",\"params\":{\"protocolVersion\":\"2025-06-18\","
            "\"capabilities\":{},\"clientInfo\":{\"name\":\"selftest\",\"version\":\"0\"}}}");
    ok = st_valid() && st_has("\"id\":1,") && st_has("\"protocolVersion\":\"2025-06-18\"") &&
         st_has("\"capabilities\":{\"tools\":{") && st_has("\"serverInfo\":{\"name\":\"agent_wire_mcp\"");
    st_call("{\"jsonrpc\":\"2.0\",\"id\":\"v\",\"method\":\"initialize\",\"params\":{\"protocolVersion\":\"1999-01-01\"}}");
    ok = ok && st_valid() && st_has("\"id\":\"v\",") && st_has("\"protocolVersion\":\"2025-11-25\"");
    st_call("{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"initialize\"}");
    st_check("initialize: echoes a supported protocolVersion, offers 2025-11-25 otherwise, -32602 without params",
             ok && st_valid() && st_has("\"code\":-32602"), &passed);

    st_call("{\"jsonrpc\":\"2.0\",\"method\":\"notifications/initialized\"}");
    ok = st_reply == 0u && mcp_initialized == 1;
    st_call("{\"jsonrpc\":\"2.0\",\"method\":\"notifications/cancelled\",\"params\":{\"requestId\":9}}");
    ok = ok && st_reply == 0u;
    st_call("{\"jsonrpc\":\"2.0\",\"method\":\"tools/call\",\"params\":{\"name\":\"wire_close\"}}");
    ok = ok && st_reply == 0u && mw_tool_calls == 0u;
    st_call("{\"jsonrpc\":\"2.0\",\"id\":7,\"result\":{}}");
    ok = ok && st_reply == 0u;
    st_call("   ");
    ok = ok && st_reply == 0u;
    st_call("{\"jsonrpc\":\"2.0\",\"id\":\"p-1\",\"method\":\"ping\"}");
    st_check("Notifications, host responses and blank lines get no reply; ping -> {} with the string id echoed",
             ok && st_valid() && st_has("{\"jsonrpc\":\"2.0\",\"id\":\"p-1\",\"result\":{}}"), &passed);

    st_call("{\"jsonrpc\":\"2.0\",\"id\":3,\"method\":\"tools/list\",\"params\":{}}");
    st_check("tools/list: valid JSON with wire_status, wire_roundtrip, wire_server_start, wire_close and schemas",
             st_valid() && st_has("\"name\":\"wire_status\"") && st_has("\"name\":\"wire_roundtrip\"") &&
                 st_has("\"name\":\"wire_server_start\"") && st_has("\"name\":\"wire_close\"") &&
                 st_has("\"inputSchema\":{\"type\":\"object\""),
             &passed);

    st_call("{\"jsonrpc\":\"2.0\",\"id\":4,\"method\":");
    ok = st_valid() && st_has("\"id\":null,\"error\":{\"code\":-32700");
    st_call("[{\"jsonrpc\":\"2.0\",\"id\":4,\"method\":\"ping\"}]");
    ok = ok && st_valid() && st_has("\"id\":null,\"error\":{\"code\":-32600");
    st_call("{\"jsonrpc\":\"1.0\",\"id\":5,\"method\":\"ping\"}");
    ok = ok && st_valid() && st_has("\"id\":5,\"error\":{\"code\":-32600");
    st_call("{\"jsonrpc\":\"2.0\",\"id\":true,\"method\":\"ping\"}");
    ok = ok && st_valid() && st_has("\"id\":null,\"error\":{\"code\":-32600");
    st_call("{\"jsonrpc\":\"2.0\",\"id\":6,\"method\":\"resources/list\"}");
    st_check("Errors: parse -32700 (id null), batch and bad jsonrpc/id -32600, unknown method -32601",
             ok && st_valid() && st_has("\"id\":6,\"error\":{\"code\":-32601"), &passed);

    st_call("{\"jsonrpc\":\"2.0\",\"id\":8,\"method\":\"tools/call\",\"params\":{\"name\":\"nope\",\"arguments\":{}}}");
    ok = st_valid() && st_has("\"id\":8,\"error\":{\"code\":-32602,\"message\":\"Unknown tool\"");
    st_call("{\"jsonrpc\":\"2.0\",\"id\":9,\"method\":\"tools/call\",\"params\":{\"arguments\":{}}}");
    ok = ok && st_valid() && st_has("\"code\":-32602");
    st_call("{\"jsonrpc\":\"2.0\",\"id\":10,\"method\":\"tools/call\",\"params\":{\"name\":\"wire_close\",\"arguments\":[]}}");
    st_check("tools/call: unknown tool, missing name and non-object arguments -> -32602",
             ok && st_valid() && st_has("\"code\":-32602") && mw_tool_calls == 0u, &passed);

    st_call("{\"jsonrpc\":\"2.0\",\"id\":11,\"method\":\"tools/call\",\"params\":{\"name\":\"wire_roundtrip\","
            "\"arguments\":{\"count\":0}}}");
    ok = st_valid() && st_has("\"isError\":true") && st_has("\"structuredContent\":{\"error\":\"count must be");
    st_call("{\"jsonrpc\":\"2.0\",\"id\":12,\"method\":\"tools/call\",\"params\":{\"name\":\"wire_roundtrip\","
            "\"arguments\":{\"count\":16385}}}");
    ok = ok && st_valid() && st_has("\"isError\":true");
    st_call("{\"jsonrpc\":\"2.0\",\"id\":13,\"method\":\"tools/call\",\"params\":{\"name\":\"wire_roundtrip\","
            "\"arguments\":{\"text\":\"12345678901234567890123456789012345678901\"}}}");
    ok = ok && st_valid() && st_has("\"isError\":true") && st_has("at most 40 UTF-8 bytes");
    st_call("{\"jsonrpc\":\"2.0\",\"id\":14,\"method\":\"tools/call\",\"params\":{\"name\":\"wire_roundtrip\","
            "\"arguments\":{\"opcode\":\"RECEIPT\"}}}");
    ok = ok && st_valid() && st_has("\"isError\":true") && st_has("opcode must be \\\\\\\"PING\\\\\\\"");
    st_call("{\"jsonrpc\":\"2.0\",\"id\":15,\"method\":\"tools/call\",\"params\":{\"name\":\"wire_close\"}}");
    st_check("Tool results: bad count/text/opcode -> isError true before any pipe I/O; wire_close when idle is not an error",
             ok && st_valid() && st_has("\"isError\":false") && st_has("\"was_connected\":false") &&
                 st_has("\"text\":\"{\\\"state\\\":\\\"closed\\\"") && mw_tool_calls == 5u && mw_connects == 0u,
             &passed);

    st_write(passed == 13 ? "SELFTEST PASS 13/13\n" : "SELFTEST FAIL\n");
    return passed == 13 ? 0u : 1u;
}

/* ---- Entry point (no C runtime: the linker's default console entry name) ------ */

/* First command-line token after the program path, ASCII only, into out[cap]. Returns 0 when there is none. */
static int mw_first_argument(char *out, size_t cap) {
    const uint16_t *p = GetCommandLineW();
    size_t n = 0;
    int quoted = 0;
    while (*p != 0 && (quoted || (*p != ' ' && *p != '\t'))) { /* program path */
        if (*p == '"') quoted = !quoted;
        p++;
    }
    while (*p == ' ' || *p == '\t') p++;
    if (*p == 0) return 0;
    while (*p != 0 && *p != ' ' && *p != '\t') {
        if (n + 1u < cap) out[n++] = *p < 0x80u ? (char)*p : '?';
        p++;
    }
    out[n] = '\0';
    return 1;
}

__declspec(noreturn) void AW_API mainCRTStartup(void);
void AW_API mainCRTStartup(void) {
    static char first[32];

    (void)QueryPerformanceFrequency(&aw_frequency);
    if (!mw_first_argument(first, sizeof first)) ExitProcess(mcp_serve());

    (void)SetConsoleOutputCP(AW_CP_UTF8);
    if (aw_same(first, "--version")) { st_write(MW_VERSION "\n"); ExitProcess(0u); }
    if (aw_same(first, "--selftest")) ExitProcess(mcp_selftest());
    st_write("usage: agent_wire_mcp [--selftest | --version]   (no argument: MCP server on stdin/stdout)\n");
    ExitProcess(2u);
}

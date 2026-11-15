/*
 * win_proc_bench - what one bare Windows process costs, and how long ending it takes (Ultra-Brain-Labs)
 *
 * One C11 file. No C runtime, no SDK header, no DLL other than KERNEL32.dll. The program starts copies of
 * itself as children and measures them from outside:
 *
 *   win_proc_bench --floor [repeats]   private commit and working set of an idle child with no C runtime:
 *                                      one thread; one thread and an anonymous pipe; two threads; one
 *                                      thread and 1 MiB of static storage of which one byte is written
 *   win_proc_bench --kill [count]      a child in a Job Object is ended with TerminateJobObject; three clock
 *                                      readings in one process: the request, the return of the call, the
 *                                      moment the process object is signalled. The observer first spins on
 *                                      WaitForSingleObject(handle, 0), then, in a second series, blocks on
 *                                      it: the difference is what waking the observer costs.
 *   win_proc_bench --effect [count]    a busy child writes the clock into shared memory without pause; after
 *                                      the kill the last value it wrote says when it last did anything.
 *   win_proc_bench --selftest          arithmetic of the statistics (exit 0 = pass)
 *   win_proc_bench --version
 *
 * Children (started by the program itself): --idle, --idle-pipe, --idle-thread, --idle-ballast, --busy <handle>.
 *
 * Every figure is what Windows did on the machine that ran it, once. Percentiles are nearest rank over the
 * sorted samples. Nothing here says how long ending a larger process takes.
 *
 * Build (LLVM clang for Windows, lld):
 *   clang -std=c11 -O2 -Wall -Wextra -pedantic -Werror -ffreestanding -nostdlib -fuse-ld=lld
 *         -Wl,/subsystem:console src/win_proc_bench.c -lkernel32 -o win_proc_bench.exe
 * Build (MSVC):
 *   cl /nologo /std:c11 /O2 /W4 /WX /utf-8 /GS- src\win_proc_bench.c
 *      /link /SUBSYSTEM:CONSOLE /NODEFAULTLIB kernel32.lib /OUT:win_proc_bench.exe
 *
 * Exit codes: 0 ok, 1 system error, 2 usage, 3 self-test failed.
 */

#include <stddef.h>
#include <stdint.h>

#define PB_VERSION "1.0.0"
#define PB_COUNT_DEFAULT 100u
#define PB_COUNT_MAX 1024u
#define PB_SETTLE_MS 30u

typedef void *pb_handle;
typedef uint32_t pb_dword;
typedef int pb_bool;

typedef struct { /* PROCESS_MEMORY_COUNTERS_EX */
    pb_dword cb;
    pb_dword page_fault_count;
    size_t peak_working_set;
    size_t working_set;
    size_t quota_peak_paged_pool;
    size_t quota_paged_pool;
    size_t quota_peak_non_paged_pool;
    size_t quota_non_paged_pool;
    size_t pagefile_usage;
    size_t peak_pagefile_usage;
    size_t private_usage;
} pb_memory;

typedef struct { /* STARTUPINFOW */
    pb_dword cb;
    uint16_t *reserved;
    uint16_t *desktop;
    uint16_t *title;
    pb_dword x, y, x_size, y_size, x_chars, y_chars, fill, flags;
    uint16_t show_window;
    uint16_t cb_reserved2;
    uint8_t *reserved2;
    pb_handle std_input, std_output, std_error;
} pb_startup;

typedef struct { /* PROCESS_INFORMATION */
    pb_handle process;
    pb_handle thread;
    pb_dword process_id;
    pb_dword thread_id;
} pb_process;

typedef struct { /* SECURITY_ATTRIBUTES */
    pb_dword length;
    void *descriptor;
    pb_bool inherit;
} pb_security;

_Static_assert(sizeof(pb_memory) == 80u && sizeof(pb_startup) == 104u && sizeof(pb_process) == 24u && sizeof(pb_security) == 24u,
               "the four structures have their x64 Windows sizes");

#define PB_IMPORT __declspec(dllimport)
#define PB_API __stdcall

PB_IMPORT pb_handle PB_API GetStdHandle(pb_dword which);
PB_IMPORT pb_bool PB_API WriteFile(pb_handle file, const void *buffer, pb_dword bytes, pb_dword *written, void *overlapped);
PB_IMPORT uint16_t *PB_API GetCommandLineW(void);
PB_IMPORT __declspec(noreturn) void PB_API ExitProcess(unsigned int code);
PB_IMPORT pb_bool PB_API QueryPerformanceCounter(int64_t *count);
PB_IMPORT pb_bool PB_API QueryPerformanceFrequency(int64_t *frequency);
PB_IMPORT pb_dword PB_API GetModuleFileNameW(pb_handle module, uint16_t *buffer, pb_dword size);
PB_IMPORT pb_bool PB_API CreateProcessW(const uint16_t *application, uint16_t *command_line, void *process_security,
                                        void *thread_security, pb_bool inherit, pb_dword flags, void *environment,
                                        const uint16_t *directory, pb_startup *startup, pb_process *process);
PB_IMPORT pb_handle PB_API CreateJobObjectW(void *security, const uint16_t *name);
PB_IMPORT pb_bool PB_API AssignProcessToJobObject(pb_handle job, pb_handle process);
PB_IMPORT pb_bool PB_API TerminateJobObject(pb_handle job, unsigned int exit_code);
PB_IMPORT pb_dword PB_API ResumeThread(pb_handle thread);
PB_IMPORT pb_dword PB_API WaitForSingleObject(pb_handle handle, pb_dword milliseconds);
PB_IMPORT pb_bool PB_API CloseHandle(pb_handle handle);
PB_IMPORT pb_bool PB_API K32GetProcessMemoryInfo(pb_handle process, pb_memory *counters, pb_dword cb);
PB_IMPORT void PB_API Sleep(pb_dword milliseconds);
PB_IMPORT pb_bool PB_API CreatePipe(pb_handle *read_end, pb_handle *write_end, pb_security *security, pb_dword size);
PB_IMPORT pb_handle PB_API CreateThread(void *security, size_t stack, pb_dword (PB_API *start)(void *), void *parameter,
                                        pb_dword flags, pb_dword *thread_id);
PB_IMPORT pb_handle PB_API CreateFileMappingW(pb_handle file, pb_security *security, pb_dword protect, pb_dword size_high,
                                              pb_dword size_low, const uint16_t *name);
PB_IMPORT void *PB_API MapViewOfFile(pb_handle mapping, pb_dword access, pb_dword offset_high, pb_dword offset_low, size_t bytes);
PB_IMPORT pb_dword PB_API GetLastError(void);
PB_IMPORT pb_bool PB_API SetConsoleOutputCP(pb_dword code_page);

#define PB_STD_OUTPUT ((pb_dword)-11)
#define PB_INFINITE 0xFFFFFFFFu
#define PB_WAIT_OBJECT_0 0u
#define PB_CREATE_SUSPENDED 0x00000004u
#define PB_PAGE_READWRITE 0x04u
#define PB_FILE_MAP_ALL_ACCESS 0x000F001Fu
#define PB_INVALID_HANDLE ((pb_handle)(intptr_t)-1)
#define PB_CP_UTF8 65001u
#define PB_KILL_EXIT_CODE 0xDEADu

/* A freestanding build still lets either compiler emit these two calls. The volatile stores keep the
 * optimizer from turning the loops back into calls. */
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

/* ---- output ------------------------------------------------------------------------------------ */
static int64_t pb_frequency = 1;

static size_t pb_length(const char *text) {
    size_t n = 0;
    while (text[n] != '\0') n++;
    return n;
}

static void pb_write(const char *text) {
    pb_dword written = 0;
    (void)WriteFile(GetStdHandle(PB_STD_OUTPUT), text, (pb_dword)pb_length(text), &written, NULL);
}

static void pb_write_u64(uint64_t value) {
    char digits[24];
    size_t at = sizeof digits - 1u;
    digits[at] = '\0';
    do {
        digits[--at] = (char)('0' + (int)(value % 10u));
        value /= 10u;
    } while (value != 0u);
    pb_write(&digits[at]);
}

static void pb_pair(const char *name, uint64_t value) {
    pb_write(" ");
    pb_write(name);
    pb_write("=");
    pb_write_u64(value);
}

static __declspec(noreturn) void pb_fail(const char *what) {
    pb_write("win_proc_bench: ");
    pb_write(what);
    pb_write(" failed, GetLastError");
    pb_pair("", (uint64_t)GetLastError());
    pb_write("\n");
    ExitProcess(1u);
}

/* ---- time and statistics ------------------------------------------------------------------------- */
static int64_t pb_ticks(void) {
    int64_t count = 0;
    (void)QueryPerformanceCounter(&count);
    return count;
}

/* Nanoseconds between two readings (a difference, so the product stays small); 0 when b is before a. */
static uint32_t pb_elapsed_ns(int64_t a, int64_t b) {
    uint64_t ns;
    if (b <= a) return 0u;
    ns = (uint64_t)(b - a) * 1000000000u / (uint64_t)pb_frequency;
    return ns > 0xFFFFFFFFu ? 0xFFFFFFFFu : (uint32_t)ns;
}

static void pb_sort(uint32_t *values, uint32_t count) {
    for (uint32_t i = 1u; i < count; i++) {
        const uint32_t v = values[i];
        uint32_t j = i;
        while (j > 0u && values[j - 1u] > v) {
            values[j] = values[j - 1u];
            j--;
        }
        values[j] = v;
    }
}

/* Nearest rank over sorted samples: the smallest value with at least percent % of them at or below it. */
static uint32_t pb_percentile(const uint32_t *sorted, uint32_t count, uint32_t percent) {
    uint32_t rank;
    if (count == 0u || percent == 0u || percent > 100u) return 0xFFFFFFFFu;
    rank = (count * percent + 99u) / 100u;
    return sorted[rank - 1u];
}

static void pb_report(const char *series, uint32_t *values, uint32_t count) {
    uint32_t within = 0u;
    pb_sort(values, count);
    for (uint32_t i = 0u; i < count; i++) {
        if (values[i] <= 1000000u) within++;
    }
    pb_write(series);
    pb_pair("samples", count);
    pb_pair("min_ns", values[0]);
    pb_pair("p50_ns", pb_percentile(values, count, 50u));
    pb_pair("p90_ns", pb_percentile(values, count, 90u));
    pb_pair("p99_ns", pb_percentile(values, count, 99u));
    pb_pair("max_ns", values[count - 1u]);
    pb_pair("at_most_1ms", within);
    pb_write("\n");
}

/* ---- arguments ----------------------------------------------------------------------------------- */
static const uint16_t *pb_skip_spaces(const uint16_t *p) {
    while (*p == ' ' || *p == '\t') p++;
    return p;
}

/* The next argument as ASCII (a character outside ASCII becomes '?'); -1 when there is none. */
static long pb_next_argument(const uint16_t **cursor, char *out, size_t cap) {
    const uint16_t *p = pb_skip_spaces(*cursor);
    size_t n = 0;
    int quoted = 0;
    if (*p == 0) return -1;
    while (*p != 0 && (quoted || (*p != ' ' && *p != '\t'))) {
        if (*p == '"') {
            quoted = !quoted;
        } else if (n + 1u < cap) {
            out[n++] = *p < 0x80 ? (char)*p : '?';
        }
        p++;
    }
    out[n] = '\0';
    *cursor = p;
    return (long)n;
}

static int pb_same(const char *a, const char *b) {
    while (*a != '\0' && *a == *b) {
        a++;
        b++;
    }
    return *a == *b;
}

static int pb_parse_u64(const char *text, uint64_t *value) {
    uint64_t v = 0;
    if (*text == '\0') return 0;
    while (*text != '\0') {
        if (*text < '0' || *text > '9' || v > 0xFFFFFFFFFFFFFFFFu / 10u - 1u) return 0;
        v = v * 10u + (uint64_t)(*text - '0');
        text++;
    }
    *value = v;
    return 1;
}

/* ---- children ------------------------------------------------------------------------------------ */
static uint16_t pb_self_path[1024];
static volatile unsigned char pb_ballast[1048576];   /* --idle-ballast: static storage that is never read */
static uint16_t pb_command[1200];

static pb_dword PB_API pb_sleeper(void *unused) {
    (void)unused;
    for (;;) Sleep(PB_INFINITE);
}

static __declspec(noreturn) void pb_child_idle(int with_pipe, int with_thread, int with_ballast) {
    pb_handle read_end = NULL, write_end = NULL;
    pb_dword thread_id = 0;
    if (with_ballast) pb_ballast[0] = 1u;                 /* one byte of the megabyte */
    if (with_pipe && !CreatePipe(&read_end, &write_end, NULL, 0u)) ExitProcess(1u);
    if (with_thread && CreateThread(NULL, 0u, pb_sleeper, NULL, 0u, &thread_id) == NULL) ExitProcess(1u);
    for (;;) Sleep(PB_INFINITE);
}

static __declspec(noreturn) void pb_child_busy(uint64_t mapping_value) {
    volatile int64_t *stamp = MapViewOfFile((pb_handle)(uintptr_t)mapping_value, PB_FILE_MAP_ALL_ACCESS, 0u, 0u, 4096u);
    if (stamp == NULL) ExitProcess(1u);
    for (;;) {
        int64_t now = 0;
        (void)QueryPerformanceCounter(&now);
        *stamp = now;
    }
}

/* Starts "<self> <mode> [number]" suspended, puts it into a new Job Object and lets it run. */
static void pb_spawn(const char *mode, int with_number, uint64_t number, pb_handle *job, pb_process *child) {
    pb_startup startup;
    size_t at = 0;
    char digits[24];
    size_t digit_at = sizeof digits - 1u;
    pb_command[at++] = '"';
    for (size_t i = 0; pb_self_path[i] != 0; i++) pb_command[at++] = pb_self_path[i];
    pb_command[at++] = '"';
    pb_command[at++] = ' ';
    for (size_t i = 0; mode[i] != '\0'; i++) pb_command[at++] = (uint16_t)(unsigned char)mode[i];
    if (with_number) {
        digits[digit_at] = '\0';
        do {
            digits[--digit_at] = (char)('0' + (int)(number % 10u));
            number /= 10u;
        } while (number != 0u);
        pb_command[at++] = ' ';
        while (digits[digit_at] != '\0') pb_command[at++] = (uint16_t)(unsigned char)digits[digit_at++];
    }
    pb_command[at] = 0;
    memset(&startup, 0, sizeof startup);
    startup.cb = (pb_dword)sizeof startup;
    memset(child, 0, sizeof *child);
    *job = CreateJobObjectW(NULL, NULL);
    if (*job == NULL) pb_fail("CreateJobObjectW");
    if (!CreateProcessW(pb_self_path, pb_command, NULL, NULL, 1, PB_CREATE_SUSPENDED, NULL, NULL, &startup, child)) pb_fail("CreateProcessW");
    if (!AssignProcessToJobObject(*job, child->process)) pb_fail("AssignProcessToJobObject");
    if (ResumeThread(child->thread) == 0xFFFFFFFFu) pb_fail("ResumeThread");
}

static void pb_release(pb_handle job, pb_process *child) {
    (void)CloseHandle(child->thread);
    (void)CloseHandle(child->process);
    (void)CloseHandle(job);
}

/* ---- --floor ------------------------------------------------------------------------------------- */
static int pb_floor(uint32_t repeats) {
    static const char *const modes[4] = { "--idle", "--idle-pipe", "--idle-thread", "--idle-ballast" };
    for (uint32_t m = 0u; m < 4u; m++) {
        for (uint32_t r = 0u; r < repeats; r++) {
            pb_handle job;
            pb_process child;
            pb_memory memory;
            pb_spawn(modes[m], 0, 0u, &job, &child);
            Sleep(300u);                                  /* let the child reach its sleep */
            memset(&memory, 0, sizeof memory);
            memory.cb = (pb_dword)sizeof memory;
            if (!K32GetProcessMemoryInfo(child.process, &memory, (pb_dword)sizeof memory)) pb_fail("K32GetProcessMemoryInfo");
            pb_write("floor mode=");
            pb_write(modes[m] + 2);
            pb_pair("private_bytes", memory.private_usage);
            pb_pair("pagefile_bytes", memory.pagefile_usage);
            pb_pair("peak_pagefile_bytes", memory.peak_pagefile_usage);
            pb_pair("working_set_bytes", memory.working_set);
            pb_pair("peak_working_set_bytes", memory.peak_working_set);
            pb_write("\n");
            if (!TerminateJobObject(job, PB_KILL_EXIT_CODE)) pb_fail("TerminateJobObject");
            (void)WaitForSingleObject(child.process, 5000u);
            pb_release(job, &child);
        }
    }
    return 0;
}

/* ---- --kill -------------------------------------------------------------------------------------- */
static uint32_t pb_call_ns[PB_COUNT_MAX];
static uint32_t pb_signal_ns[PB_COUNT_MAX];

static int pb_kill(uint32_t count) {
    for (uint32_t spin = 2u; spin-- > 0u;) {              /* 1: the observer spins; 0: it blocks */
        for (uint32_t i = 0u; i < count; i++) {
            pb_handle job;
            pb_process child;
            int64_t requested, returned, signalled;
            pb_spawn("--idle", 0, 0u, &job, &child);
            Sleep(PB_SETTLE_MS);
            requested = pb_ticks();
            if (!TerminateJobObject(job, PB_KILL_EXIT_CODE)) pb_fail("TerminateJobObject");
            returned = pb_ticks();
            if (spin) {
                while (WaitForSingleObject(child.process, 0u) != PB_WAIT_OBJECT_0) { }
            } else if (WaitForSingleObject(child.process, 5000u) != PB_WAIT_OBJECT_0) {
                pb_fail("WaitForSingleObject");
            }
            signalled = pb_ticks();
            pb_call_ns[i] = pb_elapsed_ns(requested, returned);
            pb_signal_ns[i] = pb_elapsed_ns(requested, signalled);
            pb_release(job, &child);
        }
        pb_report(spin ? "kill observer=spin series=call_returned" : "kill observer=block series=call_returned", pb_call_ns, count);
        pb_report(spin ? "kill observer=spin series=object_signalled" : "kill observer=block series=object_signalled", pb_signal_ns, count);
    }
    return 0;
}

/* ---- --effect ------------------------------------------------------------------------------------ */
static int pb_effect(uint32_t count) {
    pb_security security;
    pb_handle mapping;
    volatile int64_t *stamp;
    security.length = (pb_dword)sizeof security;
    security.descriptor = NULL;
    security.inherit = 1;
    mapping = CreateFileMappingW(PB_INVALID_HANDLE, &security, PB_PAGE_READWRITE, 0u, 4096u, NULL);
    if (mapping == NULL) pb_fail("CreateFileMappingW");
    stamp = MapViewOfFile(mapping, PB_FILE_MAP_ALL_ACCESS, 0u, 0u, 4096u);
    if (stamp == NULL) pb_fail("MapViewOfFile");
    for (uint32_t i = 0u; i < count; i++) {
        pb_handle job;
        pb_process child;
        int64_t requested, signalled;
        *stamp = 0;
        pb_spawn("--busy", 1, (uint64_t)(uintptr_t)mapping, &job, &child);
        for (uint32_t waited = 0u; *stamp == 0 && waited < 2000u; waited++) Sleep(1u);   /* until the child writes */
        if (*stamp == 0) pb_fail("busy child never wrote");
        Sleep(PB_SETTLE_MS);
        requested = pb_ticks();
        if (!TerminateJobObject(job, PB_KILL_EXIT_CODE)) pb_fail("TerminateJobObject");
        if (WaitForSingleObject(child.process, 5000u) != PB_WAIT_OBJECT_0) pb_fail("WaitForSingleObject");
        signalled = pb_ticks();
        pb_call_ns[i] = pb_elapsed_ns(requested, *stamp);          /* the last thing the child did, after the request */
        pb_signal_ns[i] = pb_elapsed_ns(requested, signalled);
        pb_release(job, &child);
    }
    pb_report("effect series=last_write_after_request", pb_call_ns, count);
    pb_report("effect series=object_signalled", pb_signal_ns, count);
    return 0;
}

/* ---- --selftest ---------------------------------------------------------------------------------- */
static int pb_selftest(void) {
    uint32_t values[5] = { 9u, 1u, 5u, 3u, 7u };
    uint64_t parsed = 0;
    int ok = 1;
    pb_sort(values, 5u);
    ok = ok && values[0] == 1u && values[1] == 3u && values[2] == 5u && values[3] == 7u && values[4] == 9u;
    ok = ok && pb_percentile(values, 5u, 50u) == 5u && pb_percentile(values, 5u, 90u) == 9u && pb_percentile(values, 5u, 1u) == 1u;
    ok = ok && pb_percentile(values, 5u, 100u) == 9u && pb_percentile(values, 5u, 0u) == 0xFFFFFFFFu && pb_percentile(values, 0u, 50u) == 0xFFFFFFFFu;
    ok = ok && pb_parse_u64("1024", &parsed) && parsed == 1024u && !pb_parse_u64("", &parsed) && !pb_parse_u64("12a", &parsed);
    ok = ok && pb_same("--kill", "--kill") && !pb_same("--kill", "--kil") && !pb_same("--kil", "--kill");
    pb_write(ok ? "win_proc_bench self-test: PASS\n" : "win_proc_bench self-test: FAIL\n");
    return ok ? 0 : 3;
}

static int pb_usage(void) {
    pb_write("usage: win_proc_bench --floor [repeats] | --kill [count] | --effect [count] | --selftest | --version\n");
    return 2;
}

__declspec(noreturn) void PB_API mainCRTStartup(void);
void PB_API mainCRTStartup(void) {
    static char first[64], second[64];
    const uint16_t *cursor = GetCommandLineW();
    long first_len, second_len;
    uint64_t number = 0;

    (void)SetConsoleOutputCP(PB_CP_UTF8);
    (void)QueryPerformanceFrequency(&pb_frequency);
    if (pb_frequency <= 0) pb_frequency = 1;
    if (GetModuleFileNameW(NULL, pb_self_path, 1000u) == 0u) pb_fail("GetModuleFileNameW");

    (void)pb_next_argument(&cursor, first, sizeof first);      /* the program path */
    first_len = pb_next_argument(&cursor, first, sizeof first);
    second_len = pb_next_argument(&cursor, second, sizeof second);
    if (first_len < 0) ExitProcess((unsigned int)pb_usage());
    if (second_len >= 0 && !pb_parse_u64(second, &number)) ExitProcess((unsigned int)pb_usage());

    if (pb_same(first, "--idle")) pb_child_idle(0, 0, 0);
    if (pb_same(first, "--idle-pipe")) pb_child_idle(1, 0, 0);
    if (pb_same(first, "--idle-thread")) pb_child_idle(0, 1, 0);
    if (pb_same(first, "--idle-ballast")) pb_child_idle(0, 0, 1);
    if (pb_same(first, "--busy")) pb_child_busy(number);
    if (pb_same(first, "--version")) { pb_write(PB_VERSION "\n"); ExitProcess(0u); }
    if (pb_same(first, "--selftest")) ExitProcess((unsigned int)pb_selftest());

    if (second_len < 0) number = pb_same(first, "--floor") ? 5u : PB_COUNT_DEFAULT;
    if (number == 0u || number > PB_COUNT_MAX) ExitProcess((unsigned int)pb_usage());
    pb_write("win_proc_bench " PB_VERSION);
    pb_pair("qpc_frequency", (uint64_t)pb_frequency);
    pb_write("\n");
    if (pb_same(first, "--floor")) ExitProcess((unsigned int)pb_floor((uint32_t)number));
    if (pb_same(first, "--kill")) ExitProcess((unsigned int)pb_kill((uint32_t)number));
    if (pb_same(first, "--effect")) ExitProcess((unsigned int)pb_effect((uint32_t)number));
    ExitProcess((unsigned int)pb_usage());
}

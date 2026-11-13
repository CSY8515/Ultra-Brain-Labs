/*
 * win_zero_cli - Windows zero-dependency CLI reference (Ultra-Brain-Labs v1.0.0)
 *
 * One C11 file. No C runtime, no SDK header, no DLL other than KERNEL32.dll:
 * the program declares the few Win32 functions it uses and links kernel32
 * only. Output is UTF-8 (the console code page is switched to 65001), memory
 * comes from a static arena, and the self-test measures its own peak working
 * set through K32GetProcessMemoryInfo (exported by KERNEL32 since Windows 7).
 *
 * Build (one line, LLVM clang for Windows):
 *   clang -std=c11 -O2 -Wall -Wextra -pedantic -Werror -ffreestanding -nostdlib -fuse-ld=lld
 *         -Wl,/entry:wz_main -Wl,/subsystem:console src/win_zero_cli.c -lkernel32 -o win_zero_cli.exe
 *
 * Usage:
 *   win_zero_cli              greeting (UTF-8, exit 0)
 *   win_zero_cli --selftest   built-in checks (exit 0 = all pass, 1 = a check failed)
 *   win_zero_cli --version    prints 1.0.0
 */

#include <stddef.h>
#include <stdint.h>

/* ---- The only external surface: KERNEL32.dll ------------------------------ */

typedef void *wz_handle;
typedef uint32_t wz_dword;
typedef int wz_bool;

typedef struct {               /* PROCESS_MEMORY_COUNTERS */
    wz_dword cb;
    wz_dword page_fault_count;
    size_t peak_working_set;
    size_t working_set;
    size_t quota_peak_paged_pool;
    size_t quota_paged_pool;
    size_t quota_peak_non_paged_pool;
    size_t quota_non_paged_pool;
    size_t pagefile_usage;
    size_t peak_pagefile_usage;
} wz_memory_counters;

#define WZ_IMPORT __declspec(dllimport)
#define WZ_API __stdcall
WZ_IMPORT wz_handle WZ_API GetStdHandle(wz_dword which);
WZ_IMPORT wz_bool WZ_API WriteFile(wz_handle file, const void *data, wz_dword size, wz_dword *written, void *overlapped);
WZ_IMPORT wz_bool WZ_API SetConsoleOutputCP(unsigned int code_page);
WZ_IMPORT const uint16_t *WZ_API GetCommandLineW(void);
WZ_IMPORT wz_handle WZ_API GetCurrentProcess(void);
WZ_IMPORT wz_bool WZ_API K32GetProcessMemoryInfo(wz_handle process, wz_memory_counters *counters, wz_dword size);
WZ_IMPORT __declspec(noreturn) void WZ_API ExitProcess(unsigned int code);

#define WZ_STD_OUTPUT ((wz_dword)-11)
#define WZ_CP_UTF8 65001u
#define WZ_VERSION "1.0.0"
#define WZ_WORKING_SET_LIMIT (15u * 1024u * 1024u)

/* A freestanding build still lets the compiler emit these two calls. MSVC needs to be told
 * that the intrinsics are being defined here (clang defines _MSC_VER too, so exclude it). */
#if defined(_MSC_VER) && !defined(__clang__)
#pragma function(memset, memcpy)
#endif
void *memset(void *dest, int value, size_t count);
void *memcpy(void *dest, const void *src, size_t count);
void *memset(void *dest, int value, size_t count) {
    unsigned char *d = dest;
    while (count--) *d++ = (unsigned char)value;
    return dest;
}
void *memcpy(void *dest, const void *src, size_t count) {
    unsigned char *d = dest;
    const unsigned char *s = src;
    while (count--) *d++ = *s++;
    return dest;
}

/* ---- Output --------------------------------------------------------------- */

static size_t wz_length(const char *text) {
    size_t n = 0;
    while (text[n] != '\0') n++;
    return n;
}

static void wz_write(const char *text) {
    wz_dword written = 0;
    (void)WriteFile(GetStdHandle(WZ_STD_OUTPUT), text, (wz_dword)wz_length(text), &written, NULL);
}

/* Unsigned decimal into a caller buffer of at least 21 bytes. */
static const char *wz_decimal(uint64_t value, char buffer[21]) {
    char *p = buffer + 20;
    *p = '\0';
    do {
        *--p = (char)('0' + value % 10u);
        value /= 10u;
    } while (value != 0u);
    return p;
}

/* ---- Static arena: no heap, no fragmentation ------------------------------- */

#define WZ_ARENA_BYTES 65536u
static unsigned char wz_arena_memory[WZ_ARENA_BYTES];
static size_t wz_arena_used;

static void *wz_arena_alloc(size_t size) {
    const size_t aligned = (wz_arena_used + 15u) & ~(size_t)15u;
    if (size == 0u || size > WZ_ARENA_BYTES || aligned > WZ_ARENA_BYTES - size) return NULL;
    wz_arena_used = aligned + size;
    return wz_arena_memory + aligned;
}

static void wz_arena_reset(void) { wz_arena_used = 0u; }

/* ---- UTF-8 validation (RFC 3629: no overlongs, no surrogates, <= U+10FFFF) -- */

static int wz_utf8_valid(const unsigned char *s, size_t n) {
    size_t i = 0;
    while (i < n) {
        const unsigned char c = s[i];
        size_t extra;
        uint32_t code, minimum;
        if (c < 0x80u) { i++; continue; }
        else if ((c & 0xE0u) == 0xC0u) { extra = 1; code = c & 0x1Fu; minimum = 0x80u; }
        else if ((c & 0xF0u) == 0xE0u) { extra = 2; code = c & 0x0Fu; minimum = 0x800u; }
        else if ((c & 0xF8u) == 0xF0u) { extra = 3; code = c & 0x07u; minimum = 0x10000u; }
        else return 0;
        if (n - i <= extra) return 0;
        for (size_t k = 1; k <= extra; k++) {
            if ((s[i + k] & 0xC0u) != 0x80u) return 0;
            code = (code << 6) | (s[i + k] & 0x3Fu);
        }
        if (code < minimum || code > 0x10FFFFu || (code >= 0xD800u && code <= 0xDFFFu)) return 0;
        i += extra + 1;
    }
    return 1;
}

/* ---- Command line: the first argument after the program name ---------------- */

static int wz_argument_is(const uint16_t *command_line, const char *expected) {
    const uint16_t *p = command_line;
    if (*p == '"') {                       /* quoted program path */
        p++;
        while (*p != 0 && *p != '"') p++;
        if (*p == '"') p++;
    } else {
        while (*p != 0 && *p != ' ' && *p != '\t') p++;
    }
    while (*p == ' ' || *p == '\t') p++;
    size_t k = 0;
    while (expected[k] != '\0' && p[k] == (uint16_t)(unsigned char)expected[k]) k++;
    return expected[k] == '\0' && (p[k] == 0 || p[k] == ' ' || p[k] == '\t');
}

static int wz_has_argument(const uint16_t *command_line) {
    const uint16_t *p = command_line;
    if (*p == '"') { p++; while (*p != 0 && *p != '"') p++; if (*p == '"') p++; }
    else { while (*p != 0 && *p != ' ' && *p != '\t') p++; }
    while (*p == ' ' || *p == '\t') p++;
    return *p != 0;
}

/* ---- Self-test ---------------------------------------------------------------- */

static int wz_check(const char *name, int ok, int *passed) {
    wz_write(ok ? "  PASS  " : "  FAIL  ");
    wz_write(name);
    wz_write("\n");
    *passed += ok;
    return ok;
}

static unsigned int wz_selftest(void) {
    static const unsigned char korean[] = "덜그덕거림 0% · 한글 UTF-8";
    static const unsigned char overlong[] = { 0xC0u, 0xAFu };          /* overlong '/' */
    static const unsigned char surrogate[] = { 0xEDu, 0xA0u, 0x80u };  /* U+D800 */
    static const unsigned char truncated[] = { 0xEDu, 0x95u };         /* cut 3-byte sequence */
    char digits[21];
    int passed = 0;

    wz_write("win_zero_cli " WZ_VERSION " self-test\n");

    wz_check("UTF-8: Korean text is valid; overlong, surrogate and truncated sequences are rejected",
             wz_utf8_valid(korean, sizeof korean - 1u) && !wz_utf8_valid(overlong, sizeof overlong) &&
             !wz_utf8_valid(surrogate, sizeof surrogate) && !wz_utf8_valid(truncated, sizeof truncated),
             &passed);

    wz_arena_reset();
    unsigned char *a = wz_arena_alloc(3u), *b = wz_arena_alloc(100u);
    void *too_big = wz_arena_alloc(WZ_ARENA_BYTES);
    wz_check("Arena: 16-byte alignment, no overlap, oversized request refused",
             a != NULL && b != NULL && ((uintptr_t)a % 16u) == 0u && ((uintptr_t)b % 16u) == 0u &&
             b >= a + 3 && too_big == NULL, &passed);
    wz_arena_reset();

    wz_check("Decimal formatter: 0 and 18446744073709551615",
             wz_length(wz_decimal(0u, digits)) == 1u && wz_decimal(0u, digits)[0] == '0' &&
             wz_length(wz_decimal(UINT64_MAX, digits)) == 20u, &passed);

    wz_memory_counters counters;
    counters.cb = (wz_dword)sizeof counters;
    const int measured = K32GetProcessMemoryInfo(GetCurrentProcess(), &counters, counters.cb) != 0;
    wz_write("  peak working set: ");
    wz_write(measured ? wz_decimal((uint64_t)counters.peak_working_set / 1024u, digits) : "unknown");
    wz_write(" KiB (limit 15360 KiB)\n");
    wz_check("Memory: peak working set measured and within 15 MiB",
             measured && counters.peak_working_set <= WZ_WORKING_SET_LIMIT, &passed);

    wz_write(passed == 4 ? "SELFTEST PASS 4/4\n" : "SELFTEST FAIL\n");
    return passed == 4 ? 0u : 1u;
}

/* ---- Entry point (no C runtime: the linker enters here directly) ------------- */

__declspec(noreturn) void WZ_API wz_main(void);
void WZ_API wz_main(void) {
    (void)SetConsoleOutputCP(WZ_CP_UTF8);
    const uint16_t *command_line = GetCommandLineW();
    if (wz_argument_is(command_line, "--selftest")) ExitProcess(wz_selftest());
    if (wz_argument_is(command_line, "--version")) {
        wz_write(WZ_VERSION "\n");
        ExitProcess(0u);
    }
    if (wz_has_argument(command_line)) {
        wz_write("usage: win_zero_cli [--selftest | --version]\n");
        ExitProcess(2u);
    }
    wz_write("win_zero_cli " WZ_VERSION " - Windows 무의존성 CLI 레퍼런스 (KERNEL32.dll 전용)\n");
    wz_write("안녕하세요. 설치할 것 없이 바로 실행됩니다. 덜그덕거림 0%.\n");
    ExitProcess(0u);
}

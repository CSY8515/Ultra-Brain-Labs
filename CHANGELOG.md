# Changelog

## v1.1.0 — 2026-11-13

**Native 0.4 ms Agent IPC Pipe & Windows Zero-Dependency Toolkit.**

### Added
- `src/agent_wire_ipc.c`: 1:1 local agent IPC over the Win32 named pipe `\\.\pipe\ultra_brain_agent_wire`. One C11 file, no C runtime, imports `KERNEL32.dll` only. Pipe layer = six functions (`CreateNamedPipeA`, `ConnectNamedPipe`, `CreateFileA`, `ReadFile`, `WriteFile`, `CloseHandle`); 4,096-byte message-mode buffers; fixed 64-byte packet (`WIRE` magic, version 0x0100, opcodes PING/TASK/STATUS/RECEIPT, FNV-1a 32 checksum over the other 60 bytes, 40-byte payload). Modes `--server [sessions]`, `--client [text]`, `--bench [count]`, `--selftest` (11 checks), `--version`.
- `tests/test_agent_wire_ipc.py`: standard-library Python drives the pipe (PING round trip, corrupted/short/forbidden packets, `--client` and `--bench` against a live server), checks the PE import table and exit codes.
- `scripts/wire_scenario.ps1`: one server, one client exchange, one 1,000-round-trip bench; saves the verbatim output.
- `build.cmd`: clang (default) or `build.cmd msvc`; both warnings-as-errors.
- CI: second job builds both tools with MSVC `/std:c11 /O2 /W4 /WX /utf-8 /GS-` (`/NODEFAULTLIB kernel32.lib`) next to the clang `-Werror` job; both jobs check `dumpbin /dependents` = `KERNEL32.dll` only, run the self-tests and the pipe scenario, and upload the binaries plus the bench transcript.
- `CHANGELOG.md`, `VERSION`, `RELEASE_NOTES_v1.1.0.md`.

### Changed
- `src/main.c` renamed to `src/win_zero_cli.c`. The only source change is a guarded `#pragma function(memset, memcpy)` so MSVC can compile the file with `/NODEFAULTLIB`; the clang binary is unchanged in behaviour (5,120 bytes, same import list).
- `README.md` rewritten as the engineering reference: what the Windows CI badge physically proves, the zero-dependency toolkit and prompt, the agent IPC pipe with measured figures and provenance, and the two-part local multi-agent architecture.

### Measured (see README §3 for provenance; nothing here is estimated)
- Build diagnostics: clang 0, MSVC 0 (both jobs fail on any compiler output).
- Imports: `KERNEL32.dll` only, both tools, both compilers.
- Bench figures are copied from the CI transcript of the run named in the README. Local execution on the author's PC was blocked by Smart App Control (unsigned binary), so no local figure is published.

## v1.0.0 — 2026-11-12

- `win_zero_cli.exe`: Windows zero-dependency CLI reference (C11, no C runtime, `KERNEL32.dll` only, 5,120 bytes) and the "덜그덕거림 0%" engineering guide. CI run 43205024784.

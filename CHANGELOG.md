# Changelog

## v1.2.0 — 2026-11-15

**Zero-dependency MCP stdio adapter for the agent wire pipe.**

### Added
- `src/agent_wire_mcp.c`: Model Context Protocol server over stdio (JSON-RPC 2.0, one message per line) for the named pipe that `agent_wire_ipc --server` serves. One C11 file, no C runtime, no JSON library, imports `KERNEL32.dll` only (23 functions). Methods `initialize`, `notifications/initialized`, `ping`, `tools/list`, `tools/call`; protocol revisions `2024-11-05`, `2025-03-26`, `2025-06-18`, `2025-11-25`. Tools `wire_status` (connect, one PING, state and round-trip time), `wire_roundtrip` (1..16,384 PING/TASK/STATUS round trips, every receipt verified, min/p50/avg/p99/max in microseconds), `wire_server_start` (starts the sibling `agent_wire_ipc.exe --server` inside a kill-on-close job object, standard handles on `NUL`), `wire_close`. Modes: no argument = MCP server, `--selftest` (13 checks), `--version`.
- `tests/test_agent_wire_mcp.py`: 14 tests. A Python MCP host drives the adapter over stdio against the unmodified pipe server: handshake and version negotiation, `tools/list` schemas, JSON-RPC error codes and line framing (CRLF, split and merged writes, an oversized line), tool argument errors, status without a server, 16,593 round trips through the real pipe with server sequence and golden checksum checks, close and reconnect, a busy pipe, `wire_server_start` (already served, and spawn plus termination with the adapter).
- `scripts/mcp_soak.py`: soak harness. One server and one adapter stay up while the host sends `tools/call` requests (one pipe round trip each); memory, handles, threads, CPU time and CPU cycles are read from outside the two processes; pass criteria are fixed in the file.
- CI: both jobs build, import-check and self-test the adapter, run all 26 unit tests (the MSVC job ran none before) and run the soak (default 20 rounds x 250,000 calls + 60 s idle; `workflow_dispatch` inputs change the size). Artifacts now include `agent_wire_mcp.exe` and `soak_*.txt` / `soak_*.json`.
- `README.md` §5: `claude_desktop_config.json` example, the four tools, the protocol scope, measured figures with their limits. `RELEASE_NOTES_v1.2.0.md`.

### Unchanged
- `src/agent_wire_ipc.c`, `src/win_zero_cli.c`, `tests/test_agent_wire_ipc.py`, `tests/test_win_zero_cli.py` and `scripts/wire_scenario.ps1` are byte-identical to v1.1.0. `agent_wire_ipc --version` still prints 1.1.0 and `win_zero_cli --version` 1.0.0: tool versions follow the source file, the repository version is `VERSION`.

### Measured (CI run 43613135707, MSVC build, `windows-latest`; details and limits in README §5)
- 20,000,000 `tools/call` requests in 1,825.2 s over one pipe session, 20,004,002 pipe round trips, every server sequence number in order, 0 rejected.
- MCP call latency seen by the Python host: p50 69.4 us, p99 116.2 us, p99.9 145.1 us, max 10,058.3 us. Pipe round trip inside the adapter: p50 30 us, p99 58 us, p99.9 76 us, max 5,741 us.
- Private bytes, end of round 1 to the last sample: adapter +8,192 B, server +12,288 B, in one step; the last 51 rounds (12,750,000 calls) are identical at 712 KiB. Handle count 44 -> 44. This is "no growth proportional to calls was measured", not "0 bytes".
- 300 s idle after the load: 0.000 ms of CPU time and 0 CPU cycles for the adapter and for the server.
- Not run: local execution (blocked by Smart App Control), Claude Desktop / Cursor / Claude Code as real hosts, soak longer than about 30 minutes.

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

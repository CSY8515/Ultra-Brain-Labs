# Ultra-Brain-Labs v1.1.0: Native Agent IPC Pipe (0.4ms) and Zero-Dependency Windows Toolkit

Two Windows tools, each one C11 file, no C runtime, imports `KERNEL32.dll` only. Both compile with 0 diagnostics under clang `-Wall -Wextra -pedantic -Werror` and MSVC `/W4 /WX`, and both are built, import-checked, self-tested and exercised on GitHub's `windows-latest` runner every push.

## New: `agent_wire_ipc.c` — local agent IPC over a Win32 named pipe
- Pipe `\\.\pipe\ultra_brain_agent_wire`, message mode, 4,096-byte buffers, remote clients rejected. Pipe layer = six KERNEL32 calls (`CreateNamedPipeA`, `ConnectNamedPipe`, `CreateFileA`, `ReadFile`, `WriteFile`, `CloseHandle`).
- Fixed 64-byte packet: `WIRE` magic, version 0x0100, opcodes PING/TASK/STATUS/RECEIPT, `project_id`, sender/receiver ids, 40-byte payload, FNV-1a 32 checksum over the other 60 bytes. Server answers every message with a RECEIPT (status 0 ACK_OK or 1–6 for size/magic/version/opcode/length/checksum faults) carrying the echoed request checksum and a per-session sequence.
- Modes: `--server [sessions]`, `--client [text]`, `--bench [count]`, `--selftest` (11 checks), `--version`. Exit codes 0 / 1 pipe error / 2 usage / 3 receipt rejected.
- Python agents connect with the standard library only (`open(r"\\.\pipe\...", "r+b", buffering=0)` + `struct`); the README shows the 20-line client, which is also what the unit tests run.

## Measured on CI (run 43373628526, `windows-latest`, 1,000 PING→RECEIPT round trips, every receipt verified)
| build | min | p50 | avg | p99 | max | throughput | peak working set | exe |
|---|---|---|---|---|---|---|---|---|
| clang 20.1.8 + lld | 0.005 ms | 0.031 ms | 0.040 ms | 0.066 ms | 7.859 ms | 24,596 rt/s | 2,960 KiB | 23,552 B |
| MSVC 19.51 | 0.018 ms | 0.026 ms | 0.038 ms | 0.063 ms | 9.770 ms | 25,855 rt/s | 2,980 KiB | 22,016 B |

The "0.4 ms" in the title is the specified upper bound; the measured steady-state average is about 0.04 ms. The max (7.9–9.8 ms) is the first round trip after connecting; warm up with one PING when latency matters. Numbers vary per runner and run. Local measurement on the author's machine was blocked by Smart App Control (unsigned binary), so no local figure is published.

## `win_zero_cli.c` (Part 1, unchanged behaviour)
- `src/main.c` renamed to `src/win_zero_cli.c`; a guarded `#pragma function(memset, memcpy)` lets MSVC build it with `/NODEFAULTLIB`. clang binary: 5,120 bytes, same 7 KERNEL32 imports; MSVC binary: 5,632 bytes.
- Peak working set 2,860 KiB (v1.0.0 run) / 3,336 KiB (this run). Local clang 19.1.5 compile+link 0.23–0.38 s.

## Repository
- `README.md` rewritten: what the CI badge physically proves and what it does not; the zero-dependency toolkit with a hallucination-resistant prompt; the pipe protocol, measured figures with provenance, Python client; the two-part offline multi-agent architecture.
- `build.cmd` (clang default, `msvc` argument), `scripts/wire_scenario.ps1`, `tests/test_agent_wire_ipc.py` (12 tests total with the Part 1 suite), `CHANGELOG.md`, `VERSION` = 1.1.0.
- CI: clang job + new MSVC job; both fail on any compiler output or any DLL other than `KERNEL32.dll`; both upload the binaries and the bench transcript (`bench_clang.txt`, `bench_msvc.txt`).

## Not in this release
- Code signing. Smart App Control may block the attached binaries; build from source with `build.cmd` or read the CI run instead.
- Windows Sandbox or ARM64 measurement, multi-client pipe instances, payloads over 40 bytes, authentication beyond the local pipe ACL.

Attached binaries are the files the tagged CI run built and tested; their SHA-256 values are listed below the notes on the release page.

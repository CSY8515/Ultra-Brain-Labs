# Ultra-Brain-Labs v1.2.0: Zero-Dependency MCP Adapter for the Native Agent IPC Pipe

`agent_wire_mcp.exe` connects an MCP host (Claude Desktop, Cursor, Claude Code) to the named pipe of `agent_wire_ipc`. It is one C11 file with no C runtime, no SDK and no JSON library, and it imports `KERNEL32.dll` only. It compiles with 0 diagnostics under clang `-Wall -Wextra -pedantic -Werror` and MSVC `/W4 /WX`. `agent_wire_ipc.c` and `win_zero_cli.c` are byte-identical to v1.1.0.

## New: `agent_wire_mcp.c`: MCP over stdio on top of the pipe
- JSON-RPC 2.0, one message per line on stdin/stdout. Methods: `initialize`, `notifications/initialized`, `ping`, `tools/list`, `tools/call`. Protocol revisions `2024-11-05`, `2025-03-26`, `2025-06-18` and `2025-11-25` are answered as requested; any other value is answered with `2025-11-25`.
- Tools:
  - `wire_status`: connects if needed, sends one PING, reports `connected` / `no_server` / `busy` / `error`, the round-trip time in microseconds, the server sequence and the adapter counters.
  - `wire_roundtrip`: 1 to 16,384 PING / TASK / STATUS round trips with a payload of at most 40 bytes; every receipt is verified; returns the sequence range and first/min/p50/avg/p99/max in microseconds.
  - `wire_server_start`: starts `agent_wire_ipc.exe --server` from the adapter's own folder unless the pipe is already served. The executable name and arguments are fixed, its standard handles are `NUL`, and a kill-on-close job object ends it when the adapter exits.
  - `wire_close`: releases the single pipe instance for another client.
- Its own JSON scanner checks RFC 8259 syntax, UTF-8 well-formedness and a nesting limit of 32. Errors: `-32700`, `-32600` (including batch arrays), `-32601`, `-32602`. Tool failures are results with `isError: true`. Results carry a text block and the same object as `structuredContent`.
- The adapter creates no thread, calls no heap function and has no timer; between messages it is blocked in `ReadFile` on stdin.

## Connect it
Put `agent_wire_mcp.exe` and `agent_wire_ipc.exe` in one folder and add this to `claude_desktop_config.json` (Cursor: the same block in `.cursor\mcp.json`), with your own path:

```json
{
  "mcpServers": {
    "agent-wire": {
      "command": "C:\\Tools\\Ultra-Brain-Labs\\agent_wire_mcp.exe",
      "args": []
    }
  }
}
```

## Verification
- Unit tests: 26 in each CI job (5 `win_zero_cli`, 7 `agent_wire_ipc`, 14 new MCP tests). The MCP tests run a Python host over stdio against the unmodified pipe server, including 16,593 round trips in one test, a busy pipe, reconnect, and the start and termination of the server child.
- Self-tests: `win_zero_cli` 4/4, `agent_wire_ipc` 11/11, `agent_wire_mcp` 13/13.
- Imports: all three tools, both compilers: `KERNEL32.dll` only (`agent_wire_mcp.exe`: 23 functions, 35,328 B with clang 20.1.8, 35,840 B with MSVC 19.51.36260).

## MSVC soak, measured on CI (`windows-latest`, Windows Server 2025, 4 logical CPUs)
One server and one adapter stay up; a Python host sends `tools/call wire_roundtrip` requests, one pipe round trip each, and checks the server sequence number of every reply. Memory, handles, threads and CPU are read from outside the processes. Pass criteria were fixed before the first measured run.

| run, build | measured calls, time | MCP call latency p50 / p99 / p99.9 / max | pipe round trip p50 / p99 / p99.9 / max | private bytes growth adapter / server | idle CPU adapter / server |
|---|---|---|---|---|---|
| [43613135707](https://github.com/CSY8515/Ultra-Brain-Labs/actions/runs/43613135707), MSVC | 20,000,000 in 1,825.2 s | 69.4 / 116.2 / 145.1 / 10,058.3 us | 30 / 58 / 76 / 5,741 us | +8,192 B / +12,288 B | 300 s: 0.000 ms, 0 cycles (both) |
| same run, clang | 20,000,000 in 1,114.4 s | 44.9 / 68.8 / 91.5 / 8,328.9 us | 20 / 34 / 50 / 8,294 us | +12,288 B / +12,288 B | 300 s: 0.000 ms, 0 cycles (both) |
| [43612233351](https://github.com/CSY8515/Ultra-Brain-Labs/actions/runs/43612233351), MSVC | 5,000,000 in 277.2 s | 44.7 / 70.3 / 108.0 / 8,247.0 us | 20 / 34 / 54 / 5,692 us | +32,768 B / +36,864 B | 60 s: 0.000 ms, 389,921 / 346,566 cycles |
| same run, clang | 5,000,000 in 461.3 s | 70.1 / 119.7 / 164.9 / 10,569.9 us | 30 / 60 / 83 / 7,846 us | -61,440 B / -61,440 B | 60 s: 0.000 ms, 0 cycles (both) |

In all four soaks every pipe round trip (20,004,002 or 5,002,502) was accepted in server sequence order with 0 rejected, the handle count stayed at 44, and both processes exited 0.

What these figures do and do not show:
- **Memory.** The growth is not 0 bytes. Private bytes follow the thread count: in the 20,000,000-call MSVC soak the adapter held 704 KiB or 712 KiB with one thread, 740 KiB with two and 764 to 776 KiB with three. The adapter creates no thread; Windows adds and removes threads inside the process, and the unmodified pipe server shows the same steps in the same rounds. The cause was not identified. After the first such episode the one-thread value was 8,192 B higher (12,288 B for the server); two later episodes left it unchanged, and the last 51 rounds (12,750,000 calls) are identical at 712 KiB. No growth proportional to the number of calls was measured. In run 43612233351 the +32,768 B and -61,440 B come from a second thread at the last sample and from three threads at the baseline sample.
- **Idle CPU.** CPU time (`GetProcessTimes`, accounted in 15.625 ms ticks) did not move in any idle window. The cycle counter (`QueryProcessCycleTime`) read 0 in six of the eight process windows; in the other two Windows started a thread inside the processes during the window (389,921 and 346,566 cycles).
- **Latency.** "Pipe round trip" is measured inside the adapter around `WriteFile` and `ReadFile`. "MCP call latency" is measured in the Python host and includes the host's own pipe I/O. The two CI jobs run on different virtual machines, so the MSVC and clang rows are not a compiler comparison.
- **Limits.** Shared CI virtual machines, one run per configuration, the longest load about 30 minutes. The first rc run [43611161432](https://github.com/CSY8515/Ultra-Brain-Labs/actions/runs/43611161432) passed with 1,000,000 calls, but its host read replies one byte at a time and reported an MCP call latency of 435 us at p50; the host was fixed and that latency figure is not used.

## Not run
- Local execution on the author's PC: Smart App Control blocked the freshly built `agent_wire_mcp.exe` (`WinError 4551`). Local verification is the 0-diagnostic build with clang 19.1.5 and MSVC 14.44.
- Claude Desktop, Cursor and Claude Code as real hosts. The verification is at protocol level.
- Soak runs of hours or days, real hardware, ARM64, Windows Sandbox.

## Not in this release
- The `2026-07-28` MCP revision, resources, prompts, logging, progress and cancellation handling, `outputSchema`, the Streamable HTTP transport.
- Code signing. Smart App Control may block the attached binaries, also when an MCP host starts them; build from source with `build.cmd` or read the CI runs instead.

Attached binaries are the files the tagged CI run built and tested; their SHA-256 values and that run's soak figures are listed below the notes on the release page.

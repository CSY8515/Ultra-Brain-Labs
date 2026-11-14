"""Soak run for agent_wire_mcp.exe: many MCP tools/call requests, each one a real named-pipe round trip.

    python scripts/mcp_soak.py --mcp agent_wire_mcp.exe --wire agent_wire_ipc.exe
                               [--rounds 20] [--calls 50000] [--idle-seconds 60]
                               [--out-text soak.txt] [--out-json soak.json] [--label msvc]

One agent_wire_ipc --server process and one agent_wire_mcp process run for the whole test. This script is
the MCP host: it writes JSON-RPC lines to the adapter's stdin and reads the replies from its stdout.

What is measured, and with which counter (all read from outside the two processes, through their handles):
  private bytes, working set   K32GetProcessMemoryInfo (PROCESS_MEMORY_COUNTERS_EX), sampled after every round
  handle count                 GetProcessHandleCount, sampled after every round
  CPU time                     GetProcessTimes (kernel + user, 100 ns units, accounted in scheduler ticks)
  CPU cycles                   QueryProcessCycleTime (exact cycle count charged to the process)
  MCP call latency             time.perf_counter_ns around "write request line, read reply line" in this script
  pipe round-trip time         rtt_us reported by the adapter (QueryPerformanceCounter around WriteFile/ReadFile)

Pass criteria, fixed before the first measured run and not tuned afterwards:
  every reply is a non-error result whose server sequence number is the expected next one (no lost,
      duplicated or rejected exchange), and the server log reports the same packet total with 0 rejected
  private bytes of each process grow by at most 65,536 bytes from the end of round 1 to the last sample
      (one leaked byte per call would exceed this bound after 65,536 calls)
  handle count of each process does not grow from the end of round 1 to the last sample
  during the idle window each process is charged at most 156,250 x 100 ns of CPU time (one 15.625 ms tick)
Exit code 0 when every criterion holds, 1 otherwise. Standard library only. Windows only.
"""
import argparse
from array import array
import ctypes
from ctypes import wintypes
import hashlib
import json
import os
from pathlib import Path
import platform
import subprocess
import sys
import tempfile
import time

PRIVATE_GROWTH_LIMIT = 65536      # bytes
IDLE_CPU_LIMIT = 156250           # 100 ns units = 15.625 ms
WARMUP_CALLS = 1000
POST_IDLE_CALLS = 1000
STATUS_EVERY = 10000              # a wire_status and a tools/list call are mixed in at this interval

kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)


class MemoryCountersEx(ctypes.Structure):
    _fields_ = [("cb", wintypes.DWORD), ("PageFaultCount", wintypes.DWORD),
                ("PeakWorkingSetSize", ctypes.c_size_t), ("WorkingSetSize", ctypes.c_size_t),
                ("QuotaPeakPagedPoolUsage", ctypes.c_size_t), ("QuotaPagedPoolUsage", ctypes.c_size_t),
                ("QuotaPeakNonPagedPoolUsage", ctypes.c_size_t), ("QuotaNonPagedPoolUsage", ctypes.c_size_t),
                ("PagefileUsage", ctypes.c_size_t), ("PeakPagefileUsage", ctypes.c_size_t),
                ("PrivateUsage", ctypes.c_size_t)]


kernel32.K32GetProcessMemoryInfo.argtypes = [wintypes.HANDLE, ctypes.POINTER(MemoryCountersEx), wintypes.DWORD]
kernel32.GetProcessHandleCount.argtypes = [wintypes.HANDLE, ctypes.POINTER(wintypes.DWORD)]
kernel32.GetProcessTimes.argtypes = [wintypes.HANDLE] + [ctypes.POINTER(wintypes.FILETIME)] * 4
kernel32.QueryProcessCycleTime.argtypes = [wintypes.HANDLE, ctypes.POINTER(ctypes.c_ulonglong)]


def sample(process: subprocess.Popen) -> dict:
    """Counters of a child process, read through its process handle."""
    handle = wintypes.HANDLE(int(process._handle))
    memory = MemoryCountersEx()
    memory.cb = ctypes.sizeof(memory)
    handles = wintypes.DWORD()
    creation, exited, kernel, user = (wintypes.FILETIME() for _ in range(4))
    cycles = ctypes.c_ulonglong()
    if not kernel32.K32GetProcessMemoryInfo(handle, ctypes.byref(memory), memory.cb):
        raise ctypes.WinError(ctypes.get_last_error())
    if not kernel32.GetProcessHandleCount(handle, ctypes.byref(handles)):
        raise ctypes.WinError(ctypes.get_last_error())
    if not kernel32.GetProcessTimes(handle, ctypes.byref(creation), ctypes.byref(exited), ctypes.byref(kernel),
                                    ctypes.byref(user)):
        raise ctypes.WinError(ctypes.get_last_error())
    if not kernel32.QueryProcessCycleTime(handle, ctypes.byref(cycles)):
        raise ctypes.WinError(ctypes.get_last_error())
    cpu = ((kernel.dwHighDateTime << 32) | kernel.dwLowDateTime) + ((user.dwHighDateTime << 32) | user.dwLowDateTime)
    return {"private_bytes": memory.PrivateUsage, "working_set_bytes": memory.WorkingSetSize,
            "peak_working_set_bytes": memory.PeakWorkingSetSize, "page_faults": memory.PageFaultCount,
            "handles": handles.value, "cpu_100ns": cpu, "cycles": cycles.value}


def distribution(values, scale: float = 1.0) -> dict:
    """min / p50 / p90 / p99 / p99.9 / max / mean of a sequence, each multiplied by scale."""
    ordered = sorted(values)
    count = len(ordered)

    def at(fraction: float) -> float:
        return ordered[min(count - 1, int(count * fraction))] * scale

    return {"samples": count, "min": ordered[0] * scale, "p50": at(0.50), "p90": at(0.90), "p99": at(0.99),
            "p999": at(0.999), "max": ordered[-1] * scale, "mean": sum(ordered) * scale / count}


class Host:
    """The MCP host side of one adapter process, with the bookkeeping the soak checks."""

    def __init__(self, mcp: Path):
        self.process = subprocess.Popen([str(mcp)], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                        stderr=subprocess.DEVNULL, bufsize=0)
        self.reader = self.process.stdout
        self.next_id = 0
        self.sequence = 0                    # server sequence number expected on the next pipe round trip
        self.tool_calls = 0
        self.call_ns = array("q")            # MCP call latency seen by this script
        self.pipe_us = array("L")            # pipe round trip measured inside the adapter

    def exchange(self, line: bytes) -> dict:
        self.process.stdin.write(line)
        raw = self.reader.readline()
        if not raw.endswith(b"\n"):
            raise RuntimeError(f"adapter closed stdout or sent a partial line: {raw!r}")
        return json.loads(raw)

    def request(self, method: str, params: dict | None = None) -> dict:
        self.next_id += 1
        message = {"jsonrpc": "2.0", "id": self.next_id, "method": method}
        if params is not None:
            message["params"] = params
        reply = self.exchange(json.dumps(message).encode("utf-8") + b"\n")
        if reply.get("id") != self.next_id or "result" not in reply:
            raise RuntimeError(f"unexpected reply to {method}: {reply!r}")
        return reply["result"]

    def roundtrip(self, index: int, record: bool) -> None:
        """One tools/call wire_roundtrip = one packet out and one verified receipt back."""
        self.next_id += 1
        kind = index & 3
        if kind == 0:
            arguments = b'{"count":1}'
        elif kind == 1:
            arguments = b'{"text":"task %d"}' % index
        elif kind == 2:
            arguments = b'{"opcode":"STATUS","text":"stage %d | %d%% | exit 0"}' % (index % 9, index % 101)
        else:
            arguments = b'{"count":1,"opcode":"PING"}'
        line = (b'{"jsonrpc":"2.0","id":%d,"method":"tools/call","params":{"name":"wire_roundtrip","arguments":%s}}\n'
                % (self.next_id, arguments))
        started = time.perf_counter_ns()
        self.process.stdin.write(line)
        raw = self.reader.readline()
        elapsed = time.perf_counter_ns() - started
        reply = json.loads(raw)
        result = reply["result"]
        body = result["structuredContent"]
        self.sequence += 1
        self.tool_calls += 1
        if (reply["id"] != self.next_id or result["isError"] or body["accepted"] != 1
                or body["last_sequence"] != self.sequence):
            raise RuntimeError(f"call {index}: expected sequence {self.sequence}, got {raw!r}")
        if record:
            self.call_ns.append(elapsed)
            self.pipe_us.append(body["rtt_us"]["first"])

    def status(self) -> dict:
        result = self.request("tools/call", {"name": "wire_status"})
        body = result["structuredContent"]
        self.sequence += 1
        self.tool_calls += 1
        if result["isError"] or body["state"] != "connected" or body["sequence"] != self.sequence:
            raise RuntimeError(f"wire_status: expected sequence {self.sequence}, got {body!r}")
        return body


def describe(path: Path) -> dict:
    data = path.read_bytes()
    return {"file": path.name, "bytes": len(data), "sha256": hashlib.sha256(data).hexdigest()}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--mcp", default="agent_wire_mcp.exe")
    parser.add_argument("--wire", default="agent_wire_ipc.exe")
    parser.add_argument("--rounds", type=int, default=20)
    parser.add_argument("--calls", type=int, default=50000, help="tools/call requests per round")
    parser.add_argument("--idle-seconds", type=float, default=60.0)
    parser.add_argument("--label", default="")
    parser.add_argument("--out-text", default="")
    parser.add_argument("--out-json", default="")
    options = parser.parse_args()
    if options.rounds < 2 or options.calls < 1:
        parser.error("--rounds must be at least 2 and --calls at least 1")
    mcp, wire = Path(options.mcp).resolve(), Path(options.wire).resolve()

    server_log = Path(tempfile.gettempdir()) / f"mcp_soak_server_{os.getpid()}.txt"
    with open(server_log, "wb") as log_file:
        server = subprocess.Popen([str(wire), "--server", "1"], stdout=log_file, stderr=subprocess.STDOUT)
    host = Host(mcp)
    failures: list[str] = []
    rounds: list[dict] = []
    try:
        initialized = host.request("initialize", {"protocolVersion": "2025-06-18", "capabilities": {},
                                                  "clientInfo": {"name": "mcp_soak", "version": "1"}})
        host.process.stdin.write(b'{"jsonrpc":"2.0","method":"notifications/initialized"}\n')
        tools = [tool["name"] for tool in host.request("tools/list")["tools"]]
        first = host.request("tools/call", {"name": "wire_status", "arguments": {"wait_ms": 5000}})
        if first["structuredContent"]["state"] != "connected":
            raise RuntimeError(f"no pipe server: {first!r}")
        host.sequence, host.tool_calls = 1, 1
        for index in range(WARMUP_CALLS):
            host.roundtrip(index, record=False)
        after_warmup = {"adapter": sample(host.process), "server": sample(server)}

        load_started = time.perf_counter()
        index = WARMUP_CALLS
        for number in range(1, options.rounds + 1):
            round_started = time.perf_counter()
            for _ in range(options.calls):
                host.roundtrip(index, record=True)
                index += 1
                if index % STATUS_EVERY == 0:
                    host.status()
                    host.request("tools/list")
            seconds = time.perf_counter() - round_started
            rounds.append({"round": number, "seconds": round(seconds, 3),
                           "calls_per_second": round(options.calls / seconds),
                           "adapter": sample(host.process), "server": sample(server)})
        load_seconds = time.perf_counter() - load_started

        before_idle = {"adapter": sample(host.process), "server": sample(server)}
        idle_started = time.perf_counter()
        time.sleep(options.idle_seconds)
        idle_seconds = time.perf_counter() - idle_started
        after_idle = {"adapter": sample(host.process), "server": sample(server)}

        for _ in range(POST_IDLE_CALLS):                 # still serving after the idle window
            host.roundtrip(index, record=False)
            index += 1
        final_status = host.status()
        final = {"adapter": sample(host.process), "server": sample(server)}
        expected_packets = host.sequence

        host.process.stdin.close()
        adapter_exit = host.process.wait(timeout=30)
        server_exit = server.wait(timeout=30)
    except Exception:
        for process in (host.process, server):
            if process.poll() is None:
                process.kill()
        raise
    finally:
        host.process.stdout.close()
    server_text = server_log.read_text(encoding="utf-8", errors="replace")
    server_log.unlink(missing_ok=True)

    # ---- verdicts -------------------------------------------------------------------------------
    if adapter_exit != 0:
        failures.append(f"adapter exit code {adapter_exit}")
    if server_exit != 0:
        failures.append(f"server exit code {server_exit}")
    closing = f"closed: {expected_packets} packets, 0 rejected"
    if closing not in server_text:
        failures.append(f"server log does not say '{closing}'")
    if final_status["round_trips"] != expected_packets or final_status["tool_calls"] != host.tool_calls:
        failures.append(f"adapter counters {final_status['round_trips']}/{final_status['tool_calls']} differ from "
                        f"{expected_packets}/{host.tool_calls}")
    baseline = rounds[0]
    growth, idle = {}, {}
    for name in ("adapter", "server"):
        private = final[name]["private_bytes"] - baseline[name]["private_bytes"]
        handles = final[name]["handles"] - baseline[name]["handles"]
        series = [entry[name]["private_bytes"] for entry in rounds]
        growth[name] = {"private_bytes_growth": private, "handles_growth": handles,
                        "private_bytes_round_min": min(series), "private_bytes_round_max": max(series),
                        "working_set_growth": final[name]["working_set_bytes"] - baseline[name]["working_set_bytes"]}
        if private > PRIVATE_GROWTH_LIMIT:
            failures.append(f"{name} private bytes grew by {private} (> {PRIVATE_GROWTH_LIMIT})")
        if handles > 0:
            failures.append(f"{name} handle count grew by {handles}")
        cpu = after_idle[name]["cpu_100ns"] - before_idle[name]["cpu_100ns"]
        cycles = after_idle[name]["cycles"] - before_idle[name]["cycles"]
        idle[name] = {"cpu_100ns": cpu, "cpu_ms": cpu / 10000, "cycles": cycles,
                      "cpu_percent_of_one_core": 100.0 * cpu / (idle_seconds * 1e7),
                      "private_bytes_change": after_idle[name]["private_bytes"] - before_idle[name]["private_bytes"]}
        if cpu > IDLE_CPU_LIMIT:
            failures.append(f"{name} used {cpu / 10000:.3f} ms of CPU while idle (> {IDLE_CPU_LIMIT / 10000} ms)")

    measured_calls = len(host.call_ns)
    call_us = distribution(host.call_ns, 0.001)
    pipe_us = distribution(host.pipe_us)
    load_cpu = {name: (before_idle[name]["cpu_100ns"] - after_warmup[name]["cpu_100ns"]) / 1e7
                for name in ("adapter", "server")}
    report = {
        "label": options.label, "verdict": "PASS" if not failures else "FAIL", "failures": failures,
        "environment": {"platform": platform.platform(), "machine": platform.machine(),
                        "processor": platform.processor(), "logical_cpus": os.cpu_count(),
                        "python": sys.version.split()[0], "runner": os.environ.get("RUNNER_NAME", "local"),
                        "github_run_id": os.environ.get("GITHUB_RUN_ID", ""), "github_sha": os.environ.get("GITHUB_SHA", "")},
        "binaries": [describe(mcp), describe(wire)],
        "protocol": {"negotiated": initialized["protocolVersion"], "server": initialized["serverInfo"], "tools": tools},
        "parameters": {"rounds": options.rounds, "calls_per_round": options.calls, "warmup_calls": WARMUP_CALLS,
                       "post_idle_calls": POST_IDLE_CALLS, "idle_seconds_requested": options.idle_seconds,
                       "status_and_tools_list_every": STATUS_EVERY},
        "criteria": {"private_bytes_growth_max": PRIVATE_GROWTH_LIMIT, "handles_growth_max": 0,
                     "idle_cpu_100ns_max": IDLE_CPU_LIMIT},
        "totals": {"measured_tool_calls": measured_calls, "tool_calls": host.tool_calls,
                   "pipe_round_trips": expected_packets, "json_rpc_requests": host.next_id,
                   "load_seconds": round(load_seconds, 3), "calls_per_second": round(measured_calls / load_seconds),
                   "adapter_cpu_seconds_under_load": round(load_cpu["adapter"], 3),
                   "server_cpu_seconds_under_load": round(load_cpu["server"], 3)},
        "mcp_call_latency_us": call_us, "pipe_round_trip_us": pipe_us,
        "memory": {"after_warmup": after_warmup, "end_of_round_1": {k: baseline[k] for k in ("adapter", "server")},
                   "final": final, "growth_round_1_to_final": growth},
        "idle": {"seconds": round(idle_seconds, 3), "before": before_idle, "after": after_idle, "delta": idle},
        "exit_codes": {"adapter": adapter_exit, "server": server_exit},
        "server_log_tail": server_text.strip().splitlines()[-2:],
        "rounds": rounds,
    }

    def kib(value: int) -> str:
        return f"{value / 1024:,.0f} KiB"

    lines = [
        f"agent_wire_mcp soak {options.label}".rstrip() + f": {report['verdict']}",
        f"  environment          {report['environment']['platform']}, {os.cpu_count()} logical CPUs, "
        f"Python {report['environment']['python']}, runner {report['environment']['runner']}",
        f"  binaries             " + "; ".join(f"{b['file']} {b['bytes']:,} B sha256 {b['sha256'][:16]}..." for b in report["binaries"]),
        f"  protocol             MCP {initialized['protocolVersion']} over stdio, tools {', '.join(tools)}",
        f"  load                 {options.rounds} rounds x {options.calls:,} tools/call wire_roundtrip = {measured_calls:,} "
        f"measured calls in {load_seconds:,.1f} s ({report['totals']['calls_per_second']:,} calls/s)",
        f"  exchanges            {expected_packets:,} pipe round trips over one pipe session, every server sequence "
        f"number in order, 0 rejected; {host.next_id:,} JSON-RPC requests in all",
        f"  MCP call latency     min {call_us['min']:.1f} us, p50 {call_us['p50']:.1f} us, mean {call_us['mean']:.1f} us, "
        f"p90 {call_us['p90']:.1f} us, p99 {call_us['p99']:.1f} us, p99.9 {call_us['p999']:.1f} us, max {call_us['max']:.1f} us "
        f"(request line written -> reply line read, in the Python host; {call_us['samples']:,} samples)",
        f"  pipe round trip      min {pipe_us['min']:.0f} us, p50 {pipe_us['p50']:.0f} us, mean {pipe_us['mean']:.1f} us, "
        f"p90 {pipe_us['p90']:.0f} us, p99 {pipe_us['p99']:.0f} us, p99.9 {pipe_us['p999']:.0f} us, max {pipe_us['max']:.0f} us "
        f"(QueryPerformanceCounter inside the adapter; {pipe_us['samples']:,} samples)",
    ]
    for name in ("adapter", "server"):
        lines.append(
            f"  {name + ' memory':<20} private bytes {kib(baseline[name]['private_bytes'])} after round 1 -> "
            f"{kib(final[name]['private_bytes'])} at the end: growth {growth[name]['private_bytes_growth']:+,} B "
            f"(limit +{PRIVATE_GROWTH_LIMIT:,}); per-round range {kib(growth[name]['private_bytes_round_min'])}.."
            f"{kib(growth[name]['private_bytes_round_max'])}; working set {kib(baseline[name]['working_set_bytes'])} -> "
            f"{kib(final[name]['working_set_bytes'])}, peak {kib(final[name]['peak_working_set_bytes'])}; "
            f"handles {baseline[name]['handles']} -> {final[name]['handles']}")
    for name in ("adapter", "server"):
        lines.append(
            f"  {name + ' idle CPU':<20} {idle[name]['cpu_ms']:.3f} ms of CPU time and {idle[name]['cycles']:,} cycles in "
            f"{idle_seconds:.1f} s idle = {idle[name]['cpu_percent_of_one_core']:.4f} % of one core "
            f"(limit {IDLE_CPU_LIMIT / 10000} ms); private bytes change {idle[name]['private_bytes_change']:+,} B")
    lines.append(f"  CPU under load       adapter {load_cpu['adapter']:.2f} s, server {load_cpu['server']:.2f} s of CPU time "
                 f"over {load_seconds:,.1f} s")
    lines.append(f"  after idle           {POST_IDLE_CALLS:,} more calls accepted; adapter exit {adapter_exit}, "
                 f"server exit {server_exit}; server log: {report['server_log_tail'][-1].strip() if report['server_log_tail'] else '(empty)'}")
    for failure in failures:
        lines.append(f"  FAIL                 {failure}")
    text = "\n".join(lines) + "\n"
    sys.stdout.write(text)
    if options.out_text:
        Path(options.out_text).write_text(text, encoding="utf-8")
    if options.out_json:
        Path(options.out_json).write_text(json.dumps(report, indent=1) + "\n", encoding="utf-8")
    return 0 if not failures else 1


if __name__ == "__main__":
    sys.exit(main())

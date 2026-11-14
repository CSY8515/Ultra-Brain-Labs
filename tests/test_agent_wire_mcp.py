"""Checks for agent_wire_mcp.exe: MCP over stdio (JSON-RPC 2.0) driven from Python, against the real pipe server.

Run on Windows after building:  python -m unittest -v tests.test_agent_wire_mcp
The adapter path defaults to ./agent_wire_mcp.exe (override with AGENT_WIRE_MCP); the pipe server is the
unmodified agent_wire_ipc.exe that tests.test_agent_wire_ipc uses. Standard library only.
Every line the adapter writes to stdout is parsed as JSON here, so anything that is not an MCP message fails a test.
"""
import ctypes
import json
import os
from pathlib import Path
import queue
import subprocess
import threading
import time
import unittest

from tests.test_agent_wire_ipc import EXE as WIRE_EXE, PIPE, TASK, STATUS, connect, finish, packet, start_server
from tests.test_win_zero_cli import imported_dlls

EXE = Path(os.environ.get("AGENT_WIRE_MCP", "agent_wire_mcp.exe")).resolve()
TOOLS = {"wire_status", "wire_roundtrip", "wire_server_start", "wire_close"}
SYNCHRONIZE = 0x00100000


def checksum_text(raw: bytes) -> str:
    """The checksum field of a 64-byte packet, formatted as the adapter reports it."""
    return f"0x{int.from_bytes(raw[20:24], 'little'):08X}"


class McpSession:
    """One agent_wire_mcp.exe process with the MCP host side of its stdio."""

    def __init__(self):
        self.process = subprocess.Popen([str(EXE)], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                        stderr=subprocess.PIPE, bufsize=0)
        self.lines = queue.Queue()
        self.next_id = 0
        threading.Thread(target=self._pump, daemon=True).start()

    def _pump(self):
        for raw in self.process.stdout:
            self.lines.put(raw)
        self.lines.put(None)

    def send(self, data: bytes):
        self.process.stdin.write(data)

    def read(self, timeout: float = 15) -> dict:
        raw = self.lines.get(timeout=timeout)
        if raw is None:
            raise AssertionError("the adapter closed stdout")
        if not raw.endswith(b"\n") or raw.count(b"\n") != 1:
            raise AssertionError(f"not a single newline-terminated line: {raw!r}")
        message = json.loads(raw.decode("utf-8"))   # strict: stdout carries MCP messages only
        if message.get("jsonrpc") != "2.0":
            raise AssertionError(f"not JSON-RPC 2.0: {raw!r}")
        return message

    def request(self, method: str, params=None, timeout: float = 15) -> dict:
        self.next_id += 1
        message = {"jsonrpc": "2.0", "id": self.next_id, "method": method}
        if params is not None:
            message["params"] = params
        self.send(json.dumps(message).encode("utf-8") + b"\n")
        reply = self.read(timeout)
        if reply.get("id") != self.next_id:
            raise AssertionError(f"reply to id {self.next_id} carries id {reply.get('id')!r}")
        return reply

    def notify(self, method: str, params=None):
        message = {"jsonrpc": "2.0", "method": method}
        if params is not None:
            message["params"] = params
        self.send(json.dumps(message).encode("utf-8") + b"\n")

    def initialize(self, version: str = "2025-06-18") -> dict:
        reply = self.request("initialize", {"protocolVersion": version, "capabilities": {},
                                            "clientInfo": {"name": "unittest", "version": "0"}})
        self.notify("notifications/initialized")
        return reply["result"]

    def call(self, tool: str, arguments=None, timeout: float = 15):
        """tools/call -> (structuredContent, isError). The text block must be the same JSON."""
        params = {"name": tool}
        if arguments is not None:
            params["arguments"] = arguments
        result = self.request("tools/call", params, timeout)["result"]
        content = result["content"]
        if len(content) != 1 or content[0]["type"] != "text":
            raise AssertionError(f"unexpected content: {content!r}")
        if json.loads(content[0]["text"]) != result["structuredContent"]:
            raise AssertionError("text block and structuredContent differ")
        return result["structuredContent"], result["isError"]

    def close(self) -> int:
        """Close stdin (the host going away) and return the exit code; stderr is kept in self.log."""
        self.process.stdin.close()
        try:
            self.process.wait(timeout=10)
        except subprocess.TimeoutExpired:
            self.process.kill()
            raise AssertionError("the adapter did not exit after stdin closed")
        self.log = self.process.stderr.read().decode("utf-8", "replace")
        self.process.stderr.close()
        self.process.stdout.close()
        return self.process.returncode

    def kill(self):
        if self.process.poll() is None:
            self.process.kill()
            self.process.wait()
        for stream in (self.process.stdin, self.process.stdout, self.process.stderr):
            if stream and not stream.closed:
                stream.close()


def pipe_is_absent(deadline: float = 5.0) -> bool:
    """True once nothing serves the pipe (opening it fails with file-not-found)."""
    end = time.monotonic() + deadline
    while True:
        try:
            open(PIPE, "r+b", buffering=0).close()
        except FileNotFoundError:
            return True
        except OSError:
            pass
        if time.monotonic() > end:
            return False
        time.sleep(0.01)


def pipe_is_served(deadline: float = 5.0) -> bool:
    """True once a server instance of the pipe is free. WaitNamedPipe does not connect, so the
    server's session count is untouched."""
    kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
    kernel32.WaitNamedPipeW.argtypes = [ctypes.c_wchar_p, ctypes.c_uint32]
    end = time.monotonic() + deadline
    while not kernel32.WaitNamedPipeW(PIPE, 1):
        if time.monotonic() > end:
            return False
        time.sleep(0.005)
    return True


class AgentWireMcpBinaryTests(unittest.TestCase):
    def test_imports_only_kernel32(self):
        self.assertEqual([name.upper() for name in imported_dlls(EXE)], ["KERNEL32.DLL"])

    def test_single_small_binary(self):
        self.assertLess(EXE.stat().st_size, 64 * 1024)

    def test_selftest_passes(self):
        result = subprocess.run([str(EXE), "--selftest"], capture_output=True, timeout=30)
        text = result.stdout.decode("utf-8")
        self.assertEqual(result.returncode, 0, text)
        self.assertIn("SELFTEST PASS 13/13", text)
        self.assertNotIn("FAIL", text)

    def test_version_and_usage_exit_codes(self):
        version = subprocess.run([str(EXE), "--version"], capture_output=True, timeout=10)
        self.assertEqual(version.stdout.decode("utf-8").strip(), "1.2.0")
        self.assertEqual(version.returncode, 0)
        self.assertEqual(subprocess.run([str(EXE), "--nope"], capture_output=True, timeout=10).returncode, 2)


class AgentWireMcpProtocolTests(unittest.TestCase):
    def setUp(self):
        self.session = McpSession()
        self.addCleanup(self.session.kill)

    def test_initialize_handshake_ping_and_tools_list(self):
        result = self.session.initialize("2025-06-18")
        self.assertEqual(result["protocolVersion"], "2025-06-18")
        self.assertEqual(result["capabilities"], {"tools": {"listChanged": False}})
        self.assertEqual((result["serverInfo"]["name"], result["serverInfo"]["version"]), ("agent_wire_mcp", "1.2.0"))
        self.assertIsInstance(result["instructions"], str)
        # notifications/initialized got no reply: the next line on stdout answers the ping
        self.assertEqual(self.session.request("ping")["result"], {})
        tools = self.session.request("tools/list")["result"]["tools"]
        self.assertEqual({tool["name"] for tool in tools}, TOOLS)
        for tool in tools:
            self.assertEqual(tool["inputSchema"]["type"], "object")
            self.assertIs(tool["inputSchema"]["additionalProperties"], False)
            self.assertIsInstance(tool["description"], str)
            self.assertIs(tool["annotations"]["openWorldHint"], False)
        schema = {tool["name"]: tool["inputSchema"]["properties"] for tool in tools}
        self.assertEqual(set(schema["wire_roundtrip"]), {"count", "opcode", "text"})
        self.assertEqual(schema["wire_roundtrip"]["count"]["maximum"], 16384)
        self.assertEqual(schema["wire_roundtrip"]["opcode"]["enum"], ["PING", "TASK", "STATUS"])
        self.assertEqual(set(schema["wire_status"]), {"wait_ms"})
        self.assertEqual(set(schema["wire_server_start"]), {"sessions"})
        self.assertEqual(self.session.close(), 0)
        self.assertIn("agent_wire_mcp 1.2.0 ready", self.session.log)   # the one stderr line

    def test_protocol_version_negotiation(self):
        for requested, answered in (("2024-11-05", "2024-11-05"), ("2025-03-26", "2025-03-26"),
                                    ("2025-11-25", "2025-11-25"), ("2099-01-01", "2025-11-25")):
            reply = self.session.request("initialize", {"protocolVersion": requested, "capabilities": {},
                                                        "clientInfo": {"name": "unittest", "version": "0"}})
            self.assertEqual(reply["result"]["protocolVersion"], answered)
        missing = self.session.request("initialize", {"capabilities": {}})
        self.assertEqual(missing["error"]["code"], -32602)
        self.assertEqual(self.session.close(), 0)

    def test_json_rpc_errors_and_framing(self):
        session = self.session
        session.send(b'{"jsonrpc":"2.0","id":1,"method":\n')                      # malformed JSON
        reply = session.read()
        self.assertEqual((reply["id"], reply["error"]["code"]), (None, -32700))
        session.send(b'[{"jsonrpc":"2.0","id":1,"method":"ping"}]\n')              # batch array
        reply = session.read()
        self.assertEqual((reply["id"], reply["error"]["code"]), (None, -32600))
        session.send(b'{"jsonrpc":"2.0","id":{"a":1},"method":"ping"}\n')          # id must be string or number
        self.assertEqual(session.read()["error"]["code"], -32600)
        session.send(b'{"id":3,"method":"ping"}\n')                                # jsonrpc member missing
        reply = session.read()
        self.assertEqual((reply["id"], reply["error"]["code"]), (3, -32600))
        self.assertEqual(session.request("resources/list")["error"]["code"], -32601)
        self.assertEqual(session.request("tools/call", {"name": "wire_nope"})["error"]["code"], -32602)
        self.assertEqual(session.request("tools/call", {"arguments": {}})["error"]["code"], -32602)
        # a string id, an escaped method name and CRLF framing
        session.send(b'{"jsonrpc":"2.0","id":"req-\\u00e9","method":"pi\\u006eg"}\r\n')
        reply = session.read()
        self.assertEqual((reply["id"], reply["result"]), ("req-é", {}))
        # two messages in one write, then a notification that must stay unanswered
        session.send(b'{"jsonrpc":"2.0","id":10,"method":"ping"}\n{"jsonrpc":"2.0","id":11,"method":"ping"}\n'
                     b'{"jsonrpc":"2.0","method":"notifications/cancelled","params":{"requestId":10}}\n')
        self.assertEqual([session.read()["id"], session.read()["id"]], [10, 11])
        # a message split across writes
        session.send(b'{"jsonrpc":"2.0","id":12,')
        time.sleep(0.05)
        session.send(b'"method":"ping"}\n')
        self.assertEqual(session.read()["id"], 12)
        # a line longer than the 64 KiB buffer is refused and the session keeps working
        session.send(b'{"jsonrpc":"2.0","id":13,"method":"ping","params":{"pad":"' + b"x" * 70000 + b'"}}\n')
        reply = session.read()
        self.assertEqual((reply["id"], reply["error"]["code"]), (None, -32600))
        self.assertEqual(session.request("ping")["result"], {})
        self.assertEqual(session.close(), 0)

    def test_tool_argument_errors_are_tool_results(self):
        for arguments in ({"count": 0}, {"count": 16385}, {"count": "5"}, {"count": 1.5}, {"text": "x" * 41},
                          {"text": 7}, {"opcode": "RECEIPT"}):
            body, is_error = self.session.call("wire_roundtrip", arguments)
            self.assertTrue(is_error, arguments)
            self.assertIn("error", body)
        body, is_error = self.session.call("wire_status", {"wait_ms": 5001})
        self.assertTrue(is_error)
        body, is_error = self.session.call("wire_close")
        self.assertEqual((body, is_error), ({"state": "closed", "was_connected": False}, False))
        self.assertEqual(self.session.close(), 0)

    def test_status_and_roundtrip_without_a_server(self):
        self.assertTrue(pipe_is_absent(), "another process serves the agent wire pipe")
        body, is_error = self.session.call("wire_status")
        self.assertFalse(is_error)
        self.assertEqual((body["state"], body["win32_error"], body["connects"]), ("no_server", 2, 0))
        self.assertEqual(body["pipe"], PIPE)
        t0 = time.monotonic()
        body, is_error = self.session.call("wire_roundtrip", {"count": 1})     # waits about 3 s for a server
        waited = time.monotonic() - t0
        self.assertTrue(is_error)
        self.assertEqual(body["state"], "no_server")
        self.assertGreater(waited, 2.5)
        self.assertLess(waited, 10)
        self.assertEqual(self.session.close(), 0)


class AgentWireMcpPipeTests(unittest.TestCase):
    """MCP stdio on one side, the real agent_wire_ipc --server pipe on the other."""

    def setUp(self):
        self.assertTrue(pipe_is_absent(), "another process serves the agent wire pipe")
        self.session = McpSession()
        self.addCleanup(self.session.kill)
        self.session.initialize()

    def test_status_and_roundtrips_through_the_real_pipe(self):
        server = start_server(1)
        try:
            status, is_error = self.session.call("wire_status", {"wait_ms": 5000})
            self.assertFalse(is_error)
            self.assertEqual((status["state"], status["sequence"], status["connects"]), ("connected", 1, 1))
            self.assertIsInstance(status["rtt_us"], int)

            ping, is_error = self.session.call("wire_roundtrip", {"count": 200})
            self.assertFalse(is_error)
            self.assertEqual((ping["opcode"], ping["count"], ping["accepted"]), ("PING", 200, 200))
            self.assertEqual((ping["first_sequence"], ping["last_sequence"]), (2, 201))
            self.assertEqual(ping["request_checksum"], "0x85BD5F54")              # golden PING checksum
            rtt = ping["rtt_us"]
            self.assertLessEqual(rtt["min"], rtt["p50"])
            self.assertLessEqual(rtt["p50"], rtt["p99"])
            self.assertLessEqual(rtt["p99"], rtt["max"])
            self.assertLess(rtt["avg"], 50_000, "average round trip should be well under 50 ms on a local pipe")

            hello, _ = self.session.call("wire_roundtrip", {"text": "hello"})    # text alone means TASK
            self.assertEqual((hello["opcode"], hello["payload_bytes"], hello["last_sequence"]), ("TASK", 5, 202))
            self.assertEqual(hello["request_checksum"], "0x716CD786")             # golden TASK "hello" checksum

            korean = "한글 ✓"                                         # 10 bytes of UTF-8
            task, _ = self.session.call("wire_roundtrip", {"opcode": "TASK", "text": korean})
            self.assertEqual(task["payload_bytes"], len(korean.encode("utf-8")))
            self.assertEqual(task["request_checksum"], checksum_text(packet(TASK, korean.encode("utf-8"))))

            stage, _ = self.session.call("wire_roundtrip", {"opcode": "STATUS", "text": "stage 3 | 42% | exit 0",
                                                            "count": 5})
            self.assertEqual((stage["opcode"], stage["accepted"], stage["last_sequence"]), ("STATUS", 5, 208))
            self.assertEqual(stage["request_checksum"], checksum_text(packet(STATUS, b"stage 3 | 42% | exit 0")))

            full, is_error = self.session.call("wire_roundtrip", {"text": "x" * 40, "count": 16384}, timeout=120)
            self.assertFalse(is_error)
            self.assertEqual((full["accepted"], full["last_sequence"]), (16384, 208 + 16384))

            status, _ = self.session.call("wire_status")
            self.assertEqual((status["state"], status["round_trips"], status["connects"], status["tool_calls"]),
                             ("connected", 209 + 16384, 1, 7))
            self.assertEqual(self.session.close(), 0)
            print(f"\n  MCP -> pipe: 200 PING min {rtt['min']} us, p50 {rtt['p50']} us, avg {rtt['avg']} us, "
                  f"p99 {rtt['p99']} us, max {rtt['max']} us (first {rtt['first']} us)")
        finally:
            log = finish(server)
        self.assertEqual(server.returncode, 0, log)
        self.assertIn(f"session 1          closed: {209 + 16384} packets, 0 rejected", log)

    def test_close_releases_the_pipe_and_the_next_call_is_a_new_session(self):
        server = start_server(2)
        try:
            first, _ = self.session.call("wire_roundtrip", {"count": 3})
            self.assertEqual((first["first_sequence"], first["last_sequence"]), (1, 3))
            closed, is_error = self.session.call("wire_close")
            self.assertEqual((closed, is_error), ({"state": "closed", "was_connected": True}, False))
            second, is_error = self.session.call("wire_roundtrip", {"count": 2})   # reconnects on its own
            self.assertFalse(is_error)
            self.assertEqual((second["first_sequence"], second["last_sequence"]), (1, 2))
            status, _ = self.session.call("wire_status")
            self.assertEqual((status["sequence"], status["connects"], status["round_trips"]), (3, 2, 6))
            self.assertEqual(self.session.close(), 0)
        finally:
            log = finish(server)
        self.assertEqual(server.returncode, 0, log)
        self.assertIn("session 1          closed: 3 packets, 0 rejected", log)
        self.assertIn("session 2          closed: 3 packets, 0 rejected", log)

    def test_busy_pipe_is_reported_and_then_served(self):
        server = start_server(2)
        try:
            with connect() as other_client:                                        # holds the single instance
                busy, is_error = self.session.call("wire_status")
                self.assertFalse(is_error)
                self.assertEqual((busy["state"], busy["win32_error"]), ("busy", 231))
                other_client.write(packet(TASK, b"held"))
                self.assertEqual(len(other_client.read(64)), 64)
            status, _ = self.session.call("wire_status", {"wait_ms": 5000})        # second session, after release
            self.assertEqual((status["state"], status["sequence"]), ("connected", 1))
            self.assertEqual(self.session.close(), 0)
        finally:
            log = finish(server)
        self.assertEqual(server.returncode, 0, log)
        self.assertIn("session 2          closed: 1 packets, 0 rejected", log)

    def test_server_start_does_nothing_when_the_pipe_is_served(self):
        server = start_server(1)
        try:
            self.assertTrue(pipe_is_served(), "the server did not create the pipe")
            started, is_error = self.session.call("wire_server_start")
            self.assertFalse(is_error)
            self.assertEqual((started["state"], started["owner"], started["pid"]),
                             ("already_running", "another process", 0))
            # the check did not use up the server's only session
            ping, is_error = self.session.call("wire_roundtrip", {"count": 1})
            self.assertFalse(is_error)
            self.assertEqual(ping["first_sequence"], 1)
            self.assertEqual(self.session.close(), 0)
        finally:
            log = finish(server)
        self.assertEqual(server.returncode, 0, log)
        self.assertIn("session 1          closed: 1 packets, 0 rejected", log)

    def test_server_start_runs_the_sibling_server_and_it_ends_with_the_adapter(self):
        sibling = EXE.with_name("agent_wire_ipc.exe")
        self.assertTrue(sibling.exists(), f"wire_server_start needs {sibling} next to the adapter")
        self.assertEqual(sibling.read_bytes(), WIRE_EXE.read_bytes(), "the sibling is not the server under test")
        kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
        kernel32.OpenProcess.restype = ctypes.c_void_p
        kernel32.OpenProcess.argtypes = [ctypes.c_uint32, ctypes.c_int, ctypes.c_uint32]
        kernel32.WaitForSingleObject.argtypes = [ctypes.c_void_p, ctypes.c_uint32]
        kernel32.CloseHandle.argtypes = [ctypes.c_void_p]

        started, is_error = self.session.call("wire_server_start")
        self.assertFalse(is_error, started)
        self.assertEqual((started["state"], started["ready"]), ("started", True))
        self.assertEqual(Path(started["exe"]), sibling)
        pid = started["pid"]
        self.assertGreater(pid, 0)
        handle = kernel32.OpenProcess(SYNCHRONIZE, 0, pid)
        self.assertTrue(handle, "the server process is not running")
        try:
            again, is_error = self.session.call("wire_server_start", {"sessions": 3})
            self.assertFalse(is_error)
            self.assertEqual((again["state"], again["owner"], again["pid"]), ("already_running", "this adapter", pid))
            ping, is_error = self.session.call("wire_roundtrip", {"count": 100})
            self.assertFalse(is_error, ping)
            self.assertEqual((ping["accepted"], ping["first_sequence"], ping["last_sequence"]), (100, 1, 100))
            status, _ = self.session.call("wire_status")
            self.assertEqual((status["state"], status["server_child_pid"]), ("connected", pid))
            self.assertEqual(kernel32.WaitForSingleObject(handle, 0), 258)        # WAIT_TIMEOUT: still running
            self.assertEqual(self.session.close(), 0)
            # the job object ends the server when the adapter exits; nothing is left serving the pipe
            self.assertEqual(kernel32.WaitForSingleObject(handle, 10000), 0)
        finally:
            kernel32.CloseHandle(handle)
        self.assertTrue(pipe_is_absent())


if __name__ == "__main__":
    unittest.main()

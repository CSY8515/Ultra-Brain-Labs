"""Checks for agent_wire_ipc.exe: imports, self-test, and the pipe protocol driven from Python.

Run on Windows after building:  python -m unittest -v tests.test_agent_wire_ipc
The executable path defaults to ./agent_wire_ipc.exe (override with AGENT_WIRE_IPC).
Standard library only. The Python side of the pipe (open / write / read / struct)
is the same code the README shows for connecting a Python agent to the C server.
"""
import os
from pathlib import Path
import struct
import subprocess
import time
import unittest

from tests.test_win_zero_cli import imported_dlls

EXE = Path(os.environ.get("AGENT_WIRE_IPC", "agent_wire_ipc.exe")).resolve()
PIPE = r"\\.\pipe\ultra_brain_agent_wire"
MAGIC, VERSION = 0x57495245, 0x0100
PING, TASK, STATUS, RECEIPT = 1, 2, 3, 4
PROJECT = 0xB1605844  # FNV-1a 32 of b"ultra_brain_agent_wire"
HEAD = "<IHHIHHI"      # magic, version, opcode, project_id, sender_id, receiver_id, payload_len


def fnv1a(data: bytes, h: int = 0x811C9DC5) -> int:
    for b in data:
        h = ((h ^ b) * 0x01000193) & 0xFFFFFFFF
    return h


def packet(opcode: int, payload: bytes = b"", project: int = PROJECT, sender: int = 2, receiver: int = 1) -> bytes:
    head = struct.pack(HEAD, MAGIC, VERSION, opcode, project, sender, receiver, len(payload))
    body = payload.ljust(40, b"\0")
    return head + struct.pack("<I", fnv1a(head + body)) + body   # checksum covers the other 60 bytes


def parse(raw: bytes) -> dict:
    magic, version, opcode, project, sender, receiver, length = struct.unpack_from(HEAD, raw)
    checksum = struct.unpack_from("<I", raw, 20)[0]
    body = raw[24:]
    return {"magic": magic, "version": version, "opcode": opcode, "project": project, "sender": sender,
            "receiver": receiver, "length": length, "checksum": checksum, "body": body,
            "checksum_ok": checksum == fnv1a(raw[:20] + body),
            "status": struct.unpack_from("<I", body, 0)[0], "request_checksum": struct.unpack_from("<I", body, 4)[0],
            "sequence": struct.unpack_from("<I", body, 8)[0], "request_opcode": struct.unpack_from("<H", body, 12)[0]}


def run(*args: str, timeout: float = 30) -> subprocess.CompletedProcess:
    return subprocess.run([str(EXE), *args], capture_output=True, timeout=timeout)


def start_server(sessions: int) -> subprocess.Popen:
    return subprocess.Popen([str(EXE), "--server", str(sessions)], stdout=subprocess.PIPE, stderr=subprocess.STDOUT)


def connect(deadline: float = 5.0):
    """Open the pipe as a client; retried while the server has not created it yet."""
    end = time.monotonic() + deadline
    while True:
        try:
            return open(PIPE, "r+b", buffering=0)
        except OSError:
            if time.monotonic() > end:
                raise
            time.sleep(0.005)


def finish(server: subprocess.Popen) -> str:
    try:
        out, _ = server.communicate(timeout=10)
    except subprocess.TimeoutExpired:
        server.kill()
        out, _ = server.communicate()
        raise AssertionError("server did not exit after its sessions:\n" + out.decode("utf-8", "replace"))
    return out.decode("utf-8")


class AgentWireIpcTests(unittest.TestCase):
    def test_imports_only_kernel32(self):
        self.assertEqual([name.upper() for name in imported_dlls(EXE)], ["KERNEL32.DLL"])

    def test_single_small_binary(self):
        self.assertLess(EXE.stat().st_size, 64 * 1024)

    def test_selftest_passes(self):
        result = run("--selftest")
        text = result.stdout.decode("utf-8")
        self.assertEqual(result.returncode, 0, text)
        self.assertIn("SELFTEST PASS 11/11", text)
        self.assertNotIn("FAIL", text)

    def test_version_and_usage_exit_codes(self):
        self.assertEqual(run("--version").stdout.decode("utf-8").strip(), "1.1.0")
        self.assertEqual(run().returncode, 2)
        self.assertEqual(run("--nope").returncode, 2)
        self.assertEqual(run("--bench", "0").returncode, 2)
        self.assertEqual(run("--bench", "16385").returncode, 2)
        self.assertEqual(run("--server", "x").returncode, 2)
        self.assertEqual(run("--client", "x" * 41).returncode, 2)

    def test_python_ping_round_trip(self):
        """The 64-byte protocol from Python: PING in, verified RECEIPT out, RTT measured."""
        server = start_server(1)
        try:
            with connect() as pipe:
                request = packet(PING)
                t0 = time.perf_counter()
                pipe.write(request)
                raw = pipe.read(64)
                rtt_us = (time.perf_counter() - t0) * 1e6
            self.assertEqual(len(raw), 64)
            receipt = parse(raw)
            self.assertTrue(receipt["checksum_ok"])
            self.assertEqual((receipt["magic"], receipt["version"], receipt["opcode"]), (MAGIC, VERSION, RECEIPT))
            self.assertEqual((receipt["sender"], receipt["receiver"], receipt["project"]), (1, 2, PROJECT))
            self.assertEqual(receipt["length"], 14)
            self.assertEqual(receipt["status"], 0)
            self.assertEqual(receipt["request_checksum"], struct.unpack_from("<I", request, 20)[0])
            self.assertEqual(receipt["request_checksum"], 0x85BD5F54)  # golden PING checksum
            self.assertEqual((receipt["sequence"], receipt["request_opcode"]), (1, PING))
            print(f"\n  python PING round trip: {rtt_us:.0f} us")
        finally:
            log = finish(server)
        self.assertEqual(server.returncode, 0, log)
        self.assertIn("1 packets, 0 rejected", log)

    def test_server_rejects_bad_packets_and_keeps_serving(self):
        server = start_server(1)
        try:
            with connect() as pipe:
                corrupted = bytearray(packet(PING))
                corrupted[30] ^= 0x01                     # payload bit flipped after the checksum was computed
                pipe.write(bytes(corrupted))
                bad_checksum = parse(pipe.read(64))
                pipe.write(b"short")                      # 5-byte message
                bad_size = parse(pipe.read(64))
                pipe.write(packet(RECEIPT))               # clients may not send RECEIPT
                bad_opcode = parse(pipe.read(64))
                pipe.write(packet(TASK, b"hello"))
                ok = parse(pipe.read(64))
            self.assertEqual(bad_checksum["status"], 6)
            self.assertTrue(bad_checksum["checksum_ok"])
            self.assertEqual((bad_size["status"], bad_size["project"], bad_size["receiver"]), (1, 0, 0))
            self.assertEqual(bad_opcode["status"], 4)
            self.assertEqual((ok["status"], ok["sequence"], ok["request_opcode"]), (0, 4, TASK))
            self.assertEqual(ok["request_checksum"], 0x716CD786)  # golden TASK "hello" checksum
        finally:
            log = finish(server)
        self.assertEqual(server.returncode, 0, log)
        self.assertIn("4 packets, 3 rejected", log)

    def test_client_and_bench_against_the_server(self):
        server = start_server(2)
        try:
            client = run("--client", "hello agent")
            bench = run("--bench", "200", timeout=120)
        finally:
            log = finish(server)
        client_text, bench_text = client.stdout.decode("utf-8"), bench.stdout.decode("utf-8")
        self.assertEqual(client.returncode, 0, client_text)
        self.assertIn("request           TASK", client_text)
        self.assertIn("payload 11 B", client_text)
        self.assertIn("status 0 (ACK_OK)", client_text)
        self.assertIn("result            ACCEPTED", client_text)
        self.assertEqual(bench.returncode, 0, bench_text)
        self.assertIn("result            200/200 receipts accepted", bench_text)
        stats = {}
        for line in bench_text.splitlines():
            parts = line.split()
            if parts and parts[0] in ("min", "p50", "avg", "p99", "max"):
                stats[parts[0]] = float(parts[1])
        self.assertEqual(set(stats), {"min", "p50", "avg", "p99", "max"})
        self.assertLessEqual(stats["min"], stats["p50"])
        self.assertLessEqual(stats["p50"], stats["max"])
        self.assertLess(stats["avg"], 50.0, "average round trip should be well under 50 ms on a local pipe")
        kib = int(bench_text.split("peak working set  ")[1].split(" KiB")[0])
        self.assertLessEqual(kib, 15 * 1024)
        print("\n  bench 200: " + ", ".join(f"{k} {v:.3f} ms" for k, v in stats.items()) + f", peak working set {kib} KiB")
        self.assertEqual(server.returncode, 0, log)
        self.assertIn("session 2          closed: 200 packets, 0 rejected", log)


if __name__ == "__main__":
    unittest.main()

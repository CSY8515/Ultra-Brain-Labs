"""Checks for win_zero_cli.exe: imports, exit codes, UTF-8 output, self-test.

Run on Windows after building:  python -m unittest -v tests.test_win_zero_cli
The executable path defaults to ./win_zero_cli.exe (override with WIN_ZERO_CLI).
The PE import table is read here with the standard library only, as an
independent check next to dumpbin / llvm-objdump.
"""
import os
from pathlib import Path
import struct
import subprocess
import unittest

EXE = Path(os.environ.get("WIN_ZERO_CLI", "win_zero_cli.exe")).resolve()


def imported_dlls(path: Path) -> list[str]:
    """DLL names in the PE import directory (PE32 and PE32+)."""
    data = path.read_bytes()
    pe = struct.unpack_from("<I", data, 0x3C)[0]
    if data[pe:pe + 4] != b"PE\0\0":
        raise ValueError("not a PE file")
    sections = struct.unpack_from("<H", data, pe + 6)[0]
    optional_size = struct.unpack_from("<H", data, pe + 20)[0]
    optional = pe + 24
    magic = struct.unpack_from("<H", data, optional)[0]
    directories = optional + (112 if magic == 0x20B else 96)
    import_rva, import_size = struct.unpack_from("<II", data, directories + 8)
    table = optional + optional_size

    def offset(rva: int) -> int:
        for i in range(sections):
            entry = table + 40 * i
            vsize, vaddr, rsize, raw = struct.unpack_from("<IIII", data, entry + 8)
            if vaddr <= rva < vaddr + max(vsize, rsize):
                return raw + rva - vaddr
        raise ValueError(f"RVA {rva:#x} outside every section")

    names, cursor = [], offset(import_rva) if import_rva else None
    while cursor is not None:
        name_rva = struct.unpack_from("<I", data, cursor + 12)[0]
        if name_rva == 0:
            break
        start = offset(name_rva)
        names.append(data[start:data.index(b"\0", start)].decode("ascii"))
        cursor += 20
    return names


def run(*args: str) -> subprocess.CompletedProcess:
    return subprocess.run([str(EXE), *args], capture_output=True, timeout=10)


class WinZeroCliTests(unittest.TestCase):
    def test_imports_only_kernel32(self):
        self.assertEqual([name.upper() for name in imported_dlls(EXE)], ["KERNEL32.DLL"])

    def test_greeting_is_utf8_korean_and_exits_zero(self):
        result = run()
        self.assertEqual(result.returncode, 0, result.stderr)
        text = result.stdout.decode("utf-8")  # strict: any non-UTF-8 byte fails
        self.assertIn("무의존성 CLI 레퍼런스", text)
        self.assertIn("덜그덕거림 0%", text)

    def test_selftest_passes_including_the_15_mib_working_set_limit(self):
        result = run("--selftest")
        text = result.stdout.decode("utf-8")
        self.assertEqual(result.returncode, 0, text)
        self.assertIn("SELFTEST PASS 4/4", text)
        kib = int(text.split("peak working set: ")[1].split(" KiB")[0])
        self.assertLessEqual(kib, 15 * 1024)

    def test_version_and_unknown_argument(self):
        self.assertEqual(run("--version").stdout.decode("utf-8").strip(), "1.0.0")
        self.assertEqual(run("--nope").returncode, 2)

    def test_single_small_binary(self):
        self.assertLess(EXE.stat().st_size, 64 * 1024)


if __name__ == "__main__":
    unittest.main()

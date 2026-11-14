# Ultra-Brain-Labs

[![Windows CI](https://github.com/CSY8515/Ultra-Brain-Labs/actions/workflows/ci.yml/badge.svg)](https://github.com/CSY8515/Ultra-Brain-Labs/actions/workflows/ci.yml)

Ultra-Brain 개발에서 나온 Windows 시스템 엔지니어링 결과물을 공개하는 저장소입니다. 세 도구 모두 **C11 파일 하나, C 런타임 없음, 임포트 DLL은 `KERNEL32.dll` 하나**입니다. 이 문서의 모든 수치는 출처(CI run 번호 또는 로컬 측정 조건)를 함께 적었고, 측정하지 않은 값은 적지 않았습니다.

| 파일 | 역할 |
|---|---|
| [`src/win_zero_cli.c`](src/win_zero_cli.c) | Part 1 — 무의존성 CLI 레퍼런스 (`win_zero_cli.exe`, 5,120 B) |
| [`src/agent_wire_ipc.c`](src/agent_wire_ipc.c) | Part 2 — Win32 named pipe 기반 로컬 에이전트 IPC (`agent_wire_ipc.exe`) |
| [`src/agent_wire_mcp.c`](src/agent_wire_mcp.c) | Part 3 — 위 파이프를 Claude Desktop·Cursor에 잇는 MCP stdio 어댑터 (`agent_wire_mcp.exe`, §5) |
| [`build.cmd`](build.cmd) | `build.cmd`(clang+lld) / `build.cmd msvc` — 둘 다 경고를 오류로 처리 |
| [`scripts/wire_scenario.ps1`](scripts/wire_scenario.ps1) | 서버 1 + 클라이언트 1회 + 벤치 1,000회 왕복 시나리오, 출력 원문 저장 |
| [`scripts/mcp_soak.py`](scripts/mcp_soak.py) | MCP `tools/call` ↔ 파이프 왕복 Soak. 메모리·핸들·스레드·유휴 CPU·지연을 프로세스 밖에서 측정 |
| [`tests/`](tests/) | Python 표준 라이브러리만으로 PE import 표 검사, 파이프 프로토콜 구동, MCP stdio JSON-RPC 구동, 종료 코드 검사 |
| [`.github/workflows/ci.yml`](.github/workflows/ci.yml) | clang job + MSVC job (아래 §1) |
| [`CHANGELOG.md`](CHANGELOG.md) · [`VERSION`](VERSION) | 변경 이력, 현재 1.2.0 |

```bat
build.cmd
```

---

## 1. Windows CI 배지가 보증하는 물리적 인증의 실체

배지 뒤에서 실제로 일어나는 일은 [`ci.yml`](.github/workflows/ci.yml) 한 파일에 전부 적혀 있습니다. 두 job이 **GitHub-hosted `windows-latest` 러너**(GitHub가 Microsoft Azure에 띄우는 순정 Windows 가상머신, 이 저장소 소유자가 손댈 수 없는 환경)에서 매 push·tag·PR마다 돕니다.

| 단계 | clang job | MSVC job | 실패 조건 |
|---|---|---|---|
| 빌드 | `clang -std=c11 -O2 -Wall -Wextra -pedantic -Werror -ffreestanding -nostdlib -fuse-ld=lld … -lkernel32` | `cl /std:c11 /O2 /W4 /WX /utf-8 /GS- … /link /SUBSYSTEM:CONSOLE /NODEFAULTLIB kernel32.lib` | 컴파일러가 **한 줄이라도** 출력하면 실패 (경고 0 강제) |
| 임포트 | `dumpbin /dependents` + `llvm-readobj --coff-imports` | `dumpbin /dependents` + `dumpbin /imports` | 의존 DLL 목록이 정확히 `KERNEL32.dll` 하나가 아니면 실패 |
| 자체 시험 | `win_zero_cli --selftest`(4) · `agent_wire_ipc --selftest`(11) · `agent_wire_mcp --selftest`(13) | 동일 | 종료 코드 ≠ 0 |
| 파이프 시나리오 | 서버 1 프로세스, `--client`, `--bench 1000` | 동일 | 영수증 1,000/1,000 미달, 서버 종료 코드 ≠ 0 |
| 단위 테스트 | Python 3.13 표준 라이브러리 26건 (PE 파서, 파이프 구동, MCP stdio 구동) | 동일 (v1.2.0부터) | 1건이라도 실패 |
| MCP Soak | [`scripts/mcp_soak.py`](scripts/mcp_soak.py) 기본 20라운드 × 250,000 `tools/call` + 유휴 60 s | 동일 | 서버 순번 불일치·거부 1건, private bytes 증가 > 65,536 B, 핸들 증가, 유휴 CPU 시간 > 15.625 ms 중 하나라도 |
| 산출물 | 세 exe + `bench_clang.txt` + `soak_clang.txt`·`.json` | 세 exe + `bench_msvc.txt` + `soak_msvc.txt`·`.json` | 파일 없으면 실패 |

v1.2.0의 증거 run: **[43613135707](https://github.com/CSY8515/Ultra-Brain-Labs/actions/runs/43613135707)** (커밋 `3c6404d`, 브랜치 `rc/v1.2.0`; `src/`·`tests/`·`scripts/`·`ci.yml`이 `v1.2.0`과 바이트 동일. Soak를 80라운드 × 250,000 호출 + 유휴 300 s로 늘려 수동 실행)와 기본 설정 run **[43612233351](https://github.com/CSY8515/Ultra-Brain-Labs/actions/runs/43612233351)** (같은 커밋). §2·§3의 수치는 v1.1.0 증거 run [43373628526](https://github.com/CSY8515/Ultra-Brain-Labs/actions/runs/43373628526)에서 온 것이고, 그 두 소스 파일은 이후 바뀌지 않았습니다. v1.2.0 run에서 확인된 사실:

- 컴파일러: clang **20.1.8** (러너 기본) / MSVC **19.51.36260**. 로컬 개발기는 clang 19.1.5 + MSVC 14.44로 같은 명령이 경고 0으로 통과했습니다.
- 세 도구 × 두 컴파일러 = 6개 바이너리 모두 의존 DLL `KERNEL32.dll` 1개. `agent_wire_ipc.exe`가 쓰는 함수 16개, `agent_wire_mcp.exe`가 쓰는 함수 23개 전부 로그에 열거됨.
- 자체 시험 4/4 · 11/11 · 13/13, 파이프 시나리오, 단위 테스트 26건(두 job 각각), MCP Soak 모두 종료 코드 0.

배지가 **증명하지 않는 것**도 분명히 적습니다. (1) 바이너리는 서명되지 않았습니다. Smart App Control이 켜진 PC는 CI가 만든 exe도, 로컬에서 직접 빌드한 exe도 차단할 수 있습니다(개발기에서 실제로 `An Application Control policy has blocked this file`로 막혀 로컬 실행은 NOT_RUN). (2) 러너는 x64 Windows Server 한 종류입니다. ARM64·구형 Windows·회사 정책(AppLocker/WDAC)은 따로 확인해야 합니다. (3) `KERNEL32.dll`만 임포트해도 함수별 최소 Windows 버전은 다릅니다. 두 도구가 쓰는 `K32GetProcessMemoryInfo`는 Windows 7 이상입니다.

---

## 2. Windows 환경 길막 분쇄 원리와 도구 (`win_zero_cli.c`)

### 해결하는 문제
Windows에서 CLI·봇을 만들 때 멈추는 지점은 네 가지로 좁혀집니다. **파이썬 가상환경 충돌**(인터프리터·패키지 버전 불일치), **DLL 누락**(`vcruntime140.dll`, `msvcp140.dll`, `ucrtbase.dll` 요구), **Smart App Control 오탐**(서명 없는 exe 차단), **안티바이러스 오인 차단**(패커로 만든 단일 exe가 휴리스틱에 걸림). 원인은 Windows가 아니라 설계입니다. 런타임을 들고 다니지 않고, 모든 Windows에 이미 있는 DLL 하나만 쓰고, 바이너리를 내려받는 대신 소스를 그 자리에서 빌드하면 네 문제의 표면이 사라집니다.

### 물리적 제원 (실측)
| 항목 | 값 | 출처 |
|---|---|---|
| 단일 실행 파일 | **5,120 B** (clang+lld) / 5,632 B (MSVC) | CI run 43373628526, v1.0.0 run 43205024784도 5,120 B |
| 임포트 | `KERNEL32.dll` — `GetStdHandle`, `WriteFile`, `SetConsoleOutputCP`, `GetCommandLineW`, `GetCurrentProcess`, `K32GetProcessMemoryInfo`, `ExitProcess` | 같은 run의 `llvm-readobj` |
| 최대 작업 집합(RAM) | **2,860 KiB (2.79 MiB)** v1.0.0 run / 3,336 KiB (3.26 MiB) 이번 run | 자체 시험이 `K32GetProcessMemoryInfo`로 측정. run마다 다릅니다 |
| 컴파일+링크 시간 | **0.23 ~ 0.38 s** | 개발기 clang 19.1.5 (`Measure-Command`, 3회). 0.1 s가 아닙니다 |
| C 런타임 | 없음 (`-ffreestanding -nostdlib`, 진입점 `wz_main`) | 소스 |
| 메모리 모델 | 힙 없음, 64 KiB 정적 아레나 | 소스 |

### 원리 세 줄
1. **C 런타임을 빼면 DLL 의존이 사라진다.** 보통의 `clang`/`cl` 빌드가 끌어오는 `ucrtbase.dll`·`vcruntime140.dll`이 바로 "DLL 누락"의 정체입니다. 진입점을 직접 쓰고(`mainCRTStartup` 또는 임의 이름) 필요한 Win32 함수를 직접 선언하면 링커가 넣을 DLL은 `KERNEL32.dll`만 남습니다. 정적 런타임(`/MT`)도 대안이지만 파일이 커집니다.
2. **인코딩은 시작 시 한 번 고정한다.** `SetConsoleOutputCP(65001)` 뒤 모든 출력을 UTF-8 바이트로 씁니다. CP949 콘솔에서 한글이 깨지는 문제가 여기서 끝납니다.
3. **빌드 산출물이 아니라 소스를 배포한다.** 받는 쪽이 한 줄 명령으로 자기 PC에서 굽습니다. 내려받은 exe에 붙는 Mark-of-the-Web은 생기지 않습니다. 단, Smart App Control은 그 표시와 별개로 **서명 없는 exe를 평판으로 차단**할 수 있으므로(§1), 보안 설정을 끄는 대신 코드 서명이나 SAC가 꺼진 환경·CI에서 검증하는 것이 정답입니다.

### AI 환각 방지 원샷 지휘문 (복사해서 Claude / ChatGPT / Cursor에 붙여넣기)

```markdown
[목표: Windows에서 설치 없이 바로 실행되는 단일 C11 실행 파일 작성 — 외부 의존성 0]

너는 Windows 저수준 시스템 엔지니어다. 아래 규칙을 어기는 코드는 제출하지 않는다.

1. 의존성: 임포트 DLL은 KERNEL32.dll 하나만 허용한다. USER32, WS2_32, ADVAPI32, 어떤 C 런타임 DLL
   (ucrtbase, vcruntime140, msvcp140)도 금지. C 런타임 없이 빌드한다(-ffreestanding -nostdlib 또는
   /NODEFAULTLIB, 진입점 직접 정의, memset/memcpy 직접 구현). Python·Node·pip 설치 요구 금지.
2. 검증: 코드와 함께 (a) 한 줄 빌드 명령, (b) `dumpbin /dependents` 또는 `llvm-readobj --coff-imports`
   결과를 KERNEL32.dll 하나로 확인하는 명령, (c) 종료 코드로 성패를 알리는 --selftest 모드를 제시한다.
3. 경고 0: clang `-Wall -Wextra -pedantic -Werror`와 MSVC `/W4 /WX` 둘 다 통과해야 한다.
4. 인코딩: 시작 시 SetConsoleOutputCP(65001) 호출, 모든 출력은 UTF-8 바이트.
5. 메모리: 힙 대신 정적 버퍼/아레나. 자체 시험에서 K32GetProcessMemoryInfo로 최대 작업 집합을 측정해
   출력하고 15 MiB 이하를 확인한다.
6. 환각 금지: 측정하지 않은 성능 수치, 존재를 확인하지 않은 API, 실행하지 않은 테스트 결과를 적지 않는다.
   모르면 "미측정/미확인"이라고 쓴다. 쓰는 KERNEL32 함수마다 최소 Windows 버전을 적는다.
7. 서명: 실행 파일은 서명되지 않았음을 명시하고, Smart App Control이 차단할 수 있음과 서명 전 검증 경로
   (CI 또는 SAC 비활성 환경)를 함께 적는다.
```

이 지휘문으로 만든 결과물이 이 저장소의 두 도구입니다. 규칙 2·3·6은 `ci.yml`이 기계적으로 집행합니다.

---

## 3. 슬랙·토큰 없는 0.4 ms Native Agent IPC Pipe (`agent_wire_ipc.c`)

### 해결하는 문제
같은 PC 안의 에이전트 둘(예: Claude Code 세션과 작업 러너, 또는 파이썬 봇과 C 도구)이 상태를 주고받으려 할 때 흔한 선택은 Slack 봇, 로컬 HTTP/WebSocket 서버, 파일 폴링입니다. 각각 토큰 발급·서버 프로세스·포트 개방·방화벽 예외가 붙고, 왕복은 수십~수백 ms입니다. Win32 named pipe는 커널이 이미 제공하는 로컬 IPC입니다. 토큰·포트·외부 런타임 없이 **두 프로세스가 64바이트 패킷을 메모리로 직접 핑퐁**합니다.

### 작동 원리
- 파이프 `\\.\pipe\ultra_brain_agent_wire`, 메시지 모드(`PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE`), 원격 클라이언트 거부(`PIPE_REJECT_REMOTE_CLIENTS`), 인스턴스 1개, 입·출력 버퍼 각 **4,096 B**.
- 파이프 계층이 쓰는 KERNEL32 함수는 **6개**: `CreateNamedPipeA`, `ConnectNamedPipe`, `CreateFileA`, `ReadFile`, `WriteFile`, `CloseHandle`. 나머지 10개 임포트(`GetStdHandle`, `GetCommandLineW`, `GetLastError`, `QueryPerformanceCounter/Frequency`, `Sleep`, `SetConsoleOutputCP`, `GetCurrentProcess`, `K32GetProcessMemoryInfo`, `ExitProcess`)는 콘솔 출력·인자·시간 측정·종료용입니다. 소켓·스레드·힙 없음.
- 서버는 세션(클라이언트 연결 1회)을 순차로 처리합니다. 세션이 끝나면 핸들을 닫고 파이프를 다시 만듭니다. 클라이언트는 `CreateFileA`가 `ERROR_FILE_NOT_FOUND`/`ERROR_PIPE_BUSY`를 돌려주면 1 ms 간격으로 최대 약 3 s 재시도합니다.

### 정규 패킷 (64 B 고정, little-endian, `#pragma pack(1)`)
```c
typedef struct {
    uint32_t magic;        /* 0x57495245 ("WIRE"; 바이트열 45 52 49 57) */
    uint16_t version;      /* 0x0100 */
    uint16_t opcode;       /* 1 PING, 2 TASK, 3 STATUS, 4 RECEIPT(서버→클라이언트 전용) */
    uint32_t project_id;   /* 프로젝트 해시. 기본값 0xB1605844 = FNV-1a 32("ultra_brain_agent_wire") */
    uint16_t sender_id;    /* 송신 에이전트 */
    uint16_t receiver_id;  /* 수신 에이전트 */
    uint32_t payload_len;  /* 0..40 */
    uint32_t checksum;     /* FNV-1a 32, 도메인 = 이 필드를 제외한 나머지 60바이트 ([0,20) ∪ [24,64)) */
    uint8_t  payload[40];
} agent_packet_t;          /* offsetof(checksum) == 20, offsetof(payload) == 24 — 소스의 static assert */
```
- **RECEIPT 본문**(`payload_len` 14): `status u32 | request_checksum u32 | sequence u32 | request_opcode u16`. `sender_id`=1(서버), `receiver_id`=요청의 `sender_id`, `project_id` 에코.
- **status**: 0 `ACK_OK`, 1 `BAD_SIZE`(메시지가 64 B가 아님), 2 `BAD_MAGIC`, 3 `BAD_VERSION`, 4 `BAD_OPCODE`(클라이언트가 RECEIPT를 보냄 등), 5 `BAD_LENGTH`(>40), 6 `BAD_CHECKSUM`. 거부된 패킷에도 영수증은 돌아가고 세션은 유지됩니다.
- 골든 값(자체 시험·Python 테스트가 같은 값을 확인): PING(기본 project, 2→1, payload 0) 체크섬 `0x85BD5F54`; TASK `"hello"` 체크섬 `0x716CD786`.
- 종료 코드: 0 성공, 1 파이프/시스템 오류, 2 사용법, 3 영수증 거부(status≠0 또는 체크섬·에코 불일치).

### 모드
```text
agent_wire_ipc --server [sessions]   파이프 생성, 접속 대기, 패킷마다 검증 후 RECEIPT 즉답 (기본: 무한)
agent_wire_ipc --client [text]       PING(또는 TASK text ≤ 40 B) 송신, 영수증 검증, 왕복 시간 µs 출력
agent_wire_ipc --bench [count]       count회(기본 1,000, 최대 16,384) PING 왕복, min/p50/avg/p99/max·처리량·최대 작업 집합
agent_wire_ipc --selftest            레이아웃·FNV 벡터·골든 체크섬·검증·영수증·포매터 11건
agent_wire_ipc --version             1.1.0
```

### 벤치마크 실측치 — CI run [43373628526](https://github.com/CSY8515/Ultra-Brain-Labs/actions/runs/43373628526), `windows-latest`, 1,000회 PING→RECEIPT 왕복, 영수증 전부 검증

| 빌드 | min | p50 | avg | p99 | max | 처리량 | 최대 작업 집합 | exe 크기 |
|---|---|---|---|---|---|---|---|---|
| clang 20.1.8 + lld | 0.005 ms | 0.031 ms | **0.040 ms** | 0.066 ms | 7.859 ms | 24,596 왕복/s | 2,960 KiB (2.89 MiB) | 23,552 B |
| MSVC 19.51 | 0.018 ms | 0.026 ms | **0.038 ms** | 0.063 ms | 9.770 ms | 25,855 왕복/s | 2,980 KiB (2.91 MiB) | 22,016 B |
| Python 3.13 클라이언트 → C 서버 (단위 테스트, 200회) | 0.027 ms | 0.033 ms | 0.034 ms | 0.063 ms | 0.084 ms | — | 2,940 KiB | — |

- **"0.4 ms"는 상한으로 읽으십시오.** 지시 사양은 0.4 ms급이었고, 실측 평균은 그 10분의 1(0.04 ms)입니다. 러너는 공유 가상머신이라 run마다 수치가 달라집니다. 같은 run의 Python 단일 PING 왕복은 58 µs였습니다.
- **max 7.9~9.8 ms는 첫 왕복입니다.** `--client`의 단발 TASK 왕복도 9.7 ms(clang)/10.5 ms(MSVC)로, 연결 직후 첫 패킷이 스케줄링·페이지 폴트 비용을 떠안습니다. 두 번째 패킷부터 수십 µs입니다. 지연이 중요한 쪽은 연결 후 PING 한 번으로 워밍업하십시오.
- **로컬 측정은 NOT_RUN.** 개발기는 Smart App Control이 빌드 직후의 exe 실행을 차단해 수치를 내지 못했습니다. 위 표의 모든 숫자는 CI 로그에서 복사했고 산출물 `labs-clang`/`labs-msvc`의 `bench_*.txt`가 원문입니다.
- 작업 집합 2.9 MiB는 64 B 패킷 1,000개를 돌린 클라이언트 프로세스의 최대값입니다. 정적 버퍼(벤치 표본 128 KiB, 파이프 inbox 4 KiB)를 포함합니다.

### 파이썬 연동 (표준 라이브러리만, 20줄)
서버를 `agent_wire_ipc --server`로 띄운 뒤 아무 파이썬 에이전트에서 다음을 실행합니다. 같은 코드가 [`tests/test_agent_wire_ipc.py`](tests/test_agent_wire_ipc.py)에서 CI마다 돕니다.

```python
import struct, time
PIPE = r"\\.\pipe\ultra_brain_agent_wire"
def fnv1a(data, h=0x811C9DC5):
    for b in data: h = ((h ^ b) * 0x01000193) & 0xFFFFFFFF
    return h
def packet(opcode, payload=b"", project=0xB1605844, sender=2, receiver=1):
    head = struct.pack("<IHHIHHI", 0x57495245, 0x0100, opcode, project, sender, receiver, len(payload))
    body = payload.ljust(40, b"\0")
    return head + struct.pack("<I", fnv1a(head + body)) + body          # checksum = other 60 bytes
with open(PIPE, "r+b", buffering=0) as pipe:                            # CreateFile on the pipe
    request = packet(2, b"task: summarize README")                      # 2 = TASK (<= 40 bytes)
    t0 = time.perf_counter(); pipe.write(request); raw = pipe.read(64)  # one 64-byte RECEIPT back
    rtt_us = (time.perf_counter() - t0) * 1e6
status, echoed, sequence, req_opcode = struct.unpack_from("<IIIH", raw, 24)
assert raw[:4] == b"ERIW" and struct.unpack_from("<H", raw, 6)[0] == 4  # magic, RECEIPT
assert struct.unpack_from("<I", raw, 20)[0] == fnv1a(raw[:20] + raw[24:])  # receipt checksum
assert status == 0 and echoed == struct.unpack_from("<I", request, 20)[0]   # ACK_OK for our packet
print(f"ACK sequence {sequence} for opcode {req_opcode}, rtt {rtt_us:.0f} us")
```
`open()`이 곧 `CreateFile`입니다. `pywin32`·`win32file`·소켓·HTTP 라이브러리가 필요 없습니다. 거부된 패킷은 예외가 아니라 `status` 1~6으로 돌아옵니다.

---

## 4. 실전 연계: Zero-Dependency Local Multi-Agent Architecture

Part 1은 에이전트가 **도구를 실행하는 비용**을, Part 2는 에이전트끼리 **상태를 주고받는 비용**을 깎습니다. 둘을 합친 구성은 다음과 같습니다.

```text
   Agent A (조정자: Claude Code / Python 봇)              Agent B (실행자: 로컬 러너)
   ┌──────────────────────────────┐                      ┌──────────────────────────────┐
   │ 1. 작업 분해, project_id 결정 │    TASK  (64 B)      │ 4. TASK 수신 → 도구 실행      │
   │ 2. TASK 송신 (sender 2→1)     │ ───────────────────▶ │    win_zero_cli 류 5 KB CLI   │
   │ 3. RECEIPT 검증, sequence 기록│ ◀─────────────────── │    (설치 없음, KERNEL32 전용) │
   │                              │   RECEIPT (64 B)     │ 5. 진척도를 STATUS로 보고     │
   │ 6. STATUS 수신 → 다음 TASK    │ ◀──────────────────▶ │    (payload: 단계/백분율/코드) │
   └──────────────────────────────┘  \\.\pipe\ultra_brain_agent_wire   └──────────────────────────────┘
                 ▲ 메모리 직통, 커널 named pipe, 왕복 ≈ 0.04 ms (CI 실측 평균), 네트워크 스택 미사용
```

- **역할 고정.** 파이프 인스턴스는 1개이고 서버(`sender_id` 1)가 영수증을 발행합니다. 조정자와 실행자 중 누가 서버가 되든 상관없지만, 한 세션에서 요청 방향은 클라이언트→서버 하나입니다. 양방향 요청이 필요하면 각자 서버 하나씩, 파이프 두 개(이름을 바꿔 빌드)로 둡니다.
- **충돌 없는 분담.** `project_id`(프로젝트 이름의 FNV-1a 32)가 네임스페이스, `sender_id`/`receiver_id`가 에이전트 주소, 서버의 `sequence`가 세션 안의 단조 증가 카운터입니다. 같은 TASK가 두 번 가면 영수증의 `request_checksum`이 같으므로 조정자가 중복을 식별합니다. 손상된 패킷은 `BAD_CHECKSUM` 영수증으로 돌아오고 세션은 끊기지 않습니다.
- **진척도 핑퐁.** STATUS payload 40 B에 "단계 번호 | 백분율 | 마지막 종료 코드 | 짧은 메시지"를 넣습니다(형식은 두 에이전트의 약속이며 서버는 길이·체크섬만 검사합니다). 조정자는 영수증 `sequence`로 유실을 감지합니다.
- **완전 오프라인.** 두 프로세스는 네트워크 스택을 열지 않습니다(WS2_32 임포트 없음). 파이프는 `PIPE_REJECT_REMOTE_CLIENTS`로 원격 접속을 거부합니다. 인터넷·DNS·토큰 서버가 없어도 CI의 시나리오와 동일하게 돕니다. Windows Sandbox의 `<Networking>Disable</Networking>` 환경에서도 동작 조건은 같습니다(이 저장소는 그 환경을 CI로 대신했고, 샌드박스 실측은 하지 않았습니다).
- **제공하지 않는 것.** 브로드캐스트·다중 클라이언트 동시 접속(인스턴스 1), 메시지 영속화, 로컬 ACL 이상의 인증, 40 B를 넘는 payload(파일 경로나 해시를 보내고 내용은 파일로 주고받는 것이 설계 의도입니다).

---

## 5. MCP 어댑터: Claude Desktop·Cursor에서 파이프 직결 (`agent_wire_mcp.c`)

### 해결하는 문제
§3의 파이프는 64바이트 패킷을 직접 조립하는 프로세스만 쓸 수 있습니다. MCP 호스트(Claude Desktop, Cursor, Claude Code)는 자식 프로세스의 stdio로 JSON-RPC 2.0을 주고받습니다. `agent_wire_mcp.exe`는 그 둘 사이의 번역기입니다. 호스트가 이 실행 파일 하나를 띄우면 모델이 툴 호출로 파이프 상태를 점검하고 패킷을 왕복시킵니다. Node·Python·MCP SDK·JSON 라이브러리를 쓰지 않습니다. C11 파일 하나, C 런타임 없음, 임포트 DLL은 `KERNEL32.dll` 하나라는 조건은 그대로입니다.

`agent_wire_ipc.c`는 v1.1.0과 바이트 단위로 같습니다(git blob `55c0c68`). 어댑터는 패킷 레이아웃과 체크섬 정의를 자기 파일 안에 따로 갖고, 일치 여부는 골든 체크섬(자체 시험)과 수정하지 않은 서버 바이너리와의 실제 왕복(단위 테스트, Soak)으로 확인합니다.

```text
 MCP 호스트 (Claude Desktop / Cursor)        agent_wire_mcp.exe                agent_wire_ipc.exe --server
 ┌────────────────────────────┐  stdin   ┌──────────────────────┐  64 B 패킷  ┌──────────────────────┐
 │ tools/call wire_roundtrip  │ ───────▶ │ JSON-RPC 2.0 해석     │ ──────────▶ │ 검증 후 RECEIPT 즉답  │
 │ 결과(JSON) 수신             │ ◀─────── │ 영수증 검증, µs 측정   │ ◀────────── │                      │
 └────────────────────────────┘  stdout  └──────────────────────┘  named pipe └──────────────────────┘
        한 줄 = JSON-RPC 메시지 1개          스레드 생성·힙 API·타이머 없음          \\.\pipe\ultra_brain_agent_wire
```

### 연결 설정 (복사해서 붙여넣기)
`build.cmd`로 만든 `agent_wire_mcp.exe`와 `agent_wire_ipc.exe`를 같은 폴더에 둡니다(`wire_server_start`가 옆 폴더가 아닌 **같은 폴더**의 `agent_wire_ipc.exe`만 실행합니다). 아래 경로는 예시이므로 실제 위치로 바꾸고, JSON 안의 역슬래시는 두 번씩 씁니다.

**Claude Desktop** — 설정 → 개발자 → 구성 편집으로 열리는 `claude_desktop_config.json`(보통 `%APPDATA%\Claude\claude_desktop_config.json`):

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

저장한 뒤 Claude Desktop을 완전히 종료했다가 다시 실행합니다. **Cursor**는 같은 `mcpServers` 블록을 `%USERPROFILE%\.cursor\mcp.json`(전역) 또는 프로젝트의 `.cursor\mcp.json`에 넣습니다. **Claude Code**는 한 줄입니다.

```bat
claude mcp add agent-wire -- C:\Tools\Ultra-Brain-Labs\agent_wire_mcp.exe
```

연결 후 첫 대화 예: "wire_status로 파이프 상태를 확인하고, 서버가 없으면 wire_server_start로 띄운 다음 wire_roundtrip을 1000회 돌려 줘."

### 툴 4개
| 툴 | 인자 | 동작과 반환 |
|---|---|---|
| `wire_status` | `wait_ms` 0..5000 (기본 0) | 미연결이면 연결을 시도하고 PING 1회 왕복. `state`(`connected`/`no_server`/`busy`/`error`), `rtt_us`, 서버 `sequence`, 누적 `connects`·`round_trips`·`tool_calls`, `server_child_pid` |
| `wire_roundtrip` | `count` 1..16384 (기본 1), `opcode` `PING`/`TASK`/`STATUS`, `text` ≤ 40 B UTF-8 | `count`회 왕복하며 영수증을 매번 검증(매직·버전·FNV-1a 체크섬·요청 체크섬 에코·opcode·project·수신자). `accepted`, `request_checksum`, `first_sequence`/`last_sequence`, `rtt_us`{`first`,`min`,`p50`,`avg`,`p99`,`max`}, `total_us`. 서버가 없으면 최대 3 s 기다린 뒤 오류 |
| `wire_server_start` | `sessions` (기본 0 = 어댑터 종료 시까지) | 파이프가 이미 서비스 중이면 아무것도 하지 않고 `already_running`. 아니면 같은 폴더의 `agent_wire_ipc.exe --server N`을 창 없이 실행. 정지 툴은 없음 |
| `wire_close` | 없음 | 어댑터의 파이프 연결을 닫아 다른 클라이언트에 인스턴스를 양보. 다음 호출이 새 세션으로 다시 연결 |

- **연결은 유지됩니다.** 파이프 인스턴스는 1개이므로(§3), 어댑터가 연결을 쥐고 있는 동안 다른 클라이언트는 `ERROR_PIPE_BUSY`를 받습니다. 반대로 다른 클라이언트가 쥐고 있으면 `wire_status`가 `busy`를 돌려줍니다. 양보하려면 `wire_close`를 호출합니다.
- **`wire_server_start`의 안전 범위.** 실행 파일 이름과 인자가 코드에 고정되어 있어 임의 명령을 실행할 수 없습니다. 자식의 표준 핸들 3개는 `NUL`이고 어댑터의 stdio 핸들은 상속되지 않으므로 서버 출력이 MCP 채널에 섞이지 않습니다. 자식은 `JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE` job에 묶여 어댑터가 끝나면 함께 끝납니다(단위 테스트가 종료와 파이프 소멸을 확인). job 편입이 실패하면 자식을 즉시 종료하고 오류를 돌려줍니다.
- 툴 실행 실패(서버 없음, 잘못된 인자, 영수증 거부)는 JSON-RPC 오류가 아니라 `isError: true` 결과로 돌아갑니다. 결과는 `content[0].text`(JSON 문자열)와 `structuredContent`(같은 객체) 양쪽에 담깁니다.

### MCP 규격 처리 범위
| 항목 | 구현 |
|---|---|
| 메서드 | `initialize`, `notifications/initialized`, `ping`, `tools/list`, `tools/call` |
| 프로토콜 버전 | `2025-11-25`, `2025-06-18`, `2025-03-26`, `2024-11-05`는 요청한 값을 그대로 응답. 그 밖의 값에는 `2025-11-25`로 응답 |
| 전송 | stdio. 한 줄에 메시지 1개(LF 또는 CRLF), 한 줄 최대 65,535 B. stdout에는 MCP 메시지만 쓰고 stderr에는 시작 시 한 줄만 씀 |
| JSON | 자체 스캐너가 RFC 8259 문법, UTF-8 적법성(과장 인코딩·서로게이트 거부), 중첩 깊이 32를 검사. 문자열 이스케이프(`\uXXXX`, 서로게이트 쌍) 해석 |
| 오류 코드 | `-32700` 파싱 실패, `-32600` 잘못된 요청(배치 배열 포함), `-32601` 모르는 메서드, `-32602` 잘못된 params·모르는 툴 |
| 알림·응답 | `id` 없는 메시지와 호스트가 보낸 응답에는 답하지 않음 |
| 구현하지 않음 | resources, prompts, logging, sampling, 진행률·취소 처리, `outputSchema`, Streamable HTTP 전송, 핸드셰이크가 없는 `2026-07-28` 개정 |

`2026-07-28` 개정만 말하는 호스트와는 연결되지 않습니다. 이 저장소는 그 조합을 시험하지 않았습니다.

### 실측 스펙
| 항목 | 값 | 출처 |
|---|---|---|
| 단일 실행 파일 | **35,328 B** (clang 20.1.8 + lld) / **35,840 B** (MSVC 19.51) | CI run 43613135707, 43612233351 |
| 임포트 | `KERNEL32.dll` 하나, 함수 23개 (파이프·stdio 5, 프로세스 기반 8, `wire_server_start`용 10) | 같은 run의 `dumpbin` / `llvm-readobj` |
| 자체 시험 · 단위 테스트 | `--selftest` 13/13, Python 단위 테스트 26건(MCP 14건 포함) 두 job 모두 OK | 같은 run |
| 메모리 | private bytes 704~712 KiB, 작업 집합 3,016~3,060 KiB(최대 3,096 KiB) — MSVC 빌드, 2,000만 호출 구간 | run 43613135707의 `soak_msvc.json` |
| 정적 버퍼 | 입력 한 줄 64 KiB, 응답 16 KiB, 왕복 표본 128 KiB. 힙 API 임포트 없음 | 소스, 임포트 표 |
| 스레드 | 어댑터가 만드는 스레드 0개 (`CreateThread` 임포트 없음). 메시지 사이에는 stdin `ReadFile`에서 대기 | 소스, 임포트 표 |

### MSVC Soak 실측 (GitHub Actions `windows-latest`, Windows Server 2025, 논리 CPU 4개)
Soak는 서버 1개(`agent_wire_ipc --server 1`)와 어댑터 1개를 끝까지 유지한 채 Python 호스트가 `tools/call wire_roundtrip`을 연속으로 보냅니다. 호출 1건이 파이프 왕복 1건이고(PING·TASK·STATUS 혼합), 10,000건마다 `wire_status`와 `tools/list`를 끼웁니다. 모든 응답에서 서버 순번이 기대값과 같은지 확인하고, 메모리·핸들·스레드·CPU는 **프로세스 밖에서** 핸들로 읽습니다(`K32GetProcessMemoryInfo`, `GetProcessHandleCount`, 스레드 스냅샷, `GetProcessTimes`, `QueryProcessCycleTime`). 합격 기준은 첫 측정 전에 정했고 이후 바꾸지 않았습니다(§1 표).

| run · 빌드 | 부하 (측정 호출 수, 시간) | 처리량 | MCP 호출 지연 p50 / p99 / p99.9 / max | 파이프 왕복 p50 / p99 / p99.9 / max | private bytes 증가 (어댑터 / 서버) | 유휴 구간 CPU (어댑터 / 서버) |
|---|---|---|---|---|---|---|
| [43613135707](https://github.com/CSY8515/Ultra-Brain-Labs/actions/runs/43613135707) · **MSVC** | 80라운드 × 250,000 = **20,000,000**건, 1,825.2 s | 10,958 호출/s | **69.4** / 116.2 / 145.1 / 10,058.3 µs | **30** / 58 / 76 / 5,741 µs | +8,192 B / +12,288 B | 300 s 동안 0.000 ms, 0 사이클 / 0.000 ms, 0 사이클 |
| 같은 run · clang | 20,000,000건, 1,114.4 s | 17,947 호출/s | 44.9 / 68.8 / 91.5 / 8,328.9 µs | 20 / 34 / 50 / 8,294 µs | +12,288 B / +12,288 B | 300 s 동안 0.000 ms, 0 사이클 (둘 다) |
| [43612233351](https://github.com/CSY8515/Ultra-Brain-Labs/actions/runs/43612233351) · **MSVC** | 20라운드 × 250,000 = 5,000,000건, 277.2 s | 18,040 호출/s | 44.7 / 70.3 / 108.0 / 8,247.0 µs | 20 / 34 / 54 / 5,692 µs | +32,768 B / +36,864 B | 60 s 동안 0.000 ms, 389,921 사이클 / 0.000 ms, 346,566 사이클 |
| 같은 run · clang | 5,000,000건, 461.3 s | 10,838 호출/s | 70.1 / 119.7 / 164.9 / 10,569.9 µs | 30 / 60 / 83 / 7,846 µs | −61,440 B / −61,440 B | 60 s 동안 0.000 ms, 0 사이클 (둘 다) |

네 번 모두 파이프 왕복 전체(20,004,002건 또는 5,002,502건)가 서버 순번 순서대로 수락되었고 거부 0건, 핸들 수 44 → 44, 유휴 뒤 1,000건 추가 호출 수락, 두 프로세스 종료 코드 0입니다. 원문은 각 run의 산출물 `labs-msvc`·`labs-clang` 안 `soak_*.txt`·`soak_*.json`(라운드별 표본 포함)입니다.

수치를 읽는 법:

- **지연.** "파이프 왕복"은 어댑터 안에서 `QueryPerformanceCounter`로 `WriteFile`→`ReadFile` 구간을 잰 값이고, "MCP 호출 지연"은 Python 호스트가 요청 한 줄을 쓰고 응답 한 줄을 읽기까지입니다. 뒤의 값에는 호스트 자신의 파이프 I/O와 stdio 두 번의 프로세스 전환이 들어 있습니다. 같은 run 안에서 MCP 계층이 덧붙인 시간은 p50 기준 약 25~40 µs입니다. max(5.7~10.6 ms)는 각 run의 최악 1건이며 공유 가상머신의 스케줄링 지연을 포함합니다.
- **빌드 간 비교는 하지 마십시오.** 두 job은 서로 다른 가상머신에 배정됩니다(run 43613135707에서는 CPU 세대도 달랐습니다: MSVC job `AMD64 Family 25 Model 1`, clang job `Model 17`). run 43613135707에서는 clang job이, 43612233351에서는 MSVC job이 약 1.6배 빨랐습니다. 차이는 컴파일러가 아니라 배정된 머신입니다.
- **메모리: "누수 0 B"라고 쓰지 않습니다.** 합격 기준(라운드 1 종료 → 마지막 표본)으로 잰 증가는 위 표대로 0이 아닙니다. 라운드별 표본에 스레드 수를 함께 기록해 보니 private bytes는 스레드 수를 그대로 따라갔습니다. MSVC 2,000만 건 run에서 어댑터는 스레드 1개일 때 704 KiB(라운드 1~2) 또는 712 KiB(이후 전부), 2개일 때 740 KiB, 3개일 때 764~776 KiB였습니다. 어댑터는 스레드를 만들지 않으므로 추가 스레드는 Windows가 프로세스 안에 만든 것이고(무수정 서버 `agent_wire_ipc.exe`에도 같은 라운드에 같이 나타남), 원인은 특정하지 못했습니다. 측정된 사실은 세 가지입니다. (1) 첫 추가 스레드 구간이 끝난 뒤 스레드 1개 상태의 값이 한 번 +8,192 B(어댑터)·+12,288 B(서버) 올랐습니다. (2) 그 뒤 두 번의 추가 스레드 구간을 더 거쳤지만 값은 다시 오르지 않았습니다. (3) 마지막 51개 라운드(1,275만 호출)는 표본 전부가 712 KiB로 동일했습니다. 즉 호출 수에 비례하는 증가는 측정되지 않았습니다. run 43612233351의 +32,768 B와 −61,440 B는 각각 마지막 표본에 스레드가 2개, 기준 표본에 스레드가 3개였기 때문입니다.
- **유휴 CPU.** 부하 뒤 아무 요청도 보내지 않는 구간(300 s 또는 60 s)에서 두 프로세스의 CPU 시간 증가는 네 번 모두 0.000 ms입니다. `GetProcessTimes`는 스케줄러 틱(15.625 ms) 단위로 누적되므로 사이클 계수기도 함께 읽었고, 여덟 개 프로세스-구간 중 여섯 개가 0 사이클입니다. 나머지 둘(run 43612233351 MSVC job)은 유휴 구간 안에서 Windows가 스레드를 하나 만든 경우로 389,921·346,566 사이클이 잡혔습니다(같은 구간에 스레드 수 1 → 2, private bytes +32 KiB). 어댑터 코드에는 폴링·타이머·백그라운드 스레드가 없습니다.
- **측정 한계.** 공유 CI 가상머신 한 종류에서 구성당 1회씩 잰 값이고 가장 긴 부하가 약 30분입니다. 시간·일 단위 Soak, 실제 하드웨어, ARM64는 측정하지 않았습니다. 첫 rc run [43611161432](https://github.com/CSY8515/Ultra-Brain-Labs/actions/runs/43611161432)(100만 건, 통과)은 호스트 스크립트가 응답을 1바이트씩 읽어 MCP 호출 지연 p50이 435 µs로 나왔습니다(같은 run의 파이프 왕복 p50은 32 µs). 호스트를 버퍼 읽기로 고친 뒤의 run만 위 표에 실었습니다.
- **실제 앱 연결은 NOT_RUN.** 검증한 것은 프로토콜 수준입니다(Python 호스트가 stdio로 `initialize`→`tools/list`→`tools/call`을 구동). Claude Desktop·Cursor·Claude Code 앱에 실제로 등록해 대화에서 호출하는 시험은 이 저장소가 수행하지 않았습니다.

---

## 직접 시험하기

```bat
build.cmd
agent_wire_ipc.exe --selftest
agent_wire_mcp.exe --selftest
pwsh scripts\wire_scenario.ps1 -Exe .\agent_wire_ipc.exe -Count 1000
python -m unittest -v tests.test_win_zero_cli tests.test_agent_wire_ipc tests.test_agent_wire_mcp
python scripts\mcp_soak.py --mcp agent_wire_mcp.exe --wire agent_wire_ipc.exe --rounds 20 --calls 250000 --idle-seconds 60
```

`dumpbin /dependents agent_wire_ipc.exe` 또는 `llvm-readobj --coff-imports agent_wire_ipc.exe`로 의존 DLL이 `KERNEL32.dll` 하나인지 직접 확인할 수 있습니다. Smart App Control이 켜진 PC에서 exe가 막히면 CI run의 로그와 산출물로 같은 검증을 읽을 수 있습니다.

## 한계 (v1.2.0)
- 바이너리는 서명되지 않았습니다. Release 첨부 exe는 CI가 빌드·검증한 파일이며 SHA-256은 Release 노트에 있습니다. Smart App Control이 켜진 PC에서는 MCP 호스트가 `agent_wire_mcp.exe`를 띄우는 것도 차단될 수 있습니다.
- 벤치·Soak 수치는 공유 CI 러너에서 구성당 1회 잰 값이며 하드웨어 보장치가 아닙니다. 첫 왕복 ~10 ms는 설계상 워밍업으로 흡수해야 합니다.
- 로컬 개발기 실측은 Smart App Control 차단(`WinError 4551`)으로 NOT_RUN입니다. Windows Sandbox 실측도 하지 않았습니다.
- MCP 어댑터는 Claude Desktop·Cursor·Claude Code 실제 앱에서 시험하지 않았습니다(프로토콜 수준 검증만). `2026-07-28` 개정, resources·prompts·logging, Streamable HTTP는 구현하지 않았습니다.
- 메모리는 "증가 0 B"가 아니라 "호출 수에 비례하는 증가가 측정되지 않음"까지 확인했습니다(§5). 30분을 넘는 Soak는 하지 않았습니다.
- `agent_wire_ipc.exe`는 `-O2` 기준 22~24 KB입니다(인라인 확장 포함). 크기가 우선이면 `-Os`로 다시 빌드할 수 있으나 이 저장소는 그 수치를 측정하지 않았습니다.

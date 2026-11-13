# Ultra-Brain-Labs

Ultra-Brain에서 나온 실전 엔지니어링 결과물을 공개하는 저장소입니다. 첫 번째 결과물은 Windows용 무의존성 CLI 가이드와, 그 가이드대로 만든 레퍼런스 구현입니다.

- **가이드**: Windows CLI·봇 개발 "덜그덕거림 0%" 엔지니어링 (아래)
- **레퍼런스 구현**: [`src/win_zero_cli.c`](src/win_zero_cli.c) → `win_zero_cli.exe`
  - C11 파일 하나, C 런타임 없음, import는 `KERNEL32.dll` 하나뿐
  - 한글 UTF-8 출력, 정적 아레나 메모리, 자체 시험
- **검증**: [Windows CI](.github/workflows/ci.yml) — 엄격 빌드 경고 0, `dumpbin`·`llvm-objdump`·자체 PE 파서로 의존 DLL 검사, 실행 시험, 단위 테스트

---

# Windows CLI·봇 개발 '덜그덕거림 0%' 엔지니어링 가이드
> AI(Claude, ChatGPT, Cursor) 프롬프트 및 시스템 엔지니어링 정본

> **검증 노트**는 이 저장소를 만들며 실제로 확인한 내용입니다. 가이드 원문과 다른 점이 있을 때만 덧붙였습니다.

## 1. 윈도우에서 CLI/봇이 덜그덕거리는 3대 근본 원인
많은 개발자와 AI가 윈도우에서 도구를 만들 때 "계속 뭐 설치해달라", "인코딩이 깨진다", "실행이 안 된다"며 포기하는 이유는 윈도우 자체의 문제가 아니라 **설계 방식의 불일치** 때문입니다:

1. **외부 의존성 지옥 (Dependency Hell)**:
   - 대부분의 CLI 오픈소스는 리눅스/맥(POSIX) 기준이라, 윈도우에 오면 `MSVC C++ Build Tools (수 GB)`, `Visual C++ Redistributable (DLL 누락)`, 파이썬 가상환경 충돌을 일으킵니다.
2. **콘솔 인코딩 및 환경 마찰**:
   - 윈도우 기본 콘솔(CP949)과 현대 UTF-8의 충돌, 줄바꿈(CRLF vs LF) 불일치로 인한 파싱 에러가 발생합니다.
3. **윈도우 보안 방파제 (Smart App Control & Mark-of-the-Web)**:
   - 외부에서 다운로드받은 미인증 `.exe`는 Windows Defender 및 SAC가 "알 수 없는 게시자"로 간주하여 차단하거나 실행을 지연시킵니다.

## 2. 덜그덕거림 원천 차단: 3대 시스템 엔지니어링 원칙

### 원칙 1: 외부 의존성 제로 (Zero-Dependency Architecture)
- **핵심**: 사용자의 PC에 Python, Node.js, C++ 빌드 툴이 없어도 100% 돌아가야 합니다.
- **해결책**:
  - 외부 런타임에 의존하지 않는 단일 C11 바이너리로 컴파일.
  - 외부 DLL을 일체 임포트하지 않고, 윈도우 95 시절부터 모든 Windows에 100% 내장된 **`KERNEL32.dll` (또는 순정 Win32 API)**만 단독 임포트하도록 링크.
  - 결과: 별도 설치 요청("뭐 설치해주세요")이 영구히 0건이 됨.

> **검증 노트**
> - `KERNEL32.dll` 자체는 모든 Windows에 있지만, **함수마다 최소 Windows 버전이 다릅니다.** 예를 들어 이 구현이 쓰는 `K32GetProcessMemoryInfo`는 Windows 7 이상에만 있습니다. 쓰는 API의 최소 버전을 함께 확인하세요.
> - 일반적인 `clang`/`cl` 빌드는 C 런타임(`ucrtbase.dll`, `vcruntime140.dll` 등)을 끌어옵니다. KERNEL32만 남기려면 C 런타임 없이 빌드하거나(이 저장소의 방식: `-ffreestanding -nostdlib`, 진입점 직접 지정) 정적 런타임(`/MT`)으로 링크해야 합니다. 결과는 `dumpbin /dependents`로 확인합니다.

### 원칙 2: 보안 차단(SAC) 없는 로컬 빌드 및 소스 러너
- **핵심**: 웹에서 다운로드된 파일에 붙는 차단 마크(`Zone.Identifier`)를 회피합니다.
- **해결책**:
  - 인터넷에서 빌드된 exe를 받지 않고, 로컬 환경에서 Clang/TCC 또는 가벼운 러너 스크립트를 통해 온디바이스로 직접 구워냅니다.
  - 파이썬 환경을 쓸 경우에도 무거운 패키징 exe 대신 소스 직접 실행(`pythonw -B -m ...`) 방식을 사용하여 SAC 오탐을 원천 회피합니다.

> **검증 노트 — 로컬 빌드도 SAC에 막힐 수 있습니다.**
> - 로컬에서 만든 파일에는 `Zone.Identifier` 표시가 붙지 않는 것이 맞습니다.
> - 하지만 Smart App Control은 그 표시와 별개로 **서명되지 않은 실행 파일을 평판으로 판정**합니다. Ultra-Brain 개발 중, Smart App Control이 켜진 PC에서 로컬로 빌드한 서명 없는 C 바이너리가 실제로 차단됐습니다(`WinError 4551`, 애플리케이션 제어 정책). 같은 PC에서 다른 빌드는 실행되기도 했습니다. 즉 결과를 미리 보장할 수 없습니다.
> - 확실한 방법은 **신뢰된 게시자 서명(code signing)**입니다. 서명 전까지는 검증을 CI(예: GitHub Actions의 Windows 러너)나 SAC가 꺼진 환경에서 하세요.
> - 보안 설정을 끄거나 우회하는 것은 해결책이 아닙니다.
> - `pythonw -B -m ...` 소스 실행은 서명된 `python.exe`가 실행되는 것이라, 별도 exe를 만들 때보다 차단될 일이 적습니다.

### 원칙 3: 오프라인 샌드박스 (Windows Sandbox) 검증
- **핵심**: "내 컴퓨터에선 되는데 남의 컴퓨터에선 안 되는" 현상을 방지합니다.
- **해결책**:
  - Windows Sandbox(`.wsb`) 설정 파일에서 `<Networking>Disable</Networking>`으로 인터넷을 완전 차단한 순정 윈도우 환경에 바이너리를 던져 테스트합니다.
  - 인터넷이 없는 깡통 윈도우에서 돌아간다면, 지구상 모든 윈도우 PC에서 100% 무결하게 돌아갑니다.

> **검증 노트** — 샌드박스 통과는 "숨은 설치 의존성이 없다"는 강한 증거입니다. 하지만 "모든 PC에서 100%"까지 증명하지는 않습니다. 샌드박스는 호스트와 **같은 Windows 버전·아키텍처**입니다. 다른 버전(예: Windows 7), ARM64, 회사 정책(AppLocker·WDAC), 실행 파일 서명 여부는 따로 확인해야 합니다.

## 3. [복붙용] AI(클로드·챗GPT·커서) 지휘 프롬프트 템플릿

> **아래 프롬프트를 복사하여 AI 코딩 도구에 그대로 붙여넣으세요.**

```markdown
[목표: Windows 환경에서 외부 의존성 없이 0초 만에 즉시 실행되는 초경량 CLI/봇 개발]

너는 Windows 저수준 시스템 엔지니어링 전문가다.
사용자가 별도의 라이브러리나 C++ 빌드 툴을 설치하느라 고통받지 않도록, 아래 5대 무결성 원칙을 엄격히 지켜 코드를 작성하라.

1. **외부 의존성 0% (Zero External Dependencies)**:
   - Python 가상환경, 무거운 외부 패키지, C++ 런타임 DLL(msvcp140 등) 요구를 절대 금지한다.
   - C/C++ 구현 시 오직 Windows 기본 내장 `KERNEL32.dll` / `USER32.dll` 표준 Win32 API만 링크하라.
   - 단일 파일(`.exe`) 실행 시 "추가 구성 요소를 설치해야 합니다" 팝업이 절대 뜨지 않아야 한다.

2. **콘솔 인코딩 무결성**:
   - 시작 시 `SetConsoleOutputCP(CP_UTF8)` 및 `SetConsoleCP(CP_UTF8)`를 명시적으로 호출하여 한글 깨짐을 원천 차단하라.

3. **메모리 및 자원 제약 (초경량 족쇄)**:
   - 메모리 누수와 단편화를 방지하기 위해 정적 고정 버퍼 또는 Arena 메모리 풀 방식을 사용하라.
   - 프로세스 상주 램(Working Set)은 최대 15MB 이하로 엄격히 고정하라.

4. **오프라인/에어갭 생존 보장**:
   - 인터넷 연결이 없는 상태에서도 핵심 로직 및 단위 테스트가 100% 자립 실행 가능하도록 설계하라.

5. **결과물 제공 형식**:
   - 누구나 바로 빌드할 수 있도록 단 한 줄의 컴파일 명령어(Clang 또는 GCC/TCC)를 함께 제시하라.
```

> **검증 노트** — 이 프롬프트를 쓸 때 AI에게 두 가지를 더 요구하면 결과가 안정적입니다.
> - "빌드 후 `dumpbin /dependents`로 의존 DLL 목록을 보여 줘."
> - "실행 파일 서명 계획(또는 서명 전까지의 검증 경로)을 적어 줘."

---

# 레퍼런스 구현: `win_zero_cli`

| 항목 | 내용 |
|---|---|
| 소스 | [`src/win_zero_cli.c`](src/win_zero_cli.c) — C11 파일 하나, SDK 헤더 없이 쓰는 Win32 함수만 직접 선언 |
| 의존 DLL | `KERNEL32.dll` 하나. 쓰는 함수: `GetStdHandle`, `WriteFile`, `SetConsoleOutputCP`, `GetCommandLineW`, `GetCurrentProcess`, `K32GetProcessMemoryInfo`, `ExitProcess` |
| C 런타임 | 없음 (`-ffreestanding -nostdlib`, 진입점 `wz_main`) |
| 메모리 | 힙 없음, 64 KiB 정적 아레나. 자체 시험이 최대 작업 집합을 재고 15 MiB 이하를 확인 |
| 최소 Windows | Windows 7 (`K32GetProcessMemoryInfo`), x64 |

## 빌드 (한 줄, LLVM clang)

```bash
clang -std=c11 -O2 -Wall -Wextra -pedantic -Werror -ffreestanding -nostdlib -fuse-ld=lld -Wl,/entry:wz_main -Wl,/subsystem:console src/win_zero_cli.c -lkernel32 -o win_zero_cli.exe
```

`kernel32.lib`는 Windows SDK에 있습니다. Visual Studio Build Tools의 LLVM이나 GitHub Actions의 `windows-latest`라면 그대로 동작합니다.

## 사용

```text
win_zero_cli              인사말 (UTF-8 한글, 종료 코드 0)
win_zero_cli --selftest   자체 시험 4개 (UTF-8 검증기, 아레나, 숫자 변환, 작업 집합 15 MiB 이하) — 모두 통과 시 0
win_zero_cli --version    1.0.0
```

## 검증

- CI: [Windows CI](.github/workflows/ci.yml) — `windows-latest`에서 다음을 확인합니다.
  - 빌드 경고 0(컴파일러 출력이 있으면 실패)
  - `dumpbin /dependents` 결과가 `KERNEL32.dll` 하나인지
  - 인사말과 자체 시험의 종료 코드 0
  - 단위 테스트 [`tests/test_win_zero_cli.py`](tests/test_win_zero_cli.py): 표준 라이브러리만으로 PE import 표 읽기, UTF-8 엄격 디코딩, 작업 집합 15 MiB 이하
- 배포 바이너리: Release에 첨부한 `win_zero_cli.exe`는 CI가 빌드하고 검증한 바로 그 파일입니다(SHA-256은 Release 노트와 CI 요약에 있음).
- **서명되지 않은 바이너리입니다.** Smart App Control이 켜진 PC에서는 막힐 수 있습니다. 그럴 때는 위 한 줄 명령으로 직접 빌드하거나 CI 결과를 확인하세요.

## 직접 시험하기

```bash
python -m unittest -v tests.test_win_zero_cli
```

`win_zero_cli.exe`가 현재 폴더에 있어야 합니다(다른 위치는 `WIN_ZERO_CLI` 환경 변수로 지정).

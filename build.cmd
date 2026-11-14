@echo off
rem Ultra-Brain-Labs build: all three tools, no C runtime, KERNEL32.dll only.
rem   build.cmd          LLVM clang + lld (default)
rem   build.cmd msvc     MSVC cl + link (run inside a "x64 Native Tools" prompt or after vcvars64.bat)
rem Every build treats warnings as errors. Output: win_zero_cli.exe, agent_wire_ipc.exe, agent_wire_mcp.exe in the current folder.
setlocal
set SRC=%~dp0src

if /i "%~1"=="msvc" goto msvc

clang -std=c11 -O2 -Wall -Wextra -pedantic -Werror -ffreestanding -nostdlib -fuse-ld=lld -Wl,/entry:wz_main -Wl,/subsystem:console "%SRC%\win_zero_cli.c" -lkernel32 -o win_zero_cli.exe || exit /b 1
clang -std=c11 -O2 -Wall -Wextra -pedantic -Werror -ffreestanding -nostdlib -fuse-ld=lld -Wl,/subsystem:console "%SRC%\agent_wire_ipc.c" -lkernel32 -o agent_wire_ipc.exe || exit /b 1
clang -std=c11 -O2 -Wall -Wextra -pedantic -Werror -ffreestanding -nostdlib -fuse-ld=lld -Wl,/subsystem:console "%SRC%\agent_wire_mcp.c" -lkernel32 -o agent_wire_mcp.exe || exit /b 1
goto done

:msvc
cl /nologo /std:c11 /O2 /W4 /WX /utf-8 /GS- "%SRC%\win_zero_cli.c" /link /SUBSYSTEM:CONSOLE /NODEFAULTLIB /ENTRY:wz_main kernel32.lib /OUT:win_zero_cli.exe || exit /b 1
cl /nologo /std:c11 /O2 /W4 /WX /utf-8 /GS- "%SRC%\agent_wire_ipc.c" /link /SUBSYSTEM:CONSOLE /NODEFAULTLIB kernel32.lib /OUT:agent_wire_ipc.exe || exit /b 1
cl /nologo /std:c11 /O2 /W4 /WX /utf-8 /GS- "%SRC%\agent_wire_mcp.c" /link /SUBSYSTEM:CONSOLE /NODEFAULTLIB kernel32.lib /OUT:agent_wire_mcp.exe || exit /b 1
del /q win_zero_cli.obj agent_wire_ipc.obj agent_wire_mcp.obj 2>nul

:done
echo built: win_zero_cli.exe agent_wire_ipc.exe agent_wire_mcp.exe
echo verify imports: dumpbin /dependents agent_wire_ipc.exe   (or: llvm-readobj --coff-imports agent_wire_ipc.exe)
exit /b 0

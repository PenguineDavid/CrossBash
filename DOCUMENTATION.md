# Lindows v2.0
*A Linux-style command shell for Windows*

Lindows is a Windows-native command shell written in C++ that provides a Linux-like command syntax, job control, pipes, redirection, and built-in commands, while still executing commands through the Windows command processor.

This project is designed as a learning-friendly but powerful shell environment, with explicit handling of Windows process management and console behavior.

---

## Features

- Linux-style prompt (`user@host:~/path$`)
- Built-in commands (`cd`, `ls`, `pwd`, `rm`, etc.)
- Command piping (`cmd1 | cmd2`)
- Input/output redirection (`>`, `>>`, `<`)
- Background jobs (`&`)
- Job listing (`jobs`)
- Ctrl+C process interruption without exiting the shell
- Environment variable expansion (`$VAR`)
- ANSI color support on Windows
- Native Windows process execution via `cmd.exe`

---

## Platform Support

- **Windows only**
- Uses the Win32 API (`CreateProcess`, `HANDLE`, `CTRL_C_EVENT`, etc.)
- Will not compile or run on Linux or macOS

---

## Build Requirements

- C++17 or newer
- Windows SDK
- Compiler with Win32 support (MSVC, MinGW, clang-cl)

### Example (MinGW)
```CMD
g++ main.cpp -std=c++17 -o lindows
```
---

**that Should work but if it dosnt**
```CMD
g++ -std=c++17 -o lindows2.exe lindows.cpp -static -lstdc++fs -static-libgcc -static-libstdc++
```

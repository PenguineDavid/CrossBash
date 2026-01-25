## v2.0 — C++ Rewrite (Architecture Reset)

> This release marks a full rewrite of Lindows from Python to C++.
> The goal of v2.0 was architectural correctness, performance, and tighter
> integration with the Windows operating system.

---

### Major Changes

- Complete rewrite from **Python → C++**
- Removed all Python runtime and dependencies
- Direct use of **Win32 API** for:
  - Process creation
  - Pipe management
  - Console control
  - Signal handling
- Established the long-term foundation for future versions

---

### Core Shell Engine

- New interactive shell loop written in C++
- Deterministic command parsing (no `shlex`)
- Explicit command segmentation for:
  - Pipes
  - Redirection
  - Background execution
- Strict execution model using:


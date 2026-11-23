# Project Handoff & Context Summary

Permanent engineering record of the clean-room `nvidia-gridd` C11 project, architectural decisions, workspace layout, build/test/packaging infrastructure, remote Linux build host, and verified baseline results.

---

## 1. Project Overview & Clean-Room Boundaries

This repository contains a standalone clean-room C11 Linux reimplementation of `nvidia-gridd` (NVIDIA vGPU / GRID licensing daemon), built strictly from externally observable protocol, configuration, cryptographic, and kernel RM ABI behavior documented in [`docs/SPECIFICATION.md`](SPECIFICATION.md).

### Strict Clean-Room Rules
- **Clean-room directories:** [`CMakeLists.txt`](../CMakeLists.txt), [`README.md`](../README.md), [`include/gridd/`](../include/gridd/), [`src/`](../src/), [`packaging/`](../packaging/), [`tests/`](../tests/), [`docs/`](.), and [`.agents/skills/build-and-package-gridd/`](../.agents/skills/build-and-package-gridd/).
- **Reference-only inputs (untouched):** `nvidia-gridd` (original ELF64 binary at workspace root) and `decompiled/` (original analysis artifacts).
- **Automated leak & hygiene guard:** [`tests/test_c_static_audit.py`](../tests/test_c_static_audit.py) automatically fails if any decompiler symbol patterns, tool names, banned unsafe C functions (`strcpy`, `strcat`, `sprintf`, `gets`), or `<curl/curl.h>` includes appear anywhere in clean-room code, tests, packaging, or documentation, and asserts all `.c` files in `src/` match `CMakeLists.txt`.

---

## 2. Key Architectural & Implementation Decisions

| Component | Design Choice | Rationale |
| --- | --- | --- |
| **HTTP / TLS Transport** (`src/http_client.c`) | Native POSIX `getaddrinfo`/`socket` + OpenSSL 3.x `TLS_client_method()` (SNI, hostname check, `SO_RCVTIMEO`/`SO_SNDTIMEO`, chunked decoding, HTTP `CONNECT` proxy with Basic auth). | Eliminates the `libcurl` dependency (`test_clean_compiler_flags_and_crypto_params` enforces zero `<curl/curl.h>` usage) while keeping only OpenSSL 3.x + libc at runtime. |
| **D-Bus IPC** (`src/dbus_server.c`) | Runtime `dlopen("libdbus-1.so.3")` resolving 24 `dbus_*` symbols + `poll()` loop over up to 8 watches. | Compiles without D-Bus `-dev` headers and degrades gracefully in minimal Linux containers without D-Bus installed. |
| **NVIDIA RM ABI** (`src/rm_client.c`) | `/dev/nvidiactl` `ioctl()` wrappers (`NV_ESC_RM_ALLOC/FREE/CONTROL`) behind `gridd_rm_ops_t` dependency-injection vtable. | Enables offline C unit testing of RM handle lifecycle and feature/license ioctls (`tests/c_unit_tests.c`) without physical GPU hardware. |
| **Trusted Local Store** (`src/trusted_store.c`) | AES-256-GCM encryption (`store_nls.dat` storing 12B IV + ciphertext, `store_nls.tag` storing 16B tag) keyed with HKDF-SHA256 from machine DMI UUID + `g_store_mutex` + `.tmp`/`rename()` writes. | Provides crash-safe atomic persistence, tamper detection (`GRIDD_ERR_AUTH`), and monotonic clock windback detection (`GRIDD_ERR_CLOCK_WINDBACK`). |
| **Concurrency** (`daemon.c`, `license_manager.c`, `main.c`) | POSIX self-pipes (`exit_pipe`, `dbus_pipe`, `wake_pipe`, `event_pipe`) + `select()`/`poll()`. | Async-signal-safe `SIGINT`/`SIGTERM` shutdown and non-blocking event dispatch across main, LM worker, D-Bus, and 30s monitor threads. |
| **Packaging** (`CMakeLists.txt`, `packaging/`) | CPack (`DEB` + `TGZ` + optional `RPM`), `CPACK_DEBIAN_PACKAGE_SHLIBDEPS ON`, `configure_file(... NEWLINE_STYLE LF)` + `chmod 0755` for `postinst`/`prerm`. | Produces repeatable `.deb` / `.tar.gz` / `.rpm` packages with resolved shared library dependencies and Windows-safe LF conversion. |

---

## 3. Complete Workspace Inventory

| Path | Role |
| --- | --- |
| [`CMakeLists.txt`](../CMakeLists.txt) | C11 (`-Wall -Wextra -Werror -Wpedantic`) build for `gridd_core`, `nvidia-gridd`, `c_unit_tests`, `c_e2e_tests`, install rules, and CPack configuration. |
| [`README.md`](../README.md) | Top-level overview, Mermaid architecture summary, documentation index, and build/test commands. |
| [`include/gridd/*.h`](../include/gridd/) (15 headers) | Public API declarations (`92` `gridd_*` functions, structs, enums, ioctl constants, and bounds). |
| [`src/*.c`](../src/) (15 sources) | Complete C11 daemon implementation (`151` total C functions including `main()` and `static` helpers). |
| [`packaging/nvidia-gridd.service`](../packaging/nvidia-gridd.service) | Systemd `Type=forking` service unit (`/usr/sbin/nvidia-gridd`, `/var/run/nvidia-gridd/nvidia-gridd.pid`). |
| [`packaging/gridd.conf.template`](../packaging/gridd.conf.template) | Sample `/etc/nvidia/gridd.conf` installed to `/usr/share/nvidia/gridd.conf.template`. |
| [`packaging/debian/postinst`](../packaging/debian/postinst) | Package post-install script (creates `/etc/nvidia/*` & `/var/lib/nvidia/*`, seeds `gridd.conf`, reloads `systemd`). |
| [`packaging/debian/prerm`](../packaging/debian/prerm) | Package pre-remove script (stops `nvidia-gridd.service` when `systemd` is active). |
| [`packaging/build-package.sh`](../packaging/build-package.sh) | Automated Linux build, test (`ctest`, `run_c_e2e.py`, `unittest`), CPack (`.deb`/`.tar.gz`/`.rpm`), `SHA256SUMS`, and `dpkg-deb` verification script. |
| [`tests/`](../tests/) (7 files) | `c_unit_tests.c`, `c_e2e_tests.c`, `mock_nls_server.py`, `mock_imds_server.py`, `run_c_e2e.py`, `test_cleanroom_spec.py`, `test_c_static_audit.py`. |
| [`docs/`](.) (7 files) | `SPECIFICATION.md`, `ARCHITECTURE.md`, `FLOWCHARTS.md`, `API_REFERENCE.md`, `CALL_GRAPH.md`, `OPERATIONS_AND_TESTING.md`, `PROJECT_HANDOFF.md`. |
| [`.agents/skills/build-and-package-gridd/`](../.agents/skills/build-and-package-gridd/) | Antigravity workspace skill (`SKILL.md` + `scripts/remote-package.ps1`) automating local Linux and remote SSH Windows-to-Ubuntu builds/packaging. |

---

## 4. Development & Remote Ubuntu Build Environment

- **Local Host (Windows):**
  - Path: `c:\Users\Sanjay\Documents\AI Projects\Gridd`
  - No native C compiler installed locally; Python 3 is available.
  - **Important convention:** Always run local Python commands with `python -B` (for example `python -B -m unittest discover -s tests -v`) to prevent creating `tests/__pycache__` in the workspace.
- **Remote Build Host (Linux / SSH):**
  - SSH host alias/IP: `192.168.1.15` (user `zero`, configured in `~/.ssh/config`)
  - OS & Toolchain: Ubuntu 26.04 LTS (`x86_64`), GCC `15.2.0`, Clang `21.1.8`, CMake, CPack, Ninja, OpenSSL `3.5.5`, `dpkg-deb`, `dpkg-shlibdeps`, `fakeroot`, Python 3 with `cryptography`.
  - Clean-room remote directory: `/home/zero/rea-projects/gridd/cleanroom` (do **not** touch files outside `cleanroom/` in `/home/zero/rea-projects/gridd/`).
  - PowerShell helper: `.agents\skills\build-and-package-gridd\scripts\remote-package.ps1` syncs files over `scp`, strips Windows `\r` line endings on shell scripts, compiles, runs tests, and invokes CPack.

---

## 5. Verified Baseline Status

All tiers were verified prior to documentation handoff:
1. **Local Windows Python Conformance & Static Audit:** `python -B -m unittest discover -s tests -v` passed **11/11** tests (`2.219s`), including balanced C delimiter/guard checks, CMake source list parity, symbol definitions, protocol/RM constant parity, and zero leakage.
2. **Remote GCC 15.2.0 + Ninja Release Build & Package (`192.168.1.15`):**
   - Compiled **21/21** CMake targets cleanly with `-std=c11 -Wall -Wextra -Werror -Wpedantic`.
   - `ctest --output-on-failure`: passed **1/1** (`c_unit_tests`).
   - `python3 -B tests/run_c_e2e.py build-pkg/c_e2e_tests`: passed live C-to-mock NLS (`12` HTTP requests) + AWS IMDSv2 verification.
   - `python3 -B -m unittest discover -s tests -v`: passed **11/11** tests (`1.987s`).
   - CPack generated `/home/zero/rea-projects/gridd/cleanroom/dist/nvidia-gridd_1.0.0_amd64.deb` (`51098` bytes, `Depends: libc6 (>= 2.38), libssl3t64 (>= 3.0.0)`) and `nvidia-gridd-1.0.0-Linux.tar.gz` with verified `SHA256SUMS`.
3. **Clang 21.1.8 Verification (earlier remote build check):** Compiled and passed `ctest` **1/1** cleanly under `-DCMAKE_C_COMPILER=clang`.

---

## 6. Quick Start in a New Conversation

- **Run local static/spec tests (Windows, no `__pycache__`):**
  ```powershell
  python -B -m unittest discover -s tests -v
  ```
- **Sync, compile, test, and package remotely via Ubuntu (`192.168.1.15`):**
  ```powershell
  powershell -ExecutionPolicy Bypass -File ".agents\skills\build-and-package-gridd\scripts\remote-package.ps1" -Clean
  ```
- **Compile and package directly on Linux:**
  ```bash
  ./packaging/build-package.sh --clean
  ```

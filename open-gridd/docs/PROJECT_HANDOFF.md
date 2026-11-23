# Project Handoff & Context Summary

Permanent engineering record of the clean-room `nvidia-gridd` C11 project, architectural decisions, workspace layout, build/test/packaging infrastructure, remote Linux build host, and verified baseline results.

---

## 1. Project Overview & Clean-Room Boundaries

This repository contains a standalone clean-room C11 Linux reimplementation of `nvidia-gridd` (NVIDIA vGPU / GRID licensing daemon), built strictly from externally observable protocol, configuration, cryptographic, and kernel RM ABI behavior documented in [`docs/SPECIFICATION.md`](SPECIFICATION.md).

### Strict Clean-Room Rules
- **Clean-room directories:** [`Makefile`](../Makefile), [`CMakeLists.txt`](../CMakeLists.txt), [`README.md`](../README.md), [`include/gridd/`](../include/gridd/), [`src/`](../src/), [`packaging/`](../packaging/), [`tests/`](../tests/), and [`docs/`](.).
- **Clean-room boundary:** Zero proprietary binaries or vendor source files live inside `open-gridd/`; all interfaces follow [`docs/SPECIFICATION.md`](SPECIFICATION.md).
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
| [`Makefile`](../Makefile) | Integrated C11 Makefile (`all`, `install`, `test`, `clean`) used by guest provisioning scripts and CI. |
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

---

## 4. Development & Remote Ubuntu Build Environment

- **Local Host (Windows):**
  - Integrated path: `open-pascal-vgpu/open-gridd/` (`open-vgpu-tesla-p4` repository)
  - No native C compiler installed locally; Python 3 is available.
  - **Important convention:** Always run local Python commands with `python -B` (for example `python -B -m unittest discover -s tests -v`) to prevent creating `tests/__pycache__` in the workspace.
- **Remote Build Host (Linux / SSH):**
  - SSH host alias/IP: `192.168.1.15` (user `zero`, configured in `~/.ssh/config`)
  - OS & Toolchain: Ubuntu 26.04 LTS (`x86_64`), GCC `15.2.0`, Clang `21.1.8`, Make, CMake, CPack, Ninja, OpenSSL `3.5.5`, `dpkg-deb`, `dpkg-shlibdeps`, `fakeroot`, Python 3 with `cryptography`.
  - Integrated remote repository: `/home/zero/open-pascal-vgpu/open-gridd` (`git` + GitHub CLI workflows).

---

## 5. Verified Baseline Status

All tiers and live FastAPI-DLS integration were verified:
1. **Local Windows Python Conformance & Static Audit:** `python -B -m unittest discover -s tests -v` passed **11/11** tests, including balanced C delimiter/guard checks, CMake source list parity, symbol definitions, protocol/RM constant parity, and zero leakage.
2. **Remote GCC 15.2.0 Make, CMake/Ninja & GitHub Actions CI Verification:**
   - `make -C open-gridd clean all test clean` passed `c_unit_tests`, `tests/run_c_e2e.py`, and `unittest` (`11/11`) in both remote Ubuntu 26.04 and GitHub Actions CI.
   - Compiled **21/21** CMake targets cleanly with `-std=c11 -Wall -Wextra -Werror -Wpedantic`, passed `ctest` (`1/1`), passed `run_c_e2e.py`, and generated `.deb`/`.tar.gz` packages with `SHA256SUMS`.
3. **Live AlmaLinux 10.3 vGPU Guest Verification (`vm-alma` @ `192.168.1.68` -> FastAPI-DLS `192.168.1.24:443`):**
   - `nvidia-gridd.service` started and restarted cleanly under systemd; `nvidia-smi -q` reported `License Status : Licensed (Expiry: 2027-2-21 17:41:22 GMT)` on `NVIDIA RTX Virtual Workstation`.
4. **Clang 21.1.8 Verification:** Compiled and passed `ctest` **1/1** cleanly under `-DCMAKE_C_COMPILER=clang`.

---

## 6. Quick Start

- **Run local static/spec tests (Windows or Linux, no `__pycache__`):**
  ```bash
  python3 -B -m unittest discover -s tests -v
  ```
- **Compile, test, and clean via integrated Makefile on Linux:**
  ```bash
  make -C open-gridd clean all test clean
  ```
- **Compile and package directly on Linux:**
  ```bash
  ./packaging/build-package.sh --clean
  ```

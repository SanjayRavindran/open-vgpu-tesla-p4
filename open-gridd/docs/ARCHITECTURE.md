# `nvidia-gridd` Clean-Room Architecture

This document describes the modular software architecture, concurrency model, build targets, and licensing orchestration of the clean-room C11 `nvidia-gridd` implementation.

## 1. Layered Module Architecture

The codebase is organized into 15 modular header/source pairs under [`include/gridd/`](../include/gridd/) and [`src/`](../src/), compiled into a shared core library (`gridd_core`) and linked into the `nvidia-gridd` daemon binary ([`src/main.c`](../src/main.c)) as well as C unit and end-to-end test executables.

```mermaid
flowchart TD
    Main["src/main.c (CLI & Top-Level Event Loop)"]

    subgraph Control["Control & Orchestration Layer"]
        Daemon["daemon.h / daemon.c (Fork, PID Lock, Signals, Self-Pipes)"]
        Config["config.h / config.c (gridd.conf Parser & Feature Map)"]
        LM["license_manager.h / license_manager.c (State Machine & Renewal Thread)"]
        DBus["dbus_server.h / dbus_server.c (dlopen libdbus-1.so.3 IPC)"]
    end

    subgraph Licensing["Licensing, Identity & Security Layer"]
        NLS["nls_client.h / nls_client.c (PKCE, Auth, Lease, NLL)"]
        Token["token.h / token.c (.tok JWT Parser & ECDSA Verify)"]
        Store["trusted_store.h / trusted_store.c (AES-256-GCM Store & Anti-Tamper)"]
        CSP["csp_platform.h / csp_platform.c (AWS IMDSv2 & GridSwCert.txt)"]
        Env["env_fingerprint.h / env_fingerprint.c (NIC, OS, CPU & GPU Fingerprint)"]
    end

    subgraph Platform["Platform, Transport & Foundation Layer"]
        RM["rm_client.h / rm_client.c (/dev/nvidiactl Ioctls & 30s Monitor)"]
        HTTP["http_client.h / http_client.c (POSIX TCP, OpenSSL TLS & Proxy)"]
        Crypto["crypto.h / crypto.c (OpenSSL SHA-256, HKDF, AES-GCM, ECDSA, PKCS7)"]
        JSON["json_util.h / json_util.c (Lightweight JSON Builder & Extractor)"]
        Log["log.h / log.c (Syslog / Stderr Logging)"]
        Types["types.h (Status Codes, Enums & Bounds)"]
    end

    Main --> Daemon
    Main --> Config
    Main --> LM
    Main --> DBus
    Main --> CSP
    Main --> Env
    Main --> Token
    Main --> NLS
    Main --> RM
    Main --> HTTP
    Main --> Log

    LM --> NLS
    LM --> RM
    DBus --> LM
    DBus --> Config
    NLS --> HTTP
    NLS --> Store
    NLS --> Token
    NLS --> Env
    NLS --> Crypto
    NLS --> JSON
    Token --> Crypto
    Token --> JSON
    Store --> Crypto
    Store --> JSON
    CSP --> HTTP
    CSP --> Crypto
    CSP --> JSON
    Env --> RM
    HTTP --> Types
    Control --> Types
    Licensing --> Types
    Platform --> Log
```

### Module Inventory

| Layer | Header / Source | Primary Responsibility |
| --- | --- | --- |
| Entry | `src/main.c` | Parses `-v` / `-f` CLI flags, initializes subsystems, enforces licensing priority (CSP -> NLL -> NLS `.tok`), and runs the main `select()` event loop. |
| Control | `daemon.h` / `daemon.c` | Handles POSIX daemonization (`umask(026)`, `fork()`, `setsid()`, `chdir("/")`), PID locking (`lockf(F_TLOCK)`), signal handling (`SIGINT`, `SIGTERM`, `SIGPIPE`), self-pipes, and state directories. |
| Control | `config.h` / `config.c` | Loads key-value pairs from `/etc/nvidia/gridd.conf`, disables UI atomically via temp file + `rename()`, and maps `FeatureType` to NLS product names. |
| Control | `license_manager.h` / `license_manager.c` | Runs the asynchronous background licensing thread managing lease acquisition, exponential backoff retries (10s to 300s), renewal timers, and graceful lease return. |
| Control | `dbus_server.h` / `dbus_server.c` | Dynamically loads `libdbus-1.so.3`, claims `nvidia.grid.server`, serves `GridLicenseState` IPC calls, and triggers config reload via self-pipe. |
| Licensing | `nls_client.h` / `nls_client.c` | Executes NLS REST flows (`/auth/v1/*`, `/leasing/v1/*`), handles HTTP 307 redirects, 401 token refresh, 409 origin conflicts, and parses offline Node-Locked Licenses (`.lic`). |
| Licensing | `token.h` / `token.c` | Discovers `.tok` client configuration tokens, splits JWT segments, verifies ES256/RS256 signatures against embedded NLS public keys, and checks `nbf`/`exp` bounds. |
| Licensing | `trusted_store.h` / `trusted_store.c` | Persists `store_nls.dat` and `store_nls.tag` encrypted with AES-256-GCM using an HKDF-SHA256 machine-UUID key; detects clock windback and fingerprint mismatches. |
| Licensing | `csp_platform.h` / `csp_platform.c` | Evaluates Cloud Service Provider entitlements across 6 marketplaces, runs AWS IMDSv2 signed document verification, and verifies `/etc/nvidia/GridSwCert.txt`. |
| Licensing | `env_fingerprint.h` / `env_fingerprint.c` | Collects NIC MAC/IP addresses, hostname, `/etc/os-release`, `/proc/cpuinfo` topology, `/sys/class/dmi/id/product_uuid` / `/etc/machine-id`, and GPU PCI IDs. |
| Platform | `rm_client.h` / `rm_client.c` | Opens `/dev/nvidiactl`, allocates RM client/device/subdevice/vGPU handles, invokes `NV_ESC_RM_*` ioctls, updates GPU license status, and runs the 30s unlicensed monitor thread. |
| Platform | `http_client.h` / `http_client.c` | Implements non-Libcurl HTTP/1.1 client over POSIX `getaddrinfo`/`socket` with `SO_RCVTIMEO`/`SO_SNDTIMEO`, OpenSSL 3.x TLS 1.2+ with SNI, chunked decoding, and HTTP `CONNECT` proxy tunneling. |
| Platform | `crypto.h` / `crypto.c` | Wraps OpenSSL EVP primitives: Base64/Base64URL, SHA-256, CSPRNG, PKCE S256, HKDF-SHA256, HMAC-SHA256, AES-256-GCM, JWT splitting, PEM verification, and PKCS#7 verification. |
| Platform | `json_util.h` / `json_util.c` | Provides overflow-guarded dynamic string buffer JSON assembly and zero-allocation scalar/raw extraction helpers. |
| Platform | `log.h` / `log.c` | Abstracts `openlog`/`syslog` vs. `stderr` logging controlled by `-f` and `-v`. |
| Platform | `types.h` | Defines `gridd_status_t`, `gridd_feature_type_t`, `gridd_lm_state_t`, `gridd_rm_license_state_t`, and buffer bounds (`GRIDD_MAX_*`). |

## 2. Master Daemon Lifecycle & Licensing Decision Flow

Upon startup in [`src/main.c`](../src/main.c), the daemon progresses deterministically through initialization, entitlement resolution (CSP Marketplace -> Node-Locked License -> NLS `.tok` Lease), and main-loop event dispatch.

```mermaid
flowchart TD
    Start(["Process Entry: main(argc, argv)"]) --> ParseArgs["Parse CLI flags (-v verbose, -f foreground)"]
    ParseArgs --> HttpInit["gridd_http_global_init() & gridd_daemon_init()"]
    HttpInit --> RmInit["gridd_rm_init() & set SW state persistence"]
    RmInit --> UnlicMon["Start 30s unlicensed state monitor thread"]
    UnlicMon --> LoadCfg["gridd_config_load(/etc/nvidia/gridd.conf; defaults FeatureType=2 on vGPU guests)"]
    LoadCfg --> CheckUI{"config.enable_ui == true?"}
    CheckUI -- "Yes" --> SpawnDBus["Spawn gridd_dbus_thread_main thread"]
    CheckUI -- "No" --> EnsureDirs["gridd_ensure_state_dirs()"]
    SpawnDBus --> EnsureDirs
    EnsureDirs --> CheckCSP{"marketplace_platform != 0?"}

    CheckCSP -- "Yes" --> RunCSP["gridd_csp_detect_and_verify()"]
    RunCSP --> CspOK{"CSP verified?"}
    CspOK -- "Yes" --> EnableCSP["gridd_rm_enable_feature() & set LICENSED + disable UI"]
    EnableCSP --> Cleanup
    CspOK -- "No" --> CollectEnv

    CheckCSP -- "No" --> CollectEnv["gridd_env_collect()"]
    CollectEnv --> TryNLL{"ignore_node_locked == false?"}
    TryNLL -- "Yes" --> RunNLL["gridd_nls_try_nll()"]
    RunNLL --> NllOK{"Valid .lic found?"}
    NllOK -- "Yes" --> EnableNLL["gridd_rm_enable_feature() & set LICENSED + disable UI"]
    EnableNLL --> Cleanup
    NllOK -- "No" --> WaitTok

    TryNLL -- "No" --> WaitTok["wait_for_token(): poll ClientConfigToken dir (10s..300s backoff)"]
    WaitTok --> StopBeforeTok{"Stop requested?"}
    StopBeforeTok -- "Yes" --> Cleanup
    StopBeforeTok -- "No" --> VerifyTok["gridd_token_parse_and_verify() (ES256/RS256 + payload pubkey)"]
    VerifyTok --> DisableUI["Disable UI if non-vGPU or gridd.conf exists & gridd_lm_create()"]
    DisableUI --> StartLM["gridd_lm_acquire() -> timed RM 0x20800187 state=2 + Expiry"]
    StartLM --> MainLoop["select() on exit_pipe_rd, lm.event_pipe_rd, dbus_pipe_rd"]

    MainLoop -- "exit_pipe readable" --> StopLM["gridd_lm_stop() returns active leases & shuts down"]
    MainLoop -- "dbus_pipe readable" --> ReloadCfg["Reload gridd.conf & apply updated config"]
    ReloadCfg --> MainLoop
    MainLoop -- "lm.event_pipe readable" --> LogEvent["Consume LM state-change event"]
    LogEvent --> MainLoop

    StopLM --> Cleanup["Join threads, gridd_rm_destroy(), gridd_daemon_cleanup(), exit(exit_code)"]
```

## 3. Concurrency Model & Self-Pipe IPC

The daemon uses up to four concurrent threads coordinated strictly by mutexes and POSIX self-pipes so `select()` / `poll()` loops remain non-blocking and async-signal-safe shutdown propagates immediately:

```mermaid
flowchart LR
    Sig["POSIX Signals (SIGINT / SIGTERM)"] -- "write 1 byte" --> ExitPipe["daemon.exit_pipe_rd / wr"]
    ExitPipe --> MainThread["Main Thread: select() Loop"]
    ExitPipe -- "stop_pipe_rd" --> UnlicThread["RM Unlicensed Monitor Thread (30s select)"]
    ExitPipe -- "stop_pipe_rd" --> DBusThread["D-Bus Server Thread (poll() on watches)"]

    MainThread -- "wake_pipe_wr (acquire / stop)" --> LMThread["License Manager Worker Thread"]
    LMThread -- "event_pipe_wr (state byte)" --> MainThread
    DBusThread -- "dbus_reload_pipe_wr (reload byte)" --> MainThread

    LMThread -. "pthread_mutex_t lm.lock" .-> DBusThread
    MainThread -. "pthread_mutex_t g_store_mutex" .-> LMThread
```

| Thread | Entry Point | Synchronization & Wake Mechanism | Blocking Primitive |
| --- | --- | --- | --- |
| **Main Thread** | `main()` in `src/main.c` | Owns `gridd_daemon_ctx_t`, `gridd_rm_client_t`, `gridd_license_manager_t`; consumes `exit_pipe_rd`, `lm.event_pipe_rd`, `dbus_pipe_rd` | `select()` |
| **License Manager Worker** | `lm_worker_main()` in `src/license_manager.c` | Reads commands from `wake_pipe_rd` under `lm.lock`; emits 1-byte notifications on `event_pipe_wr` | `select()` with `retry_interval_ms` / `renew_interval_ms` timeout |
| **D-Bus IPC Thread** | `gridd_dbus_thread_main()` in `src/dbus_server.c` | Reads `gridd_license_manager_t` state via thread-safe accessors; writes reload events to `dbus_reload_pipe_wr` | `poll()` across D-Bus watch FDs + `stop_pipe_rd` |
| **RM Unlicensed Monitor** | `rm_unlicensed_monitor_main()` in `src/rm_client.c` | Polls `/proc/driver/nvidia` and `NV2080_CTRL_CMD_GPU_GET_UNLICENSED_STATE`; exits when `stop_pipe_rd` becomes readable | `select()` with 30-second timeout |

## 4. License Manager State Machine

`gridd_lm_state_t` drives both NLS network operations and RM GPU license status updates inside `src/license_manager.c`:

```mermaid
stateDiagram-v2
    [*] --> GRIDD_LM_UNINITIALIZED
    GRIDD_LM_UNINITIALIZED --> GRIDD_LM_INITIALIZED : "gridd_lm_create()"
    GRIDD_LM_INITIALIZED --> GRIDD_LM_ACQUIRING : "gridd_lm_acquire()"

    GRIDD_LM_ACQUIRING --> GRIDD_LM_LICENSED : "HTTP 200 lease acquired -> RM LICENSED"
    GRIDD_LM_ACQUIRING --> GRIDD_LM_UNLICENSED : "Acquire failed -> backoff 10s..300s & RM UNLICENSED"
    GRIDD_LM_UNLICENSED --> GRIDD_LM_ACQUIRING : "Backoff timer expires or wake command"

    GRIDD_LM_LICENSED --> GRIDD_LM_RENEWING : "Renewal interval elapsed (dur * recommended_renewal)"
    GRIDD_LM_RENEWING --> GRIDD_LM_LICENSED : "PUT /leasing/v1/lease/{ref} returns 200 -> reset timer"
    GRIDD_LM_RENEWING --> GRIDD_LM_ACQUIRING : "Renew HTTP 404 or 502 -> re-acquire / failover"
    GRIDD_LM_RENEWING --> GRIDD_LM_UNLICENSED : "Renew failed or lease expired -> RM UNLICENSED"

    GRIDD_LM_LICENSED --> GRIDD_LM_RETURNING : "gridd_lm_stop() (daemon shutdown)"
    GRIDD_LM_RENEWING --> GRIDD_LM_RETURNING : "gridd_lm_stop() (daemon shutdown)"
    GRIDD_LM_RETURNING --> GRIDD_LM_STOPPED : "DELETE leases & POST /leasing/v1/lessor/shutdown"

    GRIDD_LM_INITIALIZED --> GRIDD_LM_STOPPED : "gridd_lm_stop()"
    GRIDD_LM_ACQUIRING --> GRIDD_LM_STOPPED : "gridd_lm_stop()"
    GRIDD_LM_UNLICENSED --> GRIDD_LM_STOPPED : "gridd_lm_stop()"
    GRIDD_LM_STOPPED --> [*]
```

## 5. Build System & Packaging

Defined in [`Makefile`](../Makefile) (repository integration & guest/CI builds) and [`CMakeLists.txt`](../CMakeLists.txt):
- **Standard & Flags:** C11 with `-Wall -Wextra -Werror -Wpedantic` (and `-D_POSIX_C_SOURCE=200809L`).
- **Makefile Targets:**
  - `make`: builds `nvidia-gridd`, `c_unit_tests`, and `c_e2e_tests` against OpenSSL 3.x (`libssl`/`libcrypto`), `pthread`, and `dl`.
  - `make install`: installs the daemon to `$(DESTDIR)$(PREFIX)/bin/nvidia-gridd` (default `/usr/bin/nvidia-gridd`) for drop-in `provision_linux_vgpu_guest.sh` and RPM/DEB/systemd replacement.
  - `make test`: executes `c_unit_tests`, `tests/run_c_e2e.py`, and `tests/test_c_static_audit.py`.
- **CMake Targets:**
  - `gridd_core` (`STATIC`): 14 library translation units under `src/` (excluding `src/main.c`). Links `OpenSSL::SSL`, `OpenSSL::Crypto`, `Threads::Threads`, `${CMAKE_DL_LIBS}`.
  - `nvidia-gridd` (`EXECUTABLE`): `src/main.c` linked against `gridd_core`. Installed to `sbin` (when packaged via CMake).
  - `c_unit_tests` (`EXECUTABLE`): [`tests/c_unit_tests.c`](../tests/c_unit_tests.c) linked against `gridd_core`; registered with CTest (`gridd_c_unit_tests`).
  - `c_e2e_tests` (`EXECUTABLE`): [`tests/c_e2e_tests.c`](../tests/c_e2e_tests.c) linked against `gridd_core`; driven by [`tests/run_c_e2e.py`](../tests/run_c_e2e.py).
- **Packaging Files:**
  - [`packaging/nvidia-gridd.service`](../packaging/nvidia-gridd.service) -> installed to `lib/systemd/system/nvidia-gridd.service` (`Type=forking`, `PIDFile=/var/run/nvidia-gridd/nvidia-gridd.pid`, `ExecStart=/usr/sbin/nvidia-gridd`, `Restart=on-failure`, `RestartSec=5`).
  - [`packaging/gridd.conf.template`](../packaging/gridd.conf.template) -> installed to `share/nvidia/gridd.conf.template` (provides default `FeatureType=2`, `EnableUI=TRUE`, `ClientConfigTokenPath=/etc/nvidia/ClientConfigToken`, and documented optional settings).

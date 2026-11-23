# Clean-Room Behavioral Specification: `nvidia-gridd` (NVIDIA vGPU / GRID Licensing Daemon)

## 1. Overview & Architecture

`nvidia-gridd` is a Linux user-space system daemon responsible for managing NVIDIA GRID / vGPU software licensing for GPUs attached to the local host or virtual machine. It bridges the NVIDIA Resource Manager (RM) kernel driver (`/dev/nvidiactl`, `/dev/nvidia<N>`) with:

1. **NVIDIA License System (NLS)** — Cloud License Service (CLS) and Delegated License Service (DLS) REST APIs, including local encrypted lease caching and offline Node-Locked Licensing (NLL).
2. **Cloud Service Provider (CSP) Marketplace Auto-Licensing** — Instance identity attestation (e.g., AWS EC2 IMDSv2) verified against `/etc/nvidia/GridSwCert.txt`.
3. **D-Bus UI IPC** — Exposes licensing status, active feature type, server coordinates, and configuration reload triggers to `nvidia-settings` over the system D-Bus (`nvidia.grid.server`).

This specification defines the clean-room behavior of the modern licensing daemon, omitting deprecated Legacy License Server / FlexNet Embedded SDK paths.

---

## 2. CLI, Daemon Lifecycle, Signals, and State Directories

### 2.1 Command-Line Invocation
- `nvidia-gridd [-v]`
- `-v`: Enables verbose logging (`setlogmask(0x7f)`, up to `LOG_DEBUG`). Default mask without `-v` is `0x3f` (up to `LOG_INFO`).

### 2.2 Daemonization Sequence
1. Block `SIGCHLD`, `SIGTSTP`, `SIGTTOU`, and `SIGTTIN` via `sigprocmask(SIG_BLOCK, ...)`.
2. Install signal handlers for `SIGINT`, `SIGTERM`, and `SIGPIPE` via `sigaction()`.
3. `fork()` and exit parent with status `0`.
4. Create a self-pipe for shutdown/reload signaling (`pipe()`).
5. `umask(026)` (`0x16`), `setsid()`, close stdin/stdout/stderr (`0`, `1`, `2`).
6. Open syslog: `openlog("nvidia-gridd", 0, LOG_LOCAL0)` (`0x18`), honoring the configured log mask.
7. `chdir("/")`.
8. Ensure `/var/run/nvidia-gridd` exists (`mkdir(..., 0755)`), verify `R_OK | W_OK` (`access(..., 6)`).
9. Open `/var/run/nvidia-gridd/nvidia-gridd.pid` (`O_RDWR | O_CREAT`, mode `0644`), acquire exclusive non-blocking lock via `lockf(fd, F_TLOCK, 0)`, write `"<pid>\n"`.
10. Log `LOG_NOTICE` (`5`): `"Started (%d)"`.

### 2.3 Required Runtime Directories
Before licensing begins, the daemon ensures the following directories exist (creating them if absent):
- `/var/lib/nvidia/GridLicensing`
- `/etc/nvidia/license`
- `/etc/nvidia/ClientConfigToken`
- `/var/lib/nvidia/vGPULicensing`
- `/etc/nvidia/vGPULicense`

### 2.4 Signal Handling & Shutdown
- `SIGINT` and `SIGTERM` set an atomic shutdown flag and write a 4-byte wakeup token to the shutdown self-pipe.
- `SIGPIPE` is ignored/absorbed so transient socket disconnects do not terminate the daemon.
- On shutdown, the daemon cancels any active monitor/D-Bus threads, signals the License Manager to return/persist leases cleanly, removes `/var/run/nvidia-gridd/nvidia-gridd.pid` (and `/var/run/nvidia-gridd` if created by this instance), and closes syslog.

---

## 3. Configuration (`/etc/nvidia/gridd.conf`)

### 3.1 Grammar
- Line-oriented `Key=Value` file, max 1024 bytes per line.
- `#` starts a comment to end of line.
- Horizontal whitespace (`' '`, `'\t'`) and CR/LF are stripped when parsing key/value pairs.

### 3.2 Supported Keys
| Key | Type | Default | Description |
| --- | --- | --- | --- |
| `FeatureType` | uint (`0..4`) | `0` | Licensed feature selector: `0`=Unlicensed/vApps, `1`=vGPU, `2`=vWS (Quadro/RTX vWS), `3`=vGaming, `4`=vCS |
| `EnableUI` | bool (`TRUE`/`FALSE`, case-insensitive) | `TRUE` | Enables D-Bus `nvidia.grid.server` IPC thread |
| `IgnoreSP` | bool (`TRUE`/`FALSE`) | `FALSE` | Ignore Service Provider detection |
| `IgnoreNodeLocked` | bool (`TRUE`/`FALSE`) | `FALSE` | Ignore Node-Locked Licensing |
| `LicenseInterval` | uint (minutes) | `0` (server default) | Requested lease duration in minutes |
| `vGamingMarketplace` | uint | `0` | CSP Marketplace platform ID (`1`=Azure, `2`=AWS EC2, `3`=GCP, `4`=Alibaba ECS, `5`=Tencent, `6`=Tencent IEG) |
| `MarketplaceLicensing` | uint | `0` | Alias for `vGamingMarketplace` |
| `ClientConfigTokenPath` | path | `/etc/nvidia/ClientConfigToken` | Directory or `.tok` file path for NLS client configuration token |
| `ProxyServerAddress` | string | empty | HTTP/HTTPS CONNECT proxy hostname or IP |
| `ProxyServerPort` | string | empty | Proxy TCP port |
| `ProxyUserName` | string | empty | Proxy Basic auth username |
| `ProxyCredentialsFilePath` | path | empty | Path to file containing proxy password/credentials |
| `WaitToConnectIMDS` | int (seconds) | `120` (clamped `20..1200`) | IMDS connection wait budget |
| `PlatformIdentification` | string | empty | Comma-separated NLL platform identifier (`os,arch,...`) |
| `LicenseFilePath` | path | empty | NLL offline license directory/file path |

### 3.3 Atomic `EnableUI=FALSE` Update
When NLS, NLL, or CSP licensing initializes, the daemon ensures `EnableUI=FALSE` is persisted in `/etc/nvidia/gridd.conf`:
- If `gridd.conf` does not exist, creates it with `EnableUI=FALSE\n`.
- If it exists, writes lines to `/etc/nvidia/.gridd.conf_temp`, replacing any line containing `EnableUI` with `EnableUI=FALSE\n` (or appending if absent), then atomically calls `rename("/etc/nvidia/.gridd.conf_temp", "/etc/nvidia/gridd.conf")`.

---

## 4. Feature & Product Name Mapping

Configuration `FeatureType` (`1..4`) maps to RM feature index `(1, 2, 3, 4)` (or `2` for vApps/vWS defaults on certain guests). RM returns product name strings per GPU, which normalize as:

| RM Product Name | Normalized NLS Product Name | Multi-GPU Lease Count |
| --- | --- | --- |
| `NVIDIA RTX Virtual Workstation` | `NVIDIA RTX Virtual Workstation` | `1` |
| `NVIDIA Cloud Gaming` | `NVIDIA vGaming` | `1` |
| `NVIDIA Virtual PC` | `NVIDIA Virtual PC` | `1` |
| `NVIDIA Virtual Applications` | `NVIDIA Virtual Applications` | `1` |
| `NVIDIA Virtual Compute Server` | `NVIDIA Virtual Compute Server` | Count of matching vCS GPUs |
| *(any other)* | `UNKNOWN` | `1` |

Note on vGPU guests (`virtualization_mode == 2`):
- `FeatureType` is coerced to `1` (`vGPU`) when parsing config.
- Supported vGPU profiles vary by license mode; unsupported profiles log `"Unsupported vGPU profile"`.

---

## 5. NVIDIA Resource Manager (RM) Driver Interface

### 5.1 Device Nodes & Ioctl Encoding
- Primary control node: `/dev/nvidiactl`.
- Per-GPU nodes: `/dev/nvidia0` .. `/dev/nvidia31` (opened as needed on `0x215`).
- Linux `_IOWR('F', nr, struct)` ioctl numbers:
  - `NV_ESC_RM_ALLOC` (`0x2b`, 32 bytes): `0xc020462b`
  - `NV_ESC_RM_FREE` (`0x29`, 16 bytes): `0xc0104629`
  - `NV_ESC_RM_CONTROL` (`0x2a`, 32 bytes): `0xc020462a`
  - `NV_ESC_CHECK_VERSION_STR` (`0xd2`, 72 bytes): `0xc04846d2`
  - `NV_ESC_CARD_INFO` (`0xc9`, 4 bytes): `0xc00446c9`
  - `NV_ESC_SYS_PARAMS` (`0xd6`, 8 bytes): `0xc00846d6`
  - `NV_ESC_ATTACH_GPUS_TO_FD` (`0xd1`, 12 bytes): `0xc00c46d1`
  - `NV_ESC_STATUS_CODE` (`0xd3`, 16 bytes): `0xc01046d3`

### 5.2 RM Object Handle Hierarchy
Per initialized RM client (`hClient`):
- Per GPU `i` (`0 <= i < gpu_count`):
  - `hDevice = 0xdae00100 | i` (RM class `0x0080` `NV01_DEVICE_0`)
  - `hSubDevice = 0xdae00200 | i` (RM class `0x2080` `NV20_SUBDEVICE_0`)
  - `hVgpu = 0xdae00300 | i` (RM class `0xa080` `KEPLER_DEVICE_VGPU`, allocated when `virtualization_mode == 2`)

### 5.3 RM Control Commands (`NV_ESC_RM_CONTROL`)
| Control ID | Buffer Size | Purpose |
| --- | --- | --- |
| `0x00000101` | `0x28` (40 B) | Get guest/local NVIDIA driver version string |
| `0x00000133` | `0x508` (1288 B) | Get host driver version string (when `virtualization_mode == 2`) |
| `0x00000201` | `0x80` (128 B) | Get attached GPU ID list (`-1` terminated `uint32[32]`) |
| `0x00000202` | `0x28` (40 B) | Get GPU instance details for a GPU ID |
| `0x00000215` | `0x84` (132 B) | Attach GPUs (probe `0xffffffff` / `0xffff`) |
| `0x00800287` | `4` B | Modify SW state persistence (`uint32 enable`) |
| `0x00800289` | `4` B | Get virtualization mode (`2` = vGPU guest) |
| `0x20800164` | `4` B | Disable licensed feature on subdevice |
| `0x20800186` | `0x84` (132 B) | Enable feature type (`uint32 feature_type`; returns active feature if reboot required) |
| `0x20800187` | `0x8c` (140 B) | Set license status, `uint32 expiry`, `uint8 license_state` |
| `0x20800189` | var | Query CSP identifier (`0` = bare/unknown, `1` = AWS, `2` = Alibaba, `3` = Azure, `5` = GCP) |
| `0x2080018c` | `0x14` (20 B) | Query unlicensed state machine state (`0`=unknown, `3`=restricted, `4`=fully restricted, `5`=licensed) |
| `0x20801801` | `0x10` (16 B) | Query GPU PCI vendor/device IDs (checks `0x13f2` Tesla M60 special handling) |
| `0x20803601` | `0x48` (72 B) | Query GSP firmware status (byte at offset 4: `!= 0` means GSP enabled; `FeatureType=4` requires GSP enabled, `1..3` require GSP disabled on non-vGPU) |
| `0x20808161` | `0x3b8` (952 B) | Query licensable features, feature count, and product names |

### 5.4 Unlicensed State Machine Monitor
When no GPU already reports an active/unrestricted state (`0x20808161`), a monitor thread wakes every 30 seconds (via `select()` on the shutdown self-pipe), confirms `/proc/driver/nvidia` is accessible, and polls `0x2080018c`:
- State `0`: logs `"Your system is in unlicensed unknown state"`.
- State `3`: logs `"Valid GRID license not found. GPU features and performance are restricted..."`.
- State `4`: logs `"Valid GRID license not found. GPU features and performance are fully restricted..."` and exits monitor thread.
- State `5`: exits monitor thread (license acquired).

---

## 6. NVIDIA License System (NLS: CLS & DLS)

### 6.1 Client Configuration Token (`.tok` JWT)
- Located in `ClientConfigTokenPath` (directory scanned for `.tok` file, or direct file path).
- Standard 3-part JWS (`header.payload.signature`, Base64URL-encoded).
- Signature verification:
  - Extract `"service_instance_public_key_pem"` from decoded JSON payload, unescaping `\\n` to `\n`.
  - Verify `header.payload` against decoded signature bytes using the embedded RSA/ECDSA PEM public key (`EVP_DigestVerify*`).
- Temporal validation:
  - Parse `"nbf"` and `"exp"` (UNIX epoch seconds).
  - If `"jti"` differs from the `jti` recorded in the local trusted store, enforce `nbf <= now <= exp`.
- Extracted configuration fields:
  - `"nls_service_instance_ref"` -> `service_instance_ref`
  - `"jti"` -> token UUID
  - `"scope_ref_list"` -> JSON string array (must be non-empty)
  - `"fulfillment_class_ref_list"` -> optional JSON string array
  - `"svc_port_set_list"` -> `"d_name"` (`"CLS"` or `"DLS"`), `"auth"` port, `"lease"` port
  - `"node_url_list"` -> up to 20 node `"url"` entries (plus `"url_qr"` when `d_name == "CLS"`).

### 6.2 Client Environment & Fingerprint
Collected once per session (and refreshed on origin update):
- `fingerprint.mac_address_list`: array of active non-loopback interface MAC addresses (`"%02x:%02x:%02x:%02x:%02x:%02x"`).
- `ip_address_list`: array of interface IPv4/IPv6 addresses (overridden by `NODE_IP` env var if set).
- `hostname`: `gethostname()` (overridden by `NODE_NAME` env var if set).
- `guest_driver_version`: from RM `0x00000101`.
- `os_platform`: `<NAME> <VERSION_ID>` from `/etc/os-release`.
- `os_version`: `<VERSION>` from `/etc/os-release`.
- `host_driver_version`: from RM `0x00000133` on vGPU guests, or `"NA"` otherwise.
- `gpu_id_list`: `["<VENDOR>-<DEVICE>", ...]` from RM `0x20801801`.
- `client_platform_id`: SMBIOS `/sys/class/dmi/id/product_uuid` (fallback `/etc/machine-id`).
- `hv_platform`: hypervisor/chassis platform string.
- `cpu_sockets`, `physical_cores`: parsed from `/proc/cpuinfo` / sysfs topology.

### 6.3 REST API Endpoints & Payloads

All request payloads are capped at `16384` bytes (`0x4000`, maximum supported TCP payload budget). Auth endpoints use `https://<node_url>:<auth_port>`; leasing endpoints use `https://<node_url>:<lease_port>`.

1. **Origin Registration — `POST /auth/v1/origin`**
   - Request JSON:
     ```json
     {
       "service_instance_ref": "<ref>",
       "candidate_origin_ref": "<existing_or_empty>",
       "environment": {
         "fingerprint": { "mac_address_list": ["..."] },
         "hostname": "...",
         "ip_address_list": ["..."],
         "guest_driver_version": "...",
         "os_platform": "...",
         "os_version": "...",
         "host_driver_version": "...",
         "gpu_id_list": ["..."],
         "client_platform_id": "...",
         "hv_platform": "...",
         "cpu_sockets": 1,
         "physical_cores": 4
       },
       "registration_pending": false,
       "update_pending": false
     }
     ```
   - Response: `{ "origin_ref": "<uuid>" }`.
   - Error handling: if HTTP error message matches `"origin reference ... already in use by different fingerprint"` or `"invalid origin environment"`, clear stored `origin_ref`, set `registered=false`, and re-register cleanly.

2. **Origin Update — `POST /auth/v1/origin/update`** (Bearer token required)
   - Request JSON: `{ "environment": { ... }, "origin_ref": "<ref>", "service_instance_ref": "<ref>" }`.

3. **PKCE Auth Code — `POST /auth/v1/code`**
   - Generate random 32-byte `code_verifier` (Base64URL), compute `code_challenge = Base64URL(SHA256(code_verifier))`.
   - Request JSON: `{ "code_challenge": "<s256>", "origin_ref": "<ref>", "service_instance_ref": "<ref>" }`.
   - Response JSON: `{ "auth_code": "<code>" }`.

4. **PKCE Token Exchange — `POST /auth/v1/token`**
   - Request JSON: `{ "auth_code": "<code>", "code_verifier": "<verifier>" }`.
   - Response JSON: `{ "auth_token": "<jwt>", "expires": "<UTC ISO-8601>" }`.
   - All subsequent leasing requests include `Authorization: Bearer <auth_token>` and refresh proactively if expired.

5. **Acquire Lease — `POST /leasing/v1/lessor`** (Bearer token required)
   - Request JSON:
     ```json
     {
       "fulfillment_context": { "fulfillment_class_ref_list": [...] },
       "lease_proposal_list": [
         {
           "duration": "P0Y0M0DT0H<LicenseInterval>M0S",
           "license_type_qualifiers": { "count": <count> },
           "product": { "name": "<product_name>" }
         }
       ],
       "proposal_evaluation_mode": "ALL_OF",
       "scope_ref_list": [...]
     }
     ```
     (`"duration"` omitted if `LicenseInterval == 0`.)
   - Response JSON contains `"lease"` object with `"ref"`, `"product": { "name": ... }`, `"license_type_qualifiers": { "count": ... }`, `"expires"`, `"recommended_lease_renewal"` (fraction `0.0..1.0` of lease lifetime), `"lease_state"`, `"feature_expired"`.
   - HTTP `307` Redirect: parse `"ipv4"`, `"ipv6"`, `"fqdn"` from response and failover to matching configured node URL.

6. **Renew Lease — `PUT /leasing/v1/lease/{lease_ref}`** (Bearer token required)
   - Updates `lease_ref`, `expires`, `recommended_lease_renewal`, and `refresh_count` in local trusted store.

7. **List Active Leases — `GET /leasing/v1/lessor/leases`** (Bearer token required)
   - Parses `"active_lease_list"`; if mismatch with local state, returns held leases before re-acquiring.

8. **Return Leases — `DELETE /leasing/v1/lessor/leases`** (Bearer token required)
   - Clears active lease in local store unless offline lease retention applies.

9. **Shutdown Lessor — `POST /leasing/v1/lessor/shutdown`** (Bearer token required)

### 6.4 HTTP Error & Status Mapping
- `200`: Success.
- `307`: Redirect / node failover (`ipv4` / `ipv6` / `fqdn`).
- `401`: Auth token expired / unauthorized — refresh auth token and retry once.
- `502`: `"Proxy responded with bad gateway. The server endpoint is not available"`.
- Error `"message"` values `"no current lease found"`, `"lease is already released"`, `"no services instances found for origin"`, `"No pool features found"` map to `NO_ACTIVE_LEASE` (`8`).

### 6.5 Encrypted Local Trusted Store (`/var/lib/nvidia/vGPULicensing/`)
- Stores JSON state containing `system_clock_snapshot` (`%Y-%m-%dT%H:%M:%S` UTC), `jti`, `origin` (`origin_ref`, `environment`, `registered`, `failed_registration_attempts`), and `lease_management` (`lease_list`, `lease_state`).
- **Clean-room cryptographic protection:**
  - Derives a 256-bit AES key via HKDF-SHA256 from machine identity (`/sys/class/dmi/id/product_uuid` or `/etc/machine-id`) + static domain separation salt/info.
  - Encrypts store JSON with AES-256-GCM using a fresh 96-bit CSPRNG IV per write.
  - Persists ciphertext (`IV || ciphertext`) and 16-byte GCM auth tag atomically via temp file + `rename()`.
- **Validation rules:**
  - **Clock windback:** Rejects cached lease if `now < system_clock_snapshot`.
  - **Fingerprint match:** Rejects cached lease if stored `mac_address_list` does not match current host MAC addresses.
  - **Expiry:** Rejects cached lease if `lease_state == "EXPIRED"` or `now >= expires`.

### 6.6 Node-Locked Licensing (NLL)
- When `IgnoreNodeLocked=FALSE`, matches `"<os>,<arch>,..."` against `PlatformIdentification` in `gridd.conf`.
- Reads offline lease file from `LicenseFilePath`, verifies JSON fields (`"name"`, `"count"`, `"activated"`, `"expires"`), applies license via RM `0x20800187`, and retains license in local trusted store.

---

## 7. HTTP Transport & Proxy Authentication
- Native POSIX TCP socket + OpenSSL 3.x TLS 1.2+ client (`TLS_client_method()`, `TLS1_2_VERSION` minimum, SNI enabled via `SSL_set_tlsext_host_name()`), supporting both standard `Content-Length` and `Transfer-Encoding: chunked` responses.
- When `ProxyServerAddress` is configured:
  - Uses `ProxyServerAddress` and `ProxyServerPort` (default `3128`) for HTTP `CONNECT host:port HTTP/1.1` tunneling.
  - If `ProxyUserName` and `ProxyCredentialsFilePath` are present: reads Base64-encoded credentials file and sends `Proxy-Authorization: Basic ...`.

---

## 8. Cloud Service Provider (CSP) Marketplace Auto-Licensing

### 8.1 Platform Registry
| Platform | `platform_id` | `csp_id` |
| --- | --- | --- |
| Microsoft Azure | `1` | `3` |
| Amazon EC2 | `2` | `1` |
| Google Cloud | `3` | `5` |
| Alibaba ECS | `4` | `2` |
| Tencent | `5` | `0` |
| Tencent IEG | `6` | `0` |

### 8.2 AWS EC2 IMDSv2 & `GridSwCert.txt` Flow
1. Read `/etc/nvidia/GridSwCert.txt` (max 16384 bytes).
2. Request IMDSv2 session token:
   - `PUT http://169.254.169.254/latest/api/token`
   - Header: `X-aws-ec2-metadata-token-ttl-seconds: 20`
   - Retries within `WaitToConnectIMDS` budget on transient failure.
3. Fetch instance identity document & signature using `X-aws-ec2-metadata-token: <token>`:
   - `GET http://169.254.169.254/latest/dynamic/instance-identity/document`
   - `GET http://169.254.169.254/latest/dynamic/instance-identity/signature`
4. Verify PKCS#7 / X.509 signature of the document against the certificate chain / public keys in `GridSwCert.txt` using OpenSSL (`PKCS7_verify` / `EVP_DigestVerify*`).
5. On success, enable requested feature (`0x20800186`) and mark GPU license state (`0x20800187`, expiry `0`, state `3`).

---

## 9. D-Bus IPC (`nvidia.grid.server`)

- Loaded optionally via `dlopen("libdbus-1.so.3", RTLD_LAZY)` when `EnableUI=TRUE`.
- Connects to `DBUS_BUS_SYSTEM` (`1`), requests bus name `"nvidia.grid.server"` with `DBUS_NAME_FLAG_REPLACE_EXISTING` (`2`), polling watch FDs and shutdown pipe via `poll()`.
- Handles method calls on interface `"nvidia.grid.license"`, member `"GridLicenseState"`:
  - Argument `INT32` (`'i'`):
    - `1`: Reply `INT32` = current Cloud License Manager state (`0..7`).
    - `2`: Write `1` to D-Bus reload pipe (triggers config reload in `main()`), reply `INT32 0`.
    - `3`: Reply `INT32` = active `FeatureType`.
    - `4`: Reply `INT32` = active license server port.
  - Argument `STRING` (`'s'`):
    - `"PrimaryServerAddress"`: Reply `STRING` = primary server URL or `"Not Configured"`.
    - `"SecondaryServerAddress"`: Reply `STRING` = secondary server URL or `"Not Configured"`.

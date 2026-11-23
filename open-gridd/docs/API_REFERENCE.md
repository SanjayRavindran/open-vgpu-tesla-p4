# `nvidia-gridd` C11 API Reference

Complete reference for all 15 header/source modules under [`include/gridd/`](../include/gridd/) and [`src/`](../src/), plus the CLI entry point [`src/main.c`](../src/main.c). Every public `gridd_*` symbol, type, enum, struct, and macro matches the C11 header declarations verbatim.

---

## Table of Contents
1. [`types.h` — Shared Constants, Status Codes & Enums](#1-typesh)
2. [`log.h` / `log.c` — Syslog & Stderr Logger](#2-logh--logc)
3. [`daemon.h` / `daemon.c` — Daemon Lifecycle, PID Lock & Self-Pipes](#3-daemonh--daemonc)
4. [`config.h` / `config.c` — Configuration Parser & Feature Mapping](#4-configh--configc)
5. [`json_util.h` / `json_util.c` — JSON Builder & Extractor](#5-json_utilh--json_utilc)
6. [`crypto.h` / `crypto.c` — OpenSSL Cryptographic Primitives](#6-cryptoh--cryptoc)
7. [`rm_client.h` / `rm_client.c` — NVIDIA RM Ioctls & Unlicensed Monitor](#7-rm_clienth--rm_clientc)
8. [`token.h` / `token.c` — Client Configuration Token (.tok) JWT Parser](#8-tokenh--tokenc)
9. [`env_fingerprint.h` / `env_fingerprint.c` — Host Environment Fingerprinting](#9-env_fingerprinth--env_fingerprintc)
10. [`trusted_store.h` / `trusted_store.c` — Encrypted Local Trusted Store](#10-trusted_storeh--trusted_storec)
11. [`http_client.h` / `http_client.c` — Native POSIX TCP / OpenSSL TLS / Proxy Client](#11-http_clienth--http_clientc)
12. [`nls_client.h` / `nls_client.c` — NLS Protocol & Node-Locked License Client](#12-nls_clienth--nls_clientc)
13. [`license_manager.h` / `license_manager.c` — Asynchronous License Manager](#13-license_managerh--license_managerc)
14. [`csp_platform.h` / `csp_platform.c` — Cloud Service Provider Marketplace Verification](#14-csp_platformh--csp_platformc)
15. [`dbus_server.h` / `dbus_server.c` — D-Bus IPC Server](#15-dbus_serverh--dbus_serverc)
16. [`src/main.c` — Daemon Entry Point](#16-srcmainc)

---

## 1. `types.h`

Header: [`include/gridd/types.h`](../include/gridd/types.h)

### Bounds & Capacity Constants
| Macro | Value | Purpose |
| --- | --- | --- |
| `GRIDD_MAX_PATH` | `1024` | Maximum filesystem path buffer length. |
| `GRIDD_MAX_STR` | `256` | Default fixed string buffer length. |
| `GRIDD_MAX_URL` | `128` | Maximum individual NLS node or QR URL length. |
| `GRIDD_MAX_NODES` | `20` | Maximum failover NLS node URLs stored in a token. |
| `GRIDD_MAX_GPUS` | `32` | Maximum attached NVIDIA GPUs tracked per daemon instance. |
| `GRIDD_MAX_FEATURES` | `8` | Maximum licensable RM features queried per GPU. |
| `GRIDD_MAX_TCP_PAYLOAD` | `16384` | Maximum TCP/certificate payload buffer size in bytes. |

### `gridd_status_t`
| Enumerator | Value | Meaning |
| --- | --- | --- |
| `GRIDD_OK` | `0` | Operation succeeded. |
| `GRIDD_ERR_GENERIC` | `1` | Unspecified internal or runtime failure. |
| `GRIDD_ERR_INVALID_PARAM` | `2` | Invalid or `NULL` caller argument or malformed input. |
| `GRIDD_ERR_AUTH` | `3` | Authentication, JWT signature, or GCM tag verification failure. |
| `GRIDD_ERR_IO` | `4` | Socket, TLS, filesystem, or HTTP transport I/O error. |
| `GRIDD_ERR_UNSUPPORTED` | `5` | Unsupported feature, platform, or operation. |
| `GRIDD_ERR_RM` | `6` | NVIDIA Resource Manager `/dev/nvidiactl` ioctl error. |
| `GRIDD_ERR_PIPE` | `7` | POSIX pipe creation or I/O error. |
| `GRIDD_ERR_NO_LEASE` | `8` | No active license lease available. |
| `GRIDD_ERR_OFFLINE_LEASE` | `9` | Offline or node-locked lease handling condition. |
| `GRIDD_ERR_CLOCK_WINDBACK` | `11` | System wall clock rolled backwards relative to trusted store snapshot. |
| `GRIDD_ERR_EXPIRED` | `14` | Token or credential outside valid `nbf`/`exp` window. |
| `GRIDD_ERR_REDIRECT` | `20` | HTTP 307 redirect received from NLS node. |
| `GRIDD_ERR_MISMATCH` | `22` | Fingerprint, identifier, or configuration mismatch. |
| `GRIDD_ERR_ENV` | `23` | Environment fingerprint collection error. |
| `GRIDD_ERR_FILE_READ` | `24` | Expected configuration, token, or certificate file missing/unreadable. |
| `GRIDD_ERR_LEASE_EXPIRED` | `25` | Active license lease reached expiration timestamp. |
| `GRIDD_ERR_NO_CACHE` | `26` | Trusted store file (`store_nls.dat` / `.tag`) not yet initialized. |

### `gridd_feature_type_t`
- `GRIDD_FEATURE_UNLICENSED = 0`
- `GRIDD_FEATURE_VGPU = 1`
- `GRIDD_FEATURE_VWS = 2`
- `GRIDD_FEATURE_VGAMING = 3`
- `GRIDD_FEATURE_VCS = 4`

### `gridd_lm_state_t`
- `GRIDD_LM_UNINITIALIZED = 0`
- `GRIDD_LM_INITIALIZED = 1`
- `GRIDD_LM_ACQUIRING = 2`
- `GRIDD_LM_LICENSED = 3`
- `GRIDD_LM_RENEWING = 4`
- `GRIDD_LM_UNLICENSED = 5`
- `GRIDD_LM_RETURNING = 6`
- `GRIDD_LM_STOPPED = 7`

### `gridd_rm_license_state_t`
- `GRIDD_LICENSE_STATE_UNLICENSED = 1`
- `GRIDD_LICENSE_STATE_TRIAL_OR_UNSPECIFIED = 2`
- `GRIDD_LICENSE_STATE_LICENSED = 3`
- `GRIDD_LICENSE_STATE_PERPETUAL = 4`

---

## 2. `log.h` / `log.c`

Header: [`include/gridd/log.h`](../include/gridd/log.h) | Source: [`src/log.c`](../src/log.c)

### Types & Macros
- `gridd_log_level_t`: `GRIDD_LOG_ERR = 3`, `GRIDD_LOG_WARN = 4`, `GRIDD_LOG_NOTICE = 5`, `GRIDD_LOG_INFO = 6`, `GRIDD_LOG_DEBUG = 7` (aligned with syslog priorities).
- Convenience macros: `LOG_E(...)`, `LOG_W(...)`, `LOG_N(...)`, `LOG_I(...)`, `LOG_D(...)`.

### Functions (4)
- `void gridd_log_init(const char *ident, bool verbose, bool use_stderr_only)`: Configures logging backend (`openlog` with `LOG_DAEMON` or stderr in foreground `-f` mode) and sets verbosity mask.
- `void gridd_log_close(void)`: Closes open syslog handle if initialized.
- `void gridd_log_set_verbose(bool verbose)`: Enables or disables `LOG_DEBUG` emission at runtime.
- `void gridd_log(gridd_log_level_t level, const char *fmt, ...)`: Formats and writes a log message filtered by the active verbosity level.

---

## 3. `daemon.h` / `daemon.c`

Header: [`include/gridd/daemon.h`](../include/gridd/daemon.h) | Source: [`src/daemon.c`](../src/daemon.c)

### Constants & Structs
- `GRIDD_PID_DIR`: `"/var/run/nvidia-gridd"`
- `GRIDD_PID_FILE`: `"/var/run/nvidia-gridd/nvidia-gridd.pid"`
- `gridd_daemon_ctx_t`: Holds self-pipe file descriptors (`exit_pipe_rd`, `exit_pipe_wr`, `dbus_pipe_rd`, `dbus_pipe_wr`), PID file descriptor and PID (`pid_fd`, `pid`), `created_pid_dir`, and `foreground` flag.

### Functions (7)
- `gridd_status_t gridd_daemon_init(gridd_daemon_ctx_t *ctx, bool verbose, bool foreground)`: Initializes logging, self-pipes (`exit_pipe` and `dbus_pipe`), signal handlers (`SIGINT`, `SIGTERM`, `SIGPIPE`; ignores `SIGCHLD`/`SIGTSTP`/`SIGTTOU`/`SIGTTIN`), and when `!foreground` performs `umask(026)`, `fork()`, `setsid()`, `chdir("/")`, creates `/var/run/nvidia-gridd/nvidia-gridd.pid`, and acquires `lockf(F_TLOCK, 0)`.
- `void gridd_daemon_cleanup(gridd_daemon_ctx_t *ctx, int exit_code)`: Closes self-pipe FDs, unlocks/unlinks PID file and created directory, closes logs.
- `bool gridd_daemon_stop_requested(void)`: Returns async-signal-safe shutdown flag status.
- `void gridd_daemon_request_stop(void)`: Sets shutdown flag and writes one byte to the exit self-pipe.
- `gridd_status_t gridd_daemon_signal_dbus_reload(gridd_daemon_ctx_t *ctx)`: Writes a reload trigger byte into `ctx->dbus_pipe_wr`.
- `gridd_status_t gridd_ensure_state_dirs(void)`: Ensures standard NVIDIA licensing directories exist (`/var/lib/nvidia/GridLicensing`, `/etc/nvidia/license`, `/etc/nvidia/ClientConfigToken`, `/var/lib/nvidia/vGPULicensing`, `/etc/nvidia/vGPULicense`).
- `gridd_status_t gridd_ensure_dir(const char *path, unsigned int mode)`: Recursively/idempotently ensures a single directory exists with `mode`.

---

## 4. `config.h` / `config.c`

Header: [`include/gridd/config.h`](../include/gridd/config.h) | Source: [`src/config.c`](../src/config.c)

### Constants & Structs
- `GRIDD_DEFAULT_CONF_PATH`: `"/etc/nvidia/gridd.conf"`
- `GRIDD_DEFAULT_CONF_TEMP`: `"/etc/nvidia/.gridd.conf_temp"`
- `GRIDD_DEFAULT_TOKEN_DIR`: `"/etc/nvidia/ClientConfigToken"`
- `GRIDD_DEFAULT_STORE_DIR`: `"/var/lib/nvidia/vGPULicensing"`
- `GRIDD_DEFAULT_SWCERT_PATH`: `"/etc/nvidia/GridSwCert.txt"`
- `gridd_config_t`: `feature_type`, `enable_ui`, `ignore_sp`, `ignore_node_locked`, `license_interval`, `marketplace_platform`, `wait_to_connect_imds` (default `120`, clamped `20..1200`), `client_config_token_path`, `proxy_server_address`, `proxy_server_port`, `proxy_user_name`, `proxy_credentials_file_path`, `platform_identification`, `license_file_path`.

### Functions (5)
- `void gridd_config_init_defaults(gridd_config_t *cfg)`: Zero-initializes `cfg` and applies defaults (`enable_ui = true`, `wait_to_connect_imds = 120`, `client_config_token_path = GRIDD_DEFAULT_TOKEN_DIR`).
- `gridd_status_t gridd_config_load(const char *path, bool is_vgpu_guest, gridd_config_t *cfg)`: Parses `#`-commented `Key=Value` lines from `path`, clamping `WaitToConnectIMDS` and applying vGPU guest rules.
- `bool gridd_config_disable_ui(const char *path, const char *temp_path)`: Rewrites `EnableUI=FALSE` into `temp_path` and atomically replaces `path` via `rename()`.
- `const char *gridd_feature_to_product_name(const char *rm_product_name)`: Normalizes RM feature product names to canonical NLS product strings.
- `uint32_t gridd_map_feature_type(uint32_t configured_feature_type)`: Maps configured `FeatureType` (`0..4`) to RM licensable feature code.

---

## 5. `json_util.h` / `json_util.c`

Header: [`include/gridd/json_util.h`](../include/gridd/json_util.h) | Source: [`src/json_util.c`](../src/json_util.c)

### Structs
- `gridd_json_buf_t`: Dynamic string buffer (`data`, `len`, `cap`, `overflow`) capped at 16 MiB (`1 << 24`).

### Functions (12)
- `void gridd_json_buf_init(gridd_json_buf_t *buf, size_t initial_cap)`: Allocates initial capacity for a JSON output buffer.
- `void gridd_json_buf_free(gridd_json_buf_t *buf)`: Frees internal buffer memory and resets fields.
- `bool gridd_json_buf_append(gridd_json_buf_t *buf, const char *str)`: Appends a raw C string, growing dynamically up to cap.
- `bool gridd_json_buf_appendf(gridd_json_buf_t *buf, const char *fmt, ...)`: Formats and appends `printf`-style text into `buf`.
- `bool gridd_json_buf_append_escaped_str(gridd_json_buf_t *buf, const char *str)`: Appends `str` wrapped in double quotes with RFC 8259 JSON escaping.
- `bool gridd_json_get_string(const char *json, const char *key, char *out, size_t out_sz)`: Extracts and unescapes a string value matching `"key"` into `out`.
- `bool gridd_json_get_raw_value(const char *json, const char *key, char *out, size_t out_sz)`: Copies the raw JSON token/object/array for `"key"` verbatim.
- `bool gridd_json_get_uint64(const char *json, const char *key, uint64_t *out)`: Parses an unsigned 64-bit integer field.
- `bool gridd_json_get_int64(const char *json, const char *key, int64_t *out)`: Parses a signed 64-bit integer field.
- `bool gridd_json_get_double(const char *json, const char *key, double *out)`: Parses a floating-point number field.
- `bool gridd_json_get_bool(const char *json, const char *key, bool *out)`: Parses a boolean `true`/`false` field.
- `bool gridd_json_get_array_raw(const char *json, const char *key, char *out, size_t out_sz)`: Extracts the raw `[...]` array substring for `"key"`.

---

## 6. `crypto.h` / `crypto.c`

Header: [`include/gridd/crypto.h`](../include/gridd/crypto.h) | Source: [`src/crypto.c`](../src/crypto.c)

### Constants
- `GRIDD_AES_KEY_LEN = 32`, `GRIDD_GCM_IV_LEN = 12`, `GRIDD_GCM_TAG_LEN = 16`

### Functions (12)
- `bool gridd_base64_encode(const uint8_t *in, size_t in_len, char *out, size_t out_sz, bool url_safe)`: Encodes bytes in standard Base64 or unpadded Base64URL (`-` / `_`).
- `bool gridd_base64_decode(const char *in, uint8_t *out, size_t out_sz, size_t *out_len)`: Decodes standard padded or unpadded Base64URL strings while stripping trailing whitespace.
- `bool gridd_sha256(const uint8_t *data, size_t len, uint8_t out_hash[32])`: Computes SHA-256 digest via OpenSSL EVP.
- `bool gridd_random_bytes(uint8_t *out, size_t len)`: Generates `len` cryptographically secure random bytes via `RAND_bytes`.
- `bool gridd_pkce_generate(char *verifier_out, size_t verifier_sz, char *challenge_out, size_t challenge_sz)`: Generates a 32-byte random Base64URL `code_verifier` and S256 `code_challenge`.
- `bool gridd_hkdf_sha256(const uint8_t *ikm, size_t ikm_len, const uint8_t *salt, size_t salt_len, const uint8_t *info, size_t info_len, uint8_t *okm, size_t okm_len)`: Derives key material using OpenSSL EVP HKDF-SHA256.
- `bool gridd_hmac_sha256(const uint8_t *key, size_t key_len, const uint8_t *data, size_t data_len, uint8_t out_mac[32])`: Computes 32-byte HMAC-SHA256 tag.
- `bool gridd_aes256_gcm_encrypt(const uint8_t key[32], const uint8_t iv[12], const uint8_t *plaintext, size_t pt_len, uint8_t *ciphertext_out, uint8_t tag_out[16])`: Encrypts plaintext using AES-256-GCM and outputs the 16-byte authentication tag.
- `bool gridd_aes256_gcm_decrypt(const uint8_t key[32], const uint8_t iv[12], const uint8_t *ciphertext, size_t ct_len, const uint8_t tag[16], uint8_t *plaintext_out)`: Verifies the 16-byte GCM tag and decrypts `ciphertext`.
- `bool gridd_jwt_split(const char *jwt, char *header_b64, size_t header_sz, char *payload_b64, size_t payload_sz, char *sig_b64, size_t sig_sz)`: Splits a `header.payload.signature` JWT into its three Base64URL parts.
- `bool gridd_verify_pem_signature(const char *pem_pubkey, const uint8_t *message, size_t msg_len, const uint8_t *sig, size_t sig_len)`: Verifies SHA-256 ECDSA/RSA signatures using a PEM public key (handles raw `r||s` JWT ECDSA P-256 signatures and DER signatures).
- `bool gridd_verify_pkcs7_pem(const char *pkcs7_pem, const char *ca_cert_pem, char *verified_content_out, size_t out_sz)`: Verifies CMS/PKCS#7 signed payloads (such as AWS IMDS identity documents) against a trusted X.509 certificate.

---

## 7. `rm_client.h` / `rm_client.c`

Header: [`include/gridd/rm_client.h`](../include/gridd/rm_client.h) | Source: [`src/rm_client.c`](../src/rm_client.c)

### Ioctl & RM Control Constants
- Escape ioctls: `NV_ESC_RM_ALLOC` (`0xc020462bU`), `NV_ESC_RM_FREE` (`0xc0104629U`), `NV_ESC_RM_CONTROL` (`0xc020462aU`), `NV_ESC_CHECK_VERSION_STR` (`0xc04846d2U`), `NV_ESC_CARD_INFO` (`0xc00446c9U`), `NV_ESC_SYS_PARAMS` (`0xc00846d6U`), `NV_ESC_ATTACH_GPUS_TO_FD` (`0xc00c46d1U`), `NV_ESC_STATUS_CODE` (`0xc01046d3U`).
- Root/Device/Subdevice controls: `NV0000_CTRL_CMD_SYSTEM_GET_BUILD_VERSION` (`0x00000101U`), `NV0000_CTRL_CMD_SYSTEM_GET_HOST_VERSION` (`0x00000133U`), `NV0000_CTRL_CMD_GPU_GET_ATTACHED_IDS` (`0x00000201U`), `NV0000_CTRL_CMD_GPU_GET_ID_INFO` (`0x00000202U`), `NV0000_CTRL_CMD_GPU_ATTACH_IDS` (`0x00000215U`), `NV0080_CTRL_CMD_GPU_MODIFY_SW_STATE_PERSISTENCE` (`0x00800287U`), `NV0080_CTRL_CMD_GPU_GET_VIRTUALIZATION_MODE` (`0x00800289U`), `NV2080_CTRL_CMD_GPU_DISABLE_LICENSED_FEATURE` (`0x20800164U`), `NV2080_CTRL_CMD_GPU_ENABLE_LICENSED_FEATURE` (`0x20800186U`), `NV2080_CTRL_CMD_GPU_SET_LICENSE_STATUS` (`0x20800187U`), `NV2080_CTRL_CMD_GPU_GET_CSP_IDENTIFIER` (`0x20800189U`), `NV2080_CTRL_CMD_GPU_GET_UNLICENSED_STATE` (`0x2080018cU`), `NV2080_CTRL_CMD_BUS_GET_PCI_INFO` (`0x20801801U`), `NV2080_CTRL_CMD_GSP_GET_FEATURES` (`0x20803601U`), `NV2080_CTRL_CMD_GPU_GET_LICENSABLE_FEATURES` (`0x20808161U`).
- Modes & IDs: `NV_VGPU_VIRTUALIZATION_MODE_GUEST` (`2U`), `NV_PCI_DEVID_TESLA_M60` (`0x13f2U`).

### Structs
- `gridd_rm_feature_info_t`: Feature code, RM product name, and active flag.
- `gridd_rm_gpu_t`: GPU ID/instance, allocated device/subdevice/vGPU handles, virtualization mode, PCI vendor/device/domain/bus/slot, licensable flag, and feature list (`GRIDD_MAX_FEATURES`).
- `gridd_rm_ops_t`: Dependency-injected vtable (`open_ctl`, `close_ctl`, `rm_alloc`, `rm_free`, `rm_control`, `ctx`) allowing unit tests to mock `/dev/nvidiactl`.
- `gridd_rm_client_t`: Holds `ops`, `ctl_fd`, `h_client`, `gpu_count`, `gpus[32]`, and `is_vgpu_guest`.

### Functions (14)
- `const gridd_rm_ops_t *gridd_rm_default_ops(void)`: Returns the production `/dev/nvidiactl` `ioctl()` vtable.
- `gridd_status_t gridd_rm_init(gridd_rm_client_t *client, const gridd_rm_ops_t *ops)`: Opens control FD, verifies driver version, allocates root client and per-GPU device/subdevice/vGPU handles, and detects guest mode.
- `void gridd_rm_destroy(gridd_rm_client_t *client)`: Frees allocated RM objects and closes control FD.
- `gridd_status_t gridd_rm_set_sw_persistence(gridd_rm_client_t *client, bool enable)`: Enables or disables RM software state persistence (`0x00800287U`) per GPU.
- `gridd_status_t gridd_rm_query_features(gridd_rm_client_t *client)`: Queries licensable features (`0x20808161U`) on attached GPUs.
- `bool gridd_rm_gpu_supports_feature(gridd_rm_client_t *client, uint32_t gpu_idx, uint32_t feature_code, uint32_t *out_feat_idx)`: Checks if GPU `gpu_idx` exposes `feature_code` and optionally outputs its feature array index.
- `gridd_status_t gridd_rm_query_pci_ids(gridd_rm_client_t *client)`: Queries PCI vendor/device/BDF metadata (`0x20801801U`) for each GPU.
- `gridd_status_t gridd_rm_get_csp_id(gridd_rm_client_t *client, uint32_t gpu_idx, uint32_t *out_csp_id)`: Reads hardware CSP identifier (`0x20800189U`).
- `gridd_status_t gridd_rm_enable_feature(gridd_rm_client_t *client, uint32_t feature_code, uint32_t *out_active_feature, bool *out_reboot_required)`: Sends `NV2080_CTRL_CMD_GPU_ENABLE_LICENSED_FEATURE` across eligible GPUs and reports reboot requirement.
- `gridd_status_t gridd_rm_set_license_status(gridd_rm_client_t *client, uint32_t feature_code, uint32_t expiry_epoch, uint8_t state)`: Updates RM kernel license status (`0x20800187U`) using `gridd_rm_license_state_t`.
- `gridd_status_t gridd_rm_disable_feature(gridd_rm_client_t *client, uint32_t feature_code)`: Disables a licensed feature (`0x20800164U`).
- `gridd_status_t gridd_rm_get_telemetry(gridd_rm_client_t *client, char *guest_ver, size_t guest_sz, char *host_ver, size_t host_sz)`: Queries guest build version (`0x00000101U`) and vGPU host driver version (`0x00000133U`).
- `gridd_status_t gridd_rm_get_unlicensed_state(gridd_rm_client_t *client, uint32_t gpu_idx, uint32_t *out_state)`: Queries `NV2080_CTRL_CMD_GPU_GET_UNLICENSED_STATE`.
- `int gridd_rm_start_unlicensed_monitor(gridd_rm_client_t *client, int stop_pipe_rd)`: Spawns detached pthread polling unlicensed status every 30s until `stop_pipe_rd` becomes readable.

---

## 8. `token.h` / `token.c`

Header: [`include/gridd/token.h`](../include/gridd/token.h) | Source: [`src/token.c`](../src/token.c)

### Structs
- `gridd_token_config_t`: `node_urls[20][128]`, `node_count`, `url_qr`, `auth_port`, `lease_port`, `is_cls`, `service_instance_ref`, `jti`, `scope_ref_list`, `fulfillment_class_ref_list`, `nbf`, `exp`.

### Functions (2)
- `bool gridd_token_find_file(const char *path_or_dir, char *out_file, size_t out_sz)`: Resolves either a direct `.tok` file path or the first `.tok` file inside a directory.
- `bool gridd_token_parse_and_verify(const char *path_or_dir, const char *cached_jti, uint64_t now_epoch, gridd_token_config_t *out_cfg, char *err_out, size_t err_sz)`: Reads `.tok` JWT, verifies ES256/RS256 signature against pinned NLS public keys (or test hook when non-production), validates `nbf`/`exp` against `now_epoch`, and populates `out_cfg`.

---

## 9. `env_fingerprint.h` / `env_fingerprint.c`

Header: [`include/gridd/env_fingerprint.h`](../include/gridd/env_fingerprint.h) | Source: [`src/env_fingerprint.c`](../src/env_fingerprint.c)

### Structs
- `gridd_environment_t`: `mac_address_list`, `ip_address_list`, `hostname`, `guest_driver_version`, `os_platform`, `os_version`, `host_driver_version`, `gpu_id_list`, `client_platform_id`, `hv_platform`, `cpu_sockets`, `physical_cores`.

### Functions (2)
- `gridd_status_t gridd_env_collect(gridd_rm_client_t *rm, gridd_environment_t *env)`: Collects network interfaces (`getifaddrs` + `NODE_IP`/`NODE_NAME` env overrides), OS info (`/etc/os-release`), CPU topology (`/proc/cpuinfo`), and RM GPU/driver telemetry.
- `bool gridd_env_read_machine_uuid(char *out, size_t out_sz)`: Reads `/sys/class/dmi/id/product_uuid`, falling back to `/etc/machine-id` (formatted as UUID) or an all-zero fallback UUID.

---

## 10. `trusted_store.h` / `trusted_store.c`

Header: [`include/gridd/trusted_store.h`](../include/gridd/trusted_store.h) | Source: [`src/trusted_store.c`](../src/trusted_store.c)

### Structs
- `gridd_trusted_store_data_t`: `system_clock_snapshot`, `jti`, `origin_ref`, `registered`, `failed_registration_attempts`, `env`, `lease_list_json`, `lease_state`.
- `gridd_trusted_store_t`: `store_dir`, `store_file` (`store_nls.dat`), `tag_file` (`store_nls.tag`), `key[32]`, and `data`.

### Functions (6)
- `gridd_status_t gridd_trusted_store_open(gridd_trusted_store_t *store, const char *store_dir, const char *machine_id, const char *jti)`: Derives AES-256-GCM key from `machine_id` via HKDF-SHA256, opens/decrypts `store_nls.dat` + `store_nls.tag` under `g_store_mutex`, or initializes empty state with `GRIDD_ERR_NO_CACHE`.
- `gridd_status_t gridd_trusted_store_save(gridd_trusted_store_t *store)`: Encrypts serialized `store->data` with fresh 12-byte IV and atomically writes `store_nls.dat` + `store_nls.tag` via `.tmp` + `rename()`.
- `gridd_status_t gridd_trusted_store_check_clock(gridd_trusted_store_t *store, uint64_t now_epoch)`: Compares `now_epoch` against `system_clock_snapshot` and returns `GRIDD_ERR_CLOCK_WINDBACK` on clock rollback beyond grace threshold.
- `bool gridd_trusted_store_match_fingerprint(const gridd_trusted_store_t *store, const gridd_environment_t *current_env)`: Compares stored environment fingerprint fields against `current_env`.
- `void gridd_format_utc_iso8601(uint64_t epoch, char *out, size_t out_sz)`: Formats Unix epoch seconds as `YYYY-MM-DDTHH:MM:SSZ`.
- `bool gridd_parse_utc_iso8601(const char *iso, uint64_t *out_epoch)`: Parses UTC ISO-8601 timestamp string into Unix epoch seconds.

---

## 11. `http_client.h` / `http_client.c`

Header: [`include/gridd/http_client.h`](../include/gridd/http_client.h) | Source: [`src/http_client.c`](../src/http_client.c)

### Structs
- `gridd_http_opts_t`: `proxy_host`, `proxy_port`, `proxy_user`, `proxy_password`, `use_kerberos`, `timeout_sec`.
- `gridd_http_response_t`: `status_code`, dynamically allocated `body`, `body_len`, `err`, `err_msg`.

### Functions (5)
- `void gridd_http_global_init(void)`: Initializes process-wide OpenSSL library state (no `libcurl` dependency).
- `void gridd_http_global_cleanup(void)`: Releases process-wide HTTP/OpenSSL cleanup hooks.
- `void gridd_http_response_free(gridd_http_response_t *resp)`: Frees `resp->body` and zeroes `gridd_http_response_t`.
- `bool gridd_http_load_proxy_opts(const char *conf_path, gridd_http_opts_t *opts)`: Reads proxy host, port, username, and optional credentials file into `opts`.
- `gridd_status_t gridd_http_request(const char *method, const char *url, const char **headers, size_t header_count, const char *body, size_t body_len, const gridd_http_opts_t *opts, gridd_http_response_t *resp)`: Executes an HTTP/1.1 request over POSIX TCP sockets with optional OpenSSL TLS 1.2+ (SNI + certificate/hostname verification), `CONNECT` proxy tunnel with Basic auth, timeouts, and chunked transfer decoding.

---

## 12. `nls_client.h` / `nls_client.c`

Header: [`include/gridd/nls_client.h`](../include/gridd/nls_client.h) | Source: [`src/nls_client.c`](../src/nls_client.c)

### Structs
- `gridd_lease_info_t`: `lease_ref`, `product_name`, `count`, `expires_iso`, `expires_epoch`, `lease_duration_sec`, `recommended_renewal`, `feature_expired`, `refresh_count`.
- `gridd_nls_client_t`: Embeds `token`, `env`, `store`, `http_opts`, `active_node_idx`, `license_interval_min`, `auth_token`, `auth_token_expires_epoch`, `lease`, `has_active_lease`, `last_error`.

### Functions (11)
- `gridd_status_t gridd_nls_init(gridd_nls_client_t *nls, const gridd_token_config_t *token, const gridd_environment_t *env, const gridd_http_opts_t *http_opts, const char *store_dir, const char *machine_id, uint32_t license_interval_min)`: Initializes client state, opens trusted store, checks clock windback, and validates environment fingerprint.
- `gridd_status_t gridd_nls_register_or_update_origin(gridd_nls_client_t *nls)`: Executes `POST /auth/v1/origin` (or `PUT` on update / HTTP 409 conflict) and persists `origin_ref`.
- `gridd_status_t gridd_nls_authenticate(gridd_nls_client_t *nls)`: Performs PKCE S256 challenge + `POST /auth/v1/code` + `POST /auth/v1/token` exchange, caching `auth_token`.
- `gridd_status_t gridd_nls_acquire_lease(gridd_nls_client_t *nls, const char *product_name, uint32_t count)`: Submits lease proposal to `POST /leasing/v1/lessor`; handles 307 redirects, 401 re-auth, and 502 node failover.
- `gridd_status_t gridd_nls_renew_lease(gridd_nls_client_t *nls, const char *product_name, uint32_t count)`: Renews active lease via `PUT /leasing/v1/lease/{ref}`, refreshing `auth_token` on 401.
- `gridd_status_t gridd_nls_get_active_leases(gridd_nls_client_t *nls, char *out_buf, size_t out_sz)`: Queries `GET /leasing/v1/lessor/leases`.
- `gridd_status_t gridd_nls_return_leases(gridd_nls_client_t *nls)`: Releases active lease(s) via `DELETE /leasing/v1/lease/{ref}`.
- `gridd_status_t gridd_nls_shutdown(gridd_nls_client_t *nls)`: Sends `POST /leasing/v1/lessor/shutdown` and clears cached store lease state.
- `bool gridd_nls_build_lease_proposal_json(const gridd_token_config_t *token, const char *product_name, uint32_t count, uint32_t duration_min, char *out_buf, size_t out_sz)`: Serializes scope refs, fulfillment class refs, product name, count, and duration JSON.
- `bool gridd_nls_parse_lease_response(const char *json, uint64_t now_epoch, bool is_renew, gridd_lease_info_t *out_lease)`: Parses NLS JSON response timestamps, recommended renewal ratio, and `lease_ref`.
- `bool gridd_nls_try_nll(const char *conf_path, const char *os_platform, const char *arch, gridd_lease_info_t *out_lease)`: Evaluates offline Node-Locked License (`.lic`) files configured via `LicenseFilePath` / standard NLL paths.

---

## 13. `license_manager.h` / `license_manager.c`

Header: [`include/gridd/license_manager.h`](../include/gridd/license_manager.h) | Source: [`src/license_manager.c`](../src/license_manager.c)

### Structs
- `gridd_license_manager_t`: Tracks `state`, `pending_state`, `pthread_mutex_t lock`, `pthread_t thread`, `wake_pipe_rd/wr`, `event_pipe_rd/wr`, `retry_interval_ms`, `renew_interval_ms`, `product_name`, `product_count`, and embedded `gridd_nls_client_t nls`.

### Functions (9)
- `gridd_status_t gridd_lm_create(gridd_license_manager_t *lm, const gridd_token_config_t *token, const gridd_environment_t *env, const gridd_http_opts_t *http_opts, const char *store_dir, const char *machine_id, uint32_t license_interval_min)`: Creates wake/event pipes, initializes NLS client, and spawns the background licensing worker thread.
- `void gridd_lm_destroy(gridd_license_manager_t *lm)`: Signals stop if running, joins worker thread, closes pipes, and destroys mutex.
- `void gridd_lm_acquire(gridd_license_manager_t *lm, const char *product_name, uint32_t count)`: Sets target product/count and wakes worker thread to acquire a lease.
- `void gridd_lm_stop(gridd_license_manager_t *lm)`: Commands worker thread to return leases (`DELETE`) and call NLS `/shutdown`.
- `gridd_lm_state_t gridd_lm_get_state(gridd_license_manager_t *lm)`: Thread-safe getter for current `gridd_lm_state_t`.
- `uint32_t gridd_lm_get_expiry_and_rm_state(gridd_license_manager_t *lm, uint32_t *out_expiry_epoch)`: Thread-safely maps LM state to `gridd_rm_license_state_t` and outputs lease expiry epoch.
- `uint32_t gridd_lm_get_lease_port(gridd_license_manager_t *lm)`: Thread-safely returns configured NLS lease port.
- `const char *gridd_lm_get_primary_url(gridd_license_manager_t *lm)`: Thread-safely returns primary NLS node URL (`node_urls[0]`).
- `const char *gridd_lm_get_secondary_url(gridd_license_manager_t *lm)`: Thread-safely returns secondary failover URL (`node_urls[1]`) or empty string.

---

## 14. `csp_platform.h` / `csp_platform.c`

Header: [`include/gridd/csp_platform.h`](../include/gridd/csp_platform.h) | Source: [`src/csp_platform.c`](../src/csp_platform.c)

### Enums & Structs
- `gridd_csp_platform_id_t`: `GRIDD_CSP_NONE = 0`, `GRIDD_CSP_AZURE = 1`, `GRIDD_CSP_AWS_EC2 = 2`, `GRIDD_CSP_GCP = 3`, `GRIDD_CSP_ALIBABA_ECS = 4`, `GRIDD_CSP_TENCENT = 5`, `GRIDD_CSP_TENCENT_IEG = 6`.
- `gridd_csp_info_t`: `platform_id`, `csp_id`, `name`, `requires_swcert`.

### Functions (2)
- `const gridd_csp_info_t *gridd_csp_find_by_platform_id(uint32_t platform_id)`: Looks up registry metadata for marketplace platform ID `1..6`.
- `bool gridd_csp_detect_and_verify(uint32_t platform_id, const char *swcert_path, const char *imds_base_url, uint32_t wait_to_connect_sec)`: Validates `GridSwCert.txt` when required and, for `GRIDD_CSP_AWS_EC2`, fetches an IMDSv2 token + signed instance identity document and verifies its signature.

---

## 15. `dbus_server.h` / `dbus_server.c`

Header: [`include/gridd/dbus_server.h`](../include/gridd/dbus_server.h) | Source: [`src/dbus_server.c`](../src/dbus_server.c)

### Constants & Structs
- `GRIDD_DBUS_BUS_NAME`: `"nvidia.grid.server"`
- `GRIDD_DBUS_INTERFACE`: `"nvidia.grid.license"`
- `GRIDD_DBUS_METHOD`: `"GridLicenseState"`
- `gridd_dbus_ctx_t`: `lm`, `active_feature_type`, `dbus_reload_pipe_wr`, `stop_pipe_rd`.

### Function (1)
- `void *gridd_dbus_thread_main(void *arg)`: Dynamically opens `libdbus-1.so.3`, resolves `dbus_*` symbols, connects to the system bus, requests `nvidia.grid.server`, runs a `poll()` event loop over D-Bus watch FDs and `stop_pipe_rd`, and serves `GridLicenseState` `INT32` (`1..4`) and `STRING` (`PrimaryServerAddress`, `SecondaryServerAddress`) requests.

---

## 16. `src/main.c`

Source: [`src/main.c`](../src/main.c)

- `int main(int argc, char **argv)`:
  - CLI options:
    - `-v`: Enable debug/verbose logging (`GRIDD_LOG_DEBUG`).
    - `-f`: Run in foreground mode (logs to `stderr` and skips daemon `fork()` / PID lock).
  - Orchestrates full initialization, CSP / NLL / NLS entitlement resolution, token polling (`10s`..`300s` exponential backoff), `select()` event loop over `exit_pipe_rd`, `lm.event_pipe_rd`, and `dbus_pipe_rd`, and clean shutdown with lease return.

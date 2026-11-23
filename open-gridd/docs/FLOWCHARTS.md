# `nvidia-gridd` Subsystem Flowcharts

This document contains detailed sequence diagrams and operational flowcharts for each major subsystem in the clean-room `nvidia-gridd` codebase.

## 1. End-to-End NLS Licensing Protocol Sequence

Covers JWT parsing, PKCE S256 generation, `/auth/v1/origin`, `/auth/v1/code`, `/auth/v1/token`, `/leasing/v1/lessor`, `/leasing/v1/lease/{ref}`, encrypted store updates, and clean shutdown via `DELETE` + `/leasing/v1/lessor/shutdown`.

```mermaid
sequenceDiagram
    autonumber
    participant Main as "main.c / lm_worker"
    participant Tok as "token.c"
    participant Env as "env_fingerprint.c"
    participant Store as "trusted_store.c"
    participant NLS as "nls_client.c"
    participant HTTP as "http_client.c (TLS)"
    participant Server as "NLS Server (auth_port / lease_port)"
    participant RM as "rm_client.c (/dev/nvidiactl)"

    Main->>Tok: "gridd_token_find_file() & gridd_token_parse_and_verify()"
    Tok-->>Main: "node_urls[], auth_port, lease_port, service_instance_ref, scope_ref_list"
    Main->>Env: "gridd_env_collect()"
    Env-->>Main: "MAC/IP, OS, host/guest driver, GPU PCI IDs, machine UUID"
    Main->>Store: "gridd_trusted_store_open(uuid, &store)"
    Store-->>Main: "last_seen_utc, origin_ref, fingerprint (or NO_CACHE)"
    Main->>Store: "gridd_trusted_store_check_clock(&store, now)"

    Main->>NLS: "gridd_nls_register_or_update_origin()"
    alt "origin_ref is empty (first boot)"
        NLS->>HTTP: "POST https://node:auth_port/auth/v1/origin (environment JSON)"
    else "origin_ref exists and fingerprint changed"
        NLS->>HTTP: "PUT https://node:auth_port/auth/v1/origin (origin_ref + environment)"
    end
    HTTP->>Server: "TLS 1.2+ HTTP/1.1"
    Server-->>HTTP: "200 OK { origin_ref } (or 409 conflict -> fallback PUT)"
    NLS->>Store: "gridd_trusted_store_save() (origin_ref + env hash)"

    Main->>NLS: "gridd_nls_authenticate()"
    Note over NLS: "Generate 32B random code_verifier & Base64URL SHA-256 code_challenge"
    NLS->>HTTP: "POST https://node:auth_port/auth/v1/code { origin_ref, code_challenge, S256 }"
    Server-->>HTTP: "200 OK { code }"
    NLS->>HTTP: "POST https://node:auth_port/auth/v1/token { code, code_verifier }"
    Server-->>HTTP: "200 OK { auth_token }"

    Main->>NLS: "gridd_nls_acquire_lease(product_name, product_count)"
    NLS->>NLS: "gridd_nls_build_lease_proposal_json()"
    NLS->>HTTP: "POST https://node:lease_port/leasing/v1/lessor (Bearer auth_token)"
    Server-->>HTTP: "200 OK { lease_ref, created, expires, recommended_renewal } (or 307/401/502)"
    NLS->>Store: "gridd_trusted_store_save() (lease_ref, created, expires, active_node_idx)"
    Main->>RM: "gridd_rm_enable_feature() & gridd_rm_set_license_status(LICENSED)"

    loop "Every recommended_renewal * duration"
        Main->>NLS: "gridd_nls_renew_lease()"
        NLS->>HTTP: "PUT https://node:lease_port/leasing/v1/lease/{lease_ref}"
        Server-->>HTTP: "200 OK { expires } (or 401 refresh / 404 re-acquire / 502 failover)"
        NLS->>Store: "gridd_trusted_store_save() (updated timestamps)"
    end

    Note over Main,Server: "On SIGINT / SIGTERM"
    Main->>NLS: "gridd_nls_return_leases() -> DELETE /leasing/v1/lease/{ref}"
    Main->>NLS: "gridd_nls_shutdown() -> POST /leasing/v1/lessor/shutdown"
    Main->>RM: "gridd_rm_set_license_status(UNLICENSED) & gridd_rm_disable_feature()"
```

## 2. Client Configuration Token (`.tok`) Discovery & JWT Verification

Executed by `gridd_token_find_file()` and `gridd_token_parse_and_verify()` in [`src/token.c`](../src/token.c):

```mermaid
flowchart TD
    Start(["gridd_token_find_file(token_dir)"]) --> ScanDir["opendir(token_dir) & scan regular files ending in .tok"]
    ScanDir --> Found{"First .tok file found?"}
    Found -- "No" --> ErrNoFile["Return GRIDD_ERR_FILE_READ (caller backs off 10s..300s)"]
    Found -- "Yes" --> ReadTok["Read entire .tok file (up to 64 KiB)"]
    ReadTok --> SplitJWT["gridd_jwt_split(): locate header.payload.signature"]
    SplitJWT --> CheckFormat{"Valid 3-part JWT?"}
    CheckFormat -- "No" --> ErrInvalid["Return GRIDD_ERR_INVALID_PARAM"]
    CheckFormat -- "Yes" --> DecodeHdr["Decode header JSON & inspect alg (ES256 / RS256)"]

    DecodeHdr --> VerifyKey["Verify signing input (header.payload) against pinned NLS root PEMs"]
    VerifyKey --> SigOK{"Signature valid?"}
    SigOK -- "No" --> ErrAuth["Return GRIDD_ERR_AUTH"]
    SigOK -- "Yes" --> DecodePay["Base64URL-decode payload JSON"]

    DecodePay --> CheckTime["Check nbf and exp claims against time(NULL)"]
    CheckTime --> TimeOK{"nbf <= now < exp?"}
    TimeOK -- "No" --> ErrExp["Return GRIDD_ERR_EXPIRED"]
    TimeOK -- "Yes" --> Extract["Extract service_instance_ref, jti, scope_ref_list, fulfillment_class_ref_list"]

    Extract --> ParseURLs["Parse service_instance_configuration.nls_service_instance_public_key / urls & ports"]
    ParseURLs --> DetectCLS{"Determine CLS vs DLS (url_qr / is_cls)"}
    DetectCLS --> Done(["Return GRIDD_OK with populated gridd_token_config_t"])
```

## 3. Encrypted Trusted Store & Anti-Tamper Check Flow

Implemented in [`src/trusted_store.c`](../src/trusted_store.c) using AES-256-GCM and HKDF-SHA256:

```mermaid
flowchart TD
    Start(["Trusted Store Open / Save"]) --> ReadUUID["gridd_env_read_machine_uuid(): /sys/class/dmi/id/product_uuid or /etc/machine-id"]
    ReadUUID --> DeriveKey["gridd_hkdf_sha256(ikm=uuid, salt='nvidia-gridd-trusted-store-salt-v1', info='aes-256-gcm-local-store-key') -> 32B key"]
    DeriveKey --> Op{"Operation?"}

    Op -- "Open" --> LockOpen["Lock g_store_mutex"]
    LockOpen --> ReadFiles["Read store_nls.dat (12B IV + ciphertext) & store_nls.tag (16B GCM tag)"]
    ReadFiles --> Exist{"Both files present & valid lengths?"}
    Exist -- "No" --> RetNoCache["Unlock & return GRIDD_ERR_NO_CACHE"]
    Exist -- "Yes" --> Decrypt["gridd_aes256_gcm_decrypt(key, iv, aad=NULL, ct, tag)"]
    Decrypt --> TagOK{"GCM tag verified?"}
    TagOK -- "No" --> RetAuth["Unlock & return GRIDD_ERR_AUTH (tampering detected)"]
    TagOK -- "Yes" --> ParseStore["Parse JSON fields: last_seen_utc, origin_ref, lease_ref, lease_expires_utc, env_fingerprint_hash, active_node_idx"]
    ParseStore --> ClockCheck["gridd_trusted_store_check_clock(): compare now vs last_seen_utc (grace tolerance)"]
    ClockCheck --> Windback{"Clock wound back?"}
    Windback -- "Yes" --> RetClock["Return GRIDD_ERR_CLOCK_WINDBACK"]
    Windback -- "No" --> RetOK["Unlock & return GRIDD_OK"]

    Op -- "Save" --> LockSave["Lock g_store_mutex"]
    LockSave --> BuildJSON["Serialize gridd_trusted_store_data_t to JSON"]
    BuildJSON --> GenIV["gridd_random_bytes(12B IV)"]
    GenIV --> Encrypt["gridd_aes256_gcm_encrypt() -> ciphertext + 16B tag"]
    Encrypt --> AtomicDat["Write store_nls.dat.tmp (IV || ct) -> rename() to store_nls.dat"]
    AtomicDat --> AtomicTag["Write store_nls.tag.tmp (16B tag) -> rename() to store_nls.tag"]
    AtomicTag --> UnlockSave["Unlock g_store_mutex & return GRIDD_OK"]
```

## 4. NVIDIA RM Device Ioctls & 30-Second Unlicensed Monitor

Implemented in [`src/rm_client.c`](../src/rm_client.c) using `/dev/nvidiactl` and mockable `gridd_rm_ops_t` vtable hooks:

```mermaid
flowchart TD
    Init(["gridd_rm_init()"]) --> OpenCtl["ops.open_fn(/dev/nvidiactl, O_RDWR)"]
    OpenCtl --> CheckVer["ioctl(NV_ESC_CHECK_VERSION_STR) & NV_ESC_SYS_PARAMS"]
    CheckVer --> AllocRoot["NV_ESC_RM_ALLOC (root client handle, class 0x0000)"]
    AllocRoot --> GetIDs["NV_ESC_RM_CONTROL(NV0000_CTRL_CMD_GPU_GET_ATTACHED_IDS -> NV0000_CTRL_CMD_GPU_ATTACH_IDS)"]
    GetIDs --> LoopGPUs["For each attached GPU (up to GRIDD_MAX_GPUS = 32)"]

    LoopGPUs --> AllocDev["NV_ESC_RM_ALLOC Device (0xdae00100 | i, class 0x0080)"]
    AllocDev --> AllocSub["NV_ESC_RM_ALLOC Subdevice (0xdae00200 | i, class 0x2080)"]
    AllocSub --> CheckVirt["NV0080_CTRL_CMD_GPU_GET_VIRTUALIZATION_MODE"]
    CheckVirt --> IsGuest{"mode == NV_VGPU_VIRTUALIZATION_MODE_GUEST (2)?"}
    IsGuest -- "Yes" --> AllocVGPU["NV_ESC_RM_ALLOC vGPU object (0xdae00300 | i, class 0xa080)"]
    IsGuest -- "No" --> QueryFeat["gridd_rm_query_features() & gridd_rm_query_pci_ids()"]
    AllocVGPU --> QueryFeat

    QueryFeat --> RuntimeOps["Runtime Controls Per License Transition"]
    RuntimeOps --> EnableF["NV2080_CTRL_CMD_GPU_ENABLE_LICENSED_FEATURE (0x20800186)"]
    RuntimeOps --> SetStatus["NV2080_CTRL_CMD_GPU_SET_LICENSE_STATUS (0x20800187: state, expiry, duration)"]
    RuntimeOps --> DisableF["NV2080_CTRL_CMD_GPU_DISABLE_LICENSED_FEATURE (0x20800164)"]

    Init --> SpawnMon["gridd_rm_start_unlicensed_monitor(stop_pipe_rd)"]
    SpawnMon --> MonLoop["select(stop_pipe_rd, timeout = 30s)"]
    MonLoop -- "Timeout" --> ReadProc["Check /proc/driver/nvidia & NV2080_CTRL_CMD_GPU_GET_UNLICENSED_STATE (0x2080018c)"]
    ReadProc --> LogState["Log unlicensed state changes if active"]
    LogState --> MonLoop
    MonLoop -- "stop_pipe_rd readable" --> ExitMon(["Detached monitor thread exits cleanly"])
```

## 5. CSP Marketplace Detection & AWS IMDSv2 Verification

Executed by `gridd_csp_detect_and_verify()` in [`src/csp_platform.c`](../src/csp_platform.c) when `marketplace_platform != 0`:

```mermaid
sequenceDiagram
    autonumber
    participant Main as "main.c"
    participant CSP as "csp_platform.c"
    participant FS as "/etc/nvidia/GridSwCert.txt"
    participant IMDS as "AWS IMDS (169.254.169.254 or override)"
    participant Crypto as "crypto.c (OpenSSL)"
    participant RM as "rm_client.c (/dev/nvidiactl)"

    Main->>CSP: "gridd_csp_detect_and_verify(platform_id, swcert_path, imds_base_url, wait_to_connect_sec)"
    CSP->>CSP: "gridd_csp_find_by_platform_id(platform_id)"

    alt "info->requires_swcert == true"
        CSP->>FS: "Read certificate file (<= 16384 bytes)"
        FS-->>CSP: "PEM / PKCS7 certificate text"
    end

    alt "platform_id == GRIDD_CSP_AWS_EC2 (2)"
        CSP->>IMDS: "PUT /latest/api/token (X-aws-ec2-metadata-token-ttl-seconds: 21600)"
        IMDS-->>CSP: "200 OK { imds_v2_token }"
        CSP->>IMDS: "GET /latest/dynamic/instance-identity/document (X-aws-ec2-metadata-token)"
        IMDS-->>CSP: "200 OK { document JSON with region }"
        CSP->>IMDS: "GET /latest/dynamic/instance-identity/signature (X-aws-ec2-metadata-token)"
        IMDS-->>CSP: "200 OK { signature }"
        CSP->>Crypto: "gridd_verify_pkcs7_pem() or gridd_verify_pem_signature()"
        Crypto-->>CSP: "true / false"
    end

    CSP-->>Main: "true (verified) / false"
    opt "verified == true"
        Main->>RM: "gridd_rm_enable_feature() & gridd_rm_set_license_status(GRIDD_LICENSE_STATE_LICENSED)"
    end
```

## 6. D-Bus IPC & Configuration Reload Flow

Implemented in [`src/dbus_server.c`](../src/dbus_server.c) via `dlopen("libdbus-1.so.3")` so the daemon links cleanly even on minimal Linux images without D-Bus dev headers:

```mermaid
sequenceDiagram
    autonumber
    participant UI as "nvidia-settings / D-Bus Client"
    participant LibDBus as "libdbus-1.so.3 (dlopen)"
    participant DBusSrv as "dbus_server.c (poll loop)"
    participant LM as "license_manager.c"
    participant Cfg as "config.c"
    participant Main as "main.c (select loop)"

    DBusSrv->>LibDBus: "dlopen('libdbus-1.so.3') & resolve 24 dbus_* symbols"
    DBusSrv->>LibDBus: "dbus_bus_get(DBUS_BUS_SYSTEM) & dbus_bus_request_name('nvidia.grid.server')"
    DBusSrv->>LibDBus: "Register watch callbacks & object path filter"

    loop "poll(watch FDs + stop_pipe_rd)"
        UI->>LibDBus: "Method call: nvidia.grid.license.GridLicenseState(arg)"
        LibDBus->>DBusSrv: "dispatch message handler"
        alt "arg is INT32: 1 (query license state)"
            DBusSrv->>LM: "gridd_lm_get_state(), gridd_lm_get_expiry_and_rm_state(), gridd_lm_get_lease_port()"
            DBusSrv-->>UI: "Reply: state, expiry, feature_type, lease_port"
        else "arg is INT32: 2 (reload gridd.conf)"
            DBusSrv->>Main: "write 1 byte to dbus_reload_pipe_wr"
            Main->>Cfg: "gridd_config_load(/etc/nvidia/gridd.conf)"
            DBusSrv-->>UI: "Reply: status OK"
        else "arg is INT32: 3 or 4 (disable / trigger UI action)"
            DBusSrv->>Cfg: "gridd_config_disable_ui() (when disabling)"
            DBusSrv-->>UI: "Reply: status"
        else "arg is STRING: PrimaryServerAddress / SecondaryServerAddress"
            DBusSrv->>LM: "gridd_lm_get_primary_url() / gridd_lm_get_secondary_url()"
            DBusSrv-->>UI: "Reply: server URL string"
        end
    end
```

## 7. Native HTTP/1.1, OpenSSL TLS 1.2+ & Proxy `CONNECT` Tunnel

Implemented without `libcurl` in [`src/http_client.c`](../src/http_client.c):

```mermaid
flowchart TD
    Req(["gridd_http_request(url, method, headers, body, opts, &resp)"]) --> ParseURL["Parse URL scheme (http / https), host, port, path"]
    ParseURL --> HasProxy{"opts->proxy_url or proxy_host configured?"}

    HasProxy -- "No" --> DirectDNS["getaddrinfo(target_host, target_port) & socket() with SO_RCVTIMEO/SO_SNDTIMEO"]
    DirectDNS --> IsHTTPS{"scheme == https?"}

    HasProxy -- "Yes" --> ProxyDNS["getaddrinfo(proxy_host, proxy_port) & connect() to proxy"]
    ProxyDNS --> NeedTunnel{"scheme == https?"}
    NeedTunnel -- "Yes" --> SendConnect["Send CONNECT target_host:target_port + Proxy-Authorization: Basic"]
    SendConnect --> TunnelOK{"HTTP/1.x 200?"}
    TunnelOK -- "No" --> FailIO["Close socket & return GRIDD_ERR_IO"]
    TunnelOK -- "Yes" --> TLSHandshake

    NeedTunnel -- "No" --> SendHTTPProxy["Send HTTP request using absolute-URI & Proxy-Authorization"]
    IsHTTPS -- "No" --> SendPlain["Send HTTP/1.1 request over plain TCP socket"]
    IsHTTPS -- "Yes" --> TLSHandshake["SSL_CTX_new(TLS_client_method()), min TLS 1.2, SNI SSL_set_tlsext_host_name()"]

    TLSHandshake --> VerifyPeer{"opts->tls_verify_peer?"}
    VerifyPeer -- "Yes" --> SetVerify["SSL_CTX_set_default_verify_paths() & SSL_VERIFY_PEER + X509 hostname check"]
    VerifyPeer -- "No" --> DoConnect["SSL_connect(ssl)"]
    SetVerify --> DoConnect
    DoConnect --> SendTLS["SSL_write(HTTP/1.1 headers + body)"]

    SendPlain --> ReadResp
    SendHTTPProxy --> ReadResp
    SendTLS --> ReadResp["Read status line & response headers (extract Content-Length / Transfer-Encoding / Location)"]

    ReadResp --> IsChunked{"Transfer-Encoding: chunked?"}
    IsChunked -- "Yes" --> DecodeChunks["Parse hex chunk lengths & reassemble payload"]
    IsChunked -- "No" --> ReadLen["Read Content-Length bytes or until EOF"]

    DecodeChunks --> Cleanup(["Free SSL / close socket, return GRIDD_OK with status_code & body"])
    ReadLen --> Cleanup
```

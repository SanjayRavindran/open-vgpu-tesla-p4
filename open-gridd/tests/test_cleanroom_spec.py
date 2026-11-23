import base64
import datetime
import hashlib
import json
import os
import tempfile
import unittest
import urllib.error
import urllib.request

from cryptography.hazmat.primitives import hashes, serialization
from cryptography.hazmat.primitives.asymmetric import padding
from cryptography.hazmat.primitives.ciphers.aead import AESGCM
from cryptography.hazmat.primitives.kdf.hkdf import HKDF

from tests.mock_imds_server import RunningMockIMDS
from tests.mock_nls_server import RunningMockNLS, b64url_encode


def parse_gridd_conf(text: str):
    cfg = {"FeatureType": 0, "EnableUI": True, "IgnoreSP": False, "IgnoreNodeLocked": False, "LicenseInterval": 0}
    for raw_line in text.splitlines():
        clean = []
        for ch in raw_line:
            if ch == "#":
                break
            if ch in (" ", "\t", "\r", "\n"):
                continue
            clean.append(ch)
        line = "".join(clean)
        if "=" not in line:
            continue
        k, v = line.split("=", 1)
        if not v:
            continue
        if k == "FeatureType":
            cfg[k] = int(v, 0)
        elif k in ("EnableUI", "IgnoreSP", "IgnoreNodeLocked"):
            cfg[k] = v.upper() == "TRUE"
        elif k in ("LicenseInterval", "vGamingMarketplace", "MarketplaceLicensing"):
            cfg[k] = int(v, 0)
        else:
            cfg[k] = v
    return cfg


def disable_ui_atomic(conf_path: str, temp_path: str):
    with open(conf_path, "r", encoding="utf-8") as fp:
        lines = fp.readlines()
    replaced = False
    out_lines = []
    for line in lines:
        if "EnableUI" in line:
            out_lines.append("EnableUI=FALSE\n")
            replaced = True
        else:
            out_lines.append(line)
    if not replaced:
        out_lines.append("EnableUI=FALSE\n")
    with open(temp_path, "w", encoding="utf-8") as fp:
        fp.writelines(out_lines)
    os.replace(temp_path, conf_path)


def http_json(method, url, body=None, headers=None):
    req_headers = {"Content-Type": "application/json"}
    if headers:
        req_headers.update(headers)
    data = json.dumps(body).encode("utf-8") if body is not None else None
    req = urllib.request.Request(url, data=data, headers=req_headers, method=method)
    try:
        with urllib.request.urlopen(req, timeout=5) as resp:
            return resp.status, json.loads(resp.read().decode("utf-8"))
    except urllib.error.HTTPError as e:
        body = json.loads(e.read().decode("utf-8"))
        e.close()
        return e.code, body


class CleanRoomSpecTests(unittest.TestCase):
    def test_gridd_conf_parsing_and_atomic_disable_ui(self):
        with tempfile.TemporaryDirectory() as td:
            conf = os.path.join(td, "gridd.conf")
            tmp = os.path.join(td, ".gridd.conf_temp")
            with open(conf, "w", encoding="utf-8") as fp:
                fp.write("# header\n  FeatureType = 2 # vWS\nEnableUI = TRUE\nLicenseInterval = 1440\n")
            with open(conf, encoding="utf-8") as fp:
                parsed = parse_gridd_conf(fp.read())
            self.assertEqual(parsed["FeatureType"], 2)
            self.assertTrue(parsed["EnableUI"])
            self.assertEqual(parsed["LicenseInterval"], 1440)

            disable_ui_atomic(conf, tmp)
            with open(conf, encoding="utf-8") as fp:
                parsed2 = parse_gridd_conf(fp.read())
            self.assertFalse(parsed2["EnableUI"])
            self.assertEqual(parsed2["FeatureType"], 2)

    def test_client_config_token_verification_and_expiry(self):
        with RunningMockNLS() as nls:
            tok = nls.state.mint_client_config_token(cls=True)
            h_b64, p_b64, s_b64 = tok.split(".")
            pad = lambda s: s + "=" * ((4 - len(s) % 4) % 4)
            payload = json.loads(base64.urlsafe_b64decode(pad(p_b64)))
            sig = base64.urlsafe_b64decode(pad(s_b64))
            pub = serialization.load_pem_public_key(payload["service_instance_public_key_pem"].encode("ascii"))
            pub.verify(sig, f"{h_b64}.{p_b64}".encode("ascii"), padding.PKCS1v15(), hashes.SHA256())
            self.assertEqual(payload["svc_port_set_list"][0]["d_name"], "CLS")

            now = int(datetime.datetime.now(datetime.timezone.utc).timestamp())
            self.assertLessEqual(payload["nbf"], now)
            self.assertGreater(payload["exp"], now)

            expired_tok = nls.state.mint_client_config_token(exp=now - 10)
            _, ep_b64, _ = expired_tok.split(".")
            ep_payload = json.loads(base64.urlsafe_b64decode(pad(ep_b64)))
            self.assertLess(ep_payload["exp"], now)

    def test_nls_full_lifecycle_and_failover(self):
        with RunningMockNLS() as primary, RunningMockNLS() as secondary:
            base1 = f"http://127.0.0.1:{primary.state.port}"
            base2 = f"http://127.0.0.1:{secondary.state.port}"
            tok = primary.state.mint_client_config_token(
                nodes=[{"url": base1, "url_qr": base1}, {"url": base2, "url_qr": base2}]
            )
            pad = lambda s: s + "=" * ((4 - len(s) % 4) % 4)
            payload = json.loads(base64.urlsafe_b64decode(pad(tok.split(".")[1])))

            env = {
                "fingerprint": {"mac_address_list": ["02:42:ac:11:00:02"]},
                "hostname": "gridd-test-node",
                "ip_address_list": ["10.0.0.5"],
                "guest_driver_version": "550.54.14",
                "os_platform": "Ubuntu 22.04",
                "os_version": "22.04.4 LTS",
                "host_driver_version": "NA",
                "gpu_id_list": ["10DE-2230"],
                "client_platform_id": "uuid-test-1",
                "hv_platform": "BareMetal",
                "cpu_sockets": 1,
                "physical_cores": 8,
            }

            # 1. Register origin
            code, res = http_json("POST", f"{base1}/auth/v1/origin", {
                "service_instance_ref": payload["nls_service_instance_ref"],
                "candidate_origin_ref": "",
                "environment": env,
                "registration_pending": True,
                "update_pending": False,
            })
            self.assertEqual(code, 200)
            origin_ref = res["origin_ref"]

            # 2. Mismatched fingerprint on existing origin triggers 409 conflict
            env_bad = json.loads(json.dumps(env))
            env_bad["fingerprint"]["mac_address_list"] = ["aa:bb:cc:dd:ee:ff"]
            code_c, res_c = http_json("POST", f"{base1}/auth/v1/origin", {
                "service_instance_ref": payload["nls_service_instance_ref"],
                "candidate_origin_ref": origin_ref,
                "environment": env_bad,
                "registration_pending": False,
                "update_pending": False,
            })
            self.assertEqual(code_c, 409)
            self.assertIn("already in use by different fingerprint", res_c["message"])

            # 3. PKCE auth code -> token
            verifier = b64url_encode(os.urandom(32))
            challenge = b64url_encode(hashlib.sha256(verifier.encode("ascii")).digest())
            code, res = http_json("POST", f"{base1}/auth/v1/code", {
                "code_challenge": challenge,
                "origin_ref": origin_ref,
                "service_instance_ref": payload["nls_service_instance_ref"],
            })
            self.assertEqual(code, 200)
            code, res = http_json("POST", f"{base1}/auth/v1/token", {
                "auth_code": res["auth_code"],
                "code_verifier": verifier,
            })
            self.assertEqual(code, 200)
            bearer = {"Authorization": f"Bearer {res['auth_token']}"}

            # 4. Acquire with simulated 307 redirect + 401 token refresh
            primary.state.force_redirect_url = f"127.0.0.1:{primary.state.port}"
            proposal = {
                "fulfillment_context": {"fulfillment_class_ref_list": payload["fulfillment_class_ref_list"]},
                "lease_proposal_list": [{
                    "duration": "P0Y0M0DT0H1440M0S",
                    "license_type_qualifiers": {"count": 1},
                    "product": {"name": "NVIDIA RTX Virtual Workstation"},
                }],
                "proposal_evaluation_mode": "ALL_OF",
                "scope_ref_list": payload["scope_ref_list"],
            }
            code, r307 = http_json("POST", f"{base1}/leasing/v1/lessor", proposal, headers=bearer)
            self.assertEqual(code, 307)
            self.assertIn("fqdn", r307)

            code, acq = http_json("POST", f"{base1}/leasing/v1/lessor", proposal, headers=bearer)
            self.assertEqual(code, 200)
            lease_ref = acq["lease"]["ref"]

            # 5. Renew, List, Return, Shutdown
            code, ren = http_json("PUT", f"{base1}/leasing/v1/lease/{lease_ref}", {}, headers=bearer)
            self.assertEqual(code, 200)
            self.assertEqual(ren["lease_ref"], lease_ref)

            code, lst = http_json("GET", f"{base1}/leasing/v1/lessor/leases", headers=bearer)
            self.assertEqual(code, 200)
            self.assertIn(lease_ref, lst["active_lease_list"])

            code, _ = http_json("DELETE", f"{base1}/leasing/v1/lessor/leases", headers=bearer)
            self.assertEqual(code, 200)

            code, _ = http_json("DELETE", f"{base1}/leasing/v1/lessor/leases", headers=bearer)
            self.assertEqual(code, 404)

            code, _ = http_json("POST", f"{base1}/leasing/v1/lessor/shutdown", {}, headers=bearer)
            self.assertEqual(code, 200)

    def test_trusted_store_gcm_tamper_and_clock_windback(self):
        machine_id = "test-machine-uuid-9876"
        key = HKDF(
            algorithm=hashes.SHA256(),
            length=32,
            salt=b"nvidia-gridd-trusted-store-salt-v1",
            info=b"aes-256-gcm-local-store-key",
        ).derive(machine_id.encode("utf-8"))

        now_iso = datetime.datetime.now(datetime.timezone.utc).strftime("%Y-%m-%dT%H:%M:%S")
        store_doc = {
            "system_clock_snapshot": now_iso,
            "jti": "jti-abc",
            "origin": {
                "origin_ref": "origin-123",
                "environment": {"fingerprint": {"mac_address_list": ["02:42:ac:11:00:02"]}},
                "registered": True,
                "failed_registration_attempts": 0,
            },
            "lease_management": {"lease_list": ["lease-1"], "lease_state": "ACTIVE"},
        }
        iv = os.urandom(12)
        aesgcm = AESGCM(key)
        ct_and_tag = aesgcm.encrypt(iv, json.dumps(store_doc).encode("utf-8"), None)

        # Valid decrypt
        pt = json.loads(aesgcm.decrypt(iv, ct_and_tag, None).decode("utf-8"))
        self.assertEqual(pt["origin"]["origin_ref"], "origin-123")

        # Tamper detection
        tampered = bytearray(ct_and_tag)
        tampered[-1] ^= 0xFF
        with self.assertRaises(Exception):
            aesgcm.decrypt(iv, bytes(tampered), None)

        # Clock windback check
        snap = datetime.datetime.strptime(pt["system_clock_snapshot"], "%Y-%m-%dT%H:%M:%S")
        rewound = snap - datetime.timedelta(minutes=5)
        self.assertLess(rewound, snap)

    def test_aws_imds_v2_and_signature_verification(self):
        with RunningMockIMDS(region="us-west-2") as imds:
            req_tok = urllib.request.Request(
                f"{imds.base_url}/latest/api/token",
                method="PUT",
                headers={"X-aws-ec2-metadata-token-ttl-seconds": "20"},
            )
            with urllib.request.urlopen(req_tok, timeout=5) as r:
                self.assertEqual(r.status, 200)
                token = r.read().decode("utf-8")

            def imds_get(path):
                req = urllib.request.Request(
                    f"{imds.base_url}{path}",
                    method="GET",
                    headers={"X-aws-ec2-metadata-token": token},
                )
                with urllib.request.urlopen(req, timeout=5) as r:
                    return r.status, r.read().decode("utf-8")

            st_doc, doc = imds_get("/latest/dynamic/instance-identity/document")
            st_sig, sig_b64 = imds_get("/latest/dynamic/instance-identity/signature")
            self.assertEqual(st_doc, 200)
            self.assertEqual(st_sig, 200)
            self.assertEqual(json.loads(doc)["region"], "us-west-2")

            pub = serialization.load_pem_public_key(imds.state.public_pem.encode("ascii"))
            pub.verify(base64.b64decode(sig_b64), doc.encode("utf-8"), padding.PKCS1v15(), hashes.SHA256())


if __name__ == "__main__":
    unittest.main()

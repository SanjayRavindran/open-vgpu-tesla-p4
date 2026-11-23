import base64
import datetime
import hashlib
import http.server
import json
import threading
import uuid
from cryptography.hazmat.primitives import hashes, serialization
from cryptography.hazmat.primitives.asymmetric import padding, rsa


def b64url_encode(data: bytes) -> str:
    return base64.urlsafe_b64encode(data).rstrip(b"=").decode("ascii")


class MockNLSState:
    def __init__(self, host="127.0.0.1", port=0):
        self.private_key = rsa.generate_private_key(public_exponent=65537, key_size=2048)
        self.public_pem = self.private_key.public_key().public_bytes(
            serialization.Encoding.PEM,
            serialization.PublicFormat.SubjectPublicKeyInfo,
        ).decode("ascii")
        self.host = host
        self.port = port
        self.service_instance_ref = str(uuid.uuid4())
        self.scope_ref = str(uuid.uuid4())
        self.fulfillment_class_ref = str(uuid.uuid4())
        self.origins = {}       # origin_ref -> environment
        self.auth_codes = {}    # auth_code -> (code_challenge, origin_ref)
        self.auth_tokens = {}   # token -> origin_ref
        self.leases = {}        # lease_ref -> lease dict
        self.force_redirect_url = None
        self.force_401_once = False
        self.requests_seen = []

    def mint_client_config_token(self, jti=None, nbf=None, exp=None, cls=True, nodes=None):
        now = int(datetime.datetime.now(datetime.timezone.utc).timestamp())
        jti = jti or str(uuid.uuid4())
        nbf = now - 60 if nbf is None else nbf
        exp = now + 86400 if exp is None else exp
        if nodes is None:
            nodes = [{"url": f"http://{self.host}:{self.port}", "url_qr": f"http://{self.host}:{self.port}"}]
        header = {"alg": "RS256", "typ": "JWT"}
        payload = {
            "jti": jti,
            "nbf": nbf,
            "exp": exp,
            "nls_service_instance_ref": self.service_instance_ref,
            "service_instance_public_key_pem": self.public_pem,
            "scope_ref_list": [self.scope_ref],
            "fulfillment_class_ref_list": [self.fulfillment_class_ref],
            "svc_port_set_list": [
                {
                    "d_name": "CLS" if cls else "DLS",
                    "svc_port_set": [
                        {"svc_name": "auth", "port": self.port},
                        {"svc_name": "lease", "port": self.port},
                    ],
                }
            ],
            "node_url_list": nodes,
        }
        h_b64 = b64url_encode(json.dumps(header, separators=(",", ":")).encode("utf-8"))
        p_b64 = b64url_encode(json.dumps(payload, separators=(",", ":")).encode("utf-8"))
        signing_input = f"{h_b64}.{p_b64}".encode("ascii")
        sig = self.private_key.sign(signing_input, padding.PKCS1v15(), hashes.SHA256())
        return f"{h_b64}.{p_b64}.{b64url_encode(sig)}"


def make_handler(state: MockNLSState):
    class Handler(http.server.BaseHTTPRequestHandler):
        def log_message(self, format, *args):
            pass

        def _read_json(self):
            length = int(self.headers.get("Content-Length", "0"))
            raw = self.rfile.read(length) if length > 0 else b""
            return json.loads(raw.decode("utf-8")) if raw else {}

        def _send_json(self, code, obj):
            body = json.dumps(obj).encode("utf-8")
            self.send_response(code)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)

        def _check_bearer(self):
            if state.force_401_once:
                state.force_401_once = False
                self._send_json(401, {"message": "token expired"})
                return None
            auth = self.headers.get("Authorization", "")
            if not auth.startswith("Bearer "):
                self._send_json(401, {"message": "missing bearer token"})
                return None
            tok = auth[7:].strip()
            if tok not in state.auth_tokens:
                self._send_json(401, {"message": "invalid bearer token"})
                return None
            return state.auth_tokens[tok]

        def do_POST(self):
            state.requests_seen.append(("POST", self.path))
            data = self._read_json()

            if self.path == "/auth/v1/origin":
                cand = data.get("candidate_origin_ref") or ""
                env = data.get("environment", {})
                if cand and cand in state.origins:
                    prev_macs = state.origins[cand].get("fingerprint", {}).get("mac_address_list", [])
                    cur_macs = env.get("fingerprint", {}).get("mac_address_list", [])
                    if prev_macs != cur_macs:
                        self._send_json(409, {"message": f"origin reference {cand} already in use by different fingerprint"})
                        return
                origin_ref = cand if cand else str(uuid.uuid4())
                state.origins[origin_ref] = env
                self._send_json(200, {"origin_ref": origin_ref})
                return

            if self.path == "/auth/v1/origin/update":
                if self._check_bearer() is None:
                    return
                oref = data.get("origin_ref")
                state.origins[oref] = data.get("environment", {})
                self._send_json(200, {"origin_ref": oref})
                return

            if self.path == "/auth/v1/code":
                code = "code-" + uuid.uuid4().hex[:12]
                state.auth_codes[code] = (data["code_challenge"], data["origin_ref"])
                self._send_json(200, {"auth_code": code})
                return

            if self.path == "/auth/v1/token":
                code = data.get("auth_code")
                verifier = data.get("code_verifier", "")
                if code not in state.auth_codes:
                    self._send_json(400, {"message": "invalid auth code"})
                    return
                expected_challenge, origin_ref = state.auth_codes.pop(code)
                actual_challenge = b64url_encode(hashlib.sha256(verifier.encode("ascii")).digest())
                if actual_challenge != expected_challenge:
                    self._send_json(400, {"message": "pkce verification failed"})
                    return
                tok = "jwt-" + uuid.uuid4().hex
                state.auth_tokens[tok] = origin_ref
                exp = (datetime.datetime.now(datetime.timezone.utc) + datetime.timedelta(hours=1)).strftime("%Y-%m-%dT%H:%M:%S")
                self._send_json(200, {"auth_token": tok, "expires": exp})
                return

            if self.path == "/leasing/v1/lessor":
                if self._check_bearer() is None:
                    return
                if state.force_redirect_url:
                    fqdn = state.force_redirect_url
                    state.force_redirect_url = None
                    self._send_json(307, {"ipv4": "null", "ipv6": "null", "fqdn": fqdn})
                    return
                proposal = data["lease_proposal_list"][0]
                prod_name = proposal["product"]["name"]
                cnt = proposal["license_type_qualifiers"]["count"]
                lease_ref = str(uuid.uuid4())
                exp = (datetime.datetime.now(datetime.timezone.utc) + datetime.timedelta(hours=24)).strftime("%Y-%m-%dT%H:%M:%S")
                lease = {
                    "ref": lease_ref,
                    "product": {"name": prod_name},
                    "license_type_qualifiers": {"count": cnt},
                    "expires": exp,
                    "recommended_lease_renewal": 0.15,
                    "lease_state": "ACTIVE",
                    "feature_expired": False,
                }
                state.leases[lease_ref] = lease
                self._send_json(200, {"lease": lease})
                return

            if self.path == "/leasing/v1/lessor/shutdown":
                if self._check_bearer() is None:
                    return
                self._send_json(200, {"status": "ok"})
                return

            self._send_json(404, {"message": "not found"})

        def do_PUT(self):
            state.requests_seen.append(("PUT", self.path))
            if self._check_bearer() is None:
                return
            if self.path.startswith("/leasing/v1/lease/"):
                lref = self.path.split("/")[-1]
                if lref not in state.leases:
                    self._send_json(404, {"message": "no current lease found"})
                    return
                exp = (datetime.datetime.now(datetime.timezone.utc) + datetime.timedelta(hours=24)).strftime("%Y-%m-%dT%H:%M:%S")
                lease = state.leases[lref]
                lease["expires"] = exp
                self._send_json(200, {
                    "lease_ref": lref,
                    "product": lease["product"],
                    "license_type_qualifiers": lease["license_type_qualifiers"],
                    "expires": exp,
                    "recommended_lease_renewal": 0.15,
                    "lease_state": "ACTIVE",
                    "feature_expired": False,
                })
                return
            self._send_json(404, {"message": "not found"})

        def do_GET(self):
            state.requests_seen.append(("GET", self.path))
            if self._check_bearer() is None:
                return
            if self.path == "/leasing/v1/lessor/leases":
                self._send_json(200, {"active_lease_list": list(state.leases.keys())})
                return
            self._send_json(404, {"message": "not found"})

        def do_DELETE(self):
            state.requests_seen.append(("DELETE", self.path))
            if self._check_bearer() is None:
                return
            if self.path == "/leasing/v1/lessor/leases":
                if not state.leases:
                    self._send_json(404, {"message": "lease is already released"})
                    return
                state.leases.clear()
                self._send_json(200, {"released": True})
                return
            self._send_json(404, {"message": "not found"})

    return Handler


class RunningMockNLS:
    def __init__(self):
        self.state = MockNLSState()
        self.server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), make_handler(self.state))
        self.state.port = self.server.server_address[1]
        self.thread = threading.Thread(target=self.server.serve_forever, daemon=True)

    def __enter__(self):
        self.thread.start()
        return self

    def __exit__(self, *args):
        self.server.shutdown()
        self.server.server_close()

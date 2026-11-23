import base64
import http.server
import json
import threading
from cryptography.hazmat.primitives import hashes, serialization
from cryptography.hazmat.primitives.asymmetric import padding, rsa


class MockIMDSState:
    def __init__(self, region="us-east-1"):
        self.private_key = rsa.generate_private_key(public_exponent=65537, key_size=2048)
        self.public_pem = self.private_key.public_key().public_bytes(
            serialization.Encoding.PEM,
            serialization.PublicFormat.SubjectPublicKeyInfo,
        ).decode("ascii")
        self.token = "imds-v2-session-token"
        self.document = json.dumps({
            "instanceId": "i-0123456789abcdef0",
            "instanceType": "g5.xlarge",
            "region": region,
            "availabilityZone": f"{region}a",
        })
        sig = self.private_key.sign(self.document.encode("utf-8"), padding.PKCS1v15(), hashes.SHA256())
        self.signature_b64 = base64.b64encode(sig).decode("ascii")


def make_imds_handler(state: MockIMDSState):
    class Handler(http.server.BaseHTTPRequestHandler):
        def log_message(self, format, *args):
            pass

        def _reply(self, code, body_str):
            b = body_str.encode("utf-8")
            self.send_response(code)
            self.send_header("Content-Type", "text/plain")
            self.send_header("Content-Length", str(len(b)))
            self.end_headers()
            self.wfile.write(b)

        def do_PUT(self):
            if self.path == "/latest/api/token":
                ttl = self.headers.get("X-aws-ec2-metadata-token-ttl-seconds")
                if not ttl:
                    self._reply(400, "missing ttl")
                    return
                self._reply(200, state.token)
                return
            self._reply(404, "not found")

        def do_GET(self):
            tok = self.headers.get("X-aws-ec2-metadata-token")
            if tok != state.token:
                self._reply(401, "unauthorized")
                return
            if self.path == "/latest/dynamic/instance-identity/document":
                self._reply(200, state.document)
                return
            if self.path == "/latest/dynamic/instance-identity/signature":
                self._reply(200, state.signature_b64)
                return
            self._reply(404, "not found")

    return Handler


class RunningMockIMDS:
    def __init__(self, region="us-west-2"):
        self.state = MockIMDSState(region=region)
        self.server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), make_imds_handler(self.state))
        self.port = self.server.server_address[1]
        self.base_url = f"http://127.0.0.1:{self.port}"
        self.thread = threading.Thread(target=self.server.serve_forever, daemon=True)

    def __enter__(self):
        self.thread.start()
        return self

    def __exit__(self, *args):
        self.server.shutdown()
        self.server.server_close()

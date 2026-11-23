import os
import subprocess
import sys
import tempfile

sys.path.insert(0, os.path.abspath(os.path.join(os.path.dirname(__file__), "..")))

from tests.mock_imds_server import RunningMockIMDS
from tests.mock_nls_server import RunningMockNLS


def main():
    if len(sys.argv) < 2:
        print("Usage: python3 tests/run_c_e2e.py <path-to-c_e2e_tests>", file=sys.stderr)
        sys.exit(2)

    exe = sys.argv[1]
    with RunningMockNLS() as nls, RunningMockIMDS(region="us-east-1") as imds, tempfile.TemporaryDirectory() as td:
        tok_path = os.path.join(td, "client_configuration_token.tok")
        store_dir = os.path.join(td, "store")
        imds_pub = os.path.join(td, "GridSwCert.txt")
        os.makedirs(store_dir, exist_ok=True)

        with open(tok_path, "w", encoding="utf-8") as fp:
            fp.write(nls.state.mint_client_config_token(cls=True))
        with open(imds_pub, "w", encoding="utf-8") as fp:
            fp.write(imds.state.public_pem)

        cmd = [
            exe,
            "--nls-tok", tok_path,
            "--store-dir", store_dir,
            "--imds-url", imds.base_url,
            "--imds-pub", imds_pub,
        ]
        res = subprocess.run(cmd, check=False, text=True, capture_output=True)
        print(res.stdout, end="")
        if res.stderr:
            print(res.stderr, end="", file=sys.stderr)
        if res.returncode != 0:
            sys.exit(res.returncode)

        seen_paths = [p for _, p in nls.state.requests_seen]
        for expected in (
            "/auth/v1/origin",
            "/auth/v1/code",
            "/auth/v1/token",
            "/leasing/v1/lessor",
            "/leasing/v1/lessor/leases",
            "/leasing/v1/lessor/shutdown",
        ):
            assert expected in seen_paths, f"Missing expected NLS request {expected} in {seen_paths}"
        print(f"Verified {len(seen_paths)} live C->MockNLS HTTP requests.")


if __name__ == "__main__":
    main()

#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
COMPILER="gcc"
BUILD_TYPE="Release"
FORMAT="all"
VERSION="1.0.0"
BUILD_DIR="$ROOT/build-pkg"
OUT_DIR="$ROOT/dist"
SKIP_TESTS=0
CLEAN=0

usage() {
    cat <<EOF
Usage: $(basename "$0") [options]

Build, test, and package nvidia-gridd (clean-room C11 implementation).

Options:
  --compiler gcc|clang       C compiler to use (default: gcc)
  --build-type Release|Debug CMake build type (default: Release)
  --format deb|tgz|rpm|all   Package format(s) to generate (default: all)
  --version X.Y.Z            Package version string (default: 1.0.0)
  --build-dir DIR            CMake build directory (default: <repo>/build-pkg)
  --out-dir DIR              Package output directory (default: <repo>/dist)
  --skip-tests               Skip CTest, E2E, and unittest verification
  --clean                    Remove --build-dir before configuring
  -h, --help                 Show this help message
EOF
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --compiler)
            COMPILER="${2:?Missing value for --compiler}"
            shift 2
            ;;
        --build-type)
            BUILD_TYPE="${2:?Missing value for --build-type}"
            shift 2
            ;;
        --format)
            FORMAT="${2:?Missing value for --format}"
            shift 2
            ;;
        --version)
            VERSION="${2:?Missing value for --version}"
            shift 2
            ;;
        --build-dir)
            BUILD_DIR="${2:?Missing value for --build-dir}"
            shift 2
            ;;
        --out-dir)
            OUT_DIR="${2:?Missing value for --out-dir}"
            shift 2
            ;;
        --skip-tests)
            SKIP_TESTS=1
            shift
            ;;
        --clean)
            CLEAN=1
            shift
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        *)
            echo "Unknown option: $1" >&2
            usage >&2
            exit 2
            ;;
    esac
done

case "$COMPILER" in
    gcc|clang) ;;
    *)
        echo "Unsupported --compiler '$COMPILER' (expected gcc or clang)" >&2
        exit 2
        ;;
esac

case "$BUILD_TYPE" in
    Release|Debug) ;;
    *)
        echo "Unsupported --build-type '$BUILD_TYPE' (expected Release or Debug)" >&2
        exit 2
        ;;
esac

case "$FORMAT" in
    deb|tgz|rpm|all) ;;
    *)
        echo "Unsupported --format '$FORMAT' (expected deb, tgz, rpm, or all)" >&2
        exit 2
        ;;
esac

if [[ $CLEAN -eq 1 && -d "$BUILD_DIR" ]]; then
    rm -rf "$BUILD_DIR"
fi

mkdir -p "$OUT_DIR"

GENERATOR_ARGS=()
if command -v ninja >/dev/null 2>&1; then
    GENERATOR_ARGS=(-G Ninja)
fi

echo "==> Configuring CMake (compiler=$COMPILER, type=$BUILD_TYPE, version=$VERSION)"
CC="$COMPILER" cmake -S "$ROOT" -B "$BUILD_DIR" \
    "${GENERATOR_ARGS[@]}" \
    -DCMAKE_BUILD_TYPE="$BUILD_TYPE" \
    -DGRIDD_PACKAGE_VERSION="$VERSION"

echo "==> Building targets"
cmake --build "$BUILD_DIR" -j"$(nproc 2>/dev/null || echo 4)"

if [[ $SKIP_TESTS -eq 0 ]]; then
    echo "==> Running CTest unit tests"
    ctest --test-dir "$BUILD_DIR" --output-on-failure

    if command -v python3 >/dev/null 2>&1; then
        if python3 -B -c "import cryptography" >/dev/null 2>&1; then
            echo "==> Running live C E2E harness"
            python3 -B "$ROOT/tests/run_c_e2e.py" "$BUILD_DIR/c_e2e_tests"
        else
            echo "==> Skipping E2E harness (python3-cryptography not installed)"
        fi

        echo "==> Running Python conformance & static audit tests"
        python3 -B -m unittest discover -s "$ROOT/tests" -v
    else
        echo "==> Skipping Python tests (python3 not found)"
    fi
fi

GENERATORS=()
case "$FORMAT" in
    deb)
        command -v dpkg-deb >/dev/null 2>&1 || { echo "dpkg-deb is required for --format deb" >&2; exit 1; }
        GENERATORS+=(DEB)
        ;;
    tgz)
        GENERATORS+=(TGZ)
        ;;
    rpm)
        command -v rpmbuild >/dev/null 2>&1 || { echo "rpmbuild is required for --format rpm" >&2; exit 1; }
        GENERATORS+=(RPM)
        ;;
    all)
        if command -v dpkg-deb >/dev/null 2>&1; then
            GENERATORS+=(DEB)
        fi
        GENERATORS+=(TGZ)
        if command -v rpmbuild >/dev/null 2>&1; then
            GENERATORS+=(RPM)
        fi
        ;;
esac

for gen in "${GENERATORS[@]}"; do
    echo "==> Packaging with CPack generator: $gen"
    cpack --config "$BUILD_DIR/CPackConfig.cmake" -G "$gen" -B "$OUT_DIR"
done

(
    cd "$OUT_DIR"
    rm -f SHA256SUMS
    ARTIFACTS=()
    shopt -s nullglob
    for f in *.deb *.rpm *.tar.gz; do
        ARTIFACTS+=("$f")
    done
    shopt -u nullglob

    if [[ ${#ARTIFACTS[@]} -eq 0 ]]; then
        echo "No packages produced in $OUT_DIR" >&2
        exit 1
    fi

    sha256sum "${ARTIFACTS[@]}" > SHA256SUMS
    echo "==> Generated packages & checksums in $OUT_DIR:"
    cat SHA256SUMS
)

for deb_pkg in "$OUT_DIR"/*.deb; do
    [[ -e "$deb_pkg" ]] || continue
    echo "==> Verifying DEB metadata: $deb_pkg"
    dpkg-deb -I "$deb_pkg"
    echo "==> Verifying DEB contents: $deb_pkg"
    dpkg-deb -c "$deb_pkg"
done

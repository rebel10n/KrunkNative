#!/usr/bin/env bash
# Builds the pinned OpenSSL as static libraries for the macOS client packages
# (.github/workflows/client.yml caches <prefix> by version + arch): Homebrew's dylibs are built for
# the runner's macOS version, a static libssl/libcrypto for MACOSX_DEPLOYMENT_TARGET runs on all
# of them. The client links it with -DOPENSSL_ROOT_DIR=<prefix> -DOPENSSL_USE_STATIC_LIBS=TRUE
# -DKRUNKNATIVE_TLS_STATIC=ON (backend_native.c then ignores openssl.cnf and probes the CA bundle:
# /etc/ssl/cert.pem on macOS).
#
# Usage: tools/release/build-openssl.sh <prefix>
# Env: MACOSX_DEPLOYMENT_TARGET (macOS; default 11.0), CC
set -euo pipefail

[ $# -eq 1 ] || { sed -n '2,10p' "$0" >&2; exit 2; }
prefix="$1"
OPENSSL_VERSION=3.5.9 # LTS (to 2030-04)
OPENSSL_SHA256=603f5602e2eef00d77fbd429d34dcd5822bb301757a1bc9cdb24c670f1eb859a

if [ -f "$prefix/lib/libssl.a" ] && grep -qs "^$OPENSSL_VERSION\$" "$prefix/VERSION"; then
    echo "build-openssl.sh: OpenSSL $OPENSSL_VERSION already in $prefix" >&2
    exit 0
fi
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT
tarball="openssl-$OPENSSL_VERSION.tar.gz"
curl -fsSL --retry 3 -o "$work/$tarball" "https://github.com/openssl/openssl/releases/download/openssl-$OPENSSL_VERSION/$tarball"
if command -v sha256sum > /dev/null; then got="$(sha256sum "$work/$tarball" | cut -d' ' -f1)"; else got="$(shasum -a 256 "$work/$tarball" | cut -d' ' -f1)"; fi
[ "$got" = "$OPENSSL_SHA256" ] || { echo "build-openssl.sh: $tarball sha256 $got, expected $OPENSSL_SHA256" >&2; exit 1; }
tar -xzf "$work/$tarball" -C "$work"

target=""
case "$(uname -s)-$(uname -m)" in
    Darwin-arm64) target=darwin64-arm64-cc; export MACOSX_DEPLOYMENT_TARGET="${MACOSX_DEPLOYMENT_TARGET:-11.0}" ;;
    Darwin-x86_64) target=darwin64-x86_64-cc; export MACOSX_DEPLOYMENT_TARGET="${MACOSX_DEPLOYMENT_TARGET:-11.0}" ;;
esac
jobs="$(getconf _NPROCESSORS_ONLN 2> /dev/null || echo 4)"
cd "$work/openssl-$OPENSSL_VERSION"
# no-module/no-legacy: everything in libcrypto.a, nothing dlopened. openssldir: unused, the client
# never loads openssl.cnf (KRUNKNATIVE_TLS_STATIC).
./Configure ${target:+"$target"} no-shared no-module no-legacy no-tests no-docs no-apps no-engine \
    "--prefix=$prefix" --libdir=lib --openssldir=/etc/ssl > "$work/configure.log" || { cat "$work/configure.log" >&2; exit 1; }
make -j"$jobs" build_libs > "$work/make.log" 2>&1 || { tail -40 "$work/make.log" >&2; exit 1; }
rm -rf "$prefix"
make install_dev > /dev/null
echo "$OPENSSL_VERSION" > "$prefix/VERSION"
echo "build-openssl.sh: OpenSSL $OPENSSL_VERSION ($target${MACOSX_DEPLOYMENT_TARGET:+, macOS $MACOSX_DEPLOYMENT_TARGET}) in $prefix" >&2

#!/usr/bin/env sh
# install.sh — download + install ambil on Linux / macOS / *BSD.
#
# Usage:
#   curl -fsSL https://example.com/install.sh | sh
#   curl -fsSL https://example.com/install.sh | sh -s -- --version v0.2.0
#   sh install.sh --dir ~/.local/bin
#
# Environment overrides:
#   AMBIL_REPO        owner/repo on GitHub        (default: sutantodadang/ambil)
#   AMBIL_VERSION     git tag, or "latest"        (default: latest)
#   AMBIL_INSTALL_DIR install prefix              (default: /usr/local/bin if
#                                                  writable, else ~/.local/bin)
#   AMBIL_BASE_URL    asset base URL              (default: GitHub releases)
#   AMBIL_NO_VERIFY   "1" to skip SHA256 check    (default: verify)
#
# Exit codes: 0 success, non-zero on any failure.

set -eu

# ---------- defaults ----------
: "${AMBIL_REPO:=sutantodadang/ambil}"
: "${AMBIL_VERSION:=latest}"
: "${AMBIL_INSTALL_DIR:=}"
: "${AMBIL_BASE_URL:=}"
: "${AMBIL_NO_VERIFY:=0}"

# ---------- helpers ----------
log()  { printf '\033[1;36m==>\033[0m %s\n' "$*" >&2; }
warn() { printf '\033[1;33mwarn:\033[0m %s\n' "$*" >&2; }
err()  { printf '\033[1;31merror:\033[0m %s\n' "$*" >&2; exit 1; }

need() { command -v "$1" >/dev/null 2>&1 || err "missing required tool: $1"; }

# ---------- argument parsing ----------
while [ "$#" -gt 0 ]; do
    case "$1" in
        -v|--version) AMBIL_VERSION="${2:?--version requires a value}"; shift 2 ;;
        -d|--dir)     AMBIL_INSTALL_DIR="${2:?--dir requires a value}"; shift 2 ;;
        -r|--repo)    AMBIL_REPO="${2:?--repo requires a value}"; shift 2 ;;
        --no-verify)  AMBIL_NO_VERIFY=1; shift ;;
        -h|--help)
            sed -n '2,/^$/p' "$0" | sed 's/^# \{0,1\}//'
            exit 0
            ;;
        *) err "unknown argument: $1" ;;
    esac
done

# ---------- dependencies ----------
need uname
need mktemp
need tar

DOWNLOADER=""
if command -v curl >/dev/null 2>&1; then
    DOWNLOADER="curl -fsSL --retry 3 --retry-connrefused -o"
elif command -v wget >/dev/null 2>&1; then
    DOWNLOADER="wget -q -O"
else
    err "need either curl or wget on PATH"
fi

SHA256=""
for c in sha256sum shasum gsha256sum; do
    if command -v "$c" >/dev/null 2>&1; then SHA256="$c"; break; fi
done
if [ -z "$SHA256" ] && [ "$AMBIL_NO_VERIFY" != "1" ]; then
    warn "no sha256 tool found; pass --no-verify or install coreutils"
    AMBIL_NO_VERIFY=1
fi

# ---------- platform detection ----------
RAW_OS="$(uname -s)"
RAW_ARCH="$(uname -m)"

case "$RAW_OS" in
    Linux*)   OS=linux ;;
    Darwin*)  OS=darwin ;;
    FreeBSD*) OS=freebsd ;;
    *)        err "unsupported OS: $RAW_OS" ;;
esac

case "$RAW_ARCH" in
    x86_64|amd64)            ARCH=x86_64 ;;
    aarch64|arm64)           ARCH=aarch64 ;;
    armv7*|armv6*|arm)       ARCH=armv7 ;;
    *)                       err "unsupported architecture: $RAW_ARCH" ;;
esac

# ---------- resolve version ----------
fetch() {
    # $1 = url, $2 = dest
    # shellcheck disable=SC2086
    $DOWNLOADER "$2" "$1" || err "download failed: $1"
}

resolve_latest() {
    api="https://api.github.com/repos/${AMBIL_REPO}/releases/latest"
    tmp="$(mktemp)"
    fetch "$api" "$tmp" >/dev/null
    # Extract "tag_name": "vX.Y.Z" without jq.
    tag="$(sed -n 's/.*"tag_name":[[:space:]]*"\([^"]*\)".*/\1/p' "$tmp" | head -n1)"
    rm -f "$tmp"
    [ -n "$tag" ] || err "could not resolve latest release for $AMBIL_REPO"
    printf '%s\n' "$tag"
}

if [ "$AMBIL_VERSION" = "latest" ]; then
    log "resolving latest release of $AMBIL_REPO"
    AMBIL_VERSION="$(resolve_latest)"
fi

# Normalize: ensure leading 'v'.
case "$AMBIL_VERSION" in
    v*) ;;
    *)  AMBIL_VERSION="v${AMBIL_VERSION}" ;;
esac

# ---------- install dir ----------
choose_install_dir() {
    if [ -n "$AMBIL_INSTALL_DIR" ]; then
        printf '%s\n' "$AMBIL_INSTALL_DIR"
        return
    fi
    if [ -w /usr/local/bin ] 2>/dev/null; then
        printf '/usr/local/bin\n'
    else
        printf '%s/.local/bin\n' "${HOME:-/root}"
    fi
}
INSTALL_DIR="$(choose_install_dir)"
mkdir -p "$INSTALL_DIR" || err "cannot create $INSTALL_DIR"

# ---------- download asset ----------
ASSET="ambil-${AMBIL_VERSION}-${ARCH}-${OS}.tar.gz"
SUMS="SHA256SUMS"

if [ -z "$AMBIL_BASE_URL" ]; then
    AMBIL_BASE_URL="https://github.com/${AMBIL_REPO}/releases/download/${AMBIL_VERSION}"
fi

ASSET_URL="${AMBIL_BASE_URL}/${ASSET}"
SUMS_URL="${AMBIL_BASE_URL}/${SUMS}"

TMPDIR="$(mktemp -d)"
trap 'rm -rf "$TMPDIR"' EXIT INT HUP TERM

log "downloading $ASSET"
fetch "$ASSET_URL" "$TMPDIR/$ASSET"

# ---------- verify ----------
if [ "$AMBIL_NO_VERIFY" != "1" ]; then
    log "downloading $SUMS"
    fetch "$SUMS_URL" "$TMPDIR/$SUMS"

    expected="$(awk -v f="$ASSET" '$2==f || $2=="*"f {print $1; exit}' "$TMPDIR/$SUMS")"
    [ -n "$expected" ] || err "checksum for $ASSET not found in $SUMS"

    case "$SHA256" in
        sha256sum|gsha256sum) actual="$($SHA256 "$TMPDIR/$ASSET" | awk '{print $1}')" ;;
        shasum)               actual="$(shasum -a 256 "$TMPDIR/$ASSET" | awk '{print $1}')" ;;
    esac

    if [ "$expected" != "$actual" ]; then
        err "checksum mismatch for $ASSET (expected $expected, got $actual)"
    fi
    log "checksum verified"
else
    warn "skipping checksum verification"
fi

# ---------- extract + install ----------
log "extracting"
tar -C "$TMPDIR" -xzf "$TMPDIR/$ASSET" || err "tar extraction failed"

# Locate the extracted ambil binary (handles both `ambil` at archive root and
# `ambil-<version>/ambil` layouts).
BIN_PATH=""
if [ -f "$TMPDIR/ambil" ]; then
    BIN_PATH="$TMPDIR/ambil"
else
    BIN_PATH="$(find "$TMPDIR" -type f -name ambil -perm -u+x 2>/dev/null | head -n1)"
    [ -n "$BIN_PATH" ] || BIN_PATH="$(find "$TMPDIR" -type f -name ambil 2>/dev/null | head -n1)"
fi
[ -n "$BIN_PATH" ] && [ -f "$BIN_PATH" ] || err "ambil binary not found in archive"

DEST="$INSTALL_DIR/ambil"
log "installing to $DEST"

# Install atomically: copy to .new, chmod, rename.
cp "$BIN_PATH" "$DEST.new" || err "cannot write to $DEST.new"
chmod 0755 "$DEST.new"
mv "$DEST.new" "$DEST" || err "cannot move into place"

# ---------- post-install ----------
log "installed: $($DEST --version 2>/dev/null || echo "$DEST")"

case ":${PATH}:" in
    *":${INSTALL_DIR}:"*) ;;
    *)
        warn "$INSTALL_DIR is not in your PATH"
        warn "add this to your shell profile:"
        printf '    export PATH="%s:$PATH"\n' "$INSTALL_DIR" >&2
        ;;
esac

log "done. try: ambil --help"

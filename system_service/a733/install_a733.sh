#!/usr/bin/env bash
set -Eeuo pipefail

SCRIPT_DIR="$(CDPATH= cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(CDPATH= cd -- "$SCRIPT_DIR/.." && pwd)"

TARGET_PLATFORM="${TARGET_PLATFORM:-a733}"
INSTALL_BIN="${INSTALL_BIN:-/sbin/system_service}"
SERVICE_NAME="${SERVICE_NAME:-system_service.service}"
SERVICE_SRC="$SCRIPT_DIR/system_service.service"
SERVICE_DST="/etc/systemd/system/$SERVICE_NAME"

if [[ "$TARGET_PLATFORM" != "a733" ]]; then
	echo "This installer is for TARGET_PLATFORM=a733 only." >&2
	exit 1
fi

if [[ "${EUID:-$(id -u)}" -ne 0 ]]; then
	exec sudo --preserve-env=CC,PKG_CONFIG,CFLAGS,LDFLAGS,LOG_ROOT,WIFI_IFACE,BLE_NAME,AP_SSID,AP_PASSWD,SOCK_PATH,SOCK_FALLBACK_PATH,NUM_CPUS,TARGET_PLATFORM,INSTALL_BIN,SERVICE_NAME bash "$0" "$@"
fi

need_cmd() {
	if ! command -v "$1" >/dev/null 2>&1; then
		echo "Missing required command: $1" >&2
		exit 1
	fi
}

need_cmd make
need_cmd install
need_cmd systemctl
need_cmd pkg-config

cleanup_build_outputs() {
	make -C "$PROJECT_DIR" TARGET_PLATFORM=a733 clean >/dev/null
}

trap cleanup_build_outputs EXIT

echo "[a733] cleaning old build outputs"
cleanup_build_outputs

echo "[a733] building system_service"
make -C "$PROJECT_DIR" TARGET_PLATFORM=a733 "$@"

if [[ ! -x "$PROJECT_DIR/system_service" ]]; then
	echo "Build did not produce $PROJECT_DIR/system_service" >&2
	exit 1
fi

echo "[a733] stopping existing service if present"
systemctl stop "$SERVICE_NAME" >/dev/null 2>&1 || true

echo "[a733] installing binary to $INSTALL_BIN"
install -D -o root -g root -m 0755 "$PROJECT_DIR/system_service" "$INSTALL_BIN"

echo "[a733] installing systemd unit to $SERVICE_DST"
install -D -o root -g root -m 0644 "$SERVICE_SRC" "$SERVICE_DST"

echo "[a733] enabling and starting $SERVICE_NAME"
systemctl daemon-reload
systemctl enable "$SERVICE_NAME"
systemctl restart "$SERVICE_NAME"

echo "[a733] removing build intermediates from source tree"
cleanup_build_outputs
trap - EXIT

echo "[a733] installed:"
ls -l "$INSTALL_BIN"
systemctl --no-pager --full status "$SERVICE_NAME" || true

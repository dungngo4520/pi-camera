#!/usr/bin/env bash
# Cross-build picamera for aarch64 (Raspberry Pi) in Docker, then extract the binary.
# No ARM hardware required — uses qemu-user-static binfmt emulation.
#
# Output: ./picamera-arm64 (next to this repo)
#
# Requirements:
#   - docker (or podman)
#   - qemu-user-static + binfmt_misc registered for aarch64
#     (on Arch: pacman -S qemu-user-static; sudo systemctl restart systemd-binfmt)

set -euo pipefail

cd "$(dirname "$0")/.."

IMAGE="picamera-cross"
CONTAINER="pcx-extract"
BINARY="picamera-arm64"

# Pick a container runtime.
if command -v docker >/dev/null 2>&1; then
    RT=docker
elif command -v podman >/dev/null 2>&1; then
    RT=podman
else
    echo "Error: neither docker nor podman found." >&2
    exit 1
fi

echo "=== Building aarch64 image with $RT (qemu-user-static emulation) ==="
$RT build --platform linux/arm64 -t "$IMAGE" -f Dockerfile.cross .

echo "=== Extracting binary ==="
$RT rm -f "$CONTAINER" 2>/dev/null || true
$RT create --platform linux/arm64 --name "$CONTAINER" "$IMAGE"
$RT cp "$CONTAINER:/build/picamera" "./$BINARY"
$RT rm -f "$CONTAINER"

echo "=== Verifying ==="
file "./$BINARY"
ls -la "./$BINARY"
echo
echo "Done: ./$BINARY"
echo "Transfer to Pi:  scp ./$BINARY pi@raspberrypi.local:~/camera/build/picamera"

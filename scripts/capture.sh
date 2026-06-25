#!/usr/bin/env bash
set -euo pipefail

# Convenience capture script for the Pi Camera.
# Saves timestamped photos to ~/photos/.
# Usage: ./capture.sh [options]
#   --quick       Lower resolution (1920x1080), faster capture
#   --full        Full 4056x3040 (default)
#   --dir <path>  Output directory (default: ~/photos)

BINARY="$HOME/camera/build/picamera"
OUTDIR="${HOME}/photos"
RESOLUTION="4056x3040"

# Parse args
while [[ $# -gt 0 ]]; do
  case "$1" in
    --quick) RESOLUTION="1920x1080"; shift ;;
    --full)  RESOLUTION="4056x3040"; shift ;;
    --dir)   OUTDIR="$2"; shift 2 ;;
    *)       echo "Unknown option: $1"; exit 1 ;;
  esac
done

# Validate binary
if [[ ! -x "$BINARY" ]]; then
  echo "ERROR: picamera binary not found at $BINARY" >&2
  echo "Build it first: cd ~/camera/build && cmake .. && make" >&2
  exit 1
fi

# Ensure output directory exists
mkdir -p "$OUTDIR"

# Generate timestamped filename
TIMESTAMP=$(date +"%Y%m%d_%H%M%S")
FILENAME="${OUTDIR}/IMG_${TIMESTAMP}.ppm"

# Parse resolution
WIDTH="${RESOLUTION%x*}"
HEIGHT="${RESOLUTION#*x}"

echo "Capturing ${RESOLUTION} → ${FILENAME} ..."
if "$BINARY" --capture "$FILENAME" --width "$WIDTH" --height "$HEIGHT"; then
  FILESIZE=$(stat -c%s "$FILENAME" 2>/dev/null || stat -f%z "$FILENAME" 2>/dev/null)
  echo "Done: $(basename "$FILENAME") ($(( FILESIZE / 1024 )) KB)"
  # Return the filename for programmatic use
  echo "$FILENAME"
else
  echo "ERROR: Capture failed" >&2
  exit 1
fi

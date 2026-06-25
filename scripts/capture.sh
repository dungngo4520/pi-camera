#!/usr/bin/env bash
set -euo pipefail

BINARY="$HOME/camera/build/picamera"
OUTDIR="${HOME}/photos"
RESOLUTION="4056x3040"

while [[ $# -gt 0 ]]; do
  case "$1" in
    --quick) RESOLUTION="1920x1080"; shift ;;
    --full)  RESOLUTION="4056x3040"; shift ;;
    --dir)   OUTDIR="$2"; shift 2 ;;
    *)       echo "Unknown option: $1"; exit 1 ;;
  esac
done

if [[ ! -x "$BINARY" ]]; then
  echo "Build first: cd ~/camera/build && cmake .. && make" >&2
  exit 1
fi

mkdir -p "$OUTDIR"
TIMESTAMP=$(date +"%Y%m%d_%H%M%S")
FILENAME="${OUTDIR}/IMG_${TIMESTAMP}.ppm"
WIDTH="${RESOLUTION%x*}"
HEIGHT="${RESOLUTION#*x}"

echo "Capturing ${RESOLUTION} -> ${FILENAME} ..."
if "$BINARY" --capture "$FILENAME" --width "$WIDTH" --height "$HEIGHT"; then
  FILESIZE=$(stat -c%s "$FILENAME" 2>/dev/null || stat -f%z "$FILENAME" 2>/dev/null)
  echo "Done: $(basename "$FILENAME") ($(( FILESIZE / 1024 )) KB)"
  echo "$FILENAME"
else
  echo "Capture failed" >&2
  exit 1
fi

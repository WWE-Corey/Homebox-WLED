#!/bin/bash
# Fetches a Homebox attachment, converts it to raw RGB565 pixel data at
# the display's resolution, and pushes those bytes to the ESP32's own
# /image endpoint. Standalone script rather than a rest_command: HA's
# rest_command payload is Jinja-rendered text, not a sane way to
# build/send a binary body.
#
# Usage: push_display_image.sh <item-id> <attachment-id>
# Called from display_automation.yaml via a `shell_command:` entry, e.g.:
#   shell_command:
#     push_display_image: "/config/push_display_image.sh {{ item_id }} {{ attachment_id }}"
#
# attachment_id must be the item's `imageId` field (Homebox's own
# server-computed primary-photo attachment id), not a thumbnail id dug
# out of `attachments[]` — see display_automation.yaml's comment for
# why. Fetches the full-size attachment; fine at this display's
# resolution.
#
# IMAGE_AREA_WIDTH/HEIGHT (142x142) must match homebox_display.ino's
# IMAGE_AREA_WIDTH/DISPLAY_HEIGHT exactly, or the ESP32's /image size
# check rejects every frame — keep these two files' dimensions in sync
# by hand. 142x142, not wider: the panel's column-address axis only has
# ~142 real physical positions. rgb565le (little-endian) is confirmed
# correct against real hardware; the firmware does its own byte-swap
# right before writing to the panel, unrelated to the format this
# script produces.
#
# Needs `curl` and `ffmpeg` available wherever this script actually
# runs — NOT guaranteed on a stock Home Assistant OS install, since
# shell_command executes inside HA's own container, which doesn't ship
# with ffmpeg. Confirmed working on a "HA Container"/Core-in-venv style
# install; on HAOS you likely need a separate add-on or external helper.

HOMEBOX_URL="https://homebox.url"
# Kept separate from HA's secrets.yaml on purpose — shell_command has no
# access to HA's !secret-loaded Jinja values, only to whatever's on disk.
# Create this file yourself, containing just the header value, e.g.:
#   Bearer hb_xxxxxxxxxxxxxxxx
HOMEBOX_TOKEN_FILE="/config/homebox_auth_header.txt"
ESP32_DISPLAY_IP="192.168.xx.xx"
IMAGE_AREA_WIDTH=142
IMAGE_AREA_HEIGHT=142

# HA's shell_command logger only ever surfaces a bare return code, never
# this script's own stderr, and on Supervised/HAOS an SSH session may be
# a different container than the one shell_command executes in — so
# logging to /config (a volume shared across containers) is more
# trustworthy than either SSH or HA's own logger.
LOG_FILE="/config/push_display_image.log"
exec > >(tee -a "$LOG_FILE") 2>&1
echo "=== $(date -Iseconds) push_display_image.sh $* ==="

# Millisecond timestamps for stage timing below — added while tracking
# down a real ~5s-per-/image-POST stall, since resolved (it turned out
# to be entirely firmware-side, see HTTP_RAW_BUFLEN's comment in
# homebox_display/platformio.ini). Left in place: cheap, and useful for
# spotting a future regression in any of these three stages without
# needing to re-instrument from scratch.
# $EPOCHREALTIME is a bash builtin (seconds.microseconds), not an
# external command — works the same under BusyBox or GNU userland,
# unlike `date +%N` which BusyBox doesn't support. Falls back to
# whole-second resolution if it's ever unset for some reason.
now_ms() {
  if [ -n "$EPOCHREALTIME" ]; then
    local s="${EPOCHREALTIME%%.*}" us="${EPOCHREALTIME#*.}"
    echo $((s * 1000 + 10#${us:0:3}))
  else
    echo $(($(date +%s) * 1000))
  fi
}
T_START="$(now_ms)"

ITEM_ID="$1"
ATTACHMENT_ID="$2"
if [ -z "$ITEM_ID" ] || [ -z "$ATTACHMENT_ID" ]; then
  echo "Usage: $0 <item-id> <attachment-id>" >&2
  exit 1
fi

if [ ! -r "$HOMEBOX_TOKEN_FILE" ]; then
  echo "ERROR: HOMEBOX_TOKEN_FILE not readable: $HOMEBOX_TOKEN_FILE" >&2
  exit 1
fi
HOMEBOX_AUTH="$(cat "$HOMEBOX_TOKEN_FILE")"

for bin in curl ffmpeg mktemp; do
  if ! command -v "$bin" >/dev/null 2>&1; then
    echo "ERROR: $bin not found on PATH — see this script's header comment" \
      "about ffmpeg/curl not being guaranteed inside HA's shell_command" \
      "environment." >&2
    exit 1
  fi
done

# Template form (TEMPLATE ending in XXXXXX, no --suffix=) — the real HA
# Core container ships BusyBox mktemp, which doesn't understand GNU
# long options and fails silently (prints usage text into what should
# be a path) rather than erroring loudly. This form works on both
# BusyBox and GNU mktemp.
TMP_SOURCE="$(mktemp "${TMPDIR:-/tmp}/push_display_image_src.XXXXXX")"
TMP_RAW="$(mktemp "${TMPDIR:-/tmp}/push_display_image_raw.XXXXXX")"
if [ -z "$TMP_SOURCE" ] || [ -z "$TMP_RAW" ]; then
  echo "ERROR: mktemp failed to produce a temp file path (got" \
    "TMP_SOURCE='${TMP_SOURCE}' TMP_RAW='${TMP_RAW}')." >&2
  exit 1
fi
trap 'rm -f "$TMP_SOURCE" "$TMP_RAW"' EXIT

HTTP_STATUS="$(curl -s -o "$TMP_SOURCE" -w '%{http_code}' \
  -H "Authorization: ${HOMEBOX_AUTH}" \
  "${HOMEBOX_URL}/api/v1/entities/${ITEM_ID}/attachments/${ATTACHMENT_ID}")"
T_FETCH="$(now_ms)"
echo "TIMING: Homebox fetch took $((T_FETCH - T_START))ms"
if [ "$HTTP_STATUS" != "200" ] || [ ! -s "$TMP_SOURCE" ]; then
  echo "ERROR: fetching attachment from Homebox failed (HTTP ${HTTP_STATUS}," \
    "$(wc -c < "$TMP_SOURCE" 2>/dev/null || echo 0) bytes received) —" \
    "check HOMEBOX_TOKEN_FILE's contents are a valid, current" \
    "'Bearer hb_xxxxxxxx' auth header." >&2
  exit 1
fi

if ! ffmpeg -y -loglevel error -i "$TMP_SOURCE" \
  -vf "scale=${IMAGE_AREA_WIDTH}:${IMAGE_AREA_HEIGHT}:force_original_aspect_ratio=decrease,pad=${IMAGE_AREA_WIDTH}:${IMAGE_AREA_HEIGHT}:(ow-iw)/2:(oh-ih)/2" \
  -pix_fmt rgb565le -f rawvideo "$TMP_RAW"; then
  echo "ERROR: ffmpeg failed to convert the fetched attachment" \
    "(${TMP_SOURCE}, $(wc -c < "$TMP_SOURCE") bytes) — it may not be a" \
    "valid/decodable image." >&2
  exit 1
fi
T_FFMPEG="$(now_ms)"
echo "TIMING: ffmpeg convert took $((T_FFMPEG - T_FETCH))ms"

EXPECTED_BYTES=$((IMAGE_AREA_WIDTH * IMAGE_AREA_HEIGHT * 2))
ACTUAL_BYTES="$(wc -c < "$TMP_RAW")"
if [ "$ACTUAL_BYTES" -ne "$EXPECTED_BYTES" ]; then
  echo "ERROR: ffmpeg produced ${ACTUAL_BYTES} bytes, expected exactly" \
    "${EXPECTED_BYTES} (IMAGE_AREA_WIDTH*IMAGE_AREA_HEIGHT*2) — the" \
    "ESP32's /image handler will reject this." >&2
  exit 1
fi

# -H "Expect:" strips curl's automatic "Expect: 100-continue" header,
# which curl adds on its own for any POST body over ~1KB — the ESP32's
# WebServer never sends back a "100 Continue" response, so this avoids
# curl waiting out its own internal timeout for one before sending the
# body anyway. Tried as a fix for a real ~5s-per-/image-POST stall
# (turned out NOT to be the cause — see HTTP_RAW_BUFLEN's comment in
# homebox_display/platformio.ini for the actual root cause and fix,
# a firmware-side buffer-size issue) but left in place anyway since
# it's a harmless, standard curl tweak for large bodies.
HTTP_STATUS="$(curl -s -o /dev/null -w '%{http_code}' -X POST \
  -H "Content-Type: application/octet-stream" \
  -H "Expect:" \
  --data-binary "@${TMP_RAW}" \
  "http://${ESP32_DISPLAY_IP}/image")"
T_POST="$(now_ms)"
echo "TIMING: POST to ESP32 took $((T_POST - T_FFMPEG))ms"
echo "TIMING: total $((T_POST - T_START))ms"
if [ "$HTTP_STATUS" != "200" ]; then
  echo "ERROR: POSTing image to the ESP32 (${ESP32_DISPLAY_IP}) failed" \
    "(HTTP ${HTTP_STATUS})." >&2
  exit 1
fi

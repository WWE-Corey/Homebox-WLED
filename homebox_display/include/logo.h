// logo.h
//
// Baked-in idle-screen logo, shown whenever the display has no
// location/item to show (on boot, and after /clear — see handleClear()
// in homebox_display.ino). Baked into firmware rather than fetched over
// the network so the idle screen works even if Home Assistant or the
// network is briefly unreachable, and doesn't cost a round trip on
// every acknowledge.
//
// Must be #included AFTER DISPLAY_WIDTH/DISPLAY_HEIGHT are defined in
// homebox_display.ino — LOGO_WIDTH/LOGO_HEIGHT are deliberately tied
// directly to them below rather than hardcoded, specifically so they
// can never drift out of sync with whatever the display's actual
// logical (landscape, post-rotation) dimensions turn out to be.
//
// Real artwork: the "White Wolf Creations" logo (wwc-2.svg — a wolf
// mark plus wordmark), cropped to its drawing bounding box and
// letterboxed onto white to fill LOGO_WIDTH x LOGO_HEIGHT exactly. The
// actual pixel data lives in logo_data.h (LOGO_RGB565, generated —
// don't hand-edit) rather than inline here, purely because it's ~72k
// uint16_t entries and would swamp this file's comments. See
// logo_data.h's header comment for the regeneration pipeline if the
// source artwork changes.
//
// UNVERIFIED against real hardware yet: RGB565 byte order (le vs be) —
// same open item as push_display_image.sh on the HA side. If colors
// look channel-swapped/inverted once this actually renders on the
// NV3007, regenerate logo.raw with -pix_fmt rgb565be instead of
// rgb565le and rebuild logo_data.h the same way.

#pragma once

#include <Arduino.h>
#include "logo_data.h"

#define LOGO_WIDTH  DISPLAY_WIDTH
#define LOGO_HEIGHT DISPLAY_HEIGHT

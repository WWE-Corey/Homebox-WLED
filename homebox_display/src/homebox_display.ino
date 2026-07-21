// homebox_display.ino
//
// ESP32 + NV3007 SPI TFT + 3 buttons: shows the currently-viewed Homebox
// location/item (pushed from Home Assistant) and lets +/-/acknowledge
// buttons stage a quantity change that's only committed to Homebox when
// acknowledged — not on every button press, so a few extra taps don't
// spam Homebox's API and "one too many presses" is easy to correct
// before it's ever written anywhere.
//
// Target board: ESP32-S3 with PSRAM (e.g. an ESP32-S3-WROOM-1 N16R8) —
// PSRAM matters because a full 142x428 RGB565 framebuffer plus
// WiFi/HTTP/JSON overhead is tight on the chip's internal SRAM alone.
//
// This is a PlatformIO project, not an Arduino IDE sketch — that's why
// this file lives in src/ rather than a homebox_display/homebox_display.ino
// sketch-folder layout. See ../platformio.ini for the board/framework
// config and library list, and ../include/lv_conf.h for the LVGL config
// (LV_USE_NV3007 1 must be set there for the NV3007 driver functions
// used below to exist).
//
// HTTP API this device exposes (driven by rest_command in
// rest_commands.yaml, same "device runs a small JSON API" pattern as
// the WLED side of this project):
//   POST /text   {"location":"D-04","item_name":"...","item_id":"...","quantity":5}
//   POST /image  raw RGB565 bytes, exactly IMAGE_AREA_WIDTH*DISPLAY_HEIGHT*2
//                long (fills a fixed square region flush against the
//                left edge — see IMAGE_AREA_WIDTH's comment)
//   POST /clear  (empty body) clears text and shows the idle logo (logo.h)
//   POST /backlight  {"on":true|false} — GPIO-only, doesn't touch
//                     display state (see TFT_BL, requires BL rewired
//                     from 3.3V to a GPIO — see its #define comment)
//
// Outbound: POSTs {"item_id":...,"quantity":...} to HA_ACK_WEBHOOK_URL
// when the acknowledge button is pressed. display_ack_automation.yaml
// responds to that by also clearing the WLED highlight and starting its
// breathe effect immediately, as a "job's done" visual — not this
// firmware's concern, just context for why /clear fires right after.

// HTTP_RAW_BUFLEN/HTTP_UPLOAD_BUFLEN are overridden in platformio.ini's
// build_flags (-D), not here — a #define in this file only affects this
// file's own preprocessing; WebServer's raw-body reader
// (Parsing.cpp::_parseRequest) is a separately-compiled library source
// that never sees it, bench-confirmed (a local #define before
// #include <WebServer.h> below had zero effect). See platformio.ini's
// own comment on those two flags for the full story: this is what fixed
// a bench-observed ~5s stall on every /image POST. The static_assert
// below (after IMAGE_AREA_WIDTH/DISPLAY_HEIGHT are defined) fails the
// build loudly if platformio.ini's values and the real payload size
// ever drift out of sync.

#include <WiFi.h>
#include <HTTPClient.h>
#include <WebServer.h>
#include <ArduinoJson.h>
#include <SPI.h>
#include <lvgl.h>
// lvgl.h doesn't pull this one in itself — needed explicitly for
// lv_image_cache_drop(), used below to work around a canvas-content
// caching issue (see its call sites' comments).
#include <misc/cache/instance/lv_image_cache.h>

// ---- Fill in your own ----
// wifi_credentials.h is git-ignored (see ../../.gitignore) — copy
// wifi_credentials.h.example to wifi_credentials.h and fill in your real
// SSID/password/webhook URL there, not here, so real credentials never
// end up in git history.
#include "wifi_credentials.h"

// ---- Display ----
// Hardware: an Estardyn 2.79" TFT-SPI, NV3007 controller, 142x428
// native glass (the also-common 168x428 variant of this same
// controller does NOT apply here — confirmed against the actual
// purchased module's product listing and PCB silkscreen).
//
// This project wants landscape, so PANEL_NATIVE_* (the physical glass
// dimensions) and DISPLAY_* (the logical, rotated dimensions everything
// else in this file and the HA side use) are deliberately different.
// setupDisplay() rotates LVGL's output (see LV_DISPLAY_ROTATION_270
// below) so the rest of the firmware — labels, the image canvas, the
// /image size check — can just think in landscape (428 wide x 142
// tall) without caring that the panel itself is wired as portrait.
// DISPLAY_WIDTH/HEIGHT must match push_display_image.sh's own copies
// on the HA side, or the byte-count checks reject every frame.
#define PANEL_NATIVE_WIDTH  142
#define PANEL_NATIVE_HEIGHT 428
#define DISPLAY_WIDTH  PANEL_NATIVE_HEIGHT  // 428 — logical/landscape
#define DISPLAY_HEIGHT PANEL_NATIVE_WIDTH   // 142 — logical/landscape

// Fixed layout regions so text and photo never compete for the same
// pixels, regardless of item name length or photo content. IMAGE_AREA
// is flush against the LEFT edge (x=0); TEXT_AREA is everything to its
// right, starting at TEXT_AREA_X. Labels are left-aligned against
// TEXT_AREA_X (the edge adjacent to IMAGE_AREA), so a short label still
// sits close to the photo instead of leaving a gap.
//
// IMAGE_AREA_WIDTH is 142, matching DISPLAY_HEIGHT/PANEL_NATIVE_WIDTH —
// not a free software choice. The panel's CASET (column) address axis
// only has ~142 real physical positions; addressing wider than that
// (180 was tried, to make the photo bigger) produced a repeating/
// wrapped raster pattern instead of a clean image. 142 keeps IMAGE_AREA
// a clean, fully-addressable square. Changing this also changes the
// byte count the /image endpoint expects
// (IMAGE_AREA_WIDTH * DISPLAY_HEIGHT * 2) — keep in sync with
// push_display_image.sh's own IMAGE_AREA_WIDTH/HEIGHT.
#define IMAGE_AREA_WIDTH  142
#define IMAGE_AREA_X      0
#define TEXT_AREA_X       IMAGE_AREA_WIDTH
#define TEXT_AREA_WIDTH   (DISPLAY_WIDTH - IMAGE_AREA_WIDTH)

// HTTP_RAW_BUFLEN/HTTP_UPLOAD_BUFLEN (overridden above, before
// WebServer.h's include) must exactly equal the real /image payload
// size — see that comment for why. This fails the build loudly instead
// of silently reintroducing a ~5s-per-upload stall if IMAGE_AREA_WIDTH
// or DISPLAY_HEIGHT ever change again without updating those two macros.
static_assert(HTTP_RAW_BUFLEN == IMAGE_AREA_WIDTH * DISPLAY_HEIGHT * 2,
              "HTTP_RAW_BUFLEN (set before #include <WebServer.h>) must "
              "match IMAGE_AREA_WIDTH * DISPLAY_HEIGHT * 2 exactly");

// logo.h ties LOGO_WIDTH/LOGO_HEIGHT directly to DISPLAY_WIDTH/HEIGHT,
// so it must come after they're defined above.
#include "logo.h"

// SPI pins, confirmed against real hardware. TFT_MOSI is GPIO 17, not
// 23 — 23 falls in the 22-25 range that doesn't exist on the ESP32-S3
// chip at all (see the button pin comments below). The other three
// (21/5/18) don't fall in a reserved range, but haven't been confirmed
// against Hosyond's actual pinout diagram beyond "doesn't crash" — worth
// double-checking if wiring a different board.
#define TFT_DC   21
#define TFT_CS   5
#define TFT_SCK  18
#define TFT_MOSI 17
#define TFT_RST  -1  // -1 if tied to EN / not separately controlled
// Backlight control, for the idle-timeout power-saving feature (see
// handleSetBacklight()) — NOT the panel's own BL pin default. BL must be
// physically rewired from 3.3V (its out-of-the-box wiring — always on,
// no software control at all) to this GPIO for that feature to do
// anything. GPIO 8 doesn't fall in any reserved range for this board
// (see the button pin comments below) and isn't used by anything else
// in this file.
#define TFT_BL   8

// ---- Buttons ----
// Confirmed against real hardware (Hosyond N16R8 board): GPIO 32 and 27
// both crash on pinMode() — the ESP32-S3-WROOM-1's flash SPI bus
// permanently occupies GPIO 26-32 on every module of this type. GPIO 25
// also doesn't work: the ESP32-S3 chip has no GPIO 22-25 at all (unlike
// the original ESP32). GPIO 4/6/7 are confirmed clean; 5 is also clean
// but reused as TFT_CS above.
#define BUTTON_INCREASE_PIN 4
#define BUTTON_DECREASE_PIN 6
#define BUTTON_ACK_PIN      7
#define DEBOUNCE_MS 200

WebServer server(80);

// Current item state — set by /text, adjusted locally by +/- buttons,
// only sent back to HA (and thus written to Homebox) on acknowledge.
String currentItemId = "";
String currentLocation = "";
String currentItemName = "";
long committedQuantity = 0;
long stagedQuantity = 0;
bool hasItem = false;

// Increase/decrease are level-triggered and rate-limited independently
// (hold-to-repeat at DEBOUNCE_MS intervals). Acknowledge is
// edge-triggered instead — see pollButtons() — so holding it down
// can't repeatedly re-send the acknowledge webhook / re-PATCH Homebox.
unsigned long lastIncreaseMs = 0;
unsigned long lastDecreaseMs = 0;
unsigned long lastAckMs = 0;
int lastAckState = HIGH;

// ---- LVGL / NV3007 plumbing ----
// The two callbacks LVGL's NV3007 driver needs: send-command and
// send-pixel-data. Plain polling SPI (no DMA) — simplest thing that
// works. SPI_MODE0 / 40MHz — bumped from an untested, conservative
// 20MHz on the theory that it was the bottleneck in a slow /image
// POST; bench-measured to make basically no difference (the actual SPI
// write for a full image was only ~10-16ms at either clock — see
// HTTP_RAW_BUFLEN's comment in platformio.ini for where that time
// actually was going). Left at 40MHz anyway since it's a real, if
// small, improvement and has bench-confirmed no downside — this panel
// is write-only (no read-back to sanity-check timing against), so if
// content ever comes up garbled/torn after touching this, drop it back
// down first before looking elsewhere.
static void nv3007_spi_write(const uint8_t *cmd, size_t cmd_size,
                              const uint8_t *data, size_t data_size) {
  SPI.beginTransaction(SPISettings(40000000, MSBFIRST, SPI_MODE0));
  digitalWrite(TFT_CS, LOW);

  digitalWrite(TFT_DC, LOW);  // command byte(s)
  SPI.writeBytes(cmd, cmd_size);

  if (data_size > 0) {
    digitalWrite(TFT_DC, HIGH);  // parameter/pixel bytes
    SPI.writeBytes(data, data_size);
  }

  digitalWrite(TFT_CS, HIGH);
  SPI.endTransaction();
}

static void nv3007_send_cmd(lv_display_t *disp, const uint8_t *cmd, size_t cmd_size,
                             const uint8_t *param, size_t param_size) {
  nv3007_spi_write(cmd, cmd_size, param, param_size);
}

static void nv3007_send_color(lv_display_t *disp, const uint8_t *cmd, size_t cmd_size,
                               uint8_t *color_data, size_t color_size) {
  nv3007_spi_write(cmd, cmd_size, color_data, color_size);

  // Polling transfer, not DMA — the transaction is already complete by
  // the time nv3007_spi_write() returns, so it's correct to signal
  // "flush done" immediately here. LVGL's generic MIPI driver blocks on
  // this before handing over the next chunk of pixel data.
  lv_display_flush_ready(disp);
}

// millis() returns unsigned long; lv_tick_get_cb_t wants a plain
// uint32_t-returning function — same width on this chip, but a
// distinct type in C++, so it doesn't pass directly as a function
// pointer.
static uint32_t lvTickSource() {
  return millis();
}

lv_display_t *display;
lv_obj_t *labelLocation;
lv_obj_t *labelItemName;
lv_obj_t *labelQuantity;
// Shared by both the item photo (pushed via /image) and the idle logo
// (baked-in, see logo.h) — they occupy the same screen region and are
// never shown at the same time, so one canvas covers both.
lv_obj_t *imageCanvas;
// Backing pixel buffer for imageCanvas — allocated from PSRAM in
// setupDisplay(). showLogo() and handleSetText() both write into this
// same buffer rather than swapping the canvas to point at different
// buffers, so there's exactly one pixel-ownership story to reason
// about.
uint16_t *imageCanvasBuf = nullptr;
// ImageRawHandler's staging area for an incoming /image POST body, in
// LOGICAL (row-major, IMAGE_AREA_WIDTH x DISPLAY_HEIGHT) order — also
// doubles as the "last successfully received photo" cache, re-sent by
// reassertPhotoIfAny() whenever something else might have clobbered it
// on screen. See sendImageAreaDirect()'s comment for why photos bypass
// LVGL's canvas entirely and go straight to the panel.
uint16_t *imageStagingBuf = nullptr;
// Same pixel count as imageStagingBuf, reordered into NATIVE (panel)
// row-major order by sendImageAreaDirect(). Kept as a separate
// persistent buffer (not stack-allocated per call) purely to avoid
// repeated large PSRAM allocations.
uint16_t *imageNativeBuf = nullptr;
// True once a photo has been successfully written for the CURRENTLY
// displayed item — false after /clear or a fresh /text (new item, no
// photo received yet). Lets reassertPhotoIfAny() know whether there's
// anything worth re-sending after an LVGL-driven full-panel refresh.
bool hasPhotoForCurrentItem = false;

// Gap applied to the panel's native GRAM addressing to compensate for a
// hardware quirk on this specific unit (the visible glass doesn't start
// exactly at GRAM address 0 on either axis) — found via a bench sweep,
// see the README's Open Items for how. Duplicated here as named
// constants (rather than hardcoding the values twice) because
// sendImageAreaDirect() needs the exact same offsets when computing its
// own CASET/RASET window by hand, bypassing LVGL entirely for photos.
#define PANEL_GAP_X 0
#define PANEL_GAP_Y 14
// Thickness of the white border drawn along IMAGE_AREA/TEXT_AREA's true
// panel edges (see sendImageAreaDirect()/handleSetText()) to mask a
// separate, long-documented hardware border/gap artifact that's only
// visible against a black background (never visible against the white
// idle logo). 2px was bench-confirmed to fully mask it once PANEL_GAP_Y
// was correctly tuned — see the README's Open Items for the full
// gap+border investigation.
#define BORDER_THICKNESS 2

// Writes the current contents of imageStagingBuf (LOGICAL, row-major,
// IMAGE_AREA_WIDTH x DISPLAY_HEIGHT) directly to the panel via raw SPI,
// bypassing LVGL's canvas/image draw pipeline entirely for the photo.
//
// WHY BYPASS LVGL: LVGL's own compositing of canvas/image content onto
// this display's render buffer had a confirmed bug under this rotation
// setup — imageCanvasBuf itself was verified correct via serial logging
// right up until LVGL composited it, so the corruption happened
// specifically in that composite step. Text labels were unaffected (a
// different LVGL draw path). Rather than keep fighting LVGL's internal
// tiling/caching, this writes photo pixels straight to the panel's GRAM
// via the same CASET/RASET/WRITE_MEMORY_START sequence flush_cb() uses
// internally, computed by hand for the fixed IMAGE_AREA rectangle.
//
// THE TRANSFORM: direct, no axis transpose — native_x=lx, native_y=ly.
// Determined empirically on the bench (LVGL's own documented rotation
// formula suggested a transposed mapping that turned out NOT to match
// this panel's real CASET/RASET addressing convention) — see the
// README's Open Items for how this and the byte-swap below were found.
void writeImageNativeBufToPanel() {
  // Flush any PENDING LVGL redraw first. Under
  // LV_DISPLAY_RENDER_MODE_FULL, an lv_obj_invalidate() call (e.g. from
  // showLogo()/handleSetText(), just before this is typically called)
  // doesn't necessarily flush synchronously — if it's still pending when
  // we write directly to the panel below, LVGL's own later flush would
  // stomp this write right after it lands, since LVGL has no idea this
  // region was touched outside its own buffer. reassertPhotoIfAny()
  // already did this for the button-press case; every direct-SPI write
  // needs the same guarantee, not just reasserts.
  lv_timer_handler();

  int x_start = 0 + PANEL_GAP_X;
  int x_end = (IMAGE_AREA_WIDTH - 1) + PANEL_GAP_X;
  int y_start = 0 + PANEL_GAP_Y;
  int y_end = (DISPLAY_HEIGHT - 1) + PANEL_GAP_Y;

  uint8_t casetData[4] = {
      (uint8_t)((x_start >> 8) & 0xFF), (uint8_t)(x_start & 0xFF),
      (uint8_t)((x_end >> 8) & 0xFF), (uint8_t)(x_end & 0xFF),
  };
  uint8_t rasetData[4] = {
      (uint8_t)((y_start >> 8) & 0xFF), (uint8_t)(y_start & 0xFF),
      (uint8_t)((y_end >> 8) & 0xFF), (uint8_t)(y_end & 0xFF),
  };
  uint8_t casetCmd = 0x2A;  // LV_LCD_CMD_SET_COLUMN_ADDRESS
  uint8_t rasetCmd = 0x2B;  // LV_LCD_CMD_SET_PAGE_ADDRESS
  uint8_t writeCmd = 0x2C;  // LV_LCD_CMD_WRITE_MEMORY_START

  nv3007_spi_write(&casetCmd, 1, casetData, 4);
  nv3007_spi_write(&rasetCmd, 1, rasetData, 4);
  nv3007_spi_write(&writeCmd, 1, (const uint8_t *)imageNativeBuf,
                    (size_t)IMAGE_AREA_WIDTH * DISPLAY_HEIGHT * 2);
}

void sendImageAreaDirect() {
  for (int cy = 0; cy < DISPLAY_HEIGHT; cy++) {
    for (int cx = 0; cx < IMAGE_AREA_WIDTH; cx++) {
      uint16_t px = imageStagingBuf[cy * IMAGE_AREA_WIDTH + cx];
      // Byte-swapped, not a straight copy — a plain little-endian
      // RGB565 value comes out wrong on screen via this direct-SPI
      // path (bench-confirmed: solid red rendered as solid blue), while
      // the same value byte-swapped renders correctly. LVGL's own
      // flush_cb() path (used for text/logo) apparently already emits
      // pixels in this swapped order — probably why
      // LV_DRAW_SW_SUPPORT_RGB565_SWAPPED is enabled in lv_conf.h — but
      // going straight to nv3007_spi_write() here bypasses whatever
      // step normally does that conversion, so it's done by hand.
      imageNativeBuf[cy * IMAGE_AREA_WIDTH + cx] = (uint16_t)((px << 8) | (px >> 8));
    }
  }
  // White border on IMAGE_AREA's true panel edges (top, bottom, left) —
  // see BORDER_THICKNESS's comment. 0xFFFF (white) doesn't need
  // byte-swapping — both bytes are identical. The right edge
  // (cx == IMAGE_AREA_WIDTH-1) is the seam with TEXT_AREA, not a real
  // panel edge, so it's deliberately left uncolored.
  for (int b = 0; b < BORDER_THICKNESS; b++) {
    for (int cx = 0; cx < IMAGE_AREA_WIDTH; cx++) {
      imageNativeBuf[b * IMAGE_AREA_WIDTH + cx] = 0xFFFF;
      imageNativeBuf[(DISPLAY_HEIGHT - 1 - b) * IMAGE_AREA_WIDTH + cx] = 0xFFFF;
    }
    for (int cy = 0; cy < DISPLAY_HEIGHT; cy++) {
      imageNativeBuf[cy * IMAGE_AREA_WIDTH + b] = 0xFFFF;
    }
  }
  writeImageNativeBufToPanel();
}

// Re-sends the current photo (if any) directly to the panel — call
// after anything that might have triggered an LVGL full-panel refresh.
// Under LV_DISPLAY_RENDER_MODE_FULL, ANY invalidate anywhere redraws
// the WHOLE screen, and our direct-SPI photo lives outside LVGL's own
// view of the canvas — it would otherwise get overwritten by whatever
// is (or isn't) in imageCanvasBuf's IMAGE_AREA region the next time
// that happens (e.g. a quantity button press updating a label).
// writeImageNativeBufToPanel() (called via sendImageAreaDirect() below)
// already flushes any pending LVGL redraw first, so it can't race with
// one of our own writes here.
void reassertPhotoIfAny() {
  if (!hasPhotoForCurrentItem) return;
  sendImageAreaDirect();
}

void setupDisplay() {
  // Must happen before lv_nv3007_create() below — that call immediately
  // starts sending the panel's init command sequence over SPI via
  // nv3007_send_cmd(), so SPI and the CS/DC pins need to already be
  // live.
  pinMode(TFT_CS, OUTPUT);
  pinMode(TFT_DC, OUTPUT);
  digitalWrite(TFT_CS, HIGH);
  pinMode(TFT_BL, OUTPUT);
  digitalWrite(TFT_BL, HIGH);  // on by default at boot
  Serial.println("setupDisplay: pins set");
  // MISO unused — this panel is write-only (no read-back), so -1.
  SPI.begin(TFT_SCK, -1, TFT_MOSI, -1);
  Serial.println("setupDisplay: SPI.begin done");

  lv_init();
  // Required: with LV_USE_OS == LV_OS_NONE (lv_conf.h's default),
  // LVGL's internal tick counter never advances unless something drives
  // it. Without this, anything that waits on LVGL's clock (used
  // repeatedly during lv_nv3007_create()'s own init sequence below)
  // blocks forever with no crash or timeout.
  lv_tick_set_cb(lvTickSource);
  Serial.println("setupDisplay: lv_init done");

  // Created at the panel's NATIVE (portrait) resolution — this is what
  // actually gets clocked out over SPI to the glass, which doesn't care
  // about our logical landscape layout.
  display = lv_nv3007_create(PANEL_NATIVE_WIDTH, PANEL_NATIVE_HEIGHT, LV_LCD_FLAG_NONE,
                              nv3007_send_cmd, nv3007_send_color);
  Serial.println("setupDisplay: lv_nv3007_create done");
  lv_display_set_default(display);

  // REQUIRED: lv_nv3007_create()/lv_lcd_generic_mipi_create() register
  // flush_cb but never call lv_display_set_buffers(), so LVGL has no
  // render/draw buffer to compose into and nothing ever reaches the
  // panel via the normal LVGL draw path. One full-frame PSRAM buffer,
  // single-buffered (buf2 = NULL) and LV_DISPLAY_RENDER_MODE_FULL —
  // this panel is small (142x428x2 = ~119KB) and mostly idle/static, so
  // there's no need for partial/double-buffering.
  size_t displayBufSize = (size_t)PANEL_NATIVE_WIDTH * PANEL_NATIVE_HEIGHT * 2;
  void *displayBuf = ps_malloc(displayBufSize);
  lv_display_set_buffers(display, displayBuf, NULL, displayBufSize, LV_DISPLAY_RENDER_MODE_FULL);
  Serial.println("setupDisplay: lv_display_set_buffers done");

  // Gap compensation for a hardware quirk on this unit: the visible
  // glass doesn't start exactly at GRAM address 0 on either axis.
  // PANEL_GAP_X/Y (see their #define comments) were found via a bench
  // sweep of candidate values against real content, independently per
  // axis (x_gap only affects CASET/column addressing, y_gap only
  // affects RASET/row addressing) — see the README's Open Items for
  // the full history, including the later re-tuning that came with the
  // direct-SPI photo rewrite.
  lv_nv3007_set_gap(display, PANEL_GAP_X, PANEL_GAP_Y);

  // The reference TFT_eSPI config for this panel sets TFT_INVERSION_ON;
  // bench-tested backwards on this unit (true produced a solid black
  // screen instead of the expected mostly-white logo), so left false.
  lv_nv3007_set_invert(display, false);

  // Rotates LVGL's own compositing so everything downstream — labels,
  // canvas, the /image byte-count check — can just work in landscape
  // (DISPLAY_WIDTH x DISPLAY_HEIGHT) without knowing the panel is
  // physically wired portrait. LVGL's software rotation, not a
  // hardware/MADCTL rotation. _270 was confirmed correct (vs. _90, a
  // full 180° flip of it) for the case design in use — don't try to get
  // a different orientation by overriding individual MADCTL mirror bits
  // via lv_lcd_generic_mipi_set_address_mode() instead of switching the
  // whole rotation value; that was bench-confirmed to cause visible
  // tearing on this panel.
  lv_display_set_rotation(display, LV_DISPLAY_ROTATION_270);

  lv_obj_t *screen = lv_display_get_screen_active(display);

  // Created FIRST, and sized to the FULL DISPLAY_WIDTH x DISPLAY_HEIGHT
  // rather than just IMAGE_AREA_WIDTH — LVGL draws objects in creation
  // order, so this sits behind the three labels created after it below.
  // Kept full-canvas so the idle logo (sized for the full frame) can use
  // the whole panel with no competing text on top (handleClear() leaves
  // all three labels empty) — only the photo (via direct SPI, see
  // sendImageAreaDirect()) actually confines itself to IMAGE_AREA.
  imageCanvas = lv_canvas_create(screen);
  size_t canvasBufSize = lv_canvas_buf_size(DISPLAY_WIDTH, DISPLAY_HEIGHT, 16, LV_DRAW_BUF_STRIDE_ALIGN);
  imageCanvasBuf = (uint16_t *)ps_malloc(canvasBufSize);
  lv_canvas_set_buffer(imageCanvas, imageCanvasBuf, DISPLAY_WIDTH, DISPLAY_HEIGHT, LV_COLOR_FORMAT_RGB565);

  // Staging buffer for incoming /image bytes, in LOGICAL row-major
  // order — accumulated here first (see ImageRawHandler below) since
  // HTTP chunk boundaries don't align with image rows. Also serves as
  // the persistent "last received photo" cache for reassertPhotoIfAny().
  imageStagingBuf = (uint16_t *)ps_malloc((size_t)IMAGE_AREA_WIDTH * DISPLAY_HEIGHT * 2);
  // Same pixel count, reordered into NATIVE row-major order by
  // sendImageAreaDirect().
  imageNativeBuf = (uint16_t *)ps_malloc((size_t)IMAGE_AREA_WIDTH * DISPLAY_HEIGHT * 2);

  // Left-aligned within TEXT_AREA (flush against TEXT_AREA_X, the edge
  // adjacent to IMAGE_AREA — see that #define's comment). montserrat_20,
  // not the smaller LV_FONT_DEFAULT — LVGL fonts must be compiled in
  // ahead of time (LV_FONT_MONTSERRAT_20 1 in lv_conf.h). y-offsets are
  // budgeted against DISPLAY_HEIGHT (142): location near the top, item
  // name given room to wrap to 2 lines without colliding with quantity
  // at the bottom — a genuinely long item name can still overflow this
  // fixed budget.
  labelLocation = lv_label_create(screen);
  lv_obj_set_style_text_font(labelLocation, &lv_font_montserrat_20, 0);
  lv_obj_set_pos(labelLocation, TEXT_AREA_X + 8, 4);

  labelItemName = lv_label_create(screen);
  lv_obj_set_style_text_font(labelItemName, &lv_font_montserrat_20, 0);
  lv_label_set_long_mode(labelItemName, LV_LABEL_LONG_WRAP);
  lv_obj_set_width(labelItemName, TEXT_AREA_WIDTH - 16);
  lv_obj_set_pos(labelItemName, TEXT_AREA_X + 8, 30);

  labelQuantity = lv_label_create(screen);
  lv_obj_set_style_text_font(labelQuantity, &lv_font_montserrat_20, 0);
  lv_obj_set_pos(labelQuantity, TEXT_AREA_X + 8, DISPLAY_HEIGHT - 28);
}

void redrawText() {
  lv_label_set_text(labelLocation, currentLocation.c_str());
  lv_label_set_text(labelItemName, currentItemName.c_str());
  if (hasItem) {
    lv_label_set_text_fmt(labelQuantity, "Qty: %ld", stagedQuantity);
  } else {
    lv_label_set_text(labelQuantity, "");
  }
}

// Idle-screen logo — shown on boot and after /clear (including the
// acknowledge flow's clear, see display_ack_automation.yaml). Baked
// into firmware so it doesn't depend on Home Assistant/network being
// reachable. See logo.h for the real artwork and how to regenerate it.
void showLogo() {
  // LOGO_RGB565 (logo_data.h) is exactly LOGO_WIDTH x LOGO_HEIGHT ==
  // DISPLAY_WIDTH x DISPLAY_HEIGHT, matching imageCanvasBuf's size
  // exactly — a single direct copy, no tiling/scaling needed. PROGMEM
  // is a plain memory-mapped flash read on ESP32, so a bare memcpy
  // works without pgm_read_* accessors.
  memcpy(imageCanvasBuf, LOGO_RGB565, (size_t)LOGO_WIDTH * LOGO_HEIGHT * 2);
  // Canvas content changed via a raw buffer write, not an "official"
  // LVGL draw API — this may not otherwise invalidate whatever cached/
  // pre-processed version of the image LVGL is using to render.
  lv_image_cache_drop(NULL);
  lv_obj_invalidate(imageCanvas);
}

// ---- HTTP handlers ----

void handleSetText() {
  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, server.arg("plain"));
  if (err) {
    server.send(400, "text/plain", "bad json");
    return;
  }

  currentLocation = doc["location"].as<String>();
  currentItemName = doc["item_name"].as<String>();
  currentItemId = doc["item_id"].as<String>();
  committedQuantity = doc["quantity"].as<long>();
  stagedQuantity = committedQuantity;
  hasItem = currentItemId.length() > 0;
  // Navigating to a new item invalidates whatever photo was last sent
  // direct-to-panel for the PREVIOUS item — the fill-black below wipes
  // IMAGE_AREA in imageCanvasBuf same as always, but that canvas write
  // is now cosmetic-only for photos (see sendImageAreaDirect()); this
  // flag is what actually stops reassertPhotoIfAny() from redrawing a
  // stale photo over the new item's blank area on the next button press.
  hasPhotoForCurrentItem = false;

  redrawText();
  // Blank the WHOLE canvas (both the text backdrop behind TEXT_AREA and
  // the photo in IMAGE_AREA), not just IMAGE_AREA — otherwise navigating
  // from the idle logo (which fills the entire canvas) straight to an
  // item would leave old logo artwork showing behind the new text.
  lv_canvas_fill_bg(imageCanvas, lv_color_black(), LV_OPA_COVER);
  // White border along TEXT_AREA's own true panel edges (top, bottom,
  // right) — see BORDER_THICKNESS's comment. TEXT_AREA_X (the left
  // edge) is the seam with IMAGE_AREA, not a real panel edge, so it's
  // deliberately left uncolored — see sendImageAreaDirect() for
  // IMAGE_AREA's own matching border on its true edges.
  for (int b = 0; b < BORDER_THICKNESS; b++) {
    for (int x = TEXT_AREA_X; x < DISPLAY_WIDTH; x++) {
      imageCanvasBuf[b * DISPLAY_WIDTH + x] = 0xFFFF;
      imageCanvasBuf[(DISPLAY_HEIGHT - 1 - b) * DISPLAY_WIDTH + x] = 0xFFFF;
    }
    for (int y = 0; y < DISPLAY_HEIGHT; y++) {
      imageCanvasBuf[y * DISPLAY_WIDTH + (DISPLAY_WIDTH - 1 - b)] = 0xFFFF;
    }
  }
  lv_image_cache_drop(NULL);  // see showLogo()'s comment on this call
  lv_obj_invalidate(imageCanvas);

  server.send(200, "text/plain", "ok");
}

// /image needs a real RequestHandler subclass rather than a plain
// server.on() callback: server.arg("plain") builds its String via
// `String(plainBuf)`, a null-terminated C-string construction — any
// RGB565 pixel exactly 0x0000 (plain black) truncates the "body" there
// before the handler ever runs (bench-confirmed, not theoretical).
// canRaw()/raw() is WebServer's actual mechanism for receiving a binary
// body without going through String at all — it streams in
// HTTP_RAW_BUFLEN-byte chunks via raw(), with handle() called afterward
// once the whole body has arrived.
class ImageRawHandler : public RequestHandler {
 public:
  bool canHandle(HTTPMethod method, String uri) override {
    return method == HTTP_POST && uri == "/image";
  }

  bool canRaw(String uri) override {
    return uri == "/image";
  }

  void raw(WebServer &srv, String uri, HTTPRaw &rawData) override {
    switch (rawData.status) {
      case RAW_START:
        receivedBytes = 0;
        expectedBytes = (size_t)IMAGE_AREA_WIDTH * DISPLAY_HEIGHT * 2;
        break;
      case RAW_WRITE: {
        // Written into imageStagingBuf (tightly packed, exactly
        // IMAGE_AREA_WIDTH*DISPLAY_HEIGHT*2 bytes), not directly into
        // imageCanvasBuf — imageCanvasBuf is wider (DISPLAY_WIDTH, for
        // TEXT_AREA plus IMAGE_AREA), so a linear copy would run each
        // incoming row past its intended slot. sendImageAreaDirect()
        // handles placement once the full image has arrived. Arrives as
        // a single chunk in practice — see HTTP_RAW_BUFLEN's comment in
        // platformio.ini — but this still tolerates smaller chunks
        // correctly regardless.
        size_t remaining = (receivedBytes < expectedBytes) ? (expectedBytes - receivedBytes) : 0;
        size_t toCopy = min((size_t)rawData.currentSize, remaining);
        if (toCopy > 0) {
          memcpy((uint8_t *)imageStagingBuf + receivedBytes, rawData.buf, toCopy);
          receivedBytes += toCopy;
        }
        break;
      }
      case RAW_END:
      case RAW_ABORTED:
      default:
        break;
    }
  }

  bool handle(WebServer &srv, HTTPMethod method, String uri) override {
    if (method != HTTP_POST || uri != "/image") return false;

    if (receivedBytes != expectedBytes) {
      srv.send(400, "text/plain", "unexpected size");
      return true;
    }

    // Photos bypass LVGL's canvas/image compositing entirely — see
    // sendImageAreaDirect()'s comment for why.
    sendImageAreaDirect();
    hasPhotoForCurrentItem = true;
    srv.send(200, "text/plain", "ok");
    return true;
  }

 private:
  size_t receivedBytes = 0;
  size_t expectedBytes = 0;
};

ImageRawHandler imageRawHandler;

void handleClear() {
  currentItemId = "";
  currentLocation = "";
  currentItemName = "";
  committedQuantity = 0;
  stagedQuantity = 0;
  hasItem = false;
  hasPhotoForCurrentItem = false;
  redrawText();
  showLogo();
  server.send(200, "text/plain", "ok");
}

// {"on": true|false} — HA calls this on the same idle timeout as
// wled_power_off, to save backlight lifespan while the display isn't
// being looked at anyway. Only touches the backlight GPIO, not any
// display state — whatever was on screen (item, logo) is still there,
// just literally invisible, and turning the backlight back on doesn't
// need a fresh /text call to show it again.
void handleSetBacklight() {
  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, server.arg("plain"));
  if (err) {
    server.send(400, "text/plain", "bad json");
    return;
  }
  digitalWrite(TFT_BL, doc["on"].as<bool>() ? HIGH : LOW);
  server.send(200, "text/plain", "ok");
}

// ---- Buttons ----

void sendAcknowledge() {
  if (!hasItem) return;

  HTTPClient http;
  http.begin(HA_ACK_WEBHOOK_URL);
  http.addHeader("Content-Type", "application/json");

  JsonDocument doc;
  doc["item_id"] = currentItemId;
  doc["quantity"] = stagedQuantity;
  String body;
  serializeJson(doc, body);

  http.POST(body);
  http.end();

  // Local state clears immediately for a responsive feel; HA's ack
  // automation will also send an explicit /clear, which is fine — this
  // just avoids waiting on that round trip before the display resets.
  handleClear();
}

void pollButtons() {
  unsigned long now = millis();

  // Hold-to-repeat: fires again every DEBOUNCE_MS for as long as the
  // button stays held, which also happens to absorb contact bounce
  // (a few ms to a few tens of ms, well under DEBOUNCE_MS) since bounce
  // can only ever trigger an *extra* repeat within a window that would
  // have re-fired anyway.
  if (digitalRead(BUTTON_INCREASE_PIN) == LOW && now - lastIncreaseMs >= DEBOUNCE_MS) {
    stagedQuantity++;
    redrawText();
    // redrawText()'s label update triggers a RENDER_MODE_FULL reflush of
    // the WHOLE panel (see reassertPhotoIfAny()'s comment) — without
    // this, a button press right after a photo loaded would blank it.
    reassertPhotoIfAny();
    lastIncreaseMs = now;
  }
  if (digitalRead(BUTTON_DECREASE_PIN) == LOW && now - lastDecreaseMs >= DEBOUNCE_MS) {
    if (stagedQuantity > 0) stagedQuantity--;
    redrawText();
    reassertPhotoIfAny();
    lastDecreaseMs = now;
  }

  // Edge-triggered, unlike the two above: sendAcknowledge() PATCHes
  // Homebox, so holding this button down must fire it once, not
  // repeatedly. Requires the pin to actually change state since the
  // last accepted edge, and DEBOUNCE_MS to have passed, so contact
  // bounce around the press/release transition can't be read as
  // multiple edges.
  int ackState = digitalRead(BUTTON_ACK_PIN);
  if (ackState != lastAckState && now - lastAckMs >= DEBOUNCE_MS) {
    lastAckState = ackState;
    lastAckMs = now;
    if (ackState == LOW) {
      sendAcknowledge();
    }
  }
}

// ---- Setup / loop ----

void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println("homebox_display: booting");

  pinMode(BUTTON_INCREASE_PIN, INPUT_PULLUP);
  pinMode(BUTTON_DECREASE_PIN, INPUT_PULLUP);
  pinMode(BUTTON_ACK_PIN, INPUT_PULLUP);

  setupDisplay();
  redrawText();
  showLogo();
  Serial.println("homebox_display: display init done");

  // Must be set before WiFi.begin() — it's sent as part of the DHCP
  // request, so setting it after connecting has no effect until the
  // next reconnect. Shows up as the device name in most routers'
  // DHCP client lists/admin UIs, instead of a generic
  // "espressif"/MAC-address entry.
  WiFi.setHostname("homebox-display");
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  Serial.print("homebox_display: connecting to WiFi");
  while (WiFi.status() != WL_CONNECTED) {
    delay(250);
    Serial.print(".");
  }
  Serial.println();
  Serial.print("homebox_display: WiFi connected, IP=");
  Serial.println(WiFi.localIP());

  // WiFi modem-sleep (power-save) is ON by default on the Arduino ESP32
  // core — the radio periodically dozes between beacon intervals to save
  // power, which can add latency to network I/O. Tried as a fix for a
  // real ~5s-per-/image-POST stall (turned out NOT to be the cause —
  // see HTTP_RAW_BUFLEN's comment in platformio.ini for the actual root
  // cause and fix) but left disabled anyway: this device is
  // mains-powered with no battery-life reason to save power, so there's
  // no downside to disabling it regardless.
  WiFi.setSleep(false);

  server.on("/text", HTTP_POST, handleSetText);
  server.on("/clear", HTTP_POST, handleClear);
  server.on("/backlight", HTTP_POST, handleSetBacklight);
  // Not server.on() — see ImageRawHandler's own comment for why /image
  // needs the raw-body streaming path instead.
  server.addHandler(&imageRawHandler);
  server.begin();
  Serial.println("homebox_display: HTTP server started on port 80");
}

void loop() {
  server.handleClient();
  pollButtons();
  lv_timer_handler();
}

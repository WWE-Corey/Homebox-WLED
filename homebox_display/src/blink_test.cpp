// Board bring-up test for the Hosyond ESP32-S3 N16R8.
//
// Confirmed via bench debugging: GPIO 26-32 is the flash SPI bus on
// every ESP32-S3-WROOM-1 module (not just PSRAM variants), GPIO 33-37
// is additionally reserved for octal PSRAM on this N16R8 board, and
// GPIO 22-25 don't exist on the ESP32-S3 chip at all. See
// platformio.ini's [env] comment block for the full writeup. Run with:
//   pio run -e bringup -t upload
#include <Arduino.h>

#define BUTTON_INCREASE_PIN 4
#define BUTTON_DECREASE_PIN 6
#define BUTTON_ACK_PIN      7

void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println("=== bring-up test ===");

  Serial.printf("Chip model: %s, revision %d\n", ESP.getChipModel(), ESP.getChipRevision());
  Serial.printf("CPU freq: %d MHz\n", ESP.getCpuFreqMHz());
  Serial.printf("Flash size: %d MB\n", ESP.getFlashChipSize() / (1024 * 1024));
  Serial.printf("Free heap: %d\n", ESP.getFreeHeap());

  Serial.printf("psramFound() = %d\n", psramFound());
  Serial.printf("PSRAM size: %d\n", ESP.getPsramSize());
  Serial.printf("Free psram: %d\n", ESP.getFreePsram());

  pinMode(BUTTON_INCREASE_PIN, INPUT_PULLUP);
  pinMode(BUTTON_DECREASE_PIN, INPUT_PULLUP);
  pinMode(BUTTON_ACK_PIN, INPUT_PULLUP);
  Serial.printf("btn_inc=%d btn_dec=%d btn_ack=%d\n",
    digitalRead(BUTTON_INCREASE_PIN), digitalRead(BUTTON_DECREASE_PIN), digitalRead(BUTTON_ACK_PIN));

  Serial.println("=== bring-up test: all checks survived ===");
}

void loop() {
  delay(1000);
  Serial.printf("tick btn_inc=%d btn_dec=%d btn_ack=%d\n",
    digitalRead(BUTTON_INCREASE_PIN), digitalRead(BUTTON_DECREASE_PIN), digitalRead(BUTTON_ACK_PIN));
}

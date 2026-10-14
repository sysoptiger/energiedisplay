// Küchendisplay – Schritt 1: Testbild
// Prüft Display (Farbbalken) und Touch (Finger malt Punkte).
// Board: Waveshare ESP32-S3-Touch-LCD-7, Rev 1.2

#include <Arduino.h>
#include <Wire.h>
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_rgb.h"

static const int W = 800;
static const int H = 480;

// I2C: Touch (GT911) und IO-Expander (CH422G)
static const int PIN_SDA = 8;
static const int PIN_SCL = 9;
static const int PIN_TOUCH_INT = 4;

// CH422G: eigene I2C-Adressen je Funktion
static const uint8_t CH422_MODE = 0x24;
static const uint8_t CH422_OUT  = 0x38;
// Ausgänge: EXIO1 Touch-Reset, EXIO2 Hintergrundlicht, EXIO3 LCD-Reset, EXIO5 USB-Auswahl
static const uint8_t EXIO_TP_RST = 1 << 1;
static const uint8_t EXIO_BL     = 1 << 2;
static const uint8_t EXIO_LCD_RST = 1 << 3;
static const uint8_t EXIO_USB_SEL = 1 << 5;

static const uint8_t GT911_ADDR = 0x5D;

static esp_lcd_panel_handle_t panel = nullptr;
static uint16_t *zeile = nullptr;   // Puffer für eine Bildzeile

// ---------- IO-Expander ----------

static void ch422Write(uint8_t addr, uint8_t value) {
  Wire.beginTransmission(addr);
  Wire.write(value);
  Wire.endTransmission();
}

static void initExpanderUndTouchReset() {
  ch422Write(CH422_MODE, 0x01);  // IO0-7 als Ausgang
  // Touch-Reset aktiv, INT low -> GT911 startet mit Adresse 0x5D
  ch422Write(CH422_OUT, EXIO_BL | EXIO_LCD_RST | EXIO_USB_SEL);
  pinMode(PIN_TOUCH_INT, OUTPUT);
  digitalWrite(PIN_TOUCH_INT, LOW);
  delay(100);
  ch422Write(CH422_OUT, EXIO_TP_RST | EXIO_BL | EXIO_LCD_RST | EXIO_USB_SEL);
  delay(200);
  pinMode(PIN_TOUCH_INT, INPUT);
}

// ---------- Display ----------

static void initDisplay() {
  esp_lcd_rgb_panel_config_t cfg = {};
  cfg.clk_src = LCD_CLK_SRC_PLL160M;
  cfg.timings.pclk_hz = 16 * 1000 * 1000;
  cfg.timings.h_res = W;
  cfg.timings.v_res = H;
  cfg.timings.hsync_pulse_width = 4;
  cfg.timings.hsync_back_porch = 8;
  cfg.timings.hsync_front_porch = 8;
  cfg.timings.vsync_pulse_width = 4;
  cfg.timings.vsync_back_porch = 8;
  cfg.timings.vsync_front_porch = 8;
  cfg.timings.flags.pclk_active_neg = 1;
  cfg.data_width = 16;
  cfg.psram_trans_align = 64;
  cfg.hsync_gpio_num = 46;
  cfg.vsync_gpio_num = 3;
  cfg.de_gpio_num = 5;
  cfg.pclk_gpio_num = 7;
  cfg.disp_gpio_num = -1;
  // RGB565: B3-B7, G2-G7, R3-R7
  const int daten[16] = {14, 38, 18, 17, 10,
                         39, 0, 45, 48, 47, 21,
                         1, 2, 42, 41, 40};
  for (int i = 0; i < 16; i++) cfg.data_gpio_nums[i] = daten[i];
  cfg.flags.fb_in_psram = 1;

  ESP_ERROR_CHECK(esp_lcd_new_rgb_panel(&cfg, &panel));
  ESP_ERROR_CHECK(esp_lcd_panel_reset(panel));
  ESP_ERROR_CHECK(esp_lcd_panel_init(panel));
}

static void fillRect(int x, int y, int w, int h, uint16_t farbe) {
  if (x < 0) { w += x; x = 0; }
  if (y < 0) { h += y; y = 0; }
  if (x + w > W) w = W - x;
  if (y + h > H) h = H - y;
  if (w <= 0 || h <= 0) return;
  for (int i = 0; i < w; i++) zeile[i] = farbe;
  for (int r = 0; r < h; r++) {
    esp_lcd_panel_draw_bitmap(panel, x, y + r, x + w, y + r + 1, zeile);
  }
}

static uint16_t rgb(uint8_t r, uint8_t g, uint8_t b) {
  return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
}

static void zeichneTestbild() {
  const uint16_t farben[8] = {
    rgb(255, 255, 255), rgb(255, 255, 0), rgb(0, 255, 255), rgb(0, 255, 0),
    rgb(255, 0, 255),   rgb(255, 0, 0),   rgb(0, 0, 255),   rgb(0, 0, 0)};
  const int bw = W / 8;
  for (int i = 0; i < 8; i++) fillRect(i * bw, 0, bw, H * 2 / 3, farben[i]);

  // Unteres Drittel: Graustufen-Verlauf
  for (int x = 0; x < W; x += 10) {
    uint8_t g = x * 255 / (W - 1);
    fillRect(x, H * 2 / 3, 10, H / 3, rgb(g, g, g));
  }

  // Rahmen: zeigt, ob das Bild vollständig und nicht verschoben ist
  const uint16_t rot = rgb(255, 0, 0);
  fillRect(0, 0, W, 3, rot);
  fillRect(0, H - 3, W, 3, rot);
  fillRect(0, 0, 3, H, rot);
  fillRect(W - 3, 0, 3, H, rot);
}

// ---------- Touch (GT911) ----------

static bool gt911Read(uint16_t reg, uint8_t *buf, size_t len) {
  Wire.beginTransmission(GT911_ADDR);
  Wire.write(reg >> 8);
  Wire.write(reg & 0xFF);
  if (Wire.endTransmission(false) != 0) return false;
  if (Wire.requestFrom((int)GT911_ADDR, (int)len) != (int)len) return false;
  for (size_t i = 0; i < len; i++) buf[i] = Wire.read();
  return true;
}

static void gt911Write(uint16_t reg, uint8_t value) {
  Wire.beginTransmission(GT911_ADDR);
  Wire.write(reg >> 8);
  Wire.write(reg & 0xFF);
  Wire.write(value);
  Wire.endTransmission();
}

static bool leseTouch(int &x, int &y) {
  uint8_t status;
  if (!gt911Read(0x814E, &status, 1)) return false;
  if (!(status & 0x80)) return false;
  bool gefunden = false;
  if ((status & 0x0F) > 0) {
    uint8_t p[4];
    if (gt911Read(0x8150, p, 4)) {
      x = p[0] | (p[1] << 8);
      y = p[2] | (p[3] << 8);
      gefunden = true;
    }
  }
  gt911Write(0x814E, 0);  // Status quittieren
  return gefunden;
}

// ---------- Ablauf ----------

void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.println();
  Serial.println("Kuechendisplay Testbild startet");
  Serial.printf("PSRAM: %u KB frei\n", (unsigned)(ESP.getFreePsram() / 1024));

  Wire.begin(PIN_SDA, PIN_SCL, 400000);
  initExpanderUndTouchReset();

  zeile = (uint16_t *)heap_caps_malloc(W * sizeof(uint16_t), MALLOC_CAP_DMA);
  initDisplay();
  zeichneTestbild();
  Serial.println("Testbild gezeichnet");

  uint8_t id[4] = {0};
  if (gt911Read(0x8140, id, 4)) {
    Serial.printf("Touch-Controller: GT%c%c%c%c\n", id[0], id[1], id[2], id[3]);
  } else {
    Serial.println("Touch-Controller antwortet nicht");
  }
}

void loop() {
  int x, y;
  if (leseTouch(x, y)) {
    Serial.printf("Touch x=%d y=%d\n", x, y);
    fillRect(x - 6, y - 6, 12, 12, rgb(0, 0, 0));
    fillRect(x - 4, y - 4, 8, 8, rgb(255, 255, 255));
  }
  delay(20);
}

// Energiedisplay – Hardware
// Board: Waveshare ESP32-S3-Touch-LCD-7, Rev 1.2
// Pinbelegung und Timings wie im Testbild (Schritt 1) erprobt.

#include "hardware.h"

#include <Arduino.h>
#include <Wire.h>
#include <lvgl.h>
#include "esp_heap_caps.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_rgb.h"

static const int W = 800;
static const int H = 480;

static const int PIN_SDA = 8;
static const int PIN_SCL = 9;
static const int PIN_TOUCH_INT = 4;

// CH422G: eigene I2C-Adressen je Funktion
static const uint8_t CH422_MODE = 0x24;
static const uint8_t CH422_OUT  = 0x38;
static const uint8_t EXIO_TP_RST  = 1 << 1;
static const uint8_t EXIO_BL      = 1 << 2;
static const uint8_t EXIO_LCD_RST = 1 << 3;
static const uint8_t EXIO_USB_SEL = 1 << 5;

static const uint8_t GT911_ADDR = 0x5D;

// LVGL zeichnet streifenweise in diesen Puffer. Er liegt im internen RAM,
// damit beim Zeichnen weniger über den PSRAM läuft: Der PSRAM liefert
// gleichzeitig das Bild ans Display, und wird er zu stark belastet,
// verrutscht das Bild kurz.
static const int PUFFER_ZEILEN = 40;

static esp_lcd_panel_handle_t panel = nullptr;
static uint8_t expander_aus = 0;    // aktueller Zustand der Expander-Ausgänge
static bool licht_an = true;
static bool weckwunsch = false;
static bool warte_auf_loslassen = false;
static lv_disp_draw_buf_t draw_buf;
static lv_disp_drv_t disp_drv;
static lv_indev_drv_t indev_drv;

// ---------- IO-Expander ----------

static void ch422Write(uint8_t addr, uint8_t value) {
  Wire.beginTransmission(addr);
  Wire.write(value);
  Wire.endTransmission();
}

static void initExpanderUndTouchReset() {
  ch422Write(CH422_MODE, 0x01);
  // Touch-Reset aktiv, INT low -> GT911 startet mit Adresse 0x5D
  ch422Write(CH422_OUT, EXIO_BL | EXIO_LCD_RST | EXIO_USB_SEL);
  pinMode(PIN_TOUCH_INT, OUTPUT);
  digitalWrite(PIN_TOUCH_INT, LOW);
  delay(100);
  expander_aus = EXIO_TP_RST | EXIO_BL | EXIO_LCD_RST | EXIO_USB_SEL;
  ch422Write(CH422_OUT, expander_aus);
  delay(200);
  pinMode(PIN_TOUCH_INT, INPUT);
}

// ---------- Display ----------

static void initPanel() {
  esp_lcd_rgb_panel_config_t cfg = {};
  cfg.clk_src = LCD_CLK_SRC_PLL160M;
  // 14 MHz (ca. 34 Bilder/s) statt 16 MHz: mehr Luft für den PSRAM
  cfg.timings.pclk_hz = 14 * 1000 * 1000;
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
  const int daten[16] = {14, 38, 18, 17, 10,
                         39, 0, 45, 48, 47, 21,
                         1, 2, 42, 41, 40};
  for (int i = 0; i < 16; i++) cfg.data_gpio_nums[i] = daten[i];
  cfg.flags.fb_in_psram = 1;

  ESP_ERROR_CHECK(esp_lcd_new_rgb_panel(&cfg, &panel));
  ESP_ERROR_CHECK(esp_lcd_panel_reset(panel));
  ESP_ERROR_CHECK(esp_lcd_panel_init(panel));
}

static void flush_cb(lv_disp_drv_t *drv, const lv_area_t *area, lv_color_t *pixel) {
  // Kopiert direkt in den Bildspeicher, ist danach also schon fertig
  esp_lcd_panel_draw_bitmap(panel, area->x1, area->y1,
                            area->x2 + 1, area->y2 + 1, pixel);
  lv_disp_flush_ready(drv);
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

static void touch_cb(lv_indev_drv_t *drv, lv_indev_data_t *data) {
  static lv_coord_t last_x = 0, last_y = 0;
  static bool gedrueckt = false;

  uint8_t status;
  if (gt911Read(0x814E, &status, 1) && (status & 0x80)) {
    gedrueckt = false;
    if ((status & 0x0F) > 0) {
      uint8_t p[4];
      if (gt911Read(0x8150, p, 4)) {
        last_x = p[0] | (p[1] << 8);
        last_y = p[2] | (p[3] << 8);
        gedrueckt = true;
      }
    }
    gt911Write(0x814E, 0);
  }

  // Bei dunklem Display weckt die Berührung nur. Bis der Finger wieder
  // losgelassen wird, geht nichts an LVGL, damit kein Knopf auslöst.
  if (gedrueckt && !licht_an) {
    weckwunsch = true;
    warte_auf_loslassen = true;
  }
  if (!gedrueckt) warte_auf_loslassen = false;

  data->state = (gedrueckt && !warte_auf_loslassen) ? LV_INDEV_STATE_PR : LV_INDEV_STATE_REL;
  data->point.x = last_x;
  data->point.y = last_y;
}

void hw_licht(bool an) {
  if (an == licht_an) return;
  licht_an = an;
  if (an) expander_aus |= EXIO_BL;
  else    expander_aus &= ~EXIO_BL;
  ch422Write(CH422_OUT, expander_aus);
}

bool hw_weckwunsch() {
  bool w = weckwunsch;
  weckwunsch = false;
  return w;
}

// ---------- Start ----------

void hw_init() {
  Wire.begin(PIN_SDA, PIN_SCL, 400000);
  initExpanderUndTouchReset();
  initPanel();

  size_t groesse = W * PUFFER_ZEILEN * sizeof(lv_color_t);
  lv_color_t *buf1 = (lv_color_t *)heap_caps_malloc(groesse, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  if (!buf1) buf1 = (lv_color_t *)heap_caps_malloc(groesse, MALLOC_CAP_SPIRAM);
  lv_disp_draw_buf_init(&draw_buf, buf1, nullptr, W * PUFFER_ZEILEN);

  lv_disp_drv_init(&disp_drv);
  disp_drv.hor_res = W;
  disp_drv.ver_res = H;
  disp_drv.flush_cb = flush_cb;
  disp_drv.draw_buf = &draw_buf;
  lv_disp_drv_register(&disp_drv);

  lv_indev_drv_init(&indev_drv);
  indev_drv.type = LV_INDEV_TYPE_POINTER;
  indev_drv.read_cb = touch_cb;
  lv_indev_drv_register(&indev_drv);
}

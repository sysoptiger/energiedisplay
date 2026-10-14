// Energiedisplay – Oberfläche
// Screen 1: Strompreise mit Ampel, Preiskurve und Heute/Morgen
// Screen 2: Anlage (PV, Haus, Netz, Akku, Steuerung)

#include "ui.h"

#include <lvgl.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

LV_FONT_DECLARE(font_16);
LV_FONT_DECLARE(font_22);
LV_FONT_DECLARE(font_40);
LV_FONT_DECLARE(font_72);

// ---------- Farben ----------

#define C_HINTERGRUND lv_color_hex(0x101418)
#define C_FLAECHE     lv_color_hex(0x1C2228)
#define C_TEXT        lv_color_hex(0xE8ECEF)
#define C_GEDIMMT     lv_color_hex(0x8A949C)
#define C_LINIE       lv_color_hex(0x3A434B)
#define C_GRUEN       lv_color_hex(0x2E9E55)
#define C_GELB        lv_color_hex(0xE8B71A)
#define C_ROT         lv_color_hex(0xC93C3C)
#define C_RUHE        lv_color_hex(0x2A3138)
#define C_OK          lv_color_hex(0x3DBE6A)
#define C_FEHLER      lv_color_hex(0xE5484D)

static const uint32_t RUECKKEHR_MS = 2 * 60 * 1000;
static const int DATEN_VERALTET_S = 10 * 60;

static const char *WOCHENTAG[] = {"Sonntag", "Montag", "Dienstag", "Mittwoch",
                                  "Donnerstag", "Freitag", "Samstag"};

// ---------- Zustand ----------

static PreisTag tag_heute, tag_morgen;
static Ampel ampel;
static Anlage anlage;
static bool zeige_morgen = false;
static time_t jetzt_ts = 0;
static bool zeit_ok = false;

// Screen 1
static lv_obj_t *scr_preise;
static lv_obj_t *lbl_kopf;
static lv_obj_t *lbl_uhr1;
static lv_obj_t *punkt1;
static lv_obj_t *karte;
static lv_obj_t *lbl_titel;
static lv_obj_t *lbl_text;
static lv_obj_t *lbl_preis;
static lv_obj_t *lbl_preis_einheit;
static lv_obj_t *kurve;
static lv_obj_t *btn_heute;
static lv_obj_t *btn_morgen;
static lv_obj_t *lbl_fuss;

// Screen 2
static lv_obj_t *scr_anlage;
static lv_obj_t *lbl_uhr2;
static lv_obj_t *punkt2;
static lv_obj_t *val_pv, *sub_pv;
static lv_obj_t *val_haus, *sub_haus;
static lv_obj_t *val_netz, *sub_netz;
static lv_obj_t *val_soc, *bar_soc;
static lv_obj_t *val_akku, *sub_akku;
static lv_obj_t *val_steuer, *sub_steuer;
static lv_obj_t *lbl_anlage_stand;

// Screen 3
static lv_obj_t *scr_info;
static lv_obj_t *lbl_info_titel;
static lv_obj_t *lbl_info_text;
static bool info_festhalten = false;

// ---------- Hilfen ----------

static lv_obj_t *label(lv_obj_t *parent, const lv_font_t *font, lv_color_t farbe,
                       const char *text = "") {
  lv_obj_t *l = lv_label_create(parent);
  lv_obj_set_style_text_font(l, font, 0);
  lv_obj_set_style_text_color(l, farbe, 0);
  lv_label_set_text(l, text);
  return l;
}

// Text nur setzen, wenn er sich ändert. Jedes Neuzeichnen kostet
// PSRAM-Bandbreite, und zu viel davon lässt das Bild zappeln.
static void setText(lv_obj_t *l, const char *t) {
  if (strcmp(lv_label_get_text(l), t) != 0) lv_label_set_text(l, t);
}

static void setFarbe(lv_obj_t *o, lv_style_prop_t prop, lv_color_t c) {
  lv_style_value_t v = lv_obj_get_style_prop(o, LV_PART_MAIN, prop);
  if (v.color.full != c.full) {
    lv_style_value_t neu;
    neu.color = c;
    lv_obj_set_local_style_prop(o, prop, neu, 0);
  }
}

static void zahl(char *buf, size_t n, float wert, int stellen = 1) {
  snprintf(buf, n, "%.*f", stellen, wert);
  for (char *p = buf; *p; p++) if (*p == '.') *p = ',';
}

// Dezimalpunkt zwischen Ziffern -> Komma ("18.1 ct" -> "18,1 ct")
static void komma(char *dst, size_t n, const char *src) {
  size_t i = 0;
  for (; src[i] && i + 1 < n; i++) {
    char c = src[i];
    if (c == '.' && i > 0 && src[i - 1] >= '0' && src[i - 1] <= '9' &&
        src[i + 1] >= '0' && src[i + 1] <= '9') c = ',';
    dst[i] = c;
  }
  dst[i] = 0;
}

static void hhmm(char *buf, size_t n, time_t t) {
  struct tm tm;
  localtime_r(&t, &tm);
  strftime(buf, n, "%H:%M", &tm);
}

static void watt(char *buf, size_t n, int w) {
  if (w < 0) w = -w;
  // Auf 10 W runden: ruhigere Anzeige, weniger Neuzeichnen
  w = (w + 5) / 10 * 10;
  if (w >= 1000) {
    char z[16];
    zahl(z, sizeof(z), w / 1000.0f, 2);
    snprintf(buf, n, "%s kW", z);
  } else {
    snprintf(buf, n, "%d W", w);
  }
}

static void knopfStil(lv_obj_t *btn, bool aktiv) {
  lv_obj_set_style_bg_color(btn, aktiv ? lv_color_hex(0x3A6EA5) : C_FLAECHE, 0);
}

static lv_obj_t *knopf(lv_obj_t *parent, const char *text, int x, int y, int w,
                       lv_event_cb_t cb) {
  lv_obj_t *b = lv_btn_create(parent);
  lv_obj_set_pos(b, x, y);
  lv_obj_set_size(b, w, 44);
  lv_obj_set_style_radius(b, 10, 0);
  lv_obj_set_style_shadow_width(b, 0, 0);
  knopfStil(b, false);
  lv_obj_t *l = label(b, &font_22, C_TEXT, text);
  lv_obj_set_style_text_color(l, C_TEXT, 0);
  lv_obj_center(l);
  lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, nullptr);
  return b;
}

static lv_obj_t *statusPunkt(lv_obj_t *parent) {
  lv_obj_t *p = lv_obj_create(parent);
  lv_obj_set_size(p, 12, 12);
  lv_obj_set_style_radius(p, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_border_width(p, 0, 0);
  lv_obj_set_style_bg_color(p, C_FEHLER, 0);
  lv_obj_align(p, LV_ALIGN_TOP_RIGHT, -96, 22);
  lv_obj_clear_flag(p, LV_OBJ_FLAG_CLICKABLE);
  return p;
}

static lv_obj_t *neuerScreen() {
  lv_obj_t *s = lv_obj_create(nullptr);
  lv_obj_set_style_bg_color(s, C_HINTERGRUND, 0);
  lv_obj_set_style_bg_opa(s, LV_OPA_COVER, 0);
  lv_obj_clear_flag(s, LV_OBJ_FLAG_SCROLLABLE);
  return s;
}

static const PreisTag &angezeigterTag() { return zeige_morgen ? tag_morgen : tag_heute; }

// ---------- Preiskurve ----------

static void rect(lv_draw_ctx_t *ctx, lv_coord_t x1, lv_coord_t y1, lv_coord_t x2,
                 lv_coord_t y2, lv_color_t farbe, lv_opa_t opa, lv_coord_t radius = 0) {
  if (x2 < x1 || y2 < y1) return;
  lv_draw_rect_dsc_t d;
  lv_draw_rect_dsc_init(&d);
  d.bg_color = farbe;
  d.bg_opa = opa;
  d.radius = radius;
  lv_area_t a = {x1, y1, x2, y2};
  lv_draw_rect(ctx, &d, &a);
}

static void text(lv_draw_ctx_t *ctx, lv_coord_t x1, lv_coord_t y1, lv_coord_t x2,
                 const char *t, lv_color_t farbe, lv_text_align_t align) {
  lv_draw_label_dsc_t d;
  lv_draw_label_dsc_init(&d);
  d.font = &font_16;
  d.color = farbe;
  d.align = align;
  lv_area_t a = {x1, y1, x2, (lv_coord_t)(y1 + 20)};
  lv_draw_label(ctx, &d, &a, t, nullptr);
}

static void kurve_zeichnen(lv_event_t *e) {
  lv_draw_ctx_t *ctx = lv_event_get_draw_ctx(e);
  lv_area_t c;
  lv_obj_get_coords(kurve, &c);

  const PreisTag &tag = angezeigterTag();
  if (!tag.da || tag.anzahl <= 0) {
    text(ctx, c.x1, (c.y1 + c.y2) / 2 - 10, c.x2, "Keine Preisdaten",
         C_GEDIMMT, LV_TEXT_ALIGN_CENTER);
    return;
  }

  // Zeichenfläche: links Platz für die ct-Skala, unten für die Uhrzeiten
  const lv_coord_t px = c.x1 + 40, pr = c.x2 - 4;
  const lv_coord_t py = c.y1 + 24, pb = c.y2 - 26;
  const int pw = pr - px + 1, ph = pb - py;
  const int n = tag.anzahl;

  float lo = fminf(0.0f, floorf(tag.min / 10.0f) * 10.0f);
  float hi = ceilf(tag.max / 10.0f) * 10.0f;
  if (hi - lo < 10) hi = lo + 10;
  const float schritt = (hi - lo) > 40 ? 20 : 10;

  auto yWert = [&](float v) -> lv_coord_t {
    return pb - (lv_coord_t)lroundf((v - lo) / (hi - lo) * ph);
  };
  auto xSlot = [&](int i) -> lv_coord_t { return px + (lv_coord_t)((long)i * pw / n); };

  // Rasterlinien mit Beschriftung
  char buf[24];
  for (float v = lo; v <= hi + 0.01f; v += schritt) {
    lv_coord_t y = yWert(v);
    rect(ctx, px, y, pr, y, C_LINIE, LV_OPA_COVER);
    snprintf(buf, sizeof(buf), "%d", (int)v);
    text(ctx, c.x1, y - 10, px - 8, buf, C_GEDIMMT, LV_TEXT_ALIGN_RIGHT);
  }
  text(ctx, c.x1, c.y1, c.x1 + 120, "ct/kWh", C_GEDIMMT, LV_TEXT_ALIGN_LEFT);

  // Bestes Fenster hinterlegen
  if (ampel.da && ampel.hat_fenster) {
    long a = (long)(ampel.fenster_start - tag.start) / 900;
    long b = (long)(ampel.fenster_ende - tag.start) / 900;
    if (b > 0 && a < n) {
      if (a < 0) a = 0;
      if (b > n) b = n;
      rect(ctx, xSlot(a), py - 4, xSlot(b) - 1, pb, lv_color_white(), LV_OPA_10, 6);
      text(ctx, xSlot(a) - 60, c.y1, xSlot(b) + 60, "bestes Fenster", C_TEXT,
           LV_TEXT_ALIGN_CENTER);
    }
  }

  // Balken je Viertelstunde, Farbe nach Drittel des Tages
  const lv_coord_t y0 = yWert(0);
  const bool luecke = (pw / n) >= 5;
  for (int i = 0; i < n; i++) {
    lv_color_t farbe = tag.stufe[i] == 0 ? C_GRUEN : (tag.stufe[i] == 1 ? C_GELB : C_ROT);
    bool vorbei = zeit_ok && !zeige_morgen && tag.start + (time_t)(i + 1) * 900 <= jetzt_ts;
    lv_coord_t x1 = xSlot(i), x2 = xSlot(i + 1) - (luecke ? 2 : 1);
    lv_coord_t y = yWert(tag.ct[i]);
    if (y <= y0) rect(ctx, x1, y, x2, y0, farbe, vorbei ? LV_OPA_30 : LV_OPA_COVER);
    else         rect(ctx, x1, y0, x2, y, farbe, vorbei ? LV_OPA_30 : LV_OPA_COVER);
  }

  // Uhrzeiten alle 3 Stunden
  for (int i = 0; i < n; i++) {
    time_t t = tag.start + (time_t)i * 900;
    struct tm tm;
    localtime_r(&t, &tm);
    if (tm.tm_min == 0 && tm.tm_hour % 3 == 0) {
      lv_coord_t x = xSlot(i);
      rect(ctx, x, pb + 1, x, pb + 4, C_GEDIMMT, LV_OPA_COVER);
      snprintf(buf, sizeof(buf), "%02d:00", tm.tm_hour);
      text(ctx, x - 30, pb + 6, x + 30, buf, C_GEDIMMT, LV_TEXT_ALIGN_CENTER);
    }
  }

  // Jetzt-Markierung
  if (zeit_ok && !zeige_morgen && jetzt_ts >= tag.start &&
      jetzt_ts < tag.start + (time_t)n * 900) {
    lv_coord_t x = px + (lv_coord_t)((jetzt_ts - tag.start) * pw / ((time_t)n * 900));
    rect(ctx, x - 1, py - 4, x + 1, pb, lv_color_white(), LV_OPA_COVER);
    text(ctx, x - 40, c.y1, x + 40, "jetzt", C_TEXT, LV_TEXT_ALIGN_CENTER);
  }
}

// ---------- Screen 1: Strompreise ----------

static void aktualisiereKopf() {
  const PreisTag &tag = angezeigterTag();
  char buf[64];
  if (tag.da) {
    struct tm tm;
    time_t t = tag.start + 6 * 3600;
    localtime_r(&t, &tm);
    snprintf(buf, sizeof(buf), "Strompreis %s · %s %d.%d.", zeige_morgen ? "morgen" : "heute",
             WOCHENTAG[tm.tm_wday], tm.tm_mday, tm.tm_mon + 1);
  } else {
    snprintf(buf, sizeof(buf), "Strompreis %s", zeige_morgen ? "morgen" : "heute");
  }
  setText(lbl_kopf, buf);
  knopfStil(btn_heute, !zeige_morgen);
  knopfStil(btn_morgen, zeige_morgen);
  // Morgen-Knopf erst, wenn die Preise für morgen da sind (meist ab ca. 13 Uhr)
  if (tag_morgen.da) lv_obj_clear_flag(btn_morgen, LV_OBJ_FLAG_HIDDEN);
  else               lv_obj_add_flag(btn_morgen, LV_OBJ_FLAG_HIDDEN);
}

static void aktualisiereFuss() {
  char buf[96];
  bool veraltet = zeit_ok && ampel.da && ampel.stand > 0 &&
                  jetzt_ts - ampel.stand > DATEN_VERALTET_S;
  if (veraltet) {
    char t[8];
    hhmm(t, sizeof(t), ampel.stand);
    snprintf(buf, sizeof(buf), "Keine neuen Daten vom QNAP seit %s", t);
    setFarbe(lbl_fuss, LV_STYLE_TEXT_COLOR, C_FEHLER);
  } else if (ampel.da && ampel.hat_fenster) {
    char z[16];
    zahl(z, sizeof(z), ampel.best_ct);
    snprintf(buf, sizeof(buf), "Bestes Fenster %s · %s ct", ampel.fenster_text, z);
    setFarbe(lbl_fuss, LV_STYLE_TEXT_COLOR, C_GEDIMMT);
  } else {
    buf[0] = 0;
  }
  setText(lbl_fuss, buf);
}

static void zeigeTag(bool morgen) {
  if (morgen && !tag_morgen.da) morgen = false;
  zeige_morgen = morgen;
  aktualisiereKopf();
  lv_obj_invalidate(kurve);
}

static void klick_heute(lv_event_t *) { zeigeTag(false); }
static void klick_morgen(lv_event_t *) { zeigeTag(true); }
static void klick_anlage(lv_event_t *) { lv_scr_load(scr_anlage); }
static void klick_zurueck(lv_event_t *) { lv_scr_load(scr_preise); }
static void klick_info(lv_event_t *) { lv_scr_load(scr_info); }
static void klick_info_zurueck(lv_event_t *) {
  info_festhalten = false;
  lv_scr_load(scr_anlage);
}

static void baueScreenPreise() {
  scr_preise = neuerScreen();

  lbl_kopf = label(scr_preise, &font_22, C_TEXT, "Strompreis heute");
  lv_obj_set_pos(lbl_kopf, 20, 14);
  lbl_uhr1 = label(scr_preise, &font_22, C_TEXT, "--:--");
  lv_obj_align(lbl_uhr1, LV_ALIGN_TOP_RIGHT, -20, 14);
  punkt1 = statusPunkt(scr_preise);

  karte = lv_obj_create(scr_preise);
  lv_obj_set_pos(karte, 16, 52);
  lv_obj_set_size(karte, 768, 150);
  lv_obj_set_style_radius(karte, 16, 0);
  lv_obj_set_style_border_width(karte, 0, 0);
  lv_obj_set_style_pad_all(karte, 0, 0);
  lv_obj_set_style_bg_color(karte, C_RUHE, 0);
  lv_obj_clear_flag(karte, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_clear_flag(karte, LV_OBJ_FLAG_CLICKABLE);

  lbl_titel = label(karte, &font_40, C_TEXT, "Warte auf Daten");
  lv_obj_set_pos(lbl_titel, 24, 20);
  lbl_text = label(karte, &font_22, C_TEXT, "");
  lv_obj_set_width(lbl_text, 500);
  lv_label_set_long_mode(lbl_text, LV_LABEL_LONG_WRAP);
  lv_obj_set_pos(lbl_text, 24, 80);

  lbl_preis = label(karte, &font_72, C_TEXT, "");
  lv_obj_align(lbl_preis, LV_ALIGN_TOP_RIGHT, -24, 14);
  lbl_preis_einheit = label(karte, &font_16, C_TEXT, "");
  lv_obj_align(lbl_preis_einheit, LV_ALIGN_BOTTOM_RIGHT, -24, -18);

  kurve = lv_obj_create(scr_preise);
  lv_obj_set_pos(kurve, 16, 210);
  lv_obj_set_size(kurve, 768, 210);
  lv_obj_set_style_bg_opa(kurve, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(kurve, 0, 0);
  lv_obj_clear_flag(kurve, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_clear_flag(kurve, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(kurve, kurve_zeichnen, LV_EVENT_DRAW_MAIN, nullptr);

  btn_heute = knopf(scr_preise, "Heute", 16, 428, 120, klick_heute);
  btn_morgen = knopf(scr_preise, "Morgen", 144, 428, 120, klick_morgen);
  lbl_fuss = label(scr_preise, &font_16, C_GEDIMMT, "");
  lv_obj_set_width(lbl_fuss, 330);
  lv_obj_set_style_text_align(lbl_fuss, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_set_pos(lbl_fuss, 276, 440);
  knopf(scr_preise, "Anlage  ›", 624, 428, 160, klick_anlage);

  aktualisiereKopf();
}

// ---------- Screen 2: Anlage ----------

static lv_obj_t *kachel(lv_obj_t *parent, int x, int y, const char *titel,
                        lv_obj_t **wert, lv_obj_t **sub) {
  lv_obj_t *k = lv_obj_create(parent);
  lv_obj_set_pos(k, x, y);
  lv_obj_set_size(k, 248, 168);
  lv_obj_set_style_radius(k, 16, 0);
  lv_obj_set_style_border_width(k, 0, 0);
  lv_obj_set_style_bg_color(k, C_FLAECHE, 0);
  lv_obj_set_style_pad_all(k, 18, 0);
  lv_obj_clear_flag(k, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_clear_flag(k, LV_OBJ_FLAG_CLICKABLE);
  label(k, &font_22, C_GEDIMMT, titel);
  *wert = label(k, &font_40, C_TEXT, "–");
  lv_obj_set_pos(*wert, 0, 44);
  if (sub) {
    *sub = label(k, &font_22, C_GEDIMMT, "");
    lv_obj_align(*sub, LV_ALIGN_BOTTOM_LEFT, 0, 0);
  }
  return k;
}

static void baueScreenAnlage() {
  scr_anlage = neuerScreen();

  label(scr_anlage, &font_22, C_TEXT, "Anlage");
  lv_obj_set_pos(lv_obj_get_child(scr_anlage, 0), 20, 14);
  lbl_uhr2 = label(scr_anlage, &font_22, C_TEXT, "--:--");
  lv_obj_align(lbl_uhr2, LV_ALIGN_TOP_RIGHT, -20, 14);
  punkt2 = statusPunkt(scr_anlage);

  kachel(scr_anlage, 16, 56, "PV", &val_pv, &sub_pv);
  kachel(scr_anlage, 276, 56, "Haus", &val_haus, &sub_haus);
  kachel(scr_anlage, 536, 56, "Netz", &val_netz, &sub_netz);
  lv_obj_t *k = kachel(scr_anlage, 16, 240, "Akku", &val_soc, nullptr);
  bar_soc = lv_bar_create(k);
  lv_obj_set_size(bar_soc, 212, 14);
  lv_obj_align(bar_soc, LV_ALIGN_BOTTOM_LEFT, 0, -4);
  lv_bar_set_range(bar_soc, 0, 100);
  lv_obj_set_style_bg_color(bar_soc, C_LINIE, LV_PART_MAIN);
  lv_obj_set_style_bg_color(bar_soc, C_GRUEN, LV_PART_INDICATOR);
  kachel(scr_anlage, 276, 240, "Akku-Leistung", &val_akku, &sub_akku);
  kachel(scr_anlage, 536, 240, "Steuerung", &val_steuer, &sub_steuer);
  lv_obj_set_style_text_font(val_steuer, &font_22, 0);
  lv_obj_set_width(val_steuer, 212);
  lv_label_set_long_mode(val_steuer, LV_LABEL_LONG_WRAP);

  knopf(scr_anlage, "‹  Strompreise", 16, 428, 220, klick_zurueck);
  knopf(scr_anlage, "Info", 252, 428, 120, klick_info);
  lbl_anlage_stand = label(scr_anlage, &font_16, C_GEDIMMT, "");
  lv_obj_align(lbl_anlage_stand, LV_ALIGN_BOTTOM_RIGHT, -20, -18);
}

// ---------- Screen 3: Info ----------

static void baueScreenInfo() {
  scr_info = neuerScreen();
  label(scr_info, &font_22, C_TEXT, "Energiedisplay · Info & Einstellungen");
  lv_obj_set_pos(lv_obj_get_child(scr_info, 0), 20, 14);

  lv_obj_t *k = lv_obj_create(scr_info);
  lv_obj_set_pos(k, 16, 56);
  lv_obj_set_size(k, 768, 356);
  lv_obj_set_style_radius(k, 16, 0);
  lv_obj_set_style_border_width(k, 0, 0);
  lv_obj_set_style_bg_color(k, C_FLAECHE, 0);
  lv_obj_set_style_pad_all(k, 24, 0);
  lv_obj_clear_flag(k, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_clear_flag(k, LV_OBJ_FLAG_CLICKABLE);

  lbl_info_titel = label(k, &font_22, C_TEXT, "");
  lv_obj_set_width(lbl_info_titel, 720);
  lv_label_set_long_mode(lbl_info_titel, LV_LABEL_LONG_WRAP);
  lbl_info_text = label(k, &font_22, C_GEDIMMT, "");
  lv_obj_set_width(lbl_info_text, 720);
  lv_label_set_long_mode(lbl_info_text, LV_LABEL_LONG_WRAP);
  lv_obj_set_style_text_line_space(lbl_info_text, 8, 0);
  lv_obj_set_pos(lbl_info_text, 0, 72);

  knopf(scr_info, "‹  Zurück", 16, 428, 220, klick_info_zurueck);
}

// ---------- Schnittstelle ----------

void ui_create() {
  baueScreenPreise();
  baueScreenAnlage();
  baueScreenInfo();
  lv_scr_load(scr_preise);
}

void ui_set_preise(const PreisTag &heute, const PreisTag &morgen) {
  tag_heute = heute;
  tag_morgen = morgen;
  if (zeige_morgen && !tag_morgen.da) zeige_morgen = false;
  aktualisiereKopf();
  lv_obj_invalidate(kurve);
}

void ui_set_ampel(const Ampel &a) {
  // Die Ampel kommt alle 30 s. Die Kurve nur neu zeichnen, wenn sich das
  // beste Fenster verschoben hat.
  bool fenster_neu = a.hat_fenster != ampel.hat_fenster ||
                     a.fenster_start != ampel.fenster_start ||
                     a.fenster_ende != ampel.fenster_ende;
  ampel = a;
  lv_color_t bg, fg;
  switch (a.farbe) {
    case 0: bg = C_GRUEN; fg = lv_color_white(); break;
    case 1: bg = C_GELB; fg = lv_color_hex(0x1A1A1A); break;
    case 2: bg = C_ROT; fg = lv_color_white(); break;
    case 3: bg = C_RUHE; fg = lv_color_hex(0xC5CCD2); break;
    default: bg = C_RUHE; fg = C_GEDIMMT; break;
  }
  setFarbe(karte, LV_STYLE_BG_COLOR, bg);
  setFarbe(lbl_titel, LV_STYLE_TEXT_COLOR, fg);
  setFarbe(lbl_text, LV_STYLE_TEXT_COLOR, fg);
  setFarbe(lbl_preis, LV_STYLE_TEXT_COLOR, fg);
  setFarbe(lbl_preis_einheit, LV_STYLE_TEXT_COLOR, fg);

  char buf[160];
  setText(lbl_titel, a.titel);
  komma(buf, sizeof(buf), a.text);
  setText(lbl_text, buf);
  if (a.hat_preis) {
    zahl(buf, sizeof(buf), a.preis_ct);
    setText(lbl_preis, buf);
    setText(lbl_preis_einheit, "ct/kWh jetzt");
  } else {
    setText(lbl_preis, "");
    setText(lbl_preis_einheit, "");
  }
  aktualisiereFuss();
  if (fenster_neu) lv_obj_invalidate(kurve);
}

void ui_set_anlage(const Anlage &a) {
  anlage = a;
  char buf[32];

  watt(buf, sizeof(buf), a.pv_w);
  setText(val_pv, buf);
  setText(sub_pv, a.pv_w > 10 ? "Erzeugung" : "keine Sonne");

  watt(buf, sizeof(buf), a.haus_w);
  setText(val_haus, buf);
  setText(sub_haus, "Verbrauch");

  watt(buf, sizeof(buf), a.netz_w);
  setText(val_netz, buf);
  setText(sub_netz, a.netz_w > 10 ? "Bezug" : (a.netz_w < -10 ? "Einspeisung" : "ausgeglichen"));
  setFarbe(val_netz, LV_STYLE_TEXT_COLOR, a.netz_w > 10 ? C_TEXT : C_OK);

  if (a.hat_soc) {
    snprintf(buf, sizeof(buf), "%d %%", (int)lroundf(a.soc));
    lv_bar_set_value(bar_soc, (int)lroundf(a.soc), LV_ANIM_OFF);
  } else {
    snprintf(buf, sizeof(buf), "–");
    lv_bar_set_value(bar_soc, 0, LV_ANIM_OFF);
  }
  setText(val_soc, buf);

  watt(buf, sizeof(buf), a.akku_w);
  setText(val_akku, buf);
  setText(sub_akku, a.akku_w > 10 ? "Entladen" : (a.akku_w < -10 ? "Laden" : "Ruht"));

  if (strcmp(a.steuerung, "LADEN") == 0) {
    setText(val_steuer, "Laden aus dem Netz");
    watt(buf, sizeof(buf), a.ladeleistung_w);
    setText(sub_steuer, buf);
  } else if (strcmp(a.steuerung, "HALTEN") == 0) {
    setText(val_steuer, "Akku wird gehalten");
    setText(sub_steuer, "für teurere Stunden");
  } else if (strcmp(a.steuerung, "AUTOMATIK") == 0) {
    setText(val_steuer, "Automatik");
    setText(sub_steuer, "Hoymiles regelt");
  } else {
    setText(val_steuer, a.steuerung[0] ? a.steuerung : "–");
    setText(sub_steuer, "");
  }
}

void ui_set_info(const char *titel, const char *text) {
  setText(lbl_info_titel, titel);
  setText(lbl_info_text, text);
}

void ui_info_zeigen(bool festhalten) {
  info_festhalten = festhalten;
  if (lv_scr_act() != scr_info) lv_scr_load(scr_info);
}

void ui_info_freigeben() { info_festhalten = false; }

void ui_tick(time_t jetzt, bool zeit_gueltig, bool wlan, bool mqtt) {
  bool minute_neu = zeit_gueltig && (jetzt / 60 != jetzt_ts / 60);
  jetzt_ts = jetzt;
  zeit_ok = zeit_gueltig;

  char buf[48];
  if (zeit_gueltig) hhmm(buf, sizeof(buf), jetzt);
  else snprintf(buf, sizeof(buf), "--:--");
  setText(lbl_uhr1, buf);
  setText(lbl_uhr2, buf);

  lv_color_t p = (wlan && mqtt) ? C_OK : C_FEHLER;
  setFarbe(punkt1, LV_STYLE_BG_COLOR, p);
  setFarbe(punkt2, LV_STYLE_BG_COLOR, p);

  if (anlage.da && anlage.stand > 0 && zeit_gueltig) {
    char t[8];
    hhmm(t, sizeof(t), anlage.stand);
    snprintf(buf, sizeof(buf), "Stand %s", t);
    setText(lbl_anlage_stand, buf);
  }

  if (minute_neu) {
    lv_obj_invalidate(kurve);
    aktualisiereFuss();
  }

  // Nach 2 Minuten ohne Bedienung zurück zur Startansicht
  if (!info_festhalten && lv_disp_get_inactive_time(nullptr) > RUECKKEHR_MS) {
    if (lv_scr_act() != scr_preise) lv_scr_load(scr_preise);
    if (zeige_morgen) zeigeTag(false);
  }
}

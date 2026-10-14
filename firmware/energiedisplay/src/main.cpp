// Energiedisplay – Hauptprogramm
// Holt per MQTT vom QNAP: kueche/preise, kueche/ampel, kueche/anlage
// und zeigt sie auf zwei Screens an (siehe ui.cpp).

#include <Arduino.h>
#include <WiFi.h>
#include <ESPmDNS.h>
#include <ArduinoOTA.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <lvgl.h>
#include <string>
#include <string.h>
#include <time.h>
#include <esp_system.h>

#include "config.h"
#include "daten.h"
#include "einstellungen.h"
#include "hardware.h"
#include "ui.h"
#include "web.h"

static const char *VERSION = "1.7";

static WiFiClient netz;
static PubSubClient mqtt(netz);

// Vom MQTT-Callback abgelegt, im loop() ausgewertet
static std::string neu_ampel, neu_preise, neu_anlage;
static bool hat_ampel = false, hat_preise = false, hat_anlage = false;

static PreisTag heute, morgen;
static Ampel ampel;
static Anlage anlage;

// ---------- MQTT ----------

static void mqtt_callback(char *topic, byte *payload, unsigned int len) {
  std::string text((const char *)payload, len);
  String t(topic);
  if (t == "kueche/ampel")       { neu_ampel = text;  hat_ampel = true; }
  else if (t == "kueche/preise") { neu_preise = text; hat_preise = true; }
  else if (t == "kueche/anlage") { neu_anlage = text; hat_anlage = true; }
}

static void kopiere(char *ziel, size_t n, const char *quelle) {
  strncpy(ziel, quelle ? quelle : "", n - 1);
  ziel[n - 1] = 0;
}

static void leseAmpel(const std::string &json) {
  JsonDocument doc;
  if (deserializeJson(doc, json)) {
    Serial.println("Ampel: JSON-Fehler");
    return;
  }
  Ampel a;
  a.da = true;
  a.farbe = doc["farbe"] | -1;
  kopiere(a.titel, sizeof(a.titel), doc["titel"] | "?");
  kopiere(a.text, sizeof(a.text), doc["text"] | "");
  a.hat_preis = !doc["preis_ct"].isNull();
  a.preis_ct = doc["preis_ct"] | 0.0f;
  a.hat_fenster = !doc["fenster_start"].isNull() && !doc["fenster_ende"].isNull();
  a.fenster_start = (time_t)(doc["fenster_start"] | 0.0);
  a.fenster_ende = (time_t)(doc["fenster_ende"] | 0.0);
  kopiere(a.fenster_text, sizeof(a.fenster_text), doc["fenster_text"] | "");
  a.best_ct = doc["lauf_best_ct"] | 0.0f;
  a.stand = (time_t)(doc["stand"] | 0.0);
  ampel = a;
  ui_set_ampel(ampel);
  Serial.printf("Ampel: %s | %s\n", a.titel, a.text);
}

static void leseTag(JsonVariantConst j, PreisTag &tag) {
  tag = PreisTag();
  if (j.isNull()) return;
  JsonArrayConst ct = j["ct"];
  JsonArrayConst stufe = j["stufe"];
  int n = 0;
  for (JsonVariantConst v : ct) {
    if (n >= MAX_SLOTS) break;
    tag.ct[n] = v | 0.0f;
    tag.stufe[n] = stufe[n] | 1;
    n++;
  }
  if (n == 0) return;
  tag.anzahl = n;
  tag.start = (time_t)(j["start"] | 0.0);
  tag.min = j["min"] | 0.0f;
  tag.max = j["max"] | 0.0f;
  tag.da = true;
}

static void lesePreise(const std::string &json) {
  JsonDocument doc;
  if (deserializeJson(doc, json)) {
    Serial.println("Preise: JSON-Fehler");
    return;
  }
  leseTag(doc["heute"], heute);
  leseTag(doc["morgen"], morgen);
  ui_set_preise(heute, morgen);
  Serial.printf("Preise: heute %d Werte, morgen %d Werte\n", heute.anzahl, morgen.anzahl);
}

static void leseAnlage(const std::string &json) {
  JsonDocument doc;
  if (deserializeJson(doc, json)) {
    Serial.println("Anlage: JSON-Fehler");
    return;
  }
  Anlage a;
  a.da = true;
  a.hat_soc = !doc["soc"].isNull();
  a.soc = doc["soc"] | 0.0f;
  a.pv_w = doc["pv_w"] | 0;
  a.haus_w = doc["haus_w"] | 0;
  a.netz_w = doc["netz_w"] | 0;
  a.akku_w = doc["akku_w"] | 0;
  kopiere(a.steuerung, sizeof(a.steuerung), doc["steuerung"] | "");
  a.ladeleistung_w = doc["ladeleistung_w"] | 0;
  a.stand = (time_t)(doc["stand"] | 0.0);
  anlage = a;
  ui_set_anlage(anlage);
}

static void verbindeMqtt() {
  static uint32_t letzter_versuch = 0;
  if (millis() - letzter_versuch < 5000 && letzter_versuch != 0) return;
  letzter_versuch = millis();

  bool ok = cfg.mqtt_user.length() > 0
                ? mqtt.connect(MQTT_CLIENT, cfg.mqtt_user.c_str(), cfg.mqtt_pass.c_str())
                : mqtt.connect(MQTT_CLIENT);
  if (ok) {
    mqtt.subscribe("kueche/ampel");
    mqtt.subscribe("kueche/preise");
    mqtt.subscribe("kueche/anlage");
    Serial.println("MQTT: verbunden mit " + cfg.mqtt_host);
  } else {
    Serial.println("MQTT: Fehler " + String(mqtt.state()) + ", neuer Versuch");
  }
}

// ---------- Nachtruhe ----------

static void nachtruhe() {
  static uint32_t wach_seit = 0;
  static bool wach = false;

  if (hw_weckwunsch()) {
    wach = true;
    wach_seit = millis();
    Serial.println("Display geweckt");
  }
  const uint32_t wach_ms = cfg.wach_min * 60UL * 1000UL;
  if (wach && millis() - wach_seit > wach_ms) wach = false;
  // Solange jemand bedient, bleibt es hell (5 Minuten nach der letzten Berührung)
  bool bedient = lv_disp_get_inactive_time(nullptr) < wach_ms;

  bool nacht = false;
  time_t jetzt = time(nullptr);
  // Erst nach NTP, vorher bleibt das Licht an. Gilt auch im Hotspot-Betrieb,
  // damit ein nächtlicher WLAN-Ausfall das Display nicht einschaltet.
  if (cfg.nacht_aktiv && jetzt > 1700000000) {
    struct tm tm;
    localtime_r(&jetzt, &tm);
    int ab = cfg.nacht_ab, bis = cfg.nacht_bis;
    nacht = ab > bis ? (tm.tm_hour >= ab || tm.tm_hour < bis)
                     : (tm.tm_hour >= ab && tm.tm_hour < bis);
  }
  hw_licht(!nacht || wach || bedient);
}

// ---------- WLAN, Hotspot, Info-Screen ----------

static const uint32_t HOTSPOT_NACH_MS = 60UL * 1000UL;   // ohne WLAN so lange warten
static const uint32_t HOTSPOT_AUS_NACH_MS = 3UL * 60UL * 1000UL;

static const uint32_t NEU_VERBINDEN_MS = 30UL * 1000UL;   // ohne WLAN alle 30 s neu anmelden

// Für die Info-Seite: wie oft das WLAN seit dem Start weg war und wann zuletzt
static uint16_t wlan_abbrueche = 0;
static time_t wlan_abbruch_zuletzt = 0;

static void wlanUeberwachen(bool wlan) {
  static uint32_t getrennt_seit = 0;
  static uint32_t verbunden_seit = 0;
  static uint32_t neu_versucht = 0;
  static bool mdns_laeuft = false;
  static bool war_verbunden = false;

  if (wlan) {
    getrennt_seit = 0;
    war_verbunden = true;
    if (verbunden_seit == 0) verbunden_seit = millis();
    if (!mdns_laeuft) {
      // ArduinoOTA startet auch den Namen energiedisplay.local (mDNS)
      ArduinoOTA.begin();
      MDNS.addService("http", "tcp", 80);
      mdns_laeuft = true;
      Serial.println("Update übers WLAN bereit");
    }
    // Hotspot wieder aus, wenn das WLAN eine Weile stabil steht,
    // und die Info-Seite nicht länger festhalten
    if (web_hotspot_aktiv() && millis() - verbunden_seit > HOTSPOT_AUS_NACH_MS) {
      web_hotspot_stop();
      ui_info_freigeben();
    }
  } else {
    verbunden_seit = 0;
    if (getrennt_seit == 0) {
      getrennt_seit = millis();
      neu_versucht = millis();
      if (war_verbunden) {
        wlan_abbrueche++;
        wlan_abbruch_zuletzt = time(nullptr);
      }
    }
    // Die automatische Wiederverbindung bleibt nach manchen Router-Neustarts hängen,
    // deshalb selbst nachhelfen
    // (nicht, solange jemand am Notfall-Hotspot hängt: der Suchlauf stört ihn)
    if (millis() - neu_versucht > NEU_VERBINDEN_MS && WiFi.softAPgetStationNum() == 0) {
      neu_versucht = millis();
      Serial.println("WLAN: neuer Verbindungsversuch");
      WiFi.disconnect();
      WiFi.begin(cfg.ssid.c_str(), cfg.passwort.c_str());
    }
    if (!web_hotspot_aktiv() && millis() - getrennt_seit > HOTSPOT_NACH_MS) {
      web_hotspot_start();
      ui_info_zeigen(true);
    }
  }
}

static const char *neustartGrund() {
  switch (esp_reset_reason()) {
    case ESP_RST_POWERON:  return "Einschalten";
    case ESP_RST_EXT:      return "Reset-Taste";
    case ESP_RST_SW:       return "Neustart";
    case ESP_RST_PANIC:    return "Absturz";
    case ESP_RST_INT_WDT:
    case ESP_RST_TASK_WDT:
    case ESP_RST_WDT:      return "Watchdog";
    case ESP_RST_BROWNOUT: return "Unterspannung";
    default:               return "unbekannt";
  }
}

static const char *mqttZustand() {
  if (mqtt.connected()) return "verbunden";
  int s = mqtt.state();
  if (s == MQTT_CONNECT_BAD_CREDENTIALS || s == MQTT_CONNECT_UNAUTHORIZED) return "Anmeldung abgelehnt";
  return "getrennt";
}

static void aktualisiereInfo(bool wlan) {
  char titel[160];
  char text[600];
  if (web_hotspot_aktiv() && !wlan) {
    snprintf(titel, sizeof(titel),
             "Kein WLAN. Einrichtung über den Hotspot:");
    snprintf(text, sizeof(text),
             "1. Am Handy mit dem WLAN \"%s\" verbinden, Passwort \"%s\"\n"
             "2. Die Einstellungsseite öffnet sich, sonst http://192.168.4.1 aufrufen\n"
             "3. WLAN eintragen und speichern\n\n"
             "Gesuchtes WLAN: %s",
             SETUP_AP_NAME, SETUP_AP_PASSWORT, cfg.ssid.c_str());
  } else {
    snprintf(titel, sizeof(titel), "Einstellungen im Browser: http://%s.local", HOSTNAME);

    char abbruch[48] = "";
    if (wlan_abbrueche > 0 && wlan_abbruch_zuletzt > 1700000000) {
      struct tm tm;
      localtime_r(&wlan_abbruch_zuletzt, &tm);
      snprintf(abbruch, sizeof(abbruch), ", zuletzt %d.%d. %02d:%02d", tm.tm_mday, tm.tm_mon + 1, tm.tm_hour, tm.tm_min);
    }
    char start[48] = "";
    time_t jetzt = time(nullptr);
    if (jetzt > 1700000000) {
      time_t t0 = jetzt - (time_t)(millis() / 1000);
      struct tm tm;
      localtime_r(&t0, &tm);
      snprintf(start, sizeof(start), " %d.%d. %02d:%02d", tm.tm_mday, tm.tm_mon + 1, tm.tm_hour, tm.tm_min);
    }

    snprintf(text, sizeof(text),
             "oder http://%s\n"
             "WLAN: %s, %s\n"
             "WLAN-Abbrüche seit Start: %u%s\n"
             "MQTT: %s:%u, %s\n"
             "Nachtruhe: %s\n"
             "Version %s, Start%s (%s)",
             wlan ? WiFi.localIP().toString().c_str() : "–",
             cfg.ssid.c_str(), wlan ? (String((int)WiFi.RSSI()) + " dBm").c_str() : "nicht verbunden",
             (unsigned)wlan_abbrueche, abbruch,
             cfg.mqtt_host.c_str(), cfg.mqtt_port, mqttZustand(),
             cfg.nacht_aktiv ? (String(cfg.nacht_ab) + " bis " + String(cfg.nacht_bis) + " Uhr, nach Berührung " +
                                String(cfg.wach_min) + " min hell").c_str()
                             : "aus",
             VERSION, start, neustartGrund());
  }
  ui_set_info(titel, text);
}

// ---------- Update übers WLAN ----------

static void otaEinrichten() {
  ArduinoOTA.setHostname(HOSTNAME);
  ArduinoOTA.setPassword(OTA_PASSWORT);
  ArduinoOTA.onStart([]() {
    // Während des Updates läuft LVGL nicht weiter, deshalb einmal sofort zeichnen
    hw_licht(true);
    ui_set_info("Update wird übertragen …", "Bitte nicht ausschalten. Das Display startet danach neu.");
    ui_info_zeigen(true);
    lv_refr_now(nullptr);
    mqtt.disconnect();
    Serial.println("Update startet");
  });
  ArduinoOTA.onEnd([]() { Serial.println("Update fertig, Neustart"); });
  ArduinoOTA.onError([](ota_error_t fehler) {
    Serial.printf("Update-Fehler %u\n", (unsigned)fehler);
    ui_set_info("Update fehlgeschlagen", "Die alte Version läuft weiter.");
  });
}

// ---------- Sekundentakt ----------

static void sekundenTakt() {
  static uint32_t zuletzt = 0;
  if (millis() - zuletzt < 1000) return;
  zuletzt = millis();

  time_t jetzt = time(nullptr);
  bool zeit_gueltig = jetzt > 1700000000;  // NTP hat die Uhr gestellt
  bool wlan = WiFi.status() == WL_CONNECTED;
  ui_tick(jetzt, zeit_gueltig, wlan, mqtt.connected());
  wlanUeberwachen(wlan);
  aktualisiereInfo(wlan);

  static bool wlan_vorher = false;
  if (wlan != wlan_vorher) {
    wlan_vorher = wlan;
    if (wlan) Serial.printf("WLAN: %s (%d dBm)\n", WiFi.localIP().toString().c_str(), (int)WiFi.RSSI());
    else Serial.println("WLAN: getrennt");
  }
}

// ---------- Ablauf ----------

void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.println();
  Serial.printf("Energiedisplay %s startet\n", VERSION);
  cfg_laden();

  lv_init();
  hw_init();
  ui_create();
  lv_timer_handler();

  WiFi.mode(WIFI_STA);
  WiFi.setHostname(HOSTNAME);
  WiFi.setAutoReconnect(true);
  WiFi.begin(cfg.ssid.c_str(), cfg.passwort.c_str());
  web_start();
  otaEinrichten();

  configTzTime("CET-1CEST,M3.5.0,M10.5.0/3", "de.pool.ntp.org", "pool.ntp.org");

  // cfg.mqtt_host bleibt bis zum Neustart unverändert, der Zeiger also gültig
  mqtt.setServer(cfg.mqtt_host.c_str(), cfg.mqtt_port);
  mqtt.setBufferSize(8192);
  mqtt.setCallback(mqtt_callback);
}

void loop() {
  if (WiFi.status() == WL_CONNECTED) {
    if (!mqtt.connected()) verbindeMqtt();
    mqtt.loop();
  } else if (mqtt.connected()) {
    mqtt.disconnect();
  }

  // Preise vor der Ampel, damit die Kurve das beste Fenster gleich passend zeigt
  if (hat_preise) { hat_preise = false; lesePreise(neu_preise); }
  if (hat_ampel)  { hat_ampel = false;  leseAmpel(neu_ampel); }
  if (hat_anlage) { hat_anlage = false; leseAnlage(neu_anlage); }

  web_loop();
  ArduinoOTA.handle();
  sekundenTakt();
  lv_timer_handler();
  nachtruhe();
  delay(5);
}

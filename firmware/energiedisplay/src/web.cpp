// Energiedisplay – Einstellungsseite im Browser
//
// Im Heimnetz:  http://energiedisplay.local  (oder die IP vom Info-Screen)
// Ohne WLAN:    Hotspot "Energiedisplay-Setup", dann http://192.168.4.1

#include "web.h"

#include <Arduino.h>
#include <DNSServer.h>
#include <WebServer.h>
#include <WiFi.h>

#include "config.h"
#include "einstellungen.h"

static WebServer server(80);
static DNSServer dns;
static bool hotspot = false;
static uint32_t neustart_um = 0;

static String esc(const String &s) {
  String r;
  r.reserve(s.length() + 8);
  for (unsigned int i = 0; i < s.length(); i++) {
    char c = s[i];
    switch (c) {
      case '&': r += "&amp;"; break;
      case '<': r += "&lt;"; break;
      case '>': r += "&gt;"; break;
      case '"': r += "&quot;"; break;
      default: r += c;
    }
  }
  return r;
}

static String stundenAuswahl(const char *name, int wert) {
  String s = String("<select name=\"") + name + "\">";
  for (int h = 0; h < 24; h++) {
    char buf[8];
    snprintf(buf, sizeof(buf), "%02d:00", h);
    s += "<option value=\"" + String(h) + "\"" + (h == wert ? " selected" : "") + ">" + buf + "</option>";
  }
  return s + "</select>";
}

static const char *KOPF = R"(<!doctype html><html lang="de"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Energiedisplay</title><style>
body{font-family:system-ui,sans-serif;background:#101418;color:#e8ecef;margin:0;padding:16px}
main{max-width:480px;margin:auto}h1{font-size:1.4em}h2{font-size:1.05em;color:#8a949c;margin:24px 0 8px}
label{display:block;margin:10px 0 4px}input,select{width:100%;box-sizing:border-box;padding:10px;
border-radius:8px;border:1px solid #3a434b;background:#1c2228;color:#e8ecef;font-size:1em}
.reihe{display:flex;gap:12px}.reihe>div{flex:1}.haken{display:flex;gap:8px;align-items:center}
.haken input{width:auto}button{margin-top:24px;width:100%;padding:14px;border:0;border-radius:10px;
background:#3a6ea5;color:#fff;font-size:1.05em}.hinweis{color:#8a949c;font-size:.9em}
.ok{background:#2e9e55;padding:12px;border-radius:8px}</style></head><body><main>)";

static void seite() {
  String h = KOPF;
  h += "<h1>Energiedisplay</h1>";
  if (server.hasArg("ok")) h += "<p class=\"ok\">Gespeichert.</p>";
  h += "<form method=\"post\" action=\"/speichern\">";

  h += "<h2>WLAN</h2>";
  h += "<label>Name (SSID)</label><input name=\"ssid\" value=\"" + esc(cfg.ssid) + "\" autocomplete=\"off\">";
  h += "<label>Passwort</label><input name=\"pass\" type=\"password\" placeholder=\"leer lassen = unverändert\" autocomplete=\"new-password\">";
  if (WiFi.status() == WL_CONNECTED) {
    h += "<p class=\"hinweis\">Verbunden mit " + esc(WiFi.SSID()) + ", " + WiFi.localIP().toString() +
         ", Signal " + String((int)WiFi.RSSI()) + " dBm</p>";
  } else {
    h += "<p class=\"hinweis\">Zurzeit nicht verbunden.</p>";
  }

  h += "<h2>MQTT (QNAP)</h2><div class=\"reihe\"><div><label>Adresse</label><input name=\"mqtt\" value=\"" +
       esc(cfg.mqtt_host) + "\"></div><div style=\"flex:.5\"><label>Port</label><input name=\"port\" type=\"number\" value=\"" +
       String(cfg.mqtt_port) + "\"></div></div>";
  h += "<div class=\"reihe\"><div><label>Benutzer</label><input name=\"muser\" value=\"" + esc(cfg.mqtt_user) +
       "\" autocomplete=\"off\" placeholder=\"leer = ohne Anmeldung\"></div><div><label>Passwort</label>"
       "<input name=\"mpass\" type=\"password\" placeholder=\"leer lassen = unverändert\" autocomplete=\"new-password\"></div></div>";

  h += "<h2>Nachtruhe</h2>";
  h += String("<label class=\"haken\"><input type=\"checkbox\" name=\"nacht\" value=\"1\"") +
       (cfg.nacht_aktiv ? " checked" : "") + "> Display nachts ausschalten</label>";
  h += "<div class=\"reihe\"><div><label>Dunkel ab</label>" + stundenAuswahl("ab", cfg.nacht_ab) +
       "</div><div><label>bis</label>" + stundenAuswahl("bis", cfg.nacht_bis) + "</div></div>";
  h += "<label>Nach Berührung hell für (Minuten)</label><input name=\"wach\" type=\"number\" min=\"1\" max=\"60\" value=\"" +
       String(cfg.wach_min) + "\">";

  h += "<button type=\"submit\">Speichern</button></form>";
  h += "<p class=\"hinweis\">Änderungen an WLAN oder MQTT starten das Display neu.</p></main></body></html>";
  server.send(200, "text/html; charset=utf-8", h);
}

static void speichern() {
  bool neustart = false;

  String ssid = server.arg("ssid");
  ssid.trim();
  String pass = server.arg("pass");
  String mqtt = server.arg("mqtt");
  mqtt.trim();
  int port = server.arg("port").toInt();
  String muser = server.arg("muser");
  muser.trim();
  String mpass = server.arg("mpass");

  if (ssid.length() > 0 && ssid != cfg.ssid) { cfg.ssid = ssid; neustart = true; }
  if (pass.length() > 0 && pass != cfg.passwort) { cfg.passwort = pass; neustart = true; }
  if (mqtt.length() > 0 && mqtt != cfg.mqtt_host) { cfg.mqtt_host = mqtt; neustart = true; }
  if (port > 0 && port < 65536 && port != cfg.mqtt_port) { cfg.mqtt_port = port; neustart = true; }
  if (muser != cfg.mqtt_user) {
    cfg.mqtt_user = muser;
    if (muser.length() == 0) cfg.mqtt_pass = "";
    neustart = true;
  }
  if (mpass.length() > 0 && mpass != cfg.mqtt_pass) { cfg.mqtt_pass = mpass; neustart = true; }

  cfg.nacht_aktiv = server.hasArg("nacht");
  cfg.nacht_ab = constrain(server.arg("ab").toInt(), 0, 23);
  cfg.nacht_bis = constrain(server.arg("bis").toInt(), 0, 23);
  cfg.wach_min = constrain(server.arg("wach").toInt(), 1, 60);

  cfg_speichern();
  Serial.println(neustart ? "Einstellungen gespeichert, Neustart" : "Einstellungen gespeichert");

  if (neustart) {
    String h = KOPF;
    h += "<h1>Energiedisplay</h1><p class=\"ok\">Gespeichert. Das Display startet neu und verbindet sich mit „" +
         esc(cfg.ssid) + "“.</p><p class=\"hinweis\">Danach erreichst du diese Seite wieder unter "
         "<b>http://" HOSTNAME ".local</b> oder über die IP, die das Display unter „Info“ zeigt.</p></main></body></html>";
    server.send(200, "text/html; charset=utf-8", h);
    neustart_um = millis() + 1500;
    if (neustart_um == 0) neustart_um = 1;
  } else {
    server.sendHeader("Location", "/?ok=1");
    server.send(303);
  }
}

static void nichtGefunden() {
  // Im Hotspot leitet jede Adresse auf die Einstellungsseite (Anmeldeseite am Handy)
  server.sendHeader("Location", hotspot ? "http://192.168.4.1/" : "/");
  server.send(302);
}

void web_start() {
  server.on("/", HTTP_GET, seite);
  server.on("/speichern", HTTP_POST, speichern);
  server.onNotFound(nichtGefunden);
  server.begin();
}

void web_loop() {
  server.handleClient();
  if (hotspot) dns.processNextRequest();
  if (neustart_um != 0 && (int32_t)(millis() - neustart_um) >= 0) ESP.restart();
}

void web_hotspot_start() {
  if (hotspot) return;
  WiFi.mode(WIFI_AP_STA);
  WiFi.softAP(SETUP_AP_NAME, SETUP_AP_PASSWORT);
  dns.start(53, "*", WiFi.softAPIP());
  hotspot = true;
  Serial.printf("Hotspot %s gestartet, %s\n", SETUP_AP_NAME, WiFi.softAPIP().toString().c_str());
}

void web_hotspot_stop() {
  if (!hotspot) return;
  dns.stop();
  WiFi.softAPdisconnect(true);
  WiFi.mode(WIFI_STA);
  hotspot = false;
  Serial.println("Hotspot beendet");
}

bool web_hotspot_aktiv() { return hotspot; }

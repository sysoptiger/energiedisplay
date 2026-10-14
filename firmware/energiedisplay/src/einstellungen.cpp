// Energiedisplay – Einstellungen

#include "einstellungen.h"

#include <Preferences.h>

#include "config.h"

Einstellungen cfg;

// Bleibt "kueche", damit die gespeicherten Einstellungen nach der
// Umbenennung in Energiedisplay erhalten bleiben
static const char *NS = "kueche";

void cfg_laden() {
  Preferences p;
  p.begin(NS, true);
  cfg.ssid = p.getString("ssid", WLAN_SSID);
  cfg.passwort = p.getString("pass", WLAN_PASSWORT);
  cfg.mqtt_host = p.getString("mqtt", MQTT_HOST);
  cfg.mqtt_port = p.getUShort("port", MQTT_PORT);
  cfg.mqtt_user = p.getString("muser", MQTT_BENUTZER);
  cfg.mqtt_pass = p.getString("mpass", MQTT_KENNWORT);
  cfg.nacht_aktiv = p.getBool("nacht", true);
  cfg.nacht_ab = p.getUChar("ab", NACHT_AB);
  cfg.nacht_bis = p.getUChar("bis", NACHT_BIS);
  cfg.wach_min = p.getUChar("wach", WACH_MINUTEN);
  p.end();
}

void cfg_speichern() {
  Preferences p;
  p.begin(NS, false);
  p.putString("ssid", cfg.ssid);
  p.putString("pass", cfg.passwort);
  p.putString("mqtt", cfg.mqtt_host);
  p.putUShort("port", cfg.mqtt_port);
  p.putString("muser", cfg.mqtt_user);
  p.putString("mpass", cfg.mqtt_pass);
  p.putBool("nacht", cfg.nacht_aktiv);
  p.putUChar("ab", cfg.nacht_ab);
  p.putUChar("bis", cfg.nacht_bis);
  p.putUChar("wach", cfg.wach_min);
  p.end();
}

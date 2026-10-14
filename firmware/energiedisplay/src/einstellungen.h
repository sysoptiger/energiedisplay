// Energiedisplay – Einstellungen, dauerhaft im Flash (NVS) gespeichert.
// Beim allerersten Start gelten die Werte aus config.h.
#pragma once

#include <Arduino.h>

struct Einstellungen {
  String ssid;
  String passwort;
  String mqtt_host;
  uint16_t mqtt_port;
  String mqtt_user;      // leer = ohne Anmeldung
  String mqtt_pass;
  bool nacht_aktiv;
  uint8_t nacht_ab;      // Stunde 0-23
  uint8_t nacht_bis;     // Stunde 0-23
  uint8_t wach_min;      // Minuten hell nach Berührung
};

extern Einstellungen cfg;

void cfg_laden();
void cfg_speichern();

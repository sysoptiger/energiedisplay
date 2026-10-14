// Energiedisplay – Startwerte
// Gelten nur beim allerersten Start. Danach werden die Einstellungen
// über die Einstellungsseite im Browser geändert und im Gerät gespeichert.
//
// Vorlage: Diese Datei nach config.h kopieren und dort die eigenen Werte
// eintragen. config.h steht in .gitignore und landet nicht im Repository.

#pragma once

#define WLAN_SSID     "MEIN-WLAN"
#define WLAN_PASSWORT "MEIN-WLAN-PASSWORT"

#define MQTT_HOST     "192.168.1.10"   // IP des MQTT-Brokers
#define MQTT_PORT     1883
#define MQTT_CLIENT   "energiedisplay"
// Anmeldung am Broker, leer = ohne Anmeldung
#define MQTT_BENUTZER ""
#define MQTT_KENNWORT ""

// Nachtruhe: Display dunkel von NACHT_AB bis NACHT_BIS Uhr.
// Eine Berührung schaltet es für WACH_MINUTEN wieder ein.
#define NACHT_AB      20
#define NACHT_BIS     6
#define WACH_MINUTEN  5

// Einstellungsseite: http://energiedisplay.local
#define HOSTNAME          "energiedisplay"

// Notfall-Hotspot, wenn das WLAN nicht erreichbar ist
#define SETUP_AP_NAME     "Energiedisplay-Setup"
#define SETUP_AP_PASSWORT "setup-passwort"   // mind. 8 Zeichen

// Update übers WLAN (PlatformIO-Umgebung "energiedisplay-wlan")
// Muss mit --auth in platformio.ini übereinstimmen.
#define OTA_PASSWORT      "ota-passwort"

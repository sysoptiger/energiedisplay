// Energiedisplay – Oberfläche
// Screen 1: Strompreise (Startansicht), Screen 2: Anlage
#pragma once

#include "daten.h"

void ui_create();
void ui_set_preise(const PreisTag &heute, const PreisTag &morgen);
void ui_set_ampel(const Ampel &a);
void ui_set_anlage(const Anlage &a);
// Einmal pro Sekunde: Uhr, Jetzt-Markierung, Verbindungsanzeige,
// Rückkehr zu Screen 1 nach 2 Minuten ohne Bedienung
void ui_tick(time_t jetzt, bool zeit_gueltig, bool wlan, bool mqtt);

// Info-Screen: Adresse der Einstellungsseite, WLAN, MQTT, Version
void ui_set_info(const char *titel, const char *text);
// Info-Screen anzeigen. festhalten = nicht automatisch zurückspringen
// (z. B. solange der Notfall-Hotspot läuft)
void ui_info_zeigen(bool festhalten);
// Festhalten aufheben: nach 2 Minuten ohne Bedienung wieder zur Startansicht
void ui_info_freigeben();

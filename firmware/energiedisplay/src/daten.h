// Energiedisplay – Daten vom QNAP (kueche/preise, kueche/ampel, kueche/anlage)
#pragma once

#include <stdint.h>
#include <time.h>

static const int MAX_SLOTS = 100;  // 96 Viertelstunden, 92/100 bei Zeitumstellung

struct PreisTag {
  bool da = false;
  time_t start = 0;          // Beginn der ersten Viertelstunde
  int anzahl = 0;
  float ct[MAX_SLOTS] = {};
  uint8_t stufe[MAX_SLOTS] = {};  // 0 günstig, 1 mittel, 2 teuer (Drittel des Tages)
  float min = 0, max = 0;
};

struct Ampel {
  bool da = false;
  int farbe = -1;            // 0 grün, 1 gelb, 2 rot, 3 Ruhe, -1 unbekannt
  char titel[48] = "";
  char text[128] = "";
  bool hat_preis = false;
  float preis_ct = 0;
  bool hat_fenster = false;
  time_t fenster_start = 0, fenster_ende = 0;
  char fenster_text[24] = "";
  float best_ct = 0;
  time_t stand = 0;
};

struct Anlage {
  bool da = false;
  bool hat_soc = false;
  float soc = 0;
  int pv_w = 0, haus_w = 0, netz_w = 0, akku_w = 0;
  char steuerung[16] = "";
  int ladeleistung_w = 0;
  time_t stand = 0;
};

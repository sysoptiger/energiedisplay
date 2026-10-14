// Energiedisplay – Hardware: RGB-Panel, Touch, IO-Expander, LVGL-Treiber
#pragma once

// Startet Display und Touch und meldet beide bei LVGL an.
// Vorher muss lv_init() aufgerufen sein.
void hw_init();

// Hintergrundlicht ein/aus (über den IO-Expander, kein Dimmen möglich)
void hw_licht(bool an);

// Liefert true, wenn seit dem letzten Aufruf bei ausgeschaltetem Licht
// das Display berührt wurde. Diese Berührung löst keinen Knopf aus.
bool hw_weckwunsch();

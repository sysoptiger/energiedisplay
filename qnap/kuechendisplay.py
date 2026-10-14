import json
import os
import signal
import sys
import threading
import time

from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

from datetime import datetime, time as dt_time, timedelta
from zoneinfo import ZoneInfo

import paho.mqtt.client as mqtt


# ============================================================
# ENERGIEDISPLAY-PUBLISHER (früher Küchendisplay)
#
# Liefert die Daten für das 7"-Energiedisplay (Waveshare ESP32-S3).
# Dateiname und MQTT-Topics (kueche/...) bleiben aus
# Kompatibilitätsgründen unverändert.
#
# Läuft UNABHÄNGIG vom HiBattery-Controller:
#   - liest nur die Preisdatei, die der Controller schreibt
#     (/app/epex_prices.json) -> gleiche Preisgrundlage
#   - hört auf MQTT mit (Hoymiles-Livewerte und die
#     Steuerbefehle des Controllers)
#   - schreibt NIE etwas an die Hoymiles-Topics
#
# Fällt dieses Programm aus, läuft die Batteriesteuerung
# unverändert weiter.
#
# Veröffentlicht (alle retained, JSON):
#   kueche/preise   Tageskurven heute/morgen im 15-Min-Raster
#   kueche/ampel    Entscheidungshilfe GUENSTIG/MITTEL/TEUER
#   kueche/anlage   PV, Haus, Netz, Akku, Steuerungszustand
#   kueche/status   online/offline
#
# Webseite für den PC (ab 1.3): http://<QNAP-IP>:8088
# Port über Umgebungsvariable WEB_PORT änderbar, 0 = aus.
#
# Test ohne MQTT:
#   python kuechendisplay.py --test
# ============================================================


VERSION = "1.4"

BERLIN = ZoneInfo("Europe/Berlin")

BROKER = os.environ.get("MQTT_BROKER", "192.168.1.10")
PORT = int(os.environ.get("MQTT_PORT", "1883"))
# Anmeldung am Broker (leer = ohne). Hier eintragen oder als
# Umgebungsvariable MQTT_USER / MQTT_PASS im Container setzen.
MQTT_USER = os.environ.get("MQTT_USER", "")
MQTT_PASS = os.environ.get("MQTT_PASS", "")

# Webseite für den PC, 0 = aus
WEB_PORT = int(os.environ.get("WEB_PORT", "8088"))

# Geräte-ID des Master-Speichers (Hoymiles), Umgebungsvariable HOYMILES_MASTER
MASTER = os.environ.get("HOYMILES_MASTER", "MSA-000000000000")

PRICE_FILE = os.environ.get("PRICE_FILE", "/app/epex_prices.json")
LOG_FILE = os.environ.get("LOG_FILE", "/app/kuechendisplay.log")


# ============================================================
# ENDPREIS
#
# Endpreis = (EPEX + Aufschläge netto) * (1 + MwSt)
#
# Aufschläge netto = Netzentgelt + Umlagen + Stromsteuer
#                    + Anbieteraufschlag, alles in ct/kWh netto.
#
# None = Display zeigt den reinen Börsenpreis (EPEX).
# ============================================================

# SimplyGreen dynamisch: 16,97 ct/kWh brutto ohne Börsenpreis
# -> 16,97 / 1,19 = 14,26 ct netto
AUFSCHLAG_NETTO_CT = 14.26
MWST = 0.19


# ============================================================
# AMPEL
#
# Vergleich: Durchschnittspreis eines Gerätelaufs ab JETZT
# gegen den günstigsten Lauf in den nächsten Stunden.
#
#   Mehrpreis <= GRUEN_MAX_CT  -> GUENSTIG (jetzt einschalten)
#   Mehrpreis <= GELB_MAX_CT   -> MITTEL   (warten lohnt etwas)
#   sonst                      -> TEUER    (besser warten)
# ============================================================

LAUFZEIT_STUNDEN = 2.0

# Horizont = die nächsten 12 Stunden ERLAUBTER Zeit.
# Um 19 Uhr also: 19-22 Uhr heute + 8-17 Uhr morgen.
HORIZONT_STUNDEN = 12

# Ruhezeit: kein Gerätelauf zwischen 22 und 8 Uhr.
# Ein Lauf muss komplett im erlaubten Zeitraum liegen.
RUHE_AB_STUNDE = 22
RUHE_BIS_STUNDE = 8

GRUEN_MAX_CT = 2.0
GELB_MAX_CT = 5.0


# ============================================================
# INTERVALLE
# ============================================================

RECHEN_INTERVALL_SECONDS = 30
ANLAGE_INTERVALL_SECONDS = 5

# Der Controller wiederholt power_ctrl alle 10-20 s, solange
# er die Kontrolle hat. Kommt länger nichts -> Hoymiles-Automatik.
STEUERUNG_TIMEOUT_SECONDS = 45

SLOT_SECONDS = 900


# ============================================================
# MQTT TOPICS
# ============================================================

TOPIC_STATE = f"homeassistant/sensor/{MASTER}/quick/state"
TOPIC_EMS_MODE = f"homeassistant/select/{MASTER}/ems_mode/command"
TOPIC_POWER = f"homeassistant/number/{MASTER}/power_ctrl/set"

TOPIC_PREISE = "kueche/preise"
TOPIC_AMPEL = "kueche/ampel"
TOPIC_ANLAGE = "kueche/anlage"
TOPIC_STATUS = "kueche/status"


# ============================================================
# ZUSTAND
# ============================================================

lock = threading.Lock()

price_days = {}
price_file_mtime = None
last_preise_payload = None

last_anlage_publish = 0

steuerung_power = None
steuerung_power_time = 0
ems_mode = None
last_anlage = None


# ============================================================
# HILFSFUNKTIONEN
# ============================================================

def now_berlin():
    return datetime.now(BERLIN)


def log(message=""):
    line = f"{now_berlin().strftime('%Y-%m-%d %H:%M:%S')} | {message}"
    print(line, flush=True)
    try:
        with open(LOG_FILE, "a", encoding="utf-8") as f:
            f.write(line + "\n")
    except Exception:
        pass


def expected_slots(day_date):
    start = datetime.combine(day_date, dt_time(0, 0), tzinfo=BERLIN)
    end = datetime.combine(day_date + timedelta(days=1), dt_time(0, 0), tzinfo=BERLIN)
    return int(round((end.timestamp() - start.timestamp()) / SLOT_SECONDS))


def endpreis(epex_ct):
    if AUFSCHLAG_NETTO_CT is None:
        return epex_ct
    return (epex_ct + AUFSCHLAG_NETTO_CT) * (1.0 + MWST)


def hhmm(ts):
    return datetime.fromtimestamp(ts, tz=BERLIN).strftime("%H:%M")


def wann(ts, now_ts):
    # "14:30" oder "morgen 02:00"
    dt = datetime.fromtimestamp(ts, tz=BERLIN)
    heute = datetime.fromtimestamp(now_ts, tz=BERLIN).date()
    prefix = "morgen " if dt.date() > heute else ""
    return prefix + dt.strftime("%H:%M")


def r1(value):
    return round(value, 1)


# ============================================================
# PREISDATEI LESEN
#
# Wird nur neu gelesen, wenn der Controller sie geändert hat.
# ============================================================

def load_prices(force=False):

    global price_days, price_file_mtime

    try:
        mtime = os.path.getmtime(PRICE_FILE)
    except OSError:
        if price_file_mtime is not None or force:
            log(f"Preisdatei nicht gefunden: {PRICE_FILE}")
        price_file_mtime = None
        return False

    if not force and mtime == price_file_mtime:
        return False

    try:
        with open(PRICE_FILE, "r", encoding="utf-8") as f:
            raw = json.load(f)
    except Exception as e:
        # Controller schreibt atomar per os.replace,
        # ein Fehler hier ist also selten -> nächster Versuch.
        log(f"Preisdatei lesen Fehler: {e}")
        return False

    loaded = {}

    for day, entries in raw.items():
        try:
            day_date = datetime.strptime(day, "%Y-%m-%d").date()
            slots = sorted(
                (int(e["timestamp"]), endpreis(float(e["price"])))
                for e in entries
            )
        except Exception:
            continue

        if len(slots) == expected_slots(day_date):
            loaded[day] = slots

    price_days = loaded
    price_file_mtime = mtime

    log(f"Preise geladen: {', '.join(sorted(loaded)) or 'keine'}")
    return True


# ============================================================
# KUECHE/PREISE
#
# Pro Tag: Startzeit + Preisliste + Farbstufe je Viertelstunde.
# Stufe innerhalb des Tages:
#   günstigstes Drittel = 0 (grün)
#   mittleres Drittel   = 1 (gelb)
#   teuerstes Drittel   = 2 (rot)
#
# Viertelstunde i beginnt bei start + i * 900.
# Funktioniert damit auch an Tagen mit 92 oder 100 Werten.
# ============================================================

def day_payload(day_string):

    slots = price_days.get(day_string)

    if slots is None:
        return None

    prices = [p for _, p in slots]
    n = len(prices)

    order = sorted(range(n), key=lambda i: prices[i])
    stufe = [0] * n

    for rank, i in enumerate(order):
        stufe[i] = 0 if rank < n / 3 else (1 if rank < 2 * n / 3 else 2)

    return {
        "datum": day_string,
        "start": slots[0][0],
        "anzahl": n,
        "ct": [r1(p) for p in prices],
        "stufe": stufe,
        "min": r1(min(prices)),
        "max": r1(max(prices)),
    }


def build_preise():

    today = now_berlin().date()
    tomorrow = today + timedelta(days=1)

    return {
        "heute": day_payload(today.strftime("%Y-%m-%d")),
        "morgen": day_payload(tomorrow.strftime("%Y-%m-%d")),
        "preisart": "boerse" if AUFSCHLAG_NETTO_CT is None else "endpreis",
    }


# ============================================================
# KUECHE/AMPEL
# ============================================================

def build_ampel(now_ts=None):

    if now_ts is None:
        now_ts = time.time()

    today = datetime.fromtimestamp(now_ts, tz=BERLIN).date()
    tomorrow = today + timedelta(days=1)

    slots = []
    for day in (today, tomorrow):
        slots.extend(price_days.get(day.strftime("%Y-%m-%d"), []))

    idx = None
    for i, (ts, _) in enumerate(slots):
        if ts <= now_ts < ts + SLOT_SECONDS:
            idx = i
            break

    if idx is None:
        return {
            "stufe": "UNBEKANNT",
            "farbe": -1,
            "titel": "Keine Preise",
            "text": "Preisdaten fehlen",
            "stand": int(now_ts),
        }

    future = slots[idx:]
    n = max(1, int(round(LAUFZEIT_STUNDEN * 4)))

    def erlaubt(i):
        # Viertelstunde i liegt außerhalb der Ruhezeit
        h = datetime.fromtimestamp(future[i][0], tz=BERLIN).hour
        return RUHE_BIS_STUNDE <= h < RUHE_AB_STUNDE

    def lauf_erlaubt(start):
        return start + n <= len(future) and all(
            erlaubt(i) for i in range(start, start + n)
        )

    def avg(start):
        window = future[start:start + n]
        return sum(p for _, p in window) / len(window)

    # Horizont: die nächsten HORIZONT_STUNDEN erlaubter Zeit
    max_slots = HORIZONT_STUNDEN * 4
    candidates = []
    erlaubte_slots = 0

    for i in range(len(future)):
        if erlaubt(i):
            erlaubte_slots += 1
            if erlaubte_slots > max_slots:
                break
        if lauf_erlaubt(i):
            candidates.append(i)

    if not candidates:
        return {
            "stufe": "UNBEKANNT",
            "farbe": -1,
            "titel": "Keine Preise",
            "text": "Morgenpreise noch nicht da",
            "preis_ct": r1(future[0][1]),
            "stand": int(now_ts),
        }

    best = min(candidates, key=avg)
    best_avg = avg(best)
    best_start = future[best][0]
    best_end = best_start + n * SLOT_SECONDS

    base = {
        "preis_ct": r1(future[0][1]),
        "lauf_best_ct": r1(best_avg),
        "fenster_start": best_start,
        "fenster_ende": best_end,
        "fenster_text": f"{hhmm(best_start)}–{hhmm(best_end)}",
        "laufzeit_h": LAUFZEIT_STUNDEN,
        "preisart": "boerse" if AUFSCHLAG_NETTO_CT is None else "endpreis",
        "stand": int(now_ts),
    }

    # Jetzt kein vollständiger Lauf mehr vor 22 Uhr möglich
    # oder Ruhezeit -> nur das nächste Fenster anzeigen.
    if not lauf_erlaubt(0):
        titel = "Nachtruhe" if not erlaubt(0) else "Heute nicht mehr"
        return {
            "stufe": "RUHE",
            "farbe": 3,
            "titel": titel,
            "text": f"Bestes Fenster ab {wann(best_start, now_ts)} "
                    f"({best_avg:.1f} ct)",
            **base,
        }

    jetzt_avg = avg(0)
    mehrpreis = jetzt_avg - best_avg

    if mehrpreis <= GRUEN_MAX_CT:
        stufe, farbe, titel = "GUENSTIG", 0, "Jetzt einschalten"
        if best > 0 and mehrpreis >= 0.5:
            text = f"ab {wann(best_start, now_ts)} nur {mehrpreis:.1f} ct günstiger"
        else:
            text = "Gute Zeit für große Verbraucher"
    elif mehrpreis <= GELB_MAX_CT:
        stufe, farbe, titel = "MITTEL", 1, "Warten lohnt etwas"
        text = f"ab {wann(best_start, now_ts)} ca. {mehrpreis:.1f} ct/kWh günstiger"
    else:
        stufe, farbe, titel = "TEUER", 2, "Besser warten"
        text = f"ab {wann(best_start, now_ts)} ca. {mehrpreis:.1f} ct/kWh günstiger"

    return {
        "stufe": stufe,
        "farbe": farbe,
        "titel": titel,
        "text": text,
        "lauf_jetzt_ct": r1(jetzt_avg),
        "mehrpreis_ct": r1(mehrpreis),
        **base,
    }


# ============================================================
# KUECHE/ANLAGE
#
# Steuerungszustand wird aus den Befehlen des Controllers
# abgeleitet (der Controller selbst bleibt unverändert):
#   power_ctrl < 0 in den letzten 45 s -> LADEN (Netz)
#   power_ctrl ~ 0 in den letzten 45 s -> HALTEN (Akku reserviert)
#   sonst                              -> AUTOMATIK (Hoymiles)
# ============================================================

def steuerung_zustand():

    if (
        steuerung_power is None
        or time.time() - steuerung_power_time > STEUERUNG_TIMEOUT_SECONDS
        or ems_mode == "general"
    ):
        return "AUTOMATIK", 0

    if steuerung_power < -1:
        return "LADEN", int(abs(steuerung_power))

    return "HALTEN", 0


def build_anlage(data):

    def num(key):
        value = data.get(key)
        try:
            return None if value is None else round(float(value))
        except (TypeError, ValueError):
            return None

    zustand, ladeleistung = steuerung_zustand()

    soc = data.get("soc")
    try:
        soc = None if soc is None else round(float(soc), 1)
    except (TypeError, ValueError):
        soc = None

    pv_w = num("sys_pv_p")
    haus_w = num("sys_load_p")
    netz_w = num("sys_grid_p")

    # bat_p kommt nur vom Master-Speicher. Bei zwei Speichern (MS 2x)
    # fehlt der zweite. Die Gesamtleistung ergibt sich aus der Bilanz:
    # Haus = PV + Akku + Netz  ->  Akku = Haus - PV - Netz
    # (> 0 = Entladen, Netz > 0 = Bezug)
    if None not in (pv_w, haus_w, netz_w):
        akku_w = haus_w - pv_w - netz_w
    else:
        akku_w = num("bat_p")

    return {
        "soc": soc,
        "pv_w": pv_w,
        "haus_w": haus_w,
        "netz_w": netz_w,
        "akku_w": akku_w,
        "akku_master_w": num("bat_p"),
        "bat_sts": data.get("bat_sts"),
        "steuerung": zustand,
        "ladeleistung_w": ladeleistung,
        "stand": int(time.time()),
    }


# ============================================================
# MQTT
# ============================================================

def publish(client, topic, payload):
    client.publish(
        topic,
        json.dumps(payload, ensure_ascii=False, separators=(",", ":")),
        qos=0,
        retain=True,
    )


def on_connect(client, userdata, flags, reason_code, properties):
    log(f"MQTT verbunden: {reason_code}")
    client.publish(TOPIC_STATUS, "online", qos=0, retain=True)
    client.subscribe([(TOPIC_STATE, 0), (TOPIC_EMS_MODE, 0), (TOPIC_POWER, 0)])

    # Nach Neuverbindung alles einmal neu senden
    global last_preise_payload
    with lock:
        last_preise_payload = None


def on_message(client, userdata, msg):

    global steuerung_power, steuerung_power_time, ems_mode
    global last_anlage_publish, last_anlage

    try:
        text = msg.payload.decode("utf-8").strip()

        if msg.topic == TOPIC_POWER:
            steuerung_power = float(text)
            steuerung_power_time = time.time()
            return

        if msg.topic == TOPIC_EMS_MODE:
            ems_mode = text
            if text == "general":
                steuerung_power = None
            return

        if msg.topic == TOPIC_STATE:
            now = time.time()
            if now - last_anlage_publish < ANLAGE_INTERVALL_SECONDS:
                return
            last_anlage_publish = now
            anlage = build_anlage(json.loads(text))
            last_anlage = anlage
            publish(client, TOPIC_ANLAGE, anlage)

    except Exception as e:
        log(f"MQTT-Datenfehler ({msg.topic}): {e}")


def worker(client):

    global last_preise_payload

    log("Rechen-Timer gestartet")

    while True:
        try:
            with lock:
                load_prices()

                preise = build_preise()
                preise_json = json.dumps(preise, sort_keys=True)

                if preise_json != last_preise_payload:
                    publish(client, TOPIC_PREISE, preise)
                    last_preise_payload = preise_json
                    log(
                        "kueche/preise gesendet | "
                        f"heute={'ja' if preise['heute'] else 'nein'} | "
                        f"morgen={'ja' if preise['morgen'] else 'nein'}"
                    )

                ampel = build_ampel()

            publish(client, TOPIC_AMPEL, ampel)

        except Exception as e:
            log(f"Rechenfehler: {e}")

        time.sleep(RECHEN_INTERVALL_SECONDS)



# ============================================================
# WEBSEITE: zeigt dasselbe wie das Display im Browser am PC.
# Nur lesend, ändert nichts an der Anlage.
# ============================================================

class WebHandler(BaseHTTPRequestHandler):

    def do_GET(self):
        pfad = self.path.split("?")[0]
        if pfad in ("/", "/index.html"):
            self.senden(200, "text/html; charset=utf-8", WEB_HTML.encode("utf-8"))
        elif pfad == "/api/daten":
            with lock:
                load_prices()
                daten = {
                    "version": VERSION,
                    "jetzt": int(time.time()),
                    "preise": build_preise(),
                    "ampel": build_ampel(),
                    "anlage": last_anlage,
                }
            body = json.dumps(daten, ensure_ascii=False, separators=(",", ":"))
            self.senden(200, "application/json; charset=utf-8", body.encode("utf-8"))
        else:
            self.senden(404, "text/plain; charset=utf-8", "nicht gefunden".encode("utf-8"))

    def senden(self, code, typ, body):
        self.send_response(code)
        self.send_header("Content-Type", typ)
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(body)

    def log_message(self, format, *args):
        pass


def web_start():
    if WEB_PORT <= 0:
        return
    try:
        server = ThreadingHTTPServer(("", WEB_PORT), WebHandler)
    except OSError as e:
        log(f"Webseite nicht gestartet (Port {WEB_PORT}): {e}")
        return
    server.daemon_threads = True
    threading.Thread(target=server.serve_forever, daemon=True, name="kueche-web").start()
    log(f"Webseite: http://<QNAP-IP>:{WEB_PORT}")


WEB_HTML = r'''<!doctype html>
<html lang="de">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Energiedisplay</title>
<style>
:root{--bg:#101418;--fl:#1C2228;--fl2:#232A31;--tx:#E8ECEF;--ged:#8A949C;--li:#3A434B;
--gr:#2E9E55;--ge:#E8B71A;--ro:#C93C3C;--ruhe:#2A3138;--ok:#3DBE6A;--feh:#E5484D}
*{box-sizing:border-box}
body{margin:0;background:var(--bg);color:var(--tx);font:16px/1.4 system-ui,-apple-system,"Segoe UI",Roboto,sans-serif}
main{max-width:1100px;margin:0 auto;padding:16px}
header{display:flex;align-items:center;gap:12px;margin-bottom:16px}
header h1{font-size:22px;font-weight:600;margin:0;flex:1}
#uhr{font-size:22px;font-variant-numeric:tabular-nums}
#punkt{width:12px;height:12px;border-radius:50%;background:var(--feh)}
.raster{display:grid;grid-template-columns:minmax(0,1fr) minmax(0,1.3fr);gap:16px}
@media(max-width:760px){.raster{grid-template-columns:1fr}}
.karte{background:var(--fl);border-radius:16px;padding:20px}
#ampel{display:flex;flex-direction:column;justify-content:center;min-height:200px}
#ampel h2{margin:0 0 6px;font-size:32px;font-weight:700}
#ampel p{margin:0;font-size:18px}
#preis{font-size:64px;font-weight:700;margin-top:12px;font-variant-numeric:tabular-nums;line-height:1}
#preis small{font-size:16px;font-weight:400;margin-left:8px}
.kacheln{display:grid;grid-template-columns:repeat(3,minmax(0,1fr));gap:12px}
@media(max-width:480px){.kacheln{grid-template-columns:repeat(2,minmax(0,1fr))}}
.kachel{background:var(--fl2);border-radius:12px;padding:14px}
.kachel .n{color:var(--ged);font-size:14px}
.kachel .w{font-size:26px;font-weight:700;margin:2px 0;font-variant-numeric:tabular-nums}
.kachel .s{color:var(--ged);font-size:14px}
.balken{height:6px;background:var(--li);border-radius:3px;margin-top:8px;overflow:hidden}
.balken div{height:100%;background:var(--ok)}
.kopf2{display:flex;align-items:center;gap:8px;margin-bottom:10px;flex-wrap:wrap}
.kopf2 h3{margin:0;font-size:18px;font-weight:600;flex:1}
button{background:var(--fl2);color:var(--tx);border:1px solid var(--li);border-radius:10px;padding:8px 16px;font:inherit;cursor:pointer}
button.an{background:var(--li)}
button:disabled{opacity:.4;cursor:default}
#kurve{width:100%;height:280px;display:block}
#tipp{color:var(--ged);font-size:14px;min-height:20px;margin-top:6px}
footer{color:var(--ged);font-size:14px;margin-top:14px;text-align:center}
.fehler{color:var(--feh)}
</style>
</head>
<body>
<main>
<header><div id="punkt" title="Verbindung"></div><h1 id="titel">Energiedisplay</h1><div id="uhr">--:--</div></header>
<div class="raster">
  <section class="karte" id="ampel"><h2 id="a_titel">Warte auf Daten</h2><p id="a_text"></p><div id="preis"></div></section>
  <section class="karte">
    <div class="kopf2"><h3>Anlage</h3><span id="stand" style="color:var(--ged);font-size:14px"></span></div>
    <div class="kacheln">
      <div class="kachel"><div class="n">PV</div><div class="w" id="pv">–</div><div class="s" id="pv_s"></div></div>
      <div class="kachel"><div class="n">Haus</div><div class="w" id="haus">–</div><div class="s">Verbrauch</div></div>
      <div class="kachel"><div class="n">Netz</div><div class="w" id="netz">–</div><div class="s" id="netz_s"></div></div>
      <div class="kachel"><div class="n">Akku</div><div class="w" id="soc">–</div><div class="balken"><div id="soc_b" style="width:0"></div></div></div>
      <div class="kachel"><div class="n">Akku-Leistung</div><div class="w" id="akku">–</div><div class="s" id="akku_s"></div></div>
      <div class="kachel"><div class="n">Steuerung</div><div class="w" id="st" style="font-size:18px">–</div><div class="s" id="st_s"></div></div>
    </div>
  </section>
</div>
<section class="karte" style="margin-top:16px">
  <div class="kopf2"><h3 id="k_titel">Strompreis heute</h3>
    <button id="b_heute" class="an">Heute</button><button id="b_morgen" disabled>Morgen</button></div>
  <canvas id="kurve"></canvas>
  <div id="tipp"></div>
</section>
<footer id="fuss"></footer>
</main>
<script>
const TZ = "Europe/Berlin";
const FARBE = ["#2E9E55", "#E8B71A", "#C93C3C"];
let daten = null, morgenZeigen = false, fehlerSeit = 0;
const $ = id => document.getElementById(id);

function uhrzeit(ts) {
  return new Date(ts * 1000).toLocaleTimeString("de-DE", {timeZone: TZ, hour: "2-digit", minute: "2-digit"});
}
function komma(v, stellen) { return v == null ? "–" : v.toFixed(stellen).replace(".", ","); }
function watt(w) {
  if (w == null) return "–";
  const a = Math.abs(w);
  return a >= 1000 ? komma(a / 1000, 2) + " kW" : Math.round(a / 10) * 10 + " W";
}

function zeigeAmpel(a) {
  const box = $("ampel");
  const stil = {0: ["#2E9E55", "#fff"], 1: ["#E8B71A", "#1A1A1A"], 2: ["#C93C3C", "#fff"], 3: ["#2A3138", "#C5CCD2"]}[a.farbe]
    || ["#2A3138", "#8A949C"];
  box.style.background = stil[0];
  box.style.color = stil[1];
  $("a_titel").textContent = a.titel || "";
  $("a_text").textContent = a.text || "";
  $("preis").innerHTML = a.preis_ct != null ? komma(a.preis_ct, 1) + "<small>ct/kWh jetzt</small>" : "";
}

function zeigeAnlage(n) {
  if (!n) return;
  $("pv").textContent = watt(n.pv_w);
  $("pv_s").textContent = n.pv_w > 10 ? "Erzeugung" : "keine Sonne";
  $("haus").textContent = watt(n.haus_w);
  $("netz").textContent = watt(n.netz_w);
  $("netz_s").textContent = n.netz_w > 10 ? "Bezug" : (n.netz_w < -10 ? "Einspeisung" : "ausgeglichen");
  $("soc").textContent = n.soc != null ? Math.round(n.soc) + " %" : "–";
  $("soc_b").style.width = (n.soc || 0) + "%";
  $("akku").textContent = watt(n.akku_w);
  $("akku_s").textContent = n.akku_w > 10 ? "Entladen" : (n.akku_w < -10 ? "Laden" : "Ruht");
  const st = {LADEN: ["Laden aus dem Netz", n.ladeleistung_w ? watt(n.ladeleistung_w) : ""],
              HALTEN: ["Akku wird gehalten", "für teurere Stunden"],
              AUTOMATIK: ["Automatik", "Hoymiles regelt"]}[n.steuerung] || [n.steuerung || "–", ""];
  $("st").textContent = st[0];
  $("st_s").textContent = st[1];
  $("stand").textContent = n.stand ? "Stand " + uhrzeit(n.stand) : "";
}

function tagDaten() { return daten && daten.preise ? (morgenZeigen ? daten.preise.morgen : daten.preise.heute) : null; }

function zeichne() {
  const cv = $("kurve"), dpr = window.devicePixelRatio || 1;
  const B = cv.clientWidth, H = cv.clientHeight;
  cv.width = B * dpr; cv.height = H * dpr;
  const c = cv.getContext("2d");
  c.scale(dpr, dpr);
  c.clearRect(0, 0, B, H);
  const tag = tagDaten();
  c.font = "13px system-ui,sans-serif";
  if (!tag) { c.fillStyle = "#8A949C"; c.textAlign = "center"; c.fillText("Keine Preisdaten", B / 2, H / 2); return; }

  const L = 44, R = 8, O = 26, U = 24, w = B - L - R, h = H - O - U;
  const lo = Math.min(0, Math.floor(tag.min / 5) * 5), hi = Math.ceil(tag.max / 5) * 5 || 5;
  const Y = v => O + h - (v - lo) / (hi - lo) * h;
  const slot = w / tag.anzahl;
  const X = i => L + i * slot;
  const jetzt = daten.jetzt;

  // Gitter + Achse
  c.strokeStyle = "#3A434B"; c.fillStyle = "#8A949C"; c.lineWidth = 1; c.textAlign = "right";
  const schritt = (hi - lo) > 30 ? 10 : 5;
  for (let v = lo; v <= hi; v += schritt) {
    c.beginPath(); c.moveTo(L, Y(v) + .5); c.lineTo(B - R, Y(v) + .5); c.stroke();
    c.fillText(v, L - 6, Y(v) + 4);
  }
  c.textAlign = "left"; c.fillText("ct/kWh", 0, 12);

  // Bestes Fenster
  const a = daten.ampel || {};
  if (a.fenster_start && a.fenster_ende) {
    const i0 = (a.fenster_start - tag.start) / 900, i1 = (a.fenster_ende - tag.start) / 900;
    if (i1 > 0 && i0 < tag.anzahl) {
      const x0 = X(Math.max(0, i0)), x1 = X(Math.min(tag.anzahl, i1));
      c.fillStyle = "rgba(232,236,239,0.10)"; c.fillRect(x0, O, x1 - x0, h);
      c.fillStyle = "#E8ECEF"; c.textAlign = "center";
      c.fillText("bestes Fenster " + (a.fenster_text || ""), (x0 + x1) / 2, O - 8);
    }
  }

  // Balken
  for (let i = 0; i < tag.anzahl; i++) {
    const v = tag.ct[i], vorbei = tag.start + (i + 1) * 900 <= jetzt;
    c.globalAlpha = vorbei ? 0.3 : 1;
    c.fillStyle = FARBE[tag.stufe[i]];
    const y0 = Y(v), y1 = Y(0);
    c.fillRect(X(i) + 0.5, Math.min(y0, y1), Math.max(1, slot - 1), Math.abs(y1 - y0) || 1);
  }
  c.globalAlpha = 1;

  // Stunden
  c.fillStyle = "#8A949C"; c.textAlign = "center";
  for (let i = 0; i < tag.anzahl; i++) {
    const ts = tag.start + i * 900;
    const d = new Date(ts * 1000);
    // parseInt, weil "de-DE" die Stunde als "06 Uhr" liefert
    const std = parseInt(d.toLocaleString("de-DE", {timeZone: TZ, hour: "2-digit", hour12: false}), 10);
    const min = parseInt(d.toLocaleString("de-DE", {timeZone: TZ, minute: "2-digit"}), 10);
    if (min === 0 && std % 3 === 0) c.fillText(String(std).padStart(2, "0") + ":00", X(i), H - 6);
  }

  // Jetzt
  if (jetzt >= tag.start && jetzt < tag.start + tag.anzahl * 900) {
    const x = X((jetzt - tag.start) / 900);
    c.strokeStyle = "#E8ECEF"; c.lineWidth = 2;
    c.beginPath(); c.moveTo(x, O); c.lineTo(x, O + h); c.stroke();
  }
}

function tippZeigen(ev) {
  const tag = tagDaten(); if (!tag) return;
  const cv = $("kurve"), r = cv.getBoundingClientRect();
  const i = Math.floor((ev.clientX - r.left - 44) / ((r.width - 52) / tag.anzahl));
  if (i < 0 || i >= tag.anzahl) { $("tipp").textContent = ""; return; }
  const ts = tag.start + i * 900;
  $("tipp").textContent = uhrzeit(ts) + "–" + uhrzeit(ts + 900) + " Uhr: " + komma(tag.ct[i], 1) + " ct/kWh";
}

function knopfe() {
  $("b_heute").classList.toggle("an", !morgenZeigen);
  $("b_morgen").classList.toggle("an", morgenZeigen);
  $("b_morgen").disabled = !(daten && daten.preise && daten.preise.morgen);
  if (morgenZeigen && $("b_morgen").disabled) morgenZeigen = false;
  const tag = tagDaten();
  let t = "Strompreis " + (morgenZeigen ? "morgen" : "heute");
  if (tag) t += " · " + new Date(tag.start * 1000).toLocaleDateString("de-DE", {timeZone: TZ, weekday: "long", day: "numeric", month: "numeric"});
  $("k_titel").textContent = t;
}

async function laden() {
  try {
    const r = await fetch("api/daten", {cache: "no-store"});
    daten = await r.json();
    fehlerSeit = 0;
    zeigeAmpel(daten.ampel || {});
    zeigeAnlage(daten.anlage);
    knopfe(); zeichne();
    const frisch = daten.anlage && daten.jetzt - daten.anlage.stand < 120;
    $("punkt").style.background = frisch ? "var(--ok)" : "var(--feh)";
    $("fuss").textContent = "Version " + daten.version + (frisch ? "" : " · keine aktuellen Anlagendaten");
  } catch (e) {
    if (!fehlerSeit) fehlerSeit = Date.now();
    $("punkt").style.background = "var(--feh)";
    $("fuss").innerHTML = '<span class="fehler">QNAP nicht erreichbar, neuer Versuch läuft</span>';
  }
}

function tick() {
  $("uhr").textContent = new Date().toLocaleTimeString("de-DE", {timeZone: TZ, hour: "2-digit", minute: "2-digit"});
}

$("b_heute").onclick = () => { morgenZeigen = false; knopfe(); zeichne(); };
$("b_morgen").onclick = () => { morgenZeigen = true; knopfe(); zeichne(); };
$("kurve").addEventListener("mousemove", tippZeigen);
$("kurve").addEventListener("mouseleave", () => $("tipp").textContent = "");
window.addEventListener("resize", zeichne);
tick(); setInterval(tick, 5000);
laden(); setInterval(laden, 10000);
</script>
</body>
</html>
'''

# ============================================================
# TESTMODUS: rechnet einmal mit der Preisdatei und gibt
# die JSON-Daten aus. Kein MQTT, nichts wird gesendet.
# ============================================================

def run_test():
    load_prices(force=True)
    print("\n=== kueche/ampel ===")
    print(json.dumps(build_ampel(), ensure_ascii=False, indent=2))
    preise = build_preise()
    print("\n=== kueche/preise (gekürzt) ===")
    for key in ("heute", "morgen"):
        day = preise[key]
        if day is None:
            print(f"{key}: keine Daten")
        else:
            print(
                f"{key}: {day['datum']} | {day['anzahl']} Werte | "
                f"{day['min']}–{day['max']} ct | "
                f"Größe {len(json.dumps(day, separators=(',', ':')))} Byte"
            )


# ============================================================
# START
# ============================================================

def handle_sigterm(signum, frame):
    raise KeyboardInterrupt


if __name__ == "__main__":

    if "--test" in sys.argv:
        run_test()
        sys.exit(0)

    signal.signal(signal.SIGTERM, handle_sigterm)

    log("")
    log(f"Energiedisplay-Publisher {VERSION} startet")
    log(f"Preisdatei: {PRICE_FILE}")
    log(
        "Preisart: "
        + ("Börsenpreis EPEX" if AUFSCHLAG_NETTO_CT is None
           else f"Endpreis (EPEX + {AUFSCHLAG_NETTO_CT} ct) * {1 + MWST:.2f}")
    )
    log(
        f"Ampel: Laufzeit {LAUFZEIT_STUNDEN} h | Horizont {HORIZONT_STUNDEN} h | "
        f"grün <= {GRUEN_MAX_CT} ct | gelb <= {GELB_MAX_CT} ct"
    )

    load_prices(force=True)
    web_start()

    client = mqtt.Client(
        mqtt.CallbackAPIVersion.VERSION2,
        client_id="energiedisplay-publisher",
    )
    if MQTT_USER:
        client.username_pw_set(MQTT_USER, MQTT_PASS)
    client.on_connect = on_connect
    client.on_message = on_message
    client.will_set(TOPIC_STATUS, "offline", qos=0, retain=True)

    threading.Thread(
        target=worker, args=(client,), daemon=True, name="kueche-worker"
    ).start()

    while True:
        try:
            client.connect(BROKER, PORT, 60)
            client.loop_forever()

        except KeyboardInterrupt:
            try:
                client.publish(TOPIC_STATUS, "offline", qos=0, retain=True)
                client.loop(timeout=0.5)
                client.disconnect()
            except Exception:
                pass
            log("Energiedisplay-Publisher beendet")
            break

        except Exception as e:
            log(f"MQTT-Verbindung Fehler: {e} | neuer Versuch in 10 s")
            time.sleep(10)

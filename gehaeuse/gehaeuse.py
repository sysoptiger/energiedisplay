"""Energiedisplay – Gehäuse (Rückschale + 2 Standfüße) als STL.

Waveshare ESP32-S3-Touch-LCD-7. Alle Maße in mm. Die Werte unter
MASSE stammen aus eigener Messung; alles andere leitet sich daraus ab.

Koordinaten der Rückschale: x = Breite, y = Höhe (oben +),
z = Tiefe ab Glasrückseite nach hinten.
"""

import math
import sys

import numpy as np
import trimesh
from manifold3d import CrossSection, JoinType, Manifold

# ============================================================
# MASSE (gemessen)
# ============================================================

GLAS_B = 193.0           # Glas außen, Breite
GLAS_H = 110.0           # Glas außen, Höhe
GLAS_DICKE = 1.2         # Glas vorne bis Glasrückseite (am Rand)
TIEFE = 17.6             # Glasrückseite bis höchstes Bauteil hinten (mit USB-Stecker)
LASCHE_Z = 7.4           # Glasrückseite bis Laschenfläche (Beginn der Bolzen)
LOCH_DX = 126.0          # Lochabstand waagerecht (Mitte-Mitte), nach Probedruck +1 mm
LOCH_DY = 65.0           # Lochabstand senkrecht (Mitte-Mitte), gemessen
BOLZEN_L = 8.0           # mitgelieferte Abstandsbolzen, Schale liegt darauf auf
LOCH_MITTE = (0.0, 0.0)  # Versatz der Lochmitte zur Glasmitte (x, y)
GEWINDE = 3.0            # M3
SCHRAUBE_L = 8.0         # Schraube von hinten in den Bolzen

# Kabel: Schlitz unten in der Rückwand
KABEL_POS = 0.0          # Mitte des Kabelschlitzes, ab Glasmitte
KABEL_D = 11.0           # Schlitzbreite (Probedruck 6 mm war zu knapp: +2,5 mm je Seite)
KABEL_H = 17.0           # Schlitzhöhe ab Unterkante (Probedruck 14 mm: +3 mm)

# ============================================================
# GESTALTUNG
# ============================================================

WAND = 2.4
LUFT_HINTEN = 3.0        # Luft hinter dem höchsten Bauteil
RADIUS = 7.5             # Eckradius wie das Glas
DOM_D = 10.0
GEWINDE_EINGRIFF = 4.0   # so tief greift die Schraube in den Bolzen
NEIGUNG = 20.0           # Grad nach hinten
FUSS_BREITE = 30.0
SEG = 48


def rundrechteck(b, h, r):
    return CrossSection.square([b - 2 * r, h - 2 * r], center=True).offset(
        r, JoinType.Round, circular_segments=SEG)


def zylinder(h, d, z0=0.0):
    return Manifold.cylinder(h, d / 2, circular_segments=SEG).translate([0, 0, z0])


def quader(x0, x1, y0, y1, z0, z1):
    return Manifold.cube([x1 - x0, y1 - y0, z1 - z0]).translate([x0, y0, z0])


def rueckschale():
    innen_t = TIEFE + LUFT_HINTEN
    aussen_t = innen_t + WAND

    schale = rundrechteck(GLAS_B, GLAS_H, RADIUS).extrude(aussen_t)
    hohl = rundrechteck(GLAS_B - 2 * WAND, GLAS_H - 2 * WAND, RADIUS - WAND / 2).extrude(innen_t)
    schale = schale - hohl

    # Schraubdome vom Bolzenende bis zum Boden, Schraube von hinten in den Bolzen
    auflage_z = LASCHE_Z + BOLZEN_L
    hinweise = []
    durchgang = GEWINDE + 0.4
    kopf_d = GEWINDE * 2 + 1.0
    plastik = SCHRAUBE_L - GEWINDE_EINGRIFF          # so viel Kunststoff darf die Schraube durchqueren
    kopf_z = auflage_z + plastik                      # Kopfauflage
    if kopf_z > aussen_t:
        kopf_z = aussen_t
    if kopf_z < auflage_z + 3:
        kopf_z = auflage_z + 3
        hinweise.append(f"Schraubenlänge: {GEWINDE_EINGRIFF + 3:.0f} mm")

    for sx in (-1, 1):
        for sy in (-1, 1):
            x = LOCH_MITTE[0] + sx * LOCH_DX / 2
            y = LOCH_MITTE[1] + sy * LOCH_DY / 2
            dom = zylinder(aussen_t - auflage_z, DOM_D, auflage_z)
            loch = zylinder(aussen_t + 2, durchgang, -1)
            senkung = zylinder(aussen_t - kopf_z + 1, kopf_d, kopf_z)
            schale = (schale + dom.translate([x, y, 0])) - loch.translate([x, y, 0]) - senkung.translate([x, y, 0])

    # Kabelschlitz unten in der Rückwand, zur Unterkante hin offen:
    # Kabel wird von unten eingelegt, kein Einfädeln nötig. Es läuft hinten
    # waagerecht raus, zwischen den beiden Standfüßen durch.
    cx, unten = KABEL_POS, -GLAS_H / 2
    schlitz_h = KABEL_H
    schlitz = quader(cx - KABEL_D / 2, cx + KABEL_D / 2, unten - 1, unten + schlitz_h - KABEL_D / 2,
                     innen_t - 1, aussen_t + 1)
    rund = zylinder(WAND + 2, KABEL_D, innen_t - 1).translate([cx, unten + schlitz_h - KABEL_D / 2, 0])
    schale = schale - schlitz - rund

    # Zugentlastung: Steg neben dem Schlitz, Kabelbinder läuft darunter durch
    if True:
        y0 = -GLAS_H / 2 + WAND + 14
        for bx in (KABEL_POS - 16, KABEL_POS + 16):
            steg = quader(bx - 5, bx + 5, y0, y0 + 6, innen_t - 6, innen_t)
            tunnel = quader(bx - 2.5, bx + 2.5, y0 - 1, y0 + 7, innen_t - 4, innen_t - 1.5)
            schale = schale + steg - tunnel

    # Lüftungsschlitze oben und unten im Boden
    for y in (GLAS_H / 2 - 14, -GLAS_H / 2 + 46):
        for i in range(-5, 6):
            schale = schale - quader(i * 8 - 1.5, i * 8 + 1.5, y - 6, y + 6, innen_t - 1, aussen_t + 1)

    return schale, aussen_t, hinweise


def standfuss(gesamt_t):
    """Seitenprofil als Fläche, in x extrudiert. Liegend druckbar.

    Ein U-förmiger Halter, um NEIGUNG nach hinten gekippt, auf einer
    Grundplatte. Das Display steht mit der Unterkante im U.
    """
    schlitz = gesamt_t + 0.6
    wand = 3.0
    boden = 3.0
    halter_h = 45.0
    lippe_h = 8.0            # vordere Lippe niedrig, damit nichts vom Bild verdeckt wird
    sockel = 4.0

    halter = CrossSection.square([schlitz + 2 * wand, halter_h]).translate([-wand, 0])
    halter = halter - CrossSection.square([schlitz, halter_h]).translate([0, boden])
    halter = halter - CrossSection.square([wand + 2, halter_h]).translate([-wand - 1, boden + lippe_h])

    # Kippen um die hintere untere Ecke, oben wandert nach hinten (+x)
    l = schlitz + 2 * wand
    w = math.radians(NEIGUNG)
    halter = halter.translate([-(l - wand), 0]).rotate(-NEIGUNG)
    vorne = (-l * math.cos(w), l * math.sin(w))
    keil = CrossSection([[(0.0, 0.0), vorne, (vorne[0], 0.0)]])

    platte = CrossSection.square([vorne[0] * -1 + 60.0, sockel]).translate([vorne[0] - 2.0, -sockel])
    profil = halter + keil + platte
    return profil.extrude(FUSS_BREITE)


def speichern(m, pfad):
    mesh = m.to_mesh()
    t = trimesh.Trimesh(vertices=np.asarray(mesh.vert_properties)[:, :3],
                        faces=np.asarray(mesh.tri_verts))
    t.export(pfad)
    return t


if __name__ == "__main__":
    ziel = sys.argv[1] if len(sys.argv) > 1 else "."
    schale, aussen_t, hinweise = rueckschale()
    # Druckage: Boden nach unten
    schale_druck = schale.mirror([0, 0, 1]).translate([0, 0, aussen_t])
    t1 = speichern(schale_druck, f"{ziel}/rueckschale.stl")
    fuss = standfuss(GLAS_DICKE + aussen_t)
    t2 = speichern(fuss, f"{ziel}/standfuss.stl")
    print("Rückschale", np.round(t1.extents, 1), "dicht" if t1.is_watertight else "NICHT dicht")
    print("Standfuß  ", np.round(t2.extents, 1), "dicht" if t2.is_watertight else "NICHT dicht")
    for h in hinweise:
        print("Hinweis:", h)

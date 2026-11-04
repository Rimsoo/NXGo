#!/usr/bin/env python3
"""Lance un .nro sur la Switch en mode jeu (toute la RAM) et garde ses logs.

Depuis n'importe où : quitte le logiciel en cours, démarre hbmenu en maintenant R
sur le premier jeu de l'écran d'accueil, active le NetLoader (Y) puis envoie le
.nro avec `nxlink -s`. La sortie de l'appli (stderr) va dans --log ; le script
rend la main dès l'envoi terminé et laisse l'appli tourner.

  SWITCH_IP=192.168.x.x devrun.py nxgo.nro [--log nxgo.log] [--shots dossier]
"""

import argparse
import io
import os
import subprocess
import sys
import time

from PIL import Image

from botbase import HOST, Botbase

NXLINK = "/opt/devkitpro/tools/bin/nxlink"


def shot(bot, shots, name):
    img = Image.open(io.BytesIO(bot.screenshot())).convert("RGB")
    if shots:
        os.makedirs(shots, exist_ok=True)
        img.save(os.path.join(shots, name + ".jpg"))
    return img


def click(bot, *buttons, delay=0.4):
    for b in buttons:
        bot.send(f"click {b}")
        time.sleep(delay)


def quit_dialog_open(img):
    # Bouton "Quitter" sélectionné : contour cyan dans la moitié droite de la boîte
    region = img.crop((640, 455, 1030, 535))
    cyan = sum(1 for r, g, b in region.getdata() if b > 180 and g > 150 and r < 120)
    return cyan > 300


def home_visible(img):
    # Écran d'accueil (thème sombre) : barre du bas gris foncé uniforme
    pixels = [img.getpixel((x, 600)) for x in (60, 640, 1220)]
    return all(35 <= c <= 60 for p in pixels for c in p)


def hbmenu_visible(img):
    # Bas de l'écran turquoise propre à hbmenu
    r, g, b = img.getpixel((640, 700))
    return g > 120 and b > 120 and r < 80


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("nro")
    p.add_argument("--log", default="nxlink.log")
    p.add_argument("--shots", help="garde les captures de chaque étape dans ce dossier")
    args = p.parse_args()

    bot = Botbase()
    for attempt in range(3):
        # HOME depuis l'accueil revient au logiciel en cours : on vérifie à chaque fois
        click(bot, "HOME", delay=1.5)
        if home_visible(shot(bot, args.shots, f"0-home{attempt}")):
            break
    else:
        bot.close()
        sys.exit("impossible d'atteindre l'écran d'accueil (voir les captures avec --shots)")
    click(bot, "X", delay=1.2)
    if quit_dialog_open(shot(bot, args.shots, "1-quit")):
        click(bot, "A", delay=3)
    else:
        click(bot, "B", delay=0.5)
    # Fermer un logiciel prend quelques secondes
    for i in range(10):
        if home_visible(shot(bot, args.shots, "1b-home")):
            break
        time.sleep(1)
    else:
        bot.close()
        sys.exit("pas revenu à l'écran d'accueil après avoir quitté le logiciel")

    # Premier jeu de l'écran d'accueil, R maintenu : hbmenu à la place du jeu
    click(bot, "DDOWN", "DUP", delay=0.4)  # rangée des jeux, quel que soit le point de départ
    click(bot, *["DLEFT"] * 8, delay=0.15)
    bot.send("press R")
    time.sleep(0.3)
    click(bot, "A", delay=4)
    bot.send("release R")
    time.sleep(1)
    if not hbmenu_visible(shot(bot, args.shots, "2-hbmenu")):
        bot.close()
        sys.exit("hbmenu n'est pas apparu (voir les captures avec --shots)")
    click(bot, "Y", delay=1.5)
    bot.close()

    log = open(args.log, "w")
    proc = subprocess.Popen(["stdbuf", "-oL", NXLINK, "-a", HOST, "-s", args.nro], stdout=log, stderr=subprocess.STDOUT,
                            start_new_session=True)
    deadline = time.time() + 120
    while time.time() < deadline and proc.poll() is None:
        time.sleep(0.5)
        if "server active" in open(args.log, errors="replace").read():
            print(f"✓ {os.path.basename(args.nro)} lancé (logs : {args.log}, nxlink pid {proc.pid})")
            return
    sys.exit("échec de l'envoi :\n" + open(args.log, errors="replace").read())


if __name__ == "__main__":
    main()

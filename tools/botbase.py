#!/usr/bin/env python3
"""Pilotage de la Switch via sys-botbase (TCP, port 6000, commandes texte).

  botbase.py version
  botbase.py screenshot [sortie.jpg]
  botbase.py click A [B HOME ...]      appuis brefs successifs
  botbase.py press A / release A      maintenir / relâcher
  botbase.py stick LEFT|RIGHT x y     -32768..32767
  botbase.py tap x y [x y ...]        touchers brefs à l'écran (1280x720)
  botbase.py raw "commande"           commande sys-botbase brute

Boutons : A B X Y RSTICK LSTICK L R ZL ZR PLUS MINUS DLEFT DUP DRIGHT DDOWN HOME CAPTURE
"""

import argparse
import os
import socket
import sys
import time

# Adresse IP de la Switch (affichée par ftpd ou dans les paramètres réseau)
HOST = os.environ.get("SWITCH_IP", "")
PORT = 6000


class Botbase:
    def __init__(self, host=None, port=PORT, timeout=10):
        host = host or HOST
        if not host:
            raise SystemExit("Indique l'IP de la Switch : export SWITCH_IP=192.168.x.x")
        self.sock = socket.create_connection((host, port), timeout=timeout)

    def send(self, command):
        self.sock.sendall(command.encode() + b"\r\n")

    def recv_line(self):
        buf = bytearray()
        while not buf.endswith(b"\n"):
            chunk = self.sock.recv(65536)
            if not chunk:
                raise ConnectionError("connexion fermée par sys-botbase")
            buf += chunk
        return buf.decode().strip()

    def query(self, command):
        self.send(command)
        return self.recv_line()

    def screenshot(self):
        # pixelPeek renvoie le JPEG de l'écran encodé en hexadécimal
        return bytes.fromhex(self.query("pixelPeek"))

    def close(self):
        self.sock.close()


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--host", default=HOST)
    sub = p.add_subparsers(dest="cmd", required=True)
    sub.add_parser("version")
    s = sub.add_parser("screenshot")
    s.add_argument("output", nargs="?", default="screen.jpg")
    s = sub.add_parser("click")
    s.add_argument("buttons", nargs="+")
    s.add_argument("--delay", type=float, default=0.5, help="pause entre deux appuis (s)")
    for name in ("press", "release"):
        sub.add_parser(name).add_argument("button")
    s = sub.add_parser("stick")
    s.add_argument("side", choices=["LEFT", "RIGHT"])
    s.add_argument("x", type=int)
    s.add_argument("y", type=int)
    s = sub.add_parser("tap")
    s.add_argument("coords", nargs="+", type=int)
    sub.add_parser("raw").add_argument("command")
    args = p.parse_args()

    if not args.host:
        sys.exit("Indique l'IP de la Switch : export SWITCH_IP=192.168.x.x (ou --host)")
    try:
        bot = Botbase(args.host)
    except OSError as e:
        sys.exit(f"Impossible de joindre sys-botbase sur {args.host}:{PORT} : {e}")
    try:
        if args.cmd == "version":
            print(bot.query("getVersion"))
        elif args.cmd == "screenshot":
            data = bot.screenshot()
            open(args.output, "wb").write(data)
            print(f"✓ capture enregistrée : {args.output} ({len(data)} o)")
        elif args.cmd == "click":
            for i, b in enumerate(args.buttons):
                if i:
                    time.sleep(args.delay)
                bot.send(f"click {b.upper()}")
        elif args.cmd in ("press", "release"):
            bot.send(f"{args.cmd} {args.button.upper()}")
        elif args.cmd == "stick":
            bot.send(f"setStick {args.side} {args.x} {args.y}")
        elif args.cmd == "tap":
            if len(args.coords) % 2:
                sys.exit("tap attend des paires x y")
            bot.send("touch " + " ".join(str(c) for c in args.coords))
        elif args.cmd == "raw":
            bot.send(args.command)
    finally:
        bot.close()


if __name__ == "__main__":
    main()

#!/bin/sh
# ADS HMI (Linux) : installe le service systemd « ads-hmi » : démarrage au boot, relance en cas d'arrêt
# anormal, journal dans journalctl. À lancer avec sudo depuis le dossier du kit (chemin sans espace).
set -eu
DIR=$(cd "$(dirname "$0")" && pwd)
. "$DIR/parametres.sh"
[ "$(id -u)" -eq 0 ] || { echo "À lancer avec sudo." >&2; exit 1; }
command -v systemctl >/dev/null || { echo "systemd absent : utiliser kiosque.sh." >&2; exit 1; }
case "$DIR" in *" "*) echo "Le chemin du kit ne doit pas contenir d'espace : $DIR" >&2; exit 1 ;; esac
UTIL=${SUDO_USER:-$(id -un)}

sed -e "s|@DIR@|$DIR|g" -e "s|@USER@|$UTIL|g" -e "s|@HOTE@|$HOTE|g" -e "s|@PORT@|$PORT|g" \
    "$DIR/ads-hmi.service.in" > /etc/systemd/system/ads-hmi.service
systemctl daemon-reload
systemctl enable --now ads-hmi.service
systemctl --no-pager --lines=5 status ads-hmi.service
echo "Journal : journalctl -u ads-hmi -f"

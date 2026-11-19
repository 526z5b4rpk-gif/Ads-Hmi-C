#!/bin/sh
# ADS HMI : lance le service au premier plan (mise en service, diagnostic). Ctrl+C : arrêt propre.
cd "$(dirname "$0")" || exit 1
. ./parametres.sh
echo "Éditeur   : http://127.0.0.1:$PORT/#/edit/$ECRAN"
echo "Exécution : http://127.0.0.1:$PORT/#/run/$ECRAN"
exec ./hmi-service --data data --web web --host "$HOTE" --port "$PORT"

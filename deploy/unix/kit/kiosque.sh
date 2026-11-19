#!/bin/sh
# ADS HMI : poste opérateur. Démarre le service s'il ne répond pas déjà (sinon : service systemd),
# puis ouvre l'écran d'exécution dans Chromium en mode kiosque.
cd "$(dirname "$0")" || exit 1
. ./parametres.sh
URL="http://127.0.0.1:$PORT/#/run/$ECRAN"

repond() { curl -s -o /dev/null --max-time 1 "http://127.0.0.1:$PORT/api/config"; }

if ! repond; then
    mkdir -p logs
    # journal limité à environ 10 Mo : le précédent est conservé en hmi-service.log.1
    if [ -f logs/hmi-service.log ] && [ "$(wc -c < logs/hmi-service.log)" -gt 10485760 ]; then
        mv -f logs/hmi-service.log logs/hmi-service.log.1
    fi
    nohup ./hmi-service --data data --web web --host "$HOTE" --port "$PORT" >> logs/hmi-service.log 2>&1 &
    i=0
    until repond; do
        i=$((i + 1))
        if [ "$i" -ge 20 ]; then echo "Le service ne répond pas : voir logs/hmi-service.log" >&2; exit 1; fi
        sleep 1
    done
fi

# profil dédié : instance séparée du navigateur de l'utilisateur (sinon --kiosk est ignoré
# quand le navigateur est déjà ouvert, et la page s'ouvre dans un simple onglet)
PROFIL="${XDG_CONFIG_HOME:-$HOME/.config}/ads-hmi-kiosque"
OPTS="--kiosk --no-first-run --noerrdialogs --disable-session-crashed-bubble --user-data-dir=$PROFIL"
for b in chromium chromium-browser google-chrome; do
    if command -v "$b" >/dev/null 2>&1; then
        exec "$b" $OPTS "$URL"
    fi
done
if [ "$(uname -s)" = Darwin ]; then exec open -na "Google Chrome" --args $OPTS "$URL"; fi
echo "Aucun navigateur Chromium trouvé : ouvrir $URL" >&2
exit 1

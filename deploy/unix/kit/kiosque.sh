#!/bin/sh
# ADS HMI : poste opérateur. Le service ouvre lui-même l'écran d'exécution dans Chromium (ou Chrome,
# Edge) en plein écran ; le bouton « Quitter » ou la fermeture du navigateur arrête le service.
# Si le service tourne déjà (service systemd) : seul le navigateur est ouvert.
cd "$(dirname "$0")" || exit 1
. ./parametres.sh
URL="http://127.0.0.1:$PORT/#/run/$ECRAN"

if ! curl -s -o /dev/null --max-time 1 "http://127.0.0.1:$PORT/api/session"; then
    mkdir -p logs
    # journal limité à environ 10 Mo : le précédent est conservé en hmi-service.log.1
    if [ -f logs/hmi-service.log ] && [ "$(wc -c < logs/hmi-service.log)" -gt 10485760 ]; then
        mv -f logs/hmi-service.log logs/hmi-service.log.1
    fi
    exec ./hmi-service --data data --web web --host "$HOTE" --port "$PORT" --kiosk --screen "$ECRAN" >> logs/hmi-service.log 2>&1
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

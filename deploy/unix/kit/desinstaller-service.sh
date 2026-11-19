#!/bin/sh
# ADS HMI (Linux) : supprime le service systemd « ads-hmi » (le dossier du kit et data/ sont conservés).
set -eu
[ "$(id -u)" -eq 0 ] || { echo "À lancer avec sudo." >&2; exit 1; }
systemctl disable --now ads-hmi.service 2>/dev/null || true
rm -f /etc/systemd/system/ads-hmi.service
systemctl daemon-reload
echo "Service ads-hmi supprimé."

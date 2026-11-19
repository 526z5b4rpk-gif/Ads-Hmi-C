#!/bin/sh
# ADS HMI : arrêt propre du service lancé par kiosque.sh (service systemd : systemctl stop ads-hmi).
if pkill -TERM -x hmi-service; then echo "Service arrêté."; else echo "Service non lancé."; fi

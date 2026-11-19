# Paramètres communs aux scripts du kit ADS HMI (modifier ici, pas dans les autres scripts).
# Une variable déjà définie dans l'environnement est conservée.

# Port HTTP du service
PORT=${PORT:-8080}

# Adresse d'écoute : 127.0.0.1 = ce poste uniquement (recommandé).
# 0.0.0.0 = accessible depuis le réseau : pas encore d'authentification, réseau machine isolé uniquement.
HOTE=${HOTE:-127.0.0.1}

# Écran affiché par kiosque.sh (nom du fichier dans data/dashboards, sans .json)
ECRAN=${ECRAN:-demo}

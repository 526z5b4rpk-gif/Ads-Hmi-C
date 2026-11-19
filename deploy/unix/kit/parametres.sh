# Paramètres communs aux scripts du kit ADS HMI (modifier ici, pas dans les autres scripts).
# Une variable déjà définie dans l'environnement est conservée.

# Port HTTP du service
PORT=${PORT:-8080}

# Adresse d'écoute : 127.0.0.1 = ce poste uniquement (recommandé).
# 0.0.0.0 = accessible depuis le réseau : les autres postes sont en LECTURE SEULE, sauf avec le jeton.
HOTE=${HOTE:-127.0.0.1}

# Jeton d'accès des postes distants (8 caractères au moins, sans espace ni ; & = ").
# Un poste distant ouvre http://<ce-pc>:8080/?token=<jeton> une fois pour pouvoir modifier.
# Vide : postes distants en lecture seule. Ce poste-ci a toujours tous les droits.
JETON=${JETON:-}
if [ -n "$JETON" ]; then HMI_TOKEN=$JETON; export HMI_TOKEN; fi

# Écran affiché par kiosque.sh (nom du fichier dans data/dashboards, sans .json)
ECRAN=${ECRAN:-demo}

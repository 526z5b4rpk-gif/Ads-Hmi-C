@rem Parametres communs aux scripts du kit ADS HMI (modifier ici, pas dans les autres scripts).
@rem Une variable deja definie dans l'environnement est conservee.

@rem Port HTTP du service
@if not defined PORT set PORT=8080

@rem Adresse d'ecoute : 127.0.0.1 = ce poste uniquement (recommande).
@rem 0.0.0.0 = accessible depuis le reseau : pas encore d'authentification, reseau machine isole uniquement.
@if not defined HOTE set HOTE=127.0.0.1

@rem Ecran affiche par kiosque.bat (nom du fichier dans data\dashboards, sans .json)
@if not defined ECRAN set ECRAN=demo

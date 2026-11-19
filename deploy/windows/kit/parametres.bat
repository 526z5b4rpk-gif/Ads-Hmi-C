@rem Parametres communs aux scripts du kit ADS HMI (modifier ici, pas dans les autres scripts).
@rem Une variable deja definie dans l'environnement est conservee.

@rem Port HTTP du service
@if not defined PORT set PORT=8080

@rem Adresse d'ecoute : 127.0.0.1 = ce poste uniquement (recommande).
@rem 0.0.0.0 = accessible depuis le reseau : les autres postes sont en LECTURE SEULE, sauf avec le jeton.
@if not defined HOTE set HOTE=127.0.0.1

@rem Jeton d'acces des postes distants (8 caracteres au moins, sans espace ni ; & = ").
@rem Un poste distant ouvre http://<ce-pc>:8080/?token=<jeton> une fois pour pouvoir modifier.
@rem Vide : postes distants en lecture seule. Ce poste-ci a toujours tous les droits.
@if not defined JETON set JETON=
@if defined JETON set HMI_TOKEN=%JETON%

@rem Ecran affiche par kiosque.bat (nom du fichier dans data\dashboards, sans .json)
@if not defined ECRAN set ECRAN=demo

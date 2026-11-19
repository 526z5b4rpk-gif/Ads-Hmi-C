# ADS HMI — service C + interface web

IHM temps réel pour automates Beckhoff (ADS). Deux parties indépendantes :

| Partie | Langage | Rôle |
|---|---|---|
| **Service** (`src/`) | C99, sans dépendance hors cJSON (compilable avec LabWindows/CVI 2020, MinGW, gcc, clang) | source de données (simulateur, ADS au jalon 2), détection des changements, serveur HTTP + WebSocket, écrans et configuration |
| **Interface** (`web/`) | HTML/CSS/JS compilés (sources TypeScript + React dans le projet `ads-hmi-studio`) ; `calculs.html` en JavaScript pur | affichage, éditeur d'écrans, édition des variables calculées ; aucune logique automate |

Le service parle exactement le même protocole que le serveur Node de `ads-hmi-studio` : le front web existant
fonctionne sans modification, et n'importe quel autre front (JavaScript pur, autre framework) peut s'y brancher.

```
Automate ──ADS/TCP──▶ [ hmi-service (C) ] ──HTTP + WebSocket──▶ [ Navigateur ]
                        hub, codec, simulateur     ◀── écritures (boutons, consignes)
```

## Compiler et lancer

```sh
make                 # build/hmi-service (~150 Ko)
make run             # http://127.0.0.1:8080
```

Options :

```
--data <dossier>   écrans et configuration (défaut : data)
--web <dossier>    front web compilé (défaut : web)
--host <adresse>   adresse d'écoute (défaut : 127.0.0.1 ; 0.0.0.0 = accessible depuis le réseau)
--port <port>      port HTTP (défaut : 8080)
```

Pages : `http://127.0.0.1:8080/#/edit/demo` (éditeur), `http://127.0.0.1:8080/#/run/demo` (exécution),
`http://127.0.0.1:8080/calculs.html` (variables calculées).
Pour un poste opérateur : `chromium --kiosk http://127.0.0.1:8080/#/run/demo` (ou Edge en mode kiosque sous Windows).

Windows : voir **[docs/WINDOWS.md](docs/WINDOWS.md)** (installation du kit, environnement de développement,
release). En bref : `deploy\windows\build.bat test` sous Windows avec gcc MinGW-w64, ou `make windows` /
`make dist-windows` en compilation croisée depuis macOS/Linux. LabWindows/CVI 2020 : [cvi/README.md](cvi/README.md).

## Variables calculées

Variables intermédiaires calculées à partir des autres, par le service : elles apparaissent dans l'éditeur
d'écrans sous `CALC.<nom>` comme n'importe quelle variable (arbre, widgets, deltas), en lecture seule.

```json
{"variables":[
  {"name":"rVolumeCuve","expr":"SIM.stHmi.rNiveau / 100 * 2500","unit":"L","comment":"Volume de la cuve"},
  {"name":"bCuvePleine","expr":"rVolumeCuve > 2000 AND NOT SIM.stHmi.bAlarm"}
]}
```

- Définition : page `calculs.html` (édition, test d'une expression, valeurs en direct) ou fichier
  `data/calculs.json` (relu au démarrage) ; API `GET/PUT /api/calc`, `POST /api/calc/test`.
- Syntaxe proche du texte structuré : `+ - * / MOD ^`, `< <= > >= = <>`, `AND OR XOR NOT`, `c ? a : b`,
  fonctions `abs round min max avg sum clamp limit bit if sqrt…` (détail dans [src/calc.h](src/calc.h)).
- Une variable peut utiliser celles définies au-dessus d'elle, par leur nom court : pas de boucle possible.
- Type : `auto` (BOOL pour une comparaison ou une opération logique, sinon LREAL), `BOOL`, `INT`, `DINT`,
  `REAL`, `LREAL`. Une valeur source absente ou un calcul impossible (division par zéro) donne une valeur
  absente (`null`) : jamais de chiffre faux affiché.
- Recalcul dans la boucle du service, à chaque changement d'une variable (au plus toutes les 10 ms) ; les
  valeurs calculées partent dans le même delta que leurs sources.

## Kit de déploiement

```sh
make dist                                   # dist/ads-hmi-<version>-<os>-<cpu>.tar.gz (machine courante)
deploy/package.sh --windows hmi-service.exe # dist/ads-hmi-<version>-windows.zip à partir d'un .exe compilé
deploy\windows\package.bat hmi-service.exe  # idem, sous Windows (après compilation CVI)
```

Le kit contient l'exécutable, `web/`, les écrans de `data/dashboards/`, une notice `LISEZMOI.txt` et des
scripts : `parametres` (port, adresse, écran — seul fichier à modifier), `demarrer`, `kiosque` (service +
navigateur plein écran), `arreter`, démarrage automatique (dossier Démarrage de Windows, ou service
systemd sous Linux). Sources des scripts : `deploy/windows/kit/` et `deploy/unix/kit/`.

## Tests

```sh
make test            # unitaires (UBSan ; ASan sous Linux ; fuites contrôlées par « leaks » sous macOS)
                     # puis bout en bout contre le vrai exécutable (Node >= 22)
```

- `tests/test_core.c` : JSON, conversions Latin-1/UTF-16, disposition mémoire TwinCAT, mise à plat
  (tableaux multidimensionnels, bits), codec (bornes, troncature, énumérations), hub (deltas), simulateur,
  poignée de main WebSocket (RFC 6455).
- `tests/e2e.mjs` : API REST, front, WebSocket (schéma, deltas, écritures, boucle commande → retour),
  écrans, changement de configuration, 20 clients simultanés, protections, `config.json` illisible.

## Architecture du service

| Fichier | Rôle |
|---|---|
| `main.c` | options, routes de l'API, fichiers statiques, boucle principale |
| `hub.c` | cœur temps réel : schéma, comparaison d'octets, messages `schema` / `d` / `status`, écritures, variables calculées (symbole `CALC`) |
| `calc.c` | expressions des variables calculées : analyse en notation polonaise inverse, évaluation |
| `types.c` | arbre de types neutre, calcul des offsets (alignement TwinCAT), mise à plat en variables |
| `codec.c` | décodage/encodage little-endian, contrôle des bornes, STRING (Latin-1) / WSTRING (UTF-16) |
| `sim.c` | simulateur de procédé (bloc binaire identique à un automate) |
| `store.c` | `config.json` et écrans, écriture atomique, configuration illisible |
| `http.c` | serveur HTTP/1.1 + WebSocket, `poll()` non bloquant, mono-thread |
| `plat.c` | seule couche dépendante du système (POSIX / Windows) |
| `util.c` | chaîne dynamique, arène, conversions de texte, journal |

Concurrence : la source tourne sur son propre thread et n'appelle le hub que par `hub_source_roots`,
`hub_source_data` et `hub_source_status` (un seul verrou). La boucle principale traite le réseau puis diffuse,
au plus toutes les 10 ms, un message unique avec toutes les variables modifiées. Un client trop lent
(plus de 4 Mo en attente) ne reçoit plus de deltas puis un schéma complet dès qu'il a rattrapé son retard.

### Protocole (identique à `ads-hmi-studio/shared/protocol.ts`)

Serveur → navigateur :
- `{"t":"schema","schema":{"version","roots","leaves":[{i,path,type,kind,comment?,enums?,attrs?}]},"values":[…]}`
- `{"t":"d","v":[indice, valeur, indice, valeur…],"ts":<ms>}`
- `{"t":"status","status":{"state","source","message","notifRate","procMs"}}`
- `{"t":"wr","id","ok","error"?}` — réponse à une écriture
- `{"t":"dashboards"}` — liste des écrans modifiée

Navigateur → serveur : `{"t":"w","id":1,"path":"SIM.stHmi.stCmd.bMarche","value":true}`

## Sécurité

- Écoute locale par défaut (127.0.0.1).
- En écoute locale, l'en-tête `Host` doit désigner la machine locale (protection contre le « DNS rebinding »).
- Écritures (API et WebSocket) refusées si elles viennent d'une autre origine : une page web malveillante
  ouverte sur le poste ne peut pas piloter l'automate.
- Chemins `..` refusés, tailles bornées (en-têtes 16 Ko, corps 20 Mo, trames 1 Mo), identifiants d'écran contrôlés.
- Ouvert au réseau (`--host 0.0.0.0`), le service n'a **pas encore d'authentification** : à réserver à un réseau
  machine isolé tant que le jeton d'accès n'est pas implémenté.

## Feuille de route

1. ~~Service C + simulateur + protocole web~~ (ce jalon)
2. Client ADS en C (AMS/TCP direct, upload des symboles et des types, notifications, écritures)
3. Lancement du navigateur en kiosque par le service, code de retour (bouton « Quitter »)
4. Jeton d'accès / mode lecture seule pour l'accès réseau ; validation Windows sous CVI 2020
   (gcc MinGW-w64 : compilé sans avertissement, tests exécutés sous Windows par la CI)

## Licences

Code du projet : à définir. cJSON : MIT (`third_party/cjson/LICENSE`).

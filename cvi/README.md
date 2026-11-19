# Compiler hmi-service avec LabWindows/CVI 2020

Le service est écrit en **C99 portable** : pas de bibliothèque CVI (pas d'interface utilisateur CVI,
pas de `InitCVIRTE`), seulement la bibliothèque C standard et l'API Win32 (Winsock, threads, fichiers).
Le compilateur de CVI 2020 (basé sur clang) convient. Il suffit de créer un projet console.

> État : le code est vérifié en C99 strict (`-std=c99 -pedantic`, sans avertissement) et testé sous
> macOS. La partie Windows (`src/plat.c`, branche `_WIN32`) compile sans avertissement avec gcc
> MinGW-w64 et la CI GitHub l'exécute sous Windows ; elle **n'a pas encore été compilée sous CVI**.
> Les premières compilations CVI peuvent demander des ajustements mineurs.

## Créer le projet (une fois, environ 5 minutes)

1. **File › New › Project** : enregistrer `hmi-service.prj` dans ce dossier `cvi/`.
2. **Edit › Add Files to Project › Source (*.c)** : ajouter
   - tous les fichiers de `src/` (`util.c plat.c types.c codec.c calc.c hub.c sim.c ads.c store.c http.c main.c`) ;
   - `third_party/cjson/cJSON.c`.
   Les en-têtes sont trouvés automatiquement (les `#include` sont relatifs aux fichiers).
3. **Edit › Add Files to Project › Library (*.lib)** : ajouter `ws2_32.lib` du SDK livré avec CVI :
   - 64 bits : `C:\Program Files (x86)\National Instruments\CVI2020\sdk\lib\msvc64\WS2_32.Lib`
   - 32 bits : `...\CVI2020\sdk\lib\msvc\WS2_32.Lib`
4. **Build › Configuration** : choisir **Release64** (ou Release).
5. **Build › Target Settings** : type *Executable*, cocher **Create console application**,
   nom du fichier `hmi-service.exe`.
6. **Options › Build Options** :
   - *C language version* : **C99** ;
   - configuration Debug : *Debugging level* = **No run-time checking** (la protection de pointeurs de
     CVI signale à tort l'arène mémoire et les tampons partagés entre threads ; utiliser Release pour
     l'exploitation, Debug sans vérification pour le pas à pas).
7. **Build › Build** (Ctrl+M).

Aucune définition de macro n'est nécessaire : `plat.h` fixe `_WIN32_WINNT` (Windows 7 minimum, pour
`WSAPoll`) et `WIN32_LEAN_AND_MEAN`.

Compilation en ligne de commande une fois le `.prj` créé (intégration continue, script de livraison) :

```bat
"C:\Program Files (x86)\National Instruments\CVI2020\compile.exe" cvi\hmi-service.prj
```

## Tester puis faire le kit

```bat
cvi\hmi-service.exe --data data --web web --port 8080
rem ouvrir http://127.0.0.1:8080/#/edit/demo

deploy\windows\package.bat cvi\hmi-service.exe
rem → dist\ads-hmi-<version>-windows.zip
```

Les tests de bout en bout tournent aussi sous Windows s'il y a Node ≥ 22 :
`node tests\e2e.mjs cvi\hmi-service.exe`.

## Déploiement d'un exécutable CVI

Un exécutable produit par CVI dépend du **LabWindows/CVI Runtime** (`cvirte.dll`, qui contient la
bibliothèque C de CVI), même sans utiliser les bibliothèques CVI. Sur le poste cible :

- installer le *LabWindows/CVI Runtime 2020* (gratuit, site NI), ou
- créer une distribution CVI (**Build › Distributions › Manage Distributions**) qui inclut le runtime et
  copie le contenu du kit (`web\`, `data\`, scripts `.bat`).

Sans runtime à installer : compiler les mêmes sources avec MinGW-w64
(`make CC=x86_64-w64-mingw32-gcc EXE=.exe LDLIBS="-lws2_32"`) ou Visual Studio.

## Points de compatibilité déjà traités

| Élément | Choix fait pour CVI |
|---|---|
| `<stdatomic.h>` (absent de CVI) | drapeau `plat_flag` + `Interlocked*` (`plat.c`) |
| `_Alignas` (C11) | union d'alignement dans l'arène (`util.c`) |
| `localtime_s` / `gmtime_s`, origine de `time_t` (historiquement 1900 dans la bibliothèque CVI) | `plat_now()` lit directement `GetLocalTime` / `GetSystemTime` |
| `<direct.h>` / `_mkdir` | `CreateDirectoryA` |
| `%lld` dans `printf` | horodatage écrit en `%.0f` (double exact) |
| `isfinite`, `isnan`, `NAN` | replis dans `util.h` si `<math.h>` ne les définit pas |
| `signal(SIGTERM)` | `SetConsoleCtrlHandler` : Ctrl+C, fermeture de la console, fermeture de session |
| Accents dans le journal | `SetConsoleOutputCP(CP_UTF8)` |

Les sources sont en UTF-8. L'éditeur de CVI les affiche en Windows-1252 : les accents des commentaires
apparaissent mal dans l'éditeur, sans effet sur la compilation ni sur les textes envoyés au navigateur.

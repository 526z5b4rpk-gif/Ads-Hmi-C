# Windows : installation, développement et release

Deux usages distincts :

| Usage | Ce qu'il faut | Section |
|---|---|---|
| **Poste d'exploitation** (opérateur, mise en service) | le zip du kit, rien à installer | [Installer la release](#installer-la-release) |
| **Poste de développement** | Git, gcc MinGW-w64, Node.js, VS Code (ou LabWindows/CVI) | [Environnement de développement](#environnement-de-développement) |

## Installer la release

1. Récupérer `ads-hmi-<version>-windows.zip` : page *Releases* du dépôt GitHub (publié par la CI à
   chaque tag `v*`), artefact `ads-hmi-windows` d'une exécution de la CI, ou zip produit localement
   (voir [Faire une release](#faire-une-release)).
2. Vérifier l'empreinte si besoin : `certutil -hashfile ads-hmi-<version>-windows.zip SHA256`.
3. Extraire dans un chemin **sans accent**, par exemple `C:\ADS-HMI`.
4. Suivre `LISEZMOI.txt` du kit : `demarrer.bat`, puis `http://127.0.0.1:8080/#/edit/demo` dans Edge ;
   en exploitation `kiosque.bat` et `demarrage-auto-installer.bat`.

Prérequis : Windows 10 (1803+) ou 11, 64 bits. L'exécutable compilé avec gcc MinGW-w64 est autonome
(lié statiquement, il n'utilise que les DLL système : UCRT et Winsock). **Aucun runtime à installer**,
aucun droit administrateur. Seul un exécutable compilé avec LabWindows/CVI demande le *CVI Runtime*.

Windows SmartScreen peut avertir au premier lancement d'un `.exe` non signé téléchargé : *Informations
complémentaires › Exécuter quand même*, ou débloquer le zip avant extraction (*Propriétés › Débloquer*).

## Environnement de développement

### Installation automatique (recommandée)

Dans PowerShell, à la racine du dépôt cloné (ou après avoir téléchargé le script) :

```powershell
powershell -ExecutionPolicy Bypass -File deploy\windows\installer-dev.ps1
```

Le script installe avec `winget` : **Git**, **gcc MinGW-w64** (WinLibs, UCRT), **Node.js LTS** (tests de
bout en bout, Node 22 ou plus) et **Visual Studio Code** avec l'extension C/C++. Ouvrir ensuite une
**nouvelle** console pour que le PATH soit à jour.

Installation manuelle équivalente : `winget install Git.Git BrechtSanders.WinLibs.POSIX.UCRT OpenJS.NodeJS.LTS Microsoft.VisualStudioCode`.

### Récupérer le code

```bat
git clone https://github.com/526z5b4rpk-gif/Ads-Hmi-C.git
cd Ads-Hmi-C
```

Les fins de ligne sont gérées par `.gitattributes` : les `.bat` et `.ps1` sont extraits en CRLF
(obligatoire pour cmd.exe), les sources C restent identiques sur toutes les plateformes.

### Compiler, tester, lancer

```bat
deploy\windows\build.bat            &rem build\hmi-service.exe
deploy\windows\build.bat test       &rem + 180 tests unitaires + 63 tests de bout en bout
deploy\windows\build.bat kit        &rem + dist\ads-hmi-<version>-windows.zip
build\hmi-service.exe --data data --web web
```

Puis `http://127.0.0.1:8080/#/edit/demo`. `build.bat` n'a besoin que de `gcc` dans le PATH (pas de make).

Avec **MSYS2** (console *UCRT64*, `pacman -S mingw-w64-ucrt-x86_64-gcc make zip`), le `Makefile` détecte
Windows et les commandes Unix fonctionnent telles quelles : `make`, `make test`, `make run`.

Différences avec macOS/Linux : pas de sanitizers avec MinGW (tests compilés normalement) ; dans les
tests de bout en bout, l'arrêt du service est forcé (Windows n'a pas de SIGTERM), l'arrêt propre
reste couvert par `SetConsoleCtrlHandler` (Ctrl+C, fermeture de la console).

### VS Code

Ouvrir le dossier du dépôt : les tâches (`Ctrl+Maj+B` pour compiler, *Terminal › Exécuter la tâche*
pour les tests, le lancement et le kit) et le débogage gdb (`F5`, configuration *Windows*) sont
préconfigurés dans `.vscode/`. Les mêmes tâches utilisent `make` sous macOS/Linux.

### LabWindows/CVI 2020

Les mêmes sources se compilent avec CVI : voir [cvi/README.md](../cvi/README.md). Le build gcc reste la
référence (CI, release) car il ne demande pas de runtime sur le poste cible.

## Faire une release

1. Mettre à jour `#define VERSION` dans `src/main.c` (ex. `0.2.0`), committer.
2. Créer et pousser le tag correspondant :
   ```sh
   git tag v0.2.0
   git push origin v0.2.0
   ```
3. La CI (`.github/workflows/ci.yml`) compile et teste sous Linux et sous Windows, puis publie
   `ads-hmi-0.2.0-windows.zip` et `SHA256SUMS.txt` dans une Release GitHub.

Sans CI, le kit se fabrique aussi localement :

- sous Windows : `deploy\windows\build.bat kit` ;
- depuis macOS/Linux (compilation croisée, `brew install mingw-w64` ou `apt install gcc-mingw-w64-x86-64`) :
  `make dist-windows`.

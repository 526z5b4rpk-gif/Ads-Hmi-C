#!/bin/sh
# Construit le kit de déploiement dans dist/ (archive + dossier + empreintes SHA-256).
#
#   deploy/package.sh                        kit pour cette machine : compile (make) puis assemble
#   deploy/package.sh --windows <exe>        kit Windows à partir d'un hmi-service.exe déjà compilé
#                                            (LabWindows/CVI, MinGW...) ; sous Windows : deploy\windows\package.bat
set -eu
cd "$(dirname "$0")/.."
VERSION=$(sed -n 's/^#define VERSION "\(.*\)"/\1/p' src/main.c)
[ -n "$VERSION" ] || { echo "VERSION introuvable dans src/main.c" >&2; exit 1; }

commun() { # $1 = dossier du kit
    mkdir -p "$1/data/dashboards" "$1/logs"
    cp -R web "$1/web"
    cp data/dashboards/*.json "$1/data/dashboards/" 2>/dev/null || true
    cp data/calculs.json "$1/data/" 2>/dev/null || true
    cp third_party/cjson/LICENSE "$1/LICENCE-cJSON.txt"
}

if [ "${1:-}" = "--windows" ]; then
    EXE=${2:?usage : deploy/package.sh --windows chemin/vers/hmi-service.exe}
    [ -f "$EXE" ] || { echo "introuvable : $EXE" >&2; exit 1; }
    NOM="ads-hmi-$VERSION-windows"
    KIT="dist/$NOM"
    rm -rf "$KIT" "$KIT.zip"
    commun "$KIT"
    cp "$EXE" "$KIT/hmi-service.exe"
    # fins de ligne Windows (CRLF) obligatoires pour cmd.exe et confortables pour le Bloc-notes
    for f in deploy/windows/kit/*.bat deploy/windows/LISEZMOI.txt; do
        sed 's/\r$//; s/$/\r/' "$f" > "$KIT/$(basename "$f")"
    done
    sed 's/\r$//; s/$/\r/' "$KIT/LICENCE-cJSON.txt" > "$KIT/LICENCE-cJSON.tmp" && mv "$KIT/LICENCE-cJSON.tmp" "$KIT/LICENCE-cJSON.txt"
    (cd dist && zip -qr "$NOM.zip" "$NOM")
    ARCHIVE="$NOM.zip"
else
    make >/dev/null
    OS=$(uname -s | tr '[:upper:]' '[:lower:]')
    NOM="ads-hmi-$VERSION-$OS-$(uname -m)"
    KIT="dist/$NOM"
    rm -rf "$KIT" "$KIT.tar.gz"
    commun "$KIT"
    cp build/hmi-service "$KIT/hmi-service"
    strip "$KIT/hmi-service" 2>/dev/null || true
    cp deploy/unix/kit/* deploy/unix/LISEZMOI.txt "$KIT/"
    chmod 755 "$KIT"/hmi-service "$KIT"/*.sh
    chmod 644 "$KIT/parametres.sh"
    (cd dist && COPYFILE_DISABLE=1 tar -czf "$NOM.tar.gz" "$NOM")
    ARCHIVE="$NOM.tar.gz"
fi

if command -v sha256sum >/dev/null; then SHA="sha256sum"; else SHA="shasum -a 256"; fi
(cd dist && $SHA "$ARCHIVE" > "$ARCHIVE.sha256")
echo "Kit prêt : dist/$ARCHIVE ($(du -h "dist/$ARCHIVE" | cut -f1))"

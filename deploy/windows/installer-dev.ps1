# Installe l'environnement de développement Windows (sans droits administrateur pour la plupart des paquets).
#   powershell -ExecutionPolicy Bypass -File deploy\windows\installer-dev.ps1
#
# Outils installés avec winget (fourni avec Windows 10 1809+ et Windows 11) :
#   Git, gcc MinGW-w64 (WinLibs, UCRT), Node.js LTS (tests de bout en bout), Visual Studio Code.
# LabWindows/CVI 2020 n'est pas concerné : il s'installe séparément (voir cvi\README.md).

$ErrorActionPreference = 'Stop'

if (-not (Get-Command winget -ErrorAction SilentlyContinue)) {
    Write-Error "winget introuvable : installer « App Installer » depuis le Microsoft Store, puis relancer."
}

$paquets = @(
    @{ Id = 'Git.Git';                           Nom = 'Git' },
    @{ Id = 'BrechtSanders.WinLibs.POSIX.UCRT';  Nom = 'gcc MinGW-w64 (WinLibs)' },
    @{ Id = 'OpenJS.NodeJS.LTS';                 Nom = 'Node.js LTS' },
    @{ Id = 'Microsoft.VisualStudioCode';        Nom = 'Visual Studio Code' }
)

foreach ($p in $paquets) {
    Write-Host "== $($p.Nom)" -ForegroundColor Cyan
    winget list --id $p.Id --exact --accept-source-agreements | Out-Null
    if ($LASTEXITCODE -eq 0) { Write-Host "   déjà installé"; continue }
    winget install --id $p.Id --exact --silent --accept-package-agreements --accept-source-agreements
}

# extensions VS Code recommandées (C/C++, débogage gdb)
$code = Get-Command code -ErrorAction SilentlyContinue
if ($code) {
    code --install-extension ms-vscode.cpptools --force | Out-Null
}

Write-Host ""
Write-Host "Terminé. Ouvrir une NOUVELLE console (PATH mis à jour), puis :" -ForegroundColor Green
Write-Host "   gcc --version ; node --version"
Write-Host "   deploy\windows\build.bat test     (compilation + tests)"
Write-Host "   build\hmi-service.exe --data data --web web"
Write-Host "   http://127.0.0.1:8080/#/edit/demo"

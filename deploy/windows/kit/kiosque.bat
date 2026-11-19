@echo off
rem ADS HMI - poste operateur : le service ouvre lui-meme l'ecran d'execution dans Edge (ou Chrome)
rem en plein ecran. Le bouton "Quitter" de l'ecran, ou la fermeture du navigateur, arrete le service.
rem Si un service tourne deja sur ce port : seul le navigateur est ouvert.
setlocal
cd /d "%~dp0"
call "%~dp0parametres.bat"
set URL=http://127.0.0.1:%PORT%/#/run/%ECRAN%

curl.exe -s -o nul --max-time 1 http://127.0.0.1:%PORT%/api/session
if not errorlevel 1 goto navigateur

if not exist logs mkdir logs
rem journal limite a environ 10 Mo : le precedent est conserve en hmi-service.log.1
if exist logs\hmi-service.log for %%F in (logs\hmi-service.log) do if %%~zF GTR 10485760 move /y logs\hmi-service.log logs\hmi-service.log.1 >nul
start "ADS HMI service" /min cmd /c "hmi-service.exe --data data --web web --host %HOTE% --port %PORT% --kiosk --screen %ECRAN% >> logs\hmi-service.log 2>&1"
exit /b 0

:navigateur
rem profil dedie : instance separee d'Edge (sinon --kiosk est ignore si Edge est deja ouvert)
start "" msedge --kiosk "%URL%" --edge-kiosk-type=fullscreen --no-first-run --user-data-dir="%LOCALAPPDATA%\ADS-HMI\kiosque"

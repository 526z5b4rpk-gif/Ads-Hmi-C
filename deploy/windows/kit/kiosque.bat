@echo off
rem ADS HMI - poste operateur : demarre le service s'il ne repond pas deja,
rem puis ouvre l'ecran d'execution dans Edge en mode kiosque (plein ecran, sans barre d'adresse).
setlocal
cd /d "%~dp0"
call "%~dp0parametres.bat"
set URL=http://127.0.0.1:%PORT%/#/run/%ECRAN%

curl.exe -s -o nul --max-time 1 http://127.0.0.1:%PORT%/api/config
if not errorlevel 1 goto navigateur

if not exist logs mkdir logs
rem journal limite a environ 10 Mo : le precedent est conserve en hmi-service.log.1
if exist logs\hmi-service.log for %%F in (logs\hmi-service.log) do if %%~zF GTR 10485760 move /y logs\hmi-service.log logs\hmi-service.log.1 >nul
start "ADS HMI service" /min cmd /c "hmi-service.exe --data data --web web --host %HOTE% --port %PORT% >> logs\hmi-service.log 2>&1"

set /a ESSAI=0
:attente
timeout /t 1 /nobreak >nul
curl.exe -s -o nul --max-time 1 http://127.0.0.1:%PORT%/api/config
if not errorlevel 1 goto navigateur
set /a ESSAI+=1
if %ESSAI% LSS 20 goto attente
echo Le service ne repond pas : voir logs\hmi-service.log
pause
exit /b 1

:navigateur
rem profil dedie : instance separee d'Edge (sinon --kiosk est ignore si Edge est deja ouvert)
start "" msedge --kiosk "%URL%" --edge-kiosk-type=fullscreen --no-first-run --user-data-dir="%LOCALAPPDATA%\ADS-HMI\kiosque"

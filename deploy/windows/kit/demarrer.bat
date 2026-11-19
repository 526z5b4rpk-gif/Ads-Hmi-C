@echo off
rem ADS HMI - lance le service dans cette console (mise en service, diagnostic).
rem Ctrl+C ou fermeture de la fenetre : arret propre.
setlocal
cd /d "%~dp0"
call "%~dp0parametres.bat"
echo Editeur   : http://127.0.0.1:%PORT%/#/edit/%ECRAN%
echo Execution : http://127.0.0.1:%PORT%/#/run/%ECRAN%
echo.
hmi-service.exe --data data --web web --host %HOTE% --port %PORT%
if errorlevel 1 pause

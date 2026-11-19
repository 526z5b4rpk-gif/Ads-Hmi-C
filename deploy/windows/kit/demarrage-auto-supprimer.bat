@echo off
rem ADS HMI - supprime le demarrage automatique installe par demarrage-auto-installer.bat
setlocal
set LIEN=%APPDATA%\Microsoft\Windows\Start Menu\Programs\Startup\ADS HMI.bat
if exist "%LIEN%" (del "%LIEN%" & echo Demarrage automatique supprime.) else echo Aucun demarrage automatique installe.
pause

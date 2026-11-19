@echo off
rem ADS HMI - lance kiosque.bat a l'ouverture de session de l'utilisateur courant
rem (dossier Demarrage de Windows, aucun droit administrateur necessaire).
setlocal
set LIEN=%APPDATA%\Microsoft\Windows\Start Menu\Programs\Startup\ADS HMI.bat
> "%LIEN%" echo @start "" /min "%~dp0kiosque.bat"
echo Demarrage automatique installe pour %USERNAME% :
echo   %LIEN%
pause

@echo off
rem Construit le kit de deploiement Windows a partir d'un hmi-service.exe deja compile (CVI, MinGW...).
rem   deploy\windows\package.bat chemin\vers\hmi-service.exe
rem Resultat : dist\ads-hmi-<version>-windows\ et dist\ads-hmi-<version>-windows.zip
setlocal
if "%~1"=="" (echo usage : deploy\windows\package.bat chemin\vers\hmi-service.exe & exit /b 2)
if not exist "%~f1" (echo introuvable : %~1 & exit /b 1)
set EXE=%~f1
cd /d "%~dp0..\.."

for /f "tokens=3" %%V in ('findstr /b /c:"#define VERSION" src\main.c') do set VERSION=%%~V
set KIT=dist\ads-hmi-%VERSION%-windows

if exist "%KIT%" rmdir /s /q "%KIT%"
mkdir "%KIT%\data\dashboards" "%KIT%\logs" || exit /b 1
copy /y "%EXE%" "%KIT%\hmi-service.exe" >nul || exit /b 1
xcopy /e /i /q /y web "%KIT%\web" >nul || exit /b 1
copy /y data\dashboards\*.json "%KIT%\data\dashboards\" >nul
if exist data\calculs.json copy /y data\calculs.json "%KIT%\data\" >nul
copy /y deploy\windows\kit\*.bat "%KIT%\" >nul || exit /b 1
copy /y deploy\windows\LISEZMOI.txt "%KIT%\" >nul || exit /b 1
copy /y third_party\cjson\LICENSE "%KIT%\LICENCE-cJSON.txt" >nul

if exist "%KIT%.zip" del "%KIT%.zip"
powershell -NoProfile -Command "Compress-Archive -Path '%KIT%' -DestinationPath '%KIT%.zip'" || exit /b 1
echo Kit pret : %KIT%.zip

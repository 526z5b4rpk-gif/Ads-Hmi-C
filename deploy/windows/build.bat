@echo off
rem Compile hmi-service.exe avec gcc (MinGW-w64 : WinLibs ou MSYS2), sans make.
rem   deploy\windows\build.bat            build\hmi-service.exe
rem   deploy\windows\build.bat test       + tests unitaires et de bout en bout (Node >= 22)
rem   deploy\windows\build.bat kit        + kit de deploiement dist\ads-hmi-<version>-windows.zip
setlocal
cd /d "%~dp0..\.."

where gcc >nul 2>nul || (echo gcc introuvable : lancer deploy\windows\installer-dev.ps1 ou ajouter MinGW-w64 au PATH & exit /b 1)

set SRC=src\util.c src\plat.c src\types.c src\codec.c src\calc.c src\hub.c src\sim.c src\ads.c src\store.c src\http.c third_party\cjson\cJSON.c
set WARN=-std=c99 -pedantic -Wall -Wextra -Wshadow -Wpointer-arith -Wstrict-prototypes -Wno-unused-parameter

if not exist build mkdir build
echo Compilation de build\hmi-service.exe...
gcc -O2 -g %WARN% -o build\hmi-service.exe %SRC% src\main.c -lws2_32 -static || exit /b 1
echo OK : build\hmi-service.exe

if /i "%~1"=="test" (
    echo Compilation des tests unitaires...
    gcc -O1 -g %WARN% -Isrc -o build\test_core.exe %SRC% tests\test_core.c -lws2_32 -static || exit /b 1
    build\test_core.exe || exit /b 1
    where node >nul 2>nul || (echo Node introuvable : tests de bout en bout ignores & exit /b 0)
    node tests\e2e.mjs build\hmi-service.exe || exit /b 1
)

if /i "%~1"=="kit" call deploy\windows\package.bat build\hmi-service.exe

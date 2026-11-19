@echo off
rem ADS HMI - arrete le service lance par kiosque.bat.
rem Arret force, sans risque pour les donnees : ecrans et configuration sont ecrits de facon atomique.
taskkill /im hmi-service.exe /f

@echo off
rem Doble clic: compila e instala UMBRAL (ver install_windows.ps1 para opciones).
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0install_windows.ps1" %*
echo.
pause

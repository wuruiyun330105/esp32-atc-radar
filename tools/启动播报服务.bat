@echo off
title TTS Server 8765
cd /d "%~dp0.."
net session >nul 2>&1
if errorlevel 1 (
    echo [1/3] Need admin for firewall rule, asking UAC...
    powershell -NoProfile -Command "Start-Process -FilePath '%~f0' -Verb RunAs"
    exit /b
)
netsh advfirewall firewall add rule name="ESP32-Radar-TTS" dir=in action=allow protocol=TCP localport=8765 >nul 2>&1
echo [2/3] Firewall port 8765 allowed.
echo [3/3] Starting TTS server, keep this window open.
python tools\tts_server.py 8765
pause

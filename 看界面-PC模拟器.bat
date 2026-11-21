@echo off
REM ESP32 Tower Radar - PC Simulator launcher (ASCII only to avoid codepage issues)
cd /d "E:\esp32_radar_fw_ful\esp32_radar_fw\build-gcc"
set RADAR_SRC=0
set SIM_SOUND=1
set SIM_ATC=1
echo ============================================
echo   ESP32 Tower Radar - PC Simulator 480x480
echo   Pages: Radar / Clock / Flights / Assistant
echo   Keys : Left/Right arrow or drag = switch
echo          Click radar target = details
echo          Q or ESC = quit
echo   Sound: SIM_SOUND=1 (0 to mute all)
echo   ATC  : SIM_ATC=1 - simulated tower
echo          dialogue every ~35s (offline
echo          SAPI TTS + radio effect)
echo   Mode : Demo flights (RADAR_SRC=0)
echo ============================================
sim.exe
echo.
echo [Simulator closed]
pause

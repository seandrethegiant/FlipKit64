@echo off
:: Installs FlipKit64 into the standard VST3 folder (Windows will ask for admin permission).
:: Close your DAW first.
net session >nul 2>&1
if errorlevel 1 (
  powershell -NoProfile -Command "Start-Process -FilePath '%~f0' -Verb RunAs"
  exit /b
)
set "SRC=%~dp0FlipKit64.vst3"
set "DST=%CommonProgramFiles%\VST3\FlipKit64.vst3"
if not exist "%CommonProgramFiles%\VST3" mkdir "%CommonProgramFiles%\VST3"
robocopy "%SRC%" "%DST%" /E /NFL /NDL /NJH /NJS >nul
if exist "%DST%\Contents\x86_64-win\FlipKit64.vst3" (
  echo FlipKit64 installed to %DST%
  echo Restart your DAW and add FlipKit64 as an instrument.
) else (
  echo Install FAILED - is your DAW still open?
)
pause

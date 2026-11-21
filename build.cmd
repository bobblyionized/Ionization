@echo off
rem Local test build -> out\Ionization-dev.exe (dev-stamped; never self-updates).
rem dist\Ionization.exe is the auto-updating release copy; leave it alone.
setlocal
cd /d "%~dp0"
for /f "usebackq tokens=*" %%i in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -property installationPath`) do set VSDIR=%%i
if not defined VSDIR (echo Visual Studio Build Tools not found & exit /b 1)
cmake -S . -B build -G "Visual Studio 17 2022" -A x64 %* || exit /b 1
cmake --build build --config Release -- /m /v:minimal || exit /b 1
if not exist out mkdir out
taskkill /im Ionization-dev.exe /f >nul 2>&1
copy /y build\Release\Ionization.exe out\Ionization-dev.exe >nul || exit /b 1
echo Built out\Ionization-dev.exe

@echo off
setlocal EnableExtensions
cd /d "%~dp0"

call :ensure_x86_cl || exit /b 1
if not exist build mkdir build
if not exist build\obj mkdir build\obj

set "BLSE_INC=include"
set "MA_INC=third_party\miniaudio"
set "SA_INC=third_party\steamaudio\include"
set "SA_LIB=third_party\steamaudio\lib\windows-x86\phonon.lib"
set "SA_DLL=third_party\steamaudio\lib\windows-x86\phonon.dll"

if not exist "%SA_LIB%" (
    echo error: Steam Audio x86 library missing at %SA_LIB%
    exit /b 1
)

set "CXX=cl /nologo /std:c++17 /DNOMINMAX /DNDEBUG /W4 /wd4100 /wd4201 /EHsc /MT /O2 /arch:SSE2 /Zi /Isrc /I"%BLSE_INC%" /I"%MA_INC%" /I"%SA_INC%" /Fobuild\obj\ /Fdbuild\obj\"
set "CORE=src\bsp_acoustics.cpp src\channel_model.cpp src\device_watch.cpp src\dynamics.cpp src\phonon_memory.cpp src\resampler.cpp src\settings.cpp src\sky_exposure.cpp src\steam_audio_engine.cpp"

copy /y "%SA_DLL%" build\phonon.dll >nul || exit /b 1

echo == auspex_audio.dll
%CXX% /MP /LD src\plugin_main.cpp src\engine_hooks.cpp %CORE% "%SA_LIB%" delayimp.lib ole32.lib user32.lib /Febuild\auspex_audio.dll /link /DELAYLOAD:phonon.dll /OPT:REF /OPT:ICF /INCREMENTAL:NO /PDB:build\auspex_audio.pdb /IMPLIB:build\obj\auspex_audio.lib || exit /b 1

echo == offline_tests.exe
%CXX% /MP tests\offline_tests.cpp %CORE% "%SA_LIB%" ole32.lib /Febuild\offline_tests.exe /link /OPT:REF /OPT:ICF /INCREMENTAL:NO || exit /b 1

if /i "%~1"=="notest" goto :done
echo == running offline_tests.exe
build\offline_tests.exe || exit /b 1

:done
echo == OK: build\auspex_audio.dll
exit /b 0

:ensure_x86_cl
where cl >nul 2>&1 && (cl 2>&1 | findstr /c:"for x86" >nul) && exit /b 0
set "VSINSTALL="
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if exist "%VSWHERE%" for /f "usebackq delims=" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSINSTALL=%%i"
if not defined VSINSTALL for %%v in (18 2022) do for %%e in (Community Professional Enterprise BuildTools) do if exist "%ProgramFiles%\Microsoft Visual Studio\%%v\%%e\Common7\Tools\VsDevCmd.bat" set "VSINSTALL=%ProgramFiles%\Microsoft Visual Studio\%%v\%%e"
if not defined VSINSTALL (echo error: Visual Studio with C++ x86 tools not found & exit /b 1)
set "VSCMD_SKIP_SENDTELEMETRY=1"
call "%VSINSTALL%\Common7\Tools\VsDevCmd.bat" -arch=x86 -host_arch=x64 >nul 2>&1
cd /d "%~dp0"
where cl >nul 2>&1 || (echo error: VsDevCmd did not provide cl.exe & exit /b 1)
exit /b 0

@echo off
REM Utenyaa NetLink — Windows build helper
REM
REM Mirrors the compile.bat conventions from Disasteroids/Flicky:
REM   * invokes the Yaul + Jo Engine toolchain via make
REM   * on success, runs JoEngineCueMaker.exe to produce the .cue
REM   * on toolchain failure falls back to the alt path (same behavior)
REM
REM Requires sh2eb-elf-gcc/g++, mkisofs, and JoEngineCueMaker.exe on PATH.

setlocal

echo [Utenyaa] Cleaning previous build...
make clean

echo [Utenyaa] Building...
make build
if errorlevel 1 goto :fallback

echo [Utenyaa] Generating .cue...
JoEngineCueMaker.exe
if errorlevel 1 goto :cueerror
echo [Utenyaa] Build OK — build.cue / build.iso ready.
goto :end

:fallback
echo [Utenyaa] Primary build failed — retrying once after clean...
make clean
make build
if errorlevel 1 goto :builderror
JoEngineCueMaker.exe
goto :end

:builderror
echo [Utenyaa] BUILD FAILED. Inspect the errors above.
exit /b 1

:cueerror
echo [Utenyaa] CUE GENERATION FAILED.
exit /b 1

:end
endlocal

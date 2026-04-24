@ECHO Off
SET COMPILER_DIR=D:\joengine-master\Compiler
SET JO_ENGINE_SRC_DIR=D:/joengine-master/jo_engine
SET PATH=%COMPILER_DIR%\WINDOWS\Other Utilities;%COMPILER_DIR%\WINDOWS\bin;%PATH%

ECHO.
ECHO === Utenyaa - NetLink Build ===
ECHO.

make all JO_ENGINE_SRC_DIR=%JO_ENGINE_SRC_DIR% COMPILER_DIR=D:/joengine-master/Compiler OS=Windows_NT
IF %ERRORLEVEL% NEQ 0 (
    ECHO.
    ECHO === Build failed at mkisofs ISO step ===
    ECHO === Checking if ELF/BIN compiled OK... ===
    IF EXIST build.elf (
        IF EXIST cd\0.bin (
            ECHO === Compile+Link OK! Creating ISO manually... ===
            mkisofs -quiet -sysid "SEGA SATURN" -volid "SaturnApp" -volset "SaturnApp" -sectype 2352 -publisher "SEGA ENTERPRISES, LTD." -preparer "SEGA ENTERPRISES, LTD." -appid "SaturnApp" -abstract "ABS.TXT" -copyright "CPY.TXT" -biblio "BIB.TXT" -generic-boot %COMPILER_DIR%\COMMON\IP.BIN -full-iso9660-filenames -o build.iso cd
            IF EXIST build.iso (
                ECHO === ISO created successfully! ===
                JoEngineCueMaker.exe
                CALL :PACKAGE_GAME
            ) ELSE (
                ECHO === ISO creation failed. build.elf and cd\0.bin are ready for manual ISO creation. ===
            )
        ) ELSE (
            ECHO === Compilation failed! ===
        )
    ) ELSE (
        ECHO === Compilation failed! ===
    )
) ELSE (
    ECHO.
    ECHO === Build successful! ===
    IF EXIST build.iso (
        JoEngineCueMaker.exe
        ECHO === CUE file generated with audio tracks ===
        CALL :PACKAGE_GAME
    )
)

ECHO.
GOTO :EOF

:PACKAGE_GAME
ECHO.
ECHO === Packaging Game Files ===
IF EXIST "Game Files" RMDIR /S /Q "Game Files"
MKDIR "Game Files"

REM Primary game artifacts: CUE (renamed so emulators/ODEs show a friendly label)
REM and the ISO. Players load the .cue — not the .iso — so audio tracks work.
COPY /Y build.iso "Game Files\game.iso" >NUL
COPY /Y build.cue "Game Files\START GAME.CUE" >NUL

REM CD audio tracks (already in repo root alongside the build system)
COPY /Y track02.wav "Game Files\track02.wav" >NUL
COPY /Y track03.wav "Game Files\track03.wav" >NUL
COPY /Y track04.wav "Game Files\track04.wav" >NUL

REM No Online/ folder in the zip — dial 199405 is already shipped in the
REM default netlink_config.ini that ships with current DreamPi and the
REM netlink.py PC tunnel script. Players only need to update to the latest.

ECHO === Game Files folder ready! ===
GOTO :EOF

@echo off
REM ---------------------------------------------------------------------------
REM build-ffmpeg.cmd  -  build FFmpeg 9.1 + AVS/AVS+ (libcavs) with MSVC
REM
REM   build-ffmpeg.cmd                minimal static build (AVS only)  -> ff91-install
REM   build-ffmpeg.cmd --shared       also produce DLLs
REM   build-ffmpeg.cmd --full         every decoder/demuxer + ffmpeg/ffprobe
REM
REM vcvars64.bat is imported natively (cmd.exe) because MSYS bash loses the
INCLUDE/LIB environment for child processes once PATH is rewritten.
REM ---------------------------------------------------------------------------
setlocal
set "MSYS_ROOT=C:\msys64"
set "VS_VCVARS=C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat"
if not exist "%VS_VCVARS%" (
    for /f "delims=" %%i in ('"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -property installationPath') do set "VS_VCVARS=%%i\VC\Auxiliary\Build\vcvars64.bat"
)
call "%VS_VCVARS%" || exit /b 1

REM msys first so that sh/make/sed/perl come from MSYS2, then nasm
set "PATH=%MSYS_ROOT%\mingw64\bin;%MSYS_ROOT%\usr\bin;%PATH%"

set "SCRIPT=%~dp0build_ffmpeg.sh"
"%MSYS_ROOT%\usr\bin\bash.exe" -e "%SCRIPT%" %*
set "RC=%ERRORLEVEL%"
endlocal & exit /b %RC%

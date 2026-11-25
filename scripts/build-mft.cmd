@echo off
REM ---------------------------------------------------------------------------
REM build-mft.cmd - build the AVS decoder MFT and its test tools.
REM
REM   scripts\build-mft.cmd              release: MFT dll + tools
REM   scripts\build-mft.cmd debug        debug build
REM   scripts\build-mft.cmd tools        only the console tools (avsmft-test, ...)
REM
REM Requires ff91-install to exist (scripts\build-ffmpeg.cmd) and Visual Studio.
REM ---------------------------------------------------------------------------
setlocal
set "ROOT=%~dp0.."
set "VCDLL="
set "VS_VCVARS=C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat"
if not exist "%VS_VCVARS%" (
    for /f "delims=" %%i in ('"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -property installationPath') do set "VS_VCVARS=%%i\VC\Auxiliary\Build\vcvars64.bat"
)
call "%VS_VCVARS%" || exit /b 1

set "CFG=Release"
set "WHAT=all"
if /i "%1"=="debug" set "CFG=Debug"
if /i "%1"=="tools" set "WHAT=tools"

set "FFMPEG=%ROOT%\ff91-install"
set "DEPS=%ROOT%\deps"
set "BIN=%ROOT%\bin"
if not exist "%BIN%" mkdir "%BIN%"

REM The FFmpeg tree and the two external decoders are referenced by the delivery, not
REM shipped in it (deps\README.md names the repositories).  Say so here: failing inside
REM cl.exe with "cannot open include file avcodec.h" hides which step was skipped.
if not exist "%FFMPEG%\include\libavcodec\avcodec.h" (
    echo MISSING "%FFMPEG%\include\libavcodec\avcodec.h"
    echo    build FFmpeg 9.1 with our patch first:  scripts\build-ffmpeg.cmd
    echo    its source: https://github.com/FFmpeg/FFmpeg  tag n9.1  - see deps\README.md
    exit /b 2
)
if not exist "%DEPS%\lib\davs2.lib" (
    echo MISSING "%DEPS%\lib\davs2.lib"
    echo    build the two external decoders first:  scripts\build-deps.cmd
    echo    their sources: https://github.com/pkuvcl/davs2  and  https://github.com/uavs3/uavs3d
    echo    see deps\README.md
    exit /b 2
)

if not "%WHAT%"=="tools" (
    echo == build AVSVideoDecoder.dll ^(cfg %CFG%^)
    msbuild /nologo /v:minimal /p:Configuration=%CFG% /p:Platform=x64 ^
        "%ROOT%\MediaFoundationTransform\AVSVideoDecoder\AVSVideoDecoder.vcxproj"
    if errorlevel 1 exit /b 1
)

echo == build tools
cl /nologo /MT /EHsc /std:c++17 /W3 /O2 /utf-8 /D_CRT_SECURE_NO_WARNINGS /DUNICODE /D_UNICODE ^
   /I "%FFMPEG%\include" "%ROOT%\tools\avsmft_test.cpp" ^
   /Fe:"%BIN%\avsmft-test.exe" ^
   /link /MAP:"%BIN%\avsmft-test.map" ^
     /LIBPATH:"%FFMPEG%\lib" /LIBPATH:"%DEPS%\lib" ^
     davs2.lib uavs3d.lib dbghelp.lib || exit /b 1

cl /nologo /EHsc /std:c++17 /W3 /O2 /utf-8 /D_CRT_SECURE_NO_WARNINGS /DUNICODE /D_UNICODE ^
   "%ROOT%\tools\avsmft_register.cpp" /Fe:"%BIN%\avsmft-register.exe" ^
   /link advapi32.lib || exit /b 1

REM the Vulkan compute kernel self test of the AVS reconstruction stage.  It is built
REM against the patched FFmpeg source tree because it includes libavcodec internals;
REM a delivery that ships the FFmpeg headers plus the patch (dist/) does not have that
REM tree.  Say so and keep going: a silent skip would hide a real build error, and a
REM hard failure would make a complete package look broken.
if not exist "%ROOT%\ffmpeg-9.1\libavcodec\avs_vk.h" (
    echo SKIP avsvk-test: needs the patched FFmpeg source tree at "%ROOT%\ffmpeg-9.1"
    goto skip_avsvk
)
cl /nologo /MT /EHsc /std:c++17 /W3 /O2 /utf-8 /D_CRT_SECURE_NO_WARNINGS ^
   /I "%FFMPEG%\include" /I "%ROOT%\ffmpeg-9.1" ^
   "%ROOT%\tools\avsvk_test.cpp" ^
   /Fe:"%BIN%\avsvk-test.exe" ^
   /link /LIBPATH:"%FFMPEG%\lib" /LIBPATH:"%DEPS%\lib" ^
     avcodec.lib avutil.lib swscale.lib davs2.lib uavs3d.lib dbghelp.lib || exit /b 1

:skip_avsvk
cl /nologo /EHsc /std:c++17 /W3 /O2 /utf-8 /D_CRT_SECURE_NO_WARNINGS /DUNICODE /D_UNICODE ^
   "%ROOT%\tools\mfprobe.cpp" /Fe:"%BIN%\mfprobe.exe" /link Ole32.lib || exit /b 1

cl /nologo /EHsc /std:c++17 /W3 /O2 /utf-8 /D_CRT_SECURE_NO_WARNINGS /DUNICODE /D_UNICODE ^
   "%ROOT%\tools\mftenum.cpp" /Fe:"%BIN%\mftenum.exe" /link Ole32.lib || exit /b 1

REM what byte stream handler a container file actually gets, and what native media type
REM it reports: the subtype string in that output is the only thing a decoder MFT can be
REM matched against, so this is the tool that answers "could the MFT side fix it instead?"
cl /nologo /EHsc /std:c++17 /W3 /O2 /utf-8 /D_CRT_SECURE_NO_WARNINGS /DUNICODE /D_UNICODE ^
   "%ROOT%\tools\typedump.cpp" /Fe:"%BIN%\typedump.exe" ^
   /link mfplat.lib mfreadwrite.lib mfuuid.lib ole32.lib || exit /b 1


REM the player's own entry point: Windows 10's built-in player drives the Media Engine,
REM not MFCreateSourceReaderFromURL and not MFPlay.  The "unsupported format: unknown"
REM message is that component's decision, so it has to be measured through its own API.
cl /nologo /EHsc /std:c++17 /W3 /O2 /utf-8 /D_CRT_SECURE_NO_WARNINGS /DUNICODE /D_UNICODE ^
   "%ROOT%\tools\mediaengine_probe.cpp" /Fe:"%ROOT%\bin\mediaengine-probe.exe" ^
   /link mfplat.lib mfuuid.lib ole32.lib || exit /b 1


REM what our own FFmpeg build actually contains, asked at runtime (tools\ffcaps.cpp): the build
REM tree keeps no config.h, so grepping the source tree cannot answer which decoders exist.
cl /nologo /MT /EHsc /std=c++17 /W3 /O2 /utf-8 /D_CRT_SECURE_NO_WARNINGS /DUNICODE /D_UNICODE ^
   /I "%FFMPEG%\include" "%ROOT%\tools\ffcaps.cpp" ^
   /Fe:"%BIN%\ffcaps.exe" ^
   /link /LIBPATH:"%FFMPEG%\lib" /LIBPATH:"%DEPS%\lib" ^
     avformat.lib avcodec.lib avutil.lib swscale.lib davs2.lib uavs3d.lib dbghelp.lib user32.lib bcrypt.lib ws2_32.lib secur32.lib crypt32.lib ncrypt.lib || exit /b 1

cl /nologo /EHsc /std:c++17 /W3 /O2 /utf-8 /D_CRT_SECURE_NO_WARNINGS /DUNICODE /D_UNICODE ^
   /I"%ROOT%\MediaFoundationTransform\AVSVideoDecoder" ^
   "%ROOT%\tools\avssource_test.cpp" /Fe:"%BIN%\avssource-test.exe" /link Ole32.lib dbghelp.lib || exit /b 1

cl /nologo /MT /EHsc /std:c++17 /W3 /O2 /utf-8 /D_CRT_SECURE_NO_WARNINGS /DUNICODE /D_UNICODE ^
   /I "%FFMPEG%\include" "%ROOT%\tools\avsaudio_test.cpp" ^
   /Fe:"%BIN%\avsaudio-test.exe" ^
   /link /LIBPATH:"%FFMPEG%\lib" /LIBPATH:"%DEPS%\lib" ^
     avformat.lib avcodec.lib avutil.lib swscale.lib davs2.lib uavs3d.lib dbghelp.lib || exit /b 1

cl /nologo /EHsc /std:c++17 /W3 /O2 /utf-8 /D_CRT_SECURE_NO_WARNINGS /DUNICODE /D_UNICODE ^
   "%ROOT%\tools\mfplay_test.cpp" /Fe:"%BIN%\mfplay-test.exe" /link Ole32.lib mfplay.lib mf.lib mfplat.lib dbghelp.lib || exit /b 1

cl /nologo /EHsc /std:c++17 /W3 /O2 /utf-8 /D_CRT_SECURE_NO_WARNINGS /DUNICODE /D_UNICODE ^
   "%ROOT%\tools\mfsession_test.cpp" /Fe:"%BIN%\mfsession-test.exe" /link dbghelp.lib || exit /b 1

cl /nologo /EHsc /std:c++17 /W3 /O2 /utf-8 /D_CRT_SECURE_NO_WARNINGS /DUNICODE /D_UNICODE ^
   "%ROOT%\tools\mftlifecycle_test.cpp" /Fe:"%BIN%\mftlifecycle-test.exe" ^
   /link Ole32.lib mf.lib mfplat.lib mfuuid.lib dbghelp.lib || exit /b 1

echo.
echo built:
echo   %ROOT%\MediaFoundationTransform\AVSVideoDecoder\bin\x64\%CFG%\AVSVideoDecoder.dll
echo   %BIN%\avsmft-test.exe  avsmft-register.exe  mfprobe.exe  mftenum.exe  avsaudio-test.exe  avssource-test.exe  mfplay-test.exe  mfsession-test.exe  mftlifecycle-test.exe
endlocal

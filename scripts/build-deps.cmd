@echo off
REM ---------------------------------------------------------------------------
REM build-deps.cmd - build the external decoder libraries FFmpeg needs for AVS2
REM and AVS3 as static MSVC libraries:
REM
REM   davs2   (pkuvcl/davs2)   AVS2-P2 decoder   -> deps\lib\davs2.lib
REM   uavs3d  (uavs3/uavs3d)   AVS3-P2 decoder   -> deps\lib\uavs3d.lib
REM
REM The sources are expected unpacked under deps\src (deps\README.md explains how
REM to fetch them); nasm for davs2's asm kernels is taken from deps\bin\nasm.exe
REM or from PATH.  FFmpeg finds the results through deps\lib\pkgconfig\*.pc and
REM scripts\pkg-config.sh; the MFT and the test tools link the two .lib files.
REM
REM Both libraries are built for one sample depth, chosen on the command line:
REM
REM   build-deps.cmd        8 bit (default): BIT_DEPTH=8  / COMPILE_10BIT=0
REM   build-deps.cmd 10     10 bit:          BIT_DEPTH=10 / COMPILE_10BIT=1
REM
REM davs2 keeps its SSE/AVX2 intrinsic kernels and its x86 asm kernels either way.
REM 10 bit matters for real broadcast material: davs2 then decodes 10 bit AVS2
REM sequences (an 8 bit stream still reports output_bit_depth = 8, so nothing
REM changes for 8 bit sources), and uavs3d decodes 10 bit / HDR AVS3 sequences -
REM in that build every picture, also from an 8 bit stream, comes out as 10 bit
REM and the MFT converts it down.  After rebuilding the libraries re-link the MFT
REM and the tools (scripts\build-mft.cmd); FFmpeg itself does not need a rebuild,
REM it only uses the headers.
REM ---------------------------------------------------------------------------
set "BITDEPTH=8"
if "%~1" == "10" set "BITDEPTH=10"
if "%BITDEPTH%" == "10" (
    set "UAVS3D_BIT=/DCOMPILE_10BIT=1"
    set "DAVS2_BIT=/DHIGH_BIT_DEPTH=1 /DBIT_DEPTH=10"
    set "DAVS2_BIT_ASM=-DHIGH_BIT_DEPTH=1 -DBIT_DEPTH=10"
) else (
    set "UAVS3D_BIT=/DCOMPILE_10BIT=0"
    set "DAVS2_BIT=/DHIGH_BIT_DEPTH=0 /DBIT_DEPTH=8"
    set "DAVS2_BIT_ASM=-DHIGH_BIT_DEPTH=0 -DBIT_DEPTH=8"
)
setlocal
pushd "%~dp0.." && set "ROOT=%cd%" && popd
set "DEPS=%ROOT%\deps"
set "SRC=%DEPS%\src"
set "OUT=%DEPS%"
set "OBJ=%ROOT%\tmp\deps-obj"
set "DAVS2=%SRC%\davs2-master"
set "UAVS3D=%SRC%\uavs3d-master"
set "NASM=%DEPS%\bin\nasm.exe"
if not exist "%NASM%" set "NASM=nasm"

set "VS_VCVARS=C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat"
if not exist "%VS_VCVARS%" (
    for /f "delims=" %%i in ('"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -property installationPath') do set "VS_VCVARS=%%i\VC\Auxiliary\Build\vcvars64.bat"
)
call "%VS_VCVARS%" >nul || exit /b 1

if not exist "%OUT%\include"  mkdir "%OUT%\include"
if not exist "%OUT%\lib\pkgconfig" mkdir "%OUT%\lib\pkgconfig"

REM ----------------------------------------------------------- uavs3d (AVS3)
if not exist "%UAVS3D%" goto :no_uavs3d
echo == uavs3d: compile decoder
rmdir /s /q "%OBJ%\uavs3d" 2>nul
mkdir "%OBJ%\uavs3d"
cd /d "%OBJ%\uavs3d" || exit /b 1
cl /c /nologo /O2 /MT /W3 /wd4018 /wd4146 /wd4244 /wd4267 /wd4996 /wd4100 /wd4101 /wd4189 ^
   /DNDEBUG /DWIN32 /D_CRT_SECURE_NO_WARNINGS /DUAVS3D_EXPORTS %UAVS3D_BIT% ^
   /I"%UAVS3D%\source\decore" /I"%UAVS3D%\source\decoder" /I"%UAVS3D%" ^
   "%UAVS3D%\source\decoder\*.c" "%UAVS3D%\source\decore\*.c" "%UAVS3D%\source\decore\sse\*.c" || exit /b 1
cl /c /nologo /O2 /MT /arch:AVX2 /W3 /wd4018 /wd4146 /wd4244 /wd4267 /wd4996 /wd4100 ^
   /DNDEBUG /DWIN32 /D_CRT_SECURE_NO_WARNINGS /DUAVS3D_EXPORTS %UAVS3D_BIT% ^
   /I"%UAVS3D%\source\decore" /I"%UAVS3D%\source\decoder" /I"%UAVS3D%" ^
   "%UAVS3D%\source\decore\avx2\*.c" || exit /b 1
lib /nologo /OUT:"%OUT%\lib\uavs3d.lib" *.obj || exit /b 1
copy /y "%UAVS3D%\source\decoder\uavs3d.h" "%OUT%\include\uavs3d.h" >nul
REM same reason as davs2.h: the installed header must not claim dllimport
powershell -NoProfile -Command "(Get-Content '%OUT%\include\uavs3d.h') -replace '__declspec\(dllimport\)','' | Set-Content '%OUT%\include\uavs3d.h'" || exit /b 1
call :write_pc uavs3d 1.2.0 "AVS3-P2/IEEE1857.10 decoder"
echo == uavs3d: "%OUT%\lib\uavs3d.lib"
:no_uavs3d

REM ------------------------------------------------------------ davs2 (AVS2)
if not exist "%DAVS2%" goto :no_davs2
REM MSVC needs two small fixes to build davs2 with 10 bit samples; the script is
REM idempotent, see scripts\patch_davs2_msvc.py.
python "%ROOT%\scripts\patch_davs2_msvc.py" "%DAVS2%" || exit /b 1
echo == davs2: compile decoder ^(C++^)
rmdir /s /q "%OBJ%\davs2" 2>nul
mkdir "%OBJ%\davs2"
cd /d "%OBJ%\davs2" || exit /b 1
cl /c /nologo /O2 /MT /EHsc /std:c++14 /W3 ^
   /wd4018 /wd4146 /wd4244 /wd4267 /wd4305 /wd4996 /wd4100 /wd4189 /wd4456 ^
   /DNDEBUG /DWIN32 /D_CRT_SECURE_NO_WARNINGS ^
   /DDAVS2_EXPORTS %DAVS2_BIT% /DARCH_X86_64=1 ^
   /I"%DAVS2%\source" /I"%DAVS2%\source\common" ^
   "%DAVS2%\source\common\*.cc" || exit /b 1

REM the SSE / AVX2 intrinsic kernels that the davs2_*_init dispatchers reference;
REM compiled per file with the matching /arch switch, like davs2's own
REM intrin_sse / intrin_avx projects do.
cl /c /nologo /O2 /MT /EHsc /std:c++14 /arch:AVX2 /W3 ^
   /wd4018 /wd4146 /wd4244 /wd4267 /wd4305 /wd4996 /wd4100 /wd4189 /wd4456 ^
   /DNDEBUG /DWIN32 /D_CRT_SECURE_NO_WARNINGS ^
   /DDAVS2_EXPORTS %DAVS2_BIT% /DARCH_X86_64=1 ^
   /I"%DAVS2%\source" /I"%DAVS2%\source\common" /I"%DAVS2%\source\common\vec" ^
   "%DAVS2%\source\common\vec\*_avx2.cc" "%DAVS2%\source\common\vec\*_avx.cc" || exit /b 1
cl /c /nologo /O2 /MT /EHsc /std:c++14 /W3 ^
   /wd4018 /wd4146 /wd4244 /wd4267 /wd4305 /wd4996 /wd4100 /wd4189 /wd4456 ^
   /DNDEBUG /DWIN32 /D_CRT_SECURE_NO_WARNINGS ^
   /DDAVS2_EXPORTS %DAVS2_BIT% /DARCH_X86_64=1 ^
   /I"%DAVS2%\source" /I"%DAVS2%\source\common" /I"%DAVS2%\source\common\vec" ^
   "%DAVS2%\source\common\vec\intrinsic.cc" "%DAVS2%\source\common\vec\intrinsic_alf.cc" ^
   "%DAVS2%\source\common\vec\intrinsic_deblock.cc" "%DAVS2%\source\common\vec\intrinsic_idct.cc" ^
   "%DAVS2%\source\common\vec\intrinsic_inter_pred.cc" ^
   "%DAVS2%\source\common\vec\intrinsic_intra-filledge.cc" ^
   "%DAVS2%\source\common\vec\intrinsic_intra-pred.cc" ^
   "%DAVS2%\source\common\vec\intrinsic_pixel.cc" ^
   "%DAVS2%\source\common\vec\intrinsic_sao.cc" || exit /b 1

REM the external x86 asm kernels (nasm); x86inc.asm / x86util.asm are include files
REM and are skipped - nasm needs the asm directory on its include path for them.
for %%f in ("%DAVS2%\source\common\x86\*.asm") do call :davs2_asm "%%f" || exit /b 1

lib /nologo /OUT:"%OUT%\lib\davs2.lib" *.obj || exit /b 1
copy /y "%DAVS2%\source\davs2.h" "%OUT%\include\davs2.h" >nul
REM the installed header would otherwise declare the API as __declspec(dllimport),
REM which makes MSVC look for the __imp_davs2_* import thunk of a DLL build.
powershell -NoProfile -Command "(Get-Content '%OUT%\include\davs2.h') -replace '__declspec\(dllimport\)','' | Set-Content '%OUT%\include\davs2.h'" || exit /b 1
call :write_pc davs2 1.6.0 "AVS2-P2/IEEE1857.4 decoder"
echo == davs2: "%OUT%\lib\davs2.lib"
:no_davs2

echo == done
exit /b 0

REM --------------------------------------------------------------- helpers
:davs2_asm
set "N=%~n1"
REM x86inc.asm / x86util.asm are macro include files, not translation units
if /i "%N%"=="x86inc"  exit /b 0
if /i "%N%"=="x86util" exit /b 0
REM ipfilter8.asm is not part of davs2's own build either (build/linux/Makefile lists
REM const-a, blockcopy8, cpu-a, dct8, mc-a2, pixeladd8, quant8); its kernels are the
REM intrinsic ones in common/vec.  Assembling it here fails on current nasm.
if /i "%N%"=="ipfilter8" exit /b 0
"%NASM%" -f win64 -DARCH_X86_64=1 %DAVS2_BIT_ASM% -I"%DAVS2%\source\common\x86\\" -o "%OBJ%\davs2\asm_%N%.obj" "%~1"
exit /b %ERRORLEVEL%

:write_pc
REM %1 module  %2 version  %3 description
set "DEPSW=%DEPS:\=/%"
> "%OUT%\lib\pkgconfig\%1.pc" echo prefix=%DEPSW%
>> "%OUT%\lib\pkgconfig\%1.pc" echo libdir=${prefix}/lib
>> "%OUT%\lib\pkgconfig\%1.pc" echo includedir=${prefix}/include
>> "%OUT%\lib\pkgconfig\%1.pc" echo Name: %1
>> "%OUT%\lib\pkgconfig\%1.pc" echo Description: %~3
>> "%OUT%\lib\pkgconfig\%1.pc" echo Version: %2
>> "%OUT%\lib\pkgconfig\%1.pc" echo Libs: -L${libdir} -l%1
>> "%OUT%\lib\pkgconfig\%1.pc" echo Cflags: -I${includedir}
exit /b 0

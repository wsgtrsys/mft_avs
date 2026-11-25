REM ---------------------------------------------------------------------------
REM verify.cmd - can this package decode AVS on this machine, without registering
REM anything in the system?  Everything it uses is inside the package: the console
REM harness talks to the DLL through DllGetClassObject, so no administrator and no
REM HKLM write is involved.
REM
REM   verify.cmd                      the package's own sample, samples\CCTV-4-short.avs
REM   verify.cmd <file> [frames]      .avs .cavs .avs2 .avs3 .ts .mkv with AVS video
REM
REM What it checks, and what "OK" means here:
REM   1. the decoder MFT is loaded and one access unit produces a real picture
REM      (avsmft-test prints "decoded N frame(s)");
REM   2. the MFT survives create/use/destroy cycles (mftlifecycle-test, 10 cycles,
REM      0 failures) - this is what protects the host application from our DLL;
REM   3. only for the package's own sample: the full decode reproduces the recorded
REM      bytes (md5 in THIRD-PARTY.md / MANIFEST.md).  That is the strongest claim
REM      this script can make, and it is why the sample is in the package.
REM Steps 1-2 run with no environment switches at all, i.e. the configuration a
REM player actually gets.  Step 3 decodes with -fmt i420 because that is the form
REM the recorded bytes were produced in.
REM ---------------------------------------------------------------------------
@echo off
setlocal
set "ROOT=%~dp0"
set "MFT=%ROOT%MediaFoundationTransform\AVSVideoDecoder\bin\x64\Release\AVSVideoDecoder.dll"
set "BIN=%ROOT%bin"
set "SAMPLE=%ROOT%samples\CCTV-4-short.avs"
set "CANARY=6c52e2bcd47d12ceee40532e1173eb0e"
set "FRAMES=%~2"
if "%FRAMES%"=="" set "FRAMES=30"
set "DEFAULTED=0"
set "FILE=%~1"
if "%FILE%"=="" (
    set "FILE=%SAMPLE%"
    set "DEFAULTED=1"
)

if not exist "%MFT%"  echo MISSING %MFT%      & exit /b 2
if not exist "%BIN%\avsmft-test.exe" echo MISSING %BIN%\avsmft-test.exe & exit /b 2
if not exist "%FILE%" echo MISSING sample: %FILE% & exit /b 2

if not exist "%ROOT%tmp" mkdir "%ROOT%tmp"

echo == 1. decode through the delivered DLL (%FILE%, first %FRAMES% pictures)
"%BIN%\avsmft-test.exe" "%FILE%" "%ROOT%tmp\verify.yuv" -mft "%MFT%" -fmt nv12 -frames %FRAMES% >"%ROOT%tmp\verify_decode.log" 2>&1
set "RC1=%ERRORLEVEL%"
findstr /C:decoded "%ROOT%tmp\verify_decode.log"
type "%ROOT%tmp\verify_decode.log" | findstr /R "result: total"

echo == 2. MFT lifecycle through the delivered DLL (10 create/use/destroy cycles)
"%BIN%\mftlifecycle-test.exe" "%MFT%" -repeat 10 -file "%FILE%" >"%ROOT%tmp\verify_lifecycle.log" 2>&1
set "RC2=%ERRORLEVEL%"
type "%ROOT%tmp\verify_lifecycle.log" | findstr /R "lifecycle failure"

if not "%RC1%"=="0" echo VERIFY=FAILED (decode exit %RC1%), see tmp\verify_decode.log & exit /b 1
if not "%RC2%"=="0" echo VERIFY=FAILED (lifecycle exit %RC2%), see tmp\verify_lifecycle.log & exit /b 1

REM no errorlevel plumbing here: the steps above leave errorlevel from findstr, which
REM would look like a failure.  The subroutine reports through a variable.
set "CANARY_RESULT="
if "%DEFAULTED%"=="1" call :canary
if "%CANARY_RESULT%"=="FAIL" exit /b 1

echo VERIFY=OK  - the delivered DLL decodes this file and survives the lifecycle test.
echo logs: tmp\verify_decode.log tmp\verify_lifecycle.log
endlocal & exit /b 0

REM ---------------------------------------------------------------------------
REM 3. byte-for-byte: the package's own sample, decoded in full, must give the
REM    recorded bytes.  certutil ships with Windows, so no extra tool is needed.
:canary
set "CANARY_RESULT=FAIL"
echo == 3. byte-level: decode the whole sample and compare with the recorded md5
"%BIN%\avsmft-test.exe" "%FILE%" "%ROOT%tmp\verify_full.yuv" -mft "%MFT%" -fmt i420 >"%ROOT%tmp\verify_full.log" 2>&1
if not "%ERRORLEVEL%"=="0" (
    echo VERIFY=FAILED ^(full decode exit %ERRORLEVEL%^), see tmp\verify_full.log
    goto :eof
)
set "GOTMD5="
for /f "skip=1 delims=" %%H in ('certutil -hashfile "%ROOT%tmp\verify_full.yuv" MD5 2^>nul') do if not defined GOTMD5 set "GOTMD5=%%H"
if /i "%GOTMD5%"=="%CANARY%" (
    echo CANARY=OK - every decoded byte matches the recorded md5 %CANARY%
    set "CANARY_RESULT=OK"
    goto :eof
)
echo VERIFY=FAILED - the decoded bytes differ: md5 %GOTMD5%, expected %CANARY%
goto :eof

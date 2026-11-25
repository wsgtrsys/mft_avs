@echo off
rem The patch gate, in one command.  It exists because the invocation was written down
rem wrong more than once: the old note said
rem     cd ffmpeg-9.1 & git apply --check ..\ffmpeg-9.1_avs3_dra_full.patch
rem and those files have not existed for weeks - the deliverables are
rem     patches\ffmpeg-9.1-avs-dra.patch
rem     patches\ffmpeg-9.1-avs-dra-upstream-hunks.patch
rem and they carry an "ffmpeg-9.1/" prefix, so -p1 only resolves from the parent directory.
rem
rem What this proves, in four checks:
rem   1 the generator runs against a pristine tree (regenerates both deliverables)
rem   2 the generated patch matches the CURRENT source tree (reverse apply against it)
rem   3 the generated patch applies to an UNMODIFIED FFmpeg 9.1 (forward apply in pristine\)
rem   4 same for the review-only patch
rem
rem One-time setup:  mkdir pristine & tar -xJf ffmpeg-9.1.tar.xz -C pristine
setlocal
rem %~dp0 ends with a backslash and points at scripts\; the root is one level up
set ROOT=%~dp0..
set FULL=%ROOT%\patches\ffmpeg-9.1-avs-dra.patch
set REVIEW=%ROOT%\patches\ffmpeg-9.1-avs-dra-upstream-hunks.patch

"C:\Python310\python.exe" "%ROOT%\scripts\make-ffmpeg-patch.py"
if errorlevel 1 (
    echo PATCH_GATE=FAILED  - the generator did not write both patches
    exit /b 1
)

if not exist "%ROOT%\pristine\ffmpeg-9.1\configure" (
    echo PATCH_GATE=FAILED  - no pristine tree at "%ROOT%\pristine\ffmpeg-9.1"
    echo   create it with:  mkdir "%ROOT%\pristine" ^&^& tar -xJf ffmpeg-9.1.tar.xz -C "%ROOT%\pristine"
    exit /b 1
)

git -C "%ROOT%" apply -p1 --check -R "%FULL%"
if errorlevel 1 (
    echo PATCH_GATE=FAILED  - the full patch does not match the current source tree
    exit /b 1
)
git -C "%ROOT%" apply -p1 --check -R "%REVIEW%"
if errorlevel 1 (
    echo PATCH_GATE=FAILED  - the review patch does not match the current source tree
    exit /b 1
)
git -C "%ROOT%\pristine" apply -p1 --check "%FULL%"
if errorlevel 1 (
    echo PATCH_GATE=FAILED  - the full patch does not apply to unmodified FFmpeg 9.1
    exit /b 1
)
git -C "%ROOT%\pristine" apply -p1 --check "%REVIEW%"
if errorlevel 1 (
    echo PATCH_GATE=FAILED  - the review patch does not apply to unmodified FFmpeg 9.1
    exit /b 1
)
echo PATCH_GATE=OK  - generated, matches the current tree, applies to unmodified 9.1
exit /b 0

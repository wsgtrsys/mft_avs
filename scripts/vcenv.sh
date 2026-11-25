#!/bin/bash
# Import *only what the MSVC tools need* from the Visual Studio x64 developer
# prompt into the current MSYS2/bash shell.
#
#   usage:  source scripts/vcenv.sh
#
# NB: we deliberately do NOT copy the whole vcvars environment: with the MSYS
# PATH plus INCLUDE/LIB/LIBPATH plus ~40 VS variables the Windows environment
# block for child processes exceeds the 32k limit and MSYS then silently drops
# variables (cl.exe loses INCLUDE -> "cannot open include file").  Only
# INCLUDE / LIB / LIBPATH and the compiler/linker tool directories are needed.

VS_VCVARS="${VS_VCVARS:-/c/Program Files/Microsoft Visual Studio/18/Community/VC/Auxiliary/Build/vcvars64.bat}"

mkdir -p /d/avs/tmp
_bat=/d/avs/tmp/__vcenv.bat
_vcv="$(cygpath -w "$VS_VCVARS")"
{
    printf '@echo off\r\n'
    printf 'call "%s" >nul\r\n' "$_vcv"
    printf 'set INCLUDE\r\nset LIB\r\nset LIBPATH\r\nset VCToolsInstallDir\r\nset WindowsSdkDir\r\nset WindowsSDKVersion\r\n'
} > "$_bat"
_tmp=/d/avs/tmp/__vcenv.txt
/c/Windows/System32/cmd.exe //c "$(cygpath -w "$_bat")" > "$_tmp"

unset INCLUDE LIB LIBPATH VCToolsInstallDir WindowsSdkDir WindowsSDKVersion
while IFS='=' read -r _name _value; do
    _name="${_name%$'\r'}"; _value="${_value%$'\r'}"
    case "$_name" in
        INCLUDE|LIB|LIBPATH|VCToolsInstallDir|WindowsSdkDir|WindowsSDKVersion)
            export "$_name=$_value" ;;
    esac
done < "$_tmp"
rm -f "$_bat" "$_tmp"

if [ -n "$VCToolsInstallDir" ]; then
    _vct="$(cygpath -u "${VCToolsInstallDir%/}")"
    export PATH="$_vct/bin/Hostx64/x64:$_vct/bin/Hostx64/x86:$PATH"
fi
if [ -n "$WindowsSdkDir" ]; then
    _sdkroot="$(cygpath -u "${WindowsSdkDir%/}")"
    for d in "$_sdkroot"/bin/*/x64; do
        [ -x "$d/rc.exe" ] && export PATH="$d:$PATH"
    done
fi
# MSYS2 helpers: make, sh, sed, ... plus nasm
export PATH="/c/msys64/mingw64/bin:/c/msys64/usr/bin:$PATH"
export TMPDIR=/d/avs/tmp
echo "vcenv: cl=$(command -v cl >/dev/null && echo yes || echo NO) lib=$(command -v lib >/dev/null && echo yes || echo NO) nasm=$(command -v nasm || echo NO) make=$(command -v make || echo NO) rc=$(command -v rc >/dev/null && echo yes || echo NO)"

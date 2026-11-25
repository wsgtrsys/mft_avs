//----------------------------------------------------------------------------------------------
// StdAfx.h - AVSVideoDecoder (AVS / AVS+ / AVS2 / AVS3 decoder MFT)
//----------------------------------------------------------------------------------------------
#ifndef STDAFX_H
#define STDAFX_H

#pragma once
#define WIN32_LEAN_AND_MEAN
#define STRICT
#define NOMINMAX
#define WINVER 0x0A00
#define _WIN32_WINNT 0x0A00

#pragma comment(lib, "mf")
#pragma comment(lib, "mfplat")
#pragma comment(lib, "mfuuid")
#pragma comment(lib, "ole32")
#pragma comment(lib, "shlwapi")
#pragma comment(lib, "strmiids")

// FFmpeg (built by scripts/build-ffmpeg.cmd into ..\..\ff91-install)
#pragma comment(lib, "avcodec")
#pragma comment(lib, "avformat")
#pragma comment(lib, "avutil")
#pragma comment(lib, "swscale")
#pragma comment(lib, "bcrypt")      // pulled in by libavutil on Windows
#pragma comment(lib, "ncrypt")
#pragma comment(lib, "ws2_32")     // avformat's network layer
#pragma comment(lib, "secur32")   // avformat's tls_schannel protocol
#pragma comment(lib, "crypt32")
#pragma comment(lib, "user32")    // avutil's dxva2 device enumeration

// AVS2 / AVS3 decoder back ends of FFmpeg (scripts\build-deps.cmd builds them into
// deps\lib; without them the avcodec objects for avs2/avs3 do not resolve).
#pragma comment(lib, "davs2")
#pragma comment(lib, "uavs3d")

//----------------------------------------------------------------------------------------------
#include <WinSDKVer.h>
#include <new>
#include <windows.h>
#include <tchar.h>
#include <assert.h>
#include <stdarg.h>
#include <stdlib.h>
#include <strsafe.h>
#include <vector>
#include <string>
#include <algorithm>
#ifdef _DEBUG
#include <crtdbg.h>
#endif
// NOTE: <initguid.h> is deliberately NOT included here.  It is included exactly
// once, in Guids.cpp, so that the GUIDs of this DLL are defined in one object file
// while every other translation unit only sees extern declarations (the Media
// Foundation ones are resolved from mfuuid.lib).
#include <Shlwapi.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mferror.h>
#include <mftransform.h>

//----------------------------------------------------------------------------------------------
// Common project files shared with the other MediaFoundationTransform samples
#ifdef _DEBUG
#define MF_USE_LOGGING 0
#endif

#include "..\Common\MFMacro.h"
#include "..\Common\MFTrace.h"
#include "..\Common\MFClassFactory.h"
#include "..\Common\MFCriticSection.h"
#include "..\Common\MFRegistry.h"

//----------------------------------------------------------------------------------------------
#include "classGuids.h"

#endif

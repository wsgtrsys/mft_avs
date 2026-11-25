// crashreport.h - small in-process crash reporter for the command line test tools.
//
// Include it once per tool and call InstallCrashReport() at the start of main().
// It prints the faulting address plus a stack walk of the faulting thread, with
// every frame reported as <module> + <offset>; link dbghelp.lib (the build
// scripts do that for the tools).
//
// The interesting case for the AVS MFT debugging was an execute fault at address
// 0 (a call through a NULL function pointer).  StackWalk64 cannot start from
// rip == 0, but on x64 a "call" pushes the return address, so the caller is the
// qword at the faulting RSP.  That is read explicitly and the walk continues
// from there.
#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <dbghelp.h>
#include <stdio.h>

static const char* CrashModName(ULONG64 addr, char* buf, size_t cch)
{
    buf[0] = 0;
    const ULONG64 base = SymGetModuleBase64(GetCurrentProcess(), addr);
    if (base)
        GetModuleFileNameA((HMODULE)(HANDLE)base, buf, (DWORD)cch - 1);
    else
    {
        MEMORY_BASIC_INFORMATION mbi = {};
        if (VirtualQuery((LPCVOID)addr, &mbi, sizeof(mbi)) && mbi.AllocationBase)
        {
            GetModuleFileNameA((HMODULE)mbi.AllocationBase, buf, (DWORD)cch - 1);
            return buf[0] ? buf : "?";
        }
    }
    return buf[0] ? "?" : "?";   // caller only needs the name, may be empty
}

static void CrashPrintFrame(int index, ULONG64 pc)
{
    if (!pc)
        return;
    char mod[MAX_PATH] = "?";
    const ULONG64 base = SymGetModuleBase64(GetCurrentProcess(), pc);
    CrashModName(pc, mod, sizeof(mod));
    const ULONG64 b = base ? base : 0;
    printf("  #%02d %s + 0x%llX\n", index, mod[0] ? mod : "?",
           (unsigned long long)(pc - b));
}

// read a pointer from the target address without faulting the reporter itself
static bool CrashPeekPtr(ULONG64 addr, ULONG64* pOut)
{
    MEMORY_BASIC_INFORMATION mbi = {};
    if (!VirtualQuery((LPCVOID)addr, &mbi, sizeof(mbi)))
        return false;
    if (mbi.State != MEM_COMMIT || !(mbi.Protect & (PAGE_READWRITE | PAGE_EXECUTE_READ |
                                                    PAGE_EXECUTE_READWRITE | PAGE_READONLY |
                                                    PAGE_WRITECOPY | PAGE_EXECUTE_WRITECOPY)))
        return false;
    __try
    {
        *pOut = *(const ULONGLONG*)addr;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
    return *pOut != 0;
}

static const char* CrashCodeName(DWORD code)
{
    switch (code)
    {
    case EXCEPTION_ACCESS_VIOLATION:          return "ACCESS VIOLATION";
    case EXCEPTION_DATATYPE_MISALIGNMENT:     return "MISALIGNMENT";
    case EXCEPTION_BREAKPOINT:                return "BREAKPOINT";
    case EXCEPTION_SINGLE_STEP:               return "SINGLE STEP";
    case EXCEPTION_ARRAY_BOUNDS_EXCEEDED:     return "ARRAY BOUNDS";
    case EXCEPTION_FLT_DENORMAL_OPERAND:      return "FLT DENORMAL";
    case EXCEPTION_FLT_DIVIDE_BY_ZERO:        return "FLT DIVIDE BY ZERO";
    case EXCEPTION_FLT_INVALID_OPERATION:     return "FLT INVALID";
    case EXCEPTION_INT_DIVIDE_BY_ZERO:        return "INT DIVIDE BY ZERO";
    case EXCEPTION_STACK_OVERFLOW:            return "STACK OVERFLOW";
    case EXCEPTION_ILLEGAL_INSTRUCTION:       return "ILLEGAL INSTRUCTION";
    case EXCEPTION_IN_PAGE_ERROR:             return "IN PAGE ERROR";
    case EXCEPTION_PRIV_INSTRUCTION:          return "PRIV INSTRUCTION";
    default:                                  return "EXCEPTION";
    }
}

static LONG CALLBACK CrashReport(PEXCEPTION_POINTERS pEp)
{
    const DWORD  code = pEp->ExceptionRecord->ExceptionCode;
    const ULONG64 rip = (ULONG64)pEp->ContextRecord->Rip;

    if (code != EXCEPTION_ACCESS_VIOLATION &&
        code != EXCEPTION_ILLEGAL_INSTRUCTION &&
        code != EXCEPTION_STACK_OVERFLOW &&
        code != EXCEPTION_IN_PAGE_ERROR &&
        code != EXCEPTION_DATATYPE_MISALIGNMENT &&
        code != 0xC0000409 /* fastfail */ &&
        code != EXCEPTION_INT_DIVIDE_BY_ZERO)
        return EXCEPTION_CONTINUE_SEARCH;

    printf("%s", CrashCodeName(code));
    if (code == EXCEPTION_ACCESS_VIOLATION)
    {
        const ULONG_PTR kind = pEp->ExceptionRecord->ExceptionInformation[0];
        const char* op = (kind == 0) ? "read" : (kind == 1) ? "write" :
                         (kind == 8) ? "execute (call/branch to)" : "op";
        printf(" %s addr=%p", op, (void*)pEp->ExceptionRecord->ExceptionInformation[1]);
    }
    char mod[MAX_PATH] = "?";
    CrashModName(rip, mod, sizeof(mod));
    printf("\n  rip=%p", (void*)rip);
    if (mod[0])
    {
        const ULONG64 base = SymGetModuleBase64(GetCurrentProcess(), rip);
        printf(" in %s + 0x%llX", mod, (unsigned long long)(rip - (base ? base : 0)));
    }
    printf("\n  thread %lu\n", GetCurrentThreadId());

    SymInitialize(GetCurrentProcess(), NULL, TRUE);

    CONTEXT ctx = *pEp->ContextRecord;
    STACKFRAME64 sf = {};
    sf.AddrPC.Mode = AddrModeFlat;
    sf.AddrFrame.Mode = AddrModeFlat;
    sf.AddrStack.Mode = AddrModeFlat;

    int n = 0;
    if (!rip)
    {
        // jumped to address 0: the return address of that call sits at RSP
        ULONG64 ret = 0;
        if (CrashPeekPtr((ULONG64)ctx.Rsp, &ret))
        {
            printf("  call through NULL; return address (the caller) = %p\n", (void*)ret);
            sf.AddrPC.Offset = ret;
            sf.AddrStack.Offset = (ULONG64)ctx.Rsp + 8;
            CrashPrintFrame(n++, ret);
        }
        else
        {
            printf("  call through NULL; no readable return address at rsp=%p\n",
                   (void*)ctx.Rsp);
            return EXCEPTION_EXECUTE_HANDLER;
        }
    }
    else
    {
        sf.AddrPC.Offset = rip;
        sf.AddrFrame.Offset = ctx.Rbp;
        sf.AddrStack.Offset = ctx.Rsp;
    }

    for (; n < 32; n++)
    {
        if (!StackWalk64(IMAGE_FILE_MACHINE_AMD64, GetCurrentProcess(),
                         GetCurrentThread(), &sf, &ctx, nullptr,
                         SymFunctionTableAccess64, SymGetModuleBase64, nullptr))
            break;
        if (!sf.AddrPC.Offset)
            break;
        CrashPrintFrame(n, sf.AddrPC.Offset);
    }
    fflush(stdout);
    return EXCEPTION_EXECUTE_HANDLER;
}

static void InstallCrashReport(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    AddVectoredExceptionHandler(1, CrashReport);
}

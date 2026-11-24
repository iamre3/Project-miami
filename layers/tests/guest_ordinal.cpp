// guest_ordinal.cpp — ordinal-import test guest for the ScarlettCompat
// harness. Imports XGameRuntimeInitialize (xgameruntime.dll, ordinal 1)
// and XMemAlloc/XMemFree (xmem.dll, ordinals 1/2) BY ORDINAL via the
// _ord import libs, so the loader's ordinal table (kOrdinalExports) is
// exercised end to end. The ordinals are SYNTHETIC, assigned by the layer
// in .def order — the real Xbox DLL ordinals must be confirmed from the
// exe's import table once it lands. This file contains NO game code; it
// is a test program written for the layer, and it is never run through the
// Windows loader directly (its Xbox imports only resolve inside the
// layer). Results go to guest_report.txt (read by scarlett_verify).
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdint.h>
#include <d3d12.h>  // IUnknown + D3D_FEATURE_LEVEL (needed by xbox_shims.h)
#include "xbox_shims.h"

// Xbox build-info section, like the shipped DLLs carry.
#pragma section(".xbld", read)
__declspec(allocate(".xbld")) static const char kXbld[] =
    "_xbld_edition_name=GXDK,October 2026 GXDK\0"
    "_xbld_edition_full_productbuild=GXDK,10.0.26100.9605\0";

static FILE* g_report = nullptr;

static void Report(const char* name, bool pass, const char* detail) {
    printf("[guest] %-12s %s %s\n", name, pass ? "PASS" : "FAIL",
           detail ? detail : "");
    if (g_report) {
        fprintf(g_report, "%s=%s\n", name, pass ? "PASS" : "FAIL");
        if (detail) fprintf(g_report, "%s_detail=%s\n", name, detail);
        fflush(g_report);
    }
}

int main() {
    fopen_s(&g_report, "guest_report.txt", "w");
    // Keep .xbld: a volatile read forces the compiler to emit the section
    // (an unreferenced static const array is dropped at /O2); /OPT:NOREF
    // keeps it at link time. This exercises the loader's .xbld detection.
    volatile const char* xbldKeep = kXbld;
    (void)*xbldKeep;
    bool ok = true;
    char detail[256] = {};

    HRESULT hr = XGameRuntimeInitialize();
    if (FAILED(hr)) {
        snprintf(detail, sizeof(detail),
                 "XGameRuntimeInitialize(ord 1) hr=0x%08x", (unsigned)hr);
        ok = false;
    }
    void* p = nullptr;
    if (ok) {
        p = XMemAlloc(1024, 0);
        if (!p) {
            snprintf(detail, sizeof(detail), "XMemAlloc(ord 1) returned NULL");
            ok = false;
        }
    }
    if (ok) {
        // Touch the allocation so a bad pointer faults here, not later.
        ((char*)p)[0] = 0x5a;
        ((char*)p)[1023] = (char)0xa5;
        XMemFree(p);
        snprintf(detail, sizeof(detail),
                 "XGameRuntimeInitialize(ord 1) S_OK, XMemAlloc(ord 1) "
                 "1024 bytes ok, XMemFree(ord 2) ok");
    }
    Report("ordinal", ok, detail);
    printf("[guest] ordinal guest %s\n", ok ? "ALL PASS" : "FAIL");
    if (g_report) fclose(g_report);
    return ok ? 0 : 1;
}

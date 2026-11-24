// pe_loader.h — Xbox Series X PE32+ loader.
// Maps a native x86-64 PE into the host process (same ISA, no CPU
// emulation), validates the Xbox .xbld section, applies relocations, and
// resolves imports: Xbox DLLs (d3d12_xs, xmem, xgameruntime, ...) resolve
// to the HLE registry; api-ms-win-* / system DLLs forward to real Windows.
#pragma once
#include "scarlett.h"

namespace sc {

struct LoadedPe {
    void* base = nullptr;       // Mapped image base.
    void* entry = nullptr;      // Entry point (after relocation).
    uint64_t preferredBase = 0;
    bool relocated = false;     // True if mapped away from preferred base.
    char xbldEdition[128] = {}; // e.g. "GXDK,October 2026 GXDK"
    char xbldBuild[64] = {};    // e.g. "GXDK,10.0.26100.9605"
    bool hasXbld = false;
    bool hasTls = false;        // TLS directory present (not yet handled).
    uint32_t importModules = 0;
    uint32_t importsResolved = 0;
    uint32_t importsFailed = 0;
};

// Map an Xbox PE32+ into this process. Does NOT run it.
bool LoadXboxPe(const wchar_t* path, LoadedPe* out, char* err, size_t errSize);

// Run the entry point on the calling thread inside an SEH guard.
// NOTE: a normal EXE entry (mainCRTStartup) calls ExitProcess on return,
// so the caller is expected to be a throwaway child process (see
// scarlett_verify --run). Returns false on SEH exception; exitCode is the
// guest's return value or the exception code.
bool RunXboxPe(LoadedPe* pe, DWORD* exitCode);

void FreeXboxPe(LoadedPe* pe);

}  // namespace sc

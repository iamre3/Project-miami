// ScarlettCompat — Xbox Series X (Scarlett) compatibility layer, scaffolding.
// Common types: HLE registry, logging, status tags (Xenia-style).
// No game code lives here. No exe is fetched or reconstructed by this layer.
#pragma once

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <stdarg.h>

namespace sc {

// HLE status tags, after Xenia's ExportResolver tags.
enum class Status : uint8_t {
    kNotImplemented = 0,  // No handler. Loader writes a stub that returns E_NOTIMPL.
    kStub = 1,            // Logs the call and returns a plausible success value.
    kImplemented = 2,     // Real host implementation (e.g. XMemAlloc -> HeapAlloc).
    kSketchy = 3,         // Implemented but known-incomplete.
    kHighFrequency = 4,   // Called very often; keep the handler cheap.
};

struct Export {
    const char* module;  // e.g. "xgameruntime.dll" (lowercase compare)
    const char* name;    // e.g. "XGameRuntimeInitialize"
    Status status;
    void* fn;            // Host implementation, or nullptr when not implemented.
    const char* note;    // What is known / what is still TBD.
};

// Logging: stderr + scarlett.log in the current directory.
void Log(const char* subsystem, const char* fmt, ...);

// HLE registry lookup (case-insensitive module + name).
const Export* FindExport(const char* module, const char* name);
size_t ExportCount();
const Export* ExportAt(size_t i);
bool IsShimModule(const char* module);

// Generic failure stub the loader writes for unimplemented imports.
extern "C" HRESULT ScNotImplementedStub();

}  // namespace sc

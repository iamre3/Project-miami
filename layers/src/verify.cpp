// scarlett_verify.cpp — verification harness.
// The bar for this scaffolding (research brief §8): a loader + shimmed boot
// path for a synthetic guest title and a first d3d12_xs frame — NOT GTA 6
// playable. This harness proves that bar per subsystem.
//
// Usage: scarlett_verify.exe <guest_pe_path> [timeout_ms]
// The parent process spawns itself as a child (--run) because a normal EXE
// entry point (mainCRTStartup) calls ExitProcess on return, and because a
// guest crash must not kill the harness: the child may die, the parent
// reports CRASH with the exit code. The child writes load_report.txt, the
// guest writes guest_report.txt; the parent reads both and prints the
// per-subsystem PASS/FAIL table.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <string>
#include <vector>
#include <cstring>

typedef bool (*ScLoadFn)(const wchar_t*, void*, char*, size_t);
typedef bool (*ScRunFn)(void*, DWORD*);
typedef void (*ScFreeFn)(void*);

// LoadedPe layout must match src/pe_loader.h (first fields only are read
// here via the report file; the struct itself is opaque to the harness).
static std::wstring ExeDir() {
    wchar_t buf[MAX_PATH];
    GetModuleFileNameW(nullptr, buf, MAX_PATH);
    std::wstring s = buf;
    size_t p = s.find_last_of(L"\\/");
    return p == std::wstring::npos ? L"." : s.substr(0, p);
}

static std::string ReadFileStr(const std::wstring& path) {
    FILE* f = nullptr;
    if (_wfopen_s(&f, path.c_str(), L"rb") != 0 || !f) return "";
    std::string out;
    char tmp[4096];
    size_t n;
    while ((n = fread(tmp, 1, sizeof(tmp), f)) > 0) out.append(tmp, n);
    fclose(f);
    return out;
}

static void WriteFileStr(const std::wstring& path, const std::string& data) {
    FILE* f = nullptr;
    if (_wfopen_s(&f, path.c_str(), L"wb") != 0 || !f) return;
    fwrite(data.data(), 1, data.size(), f);
    fclose(f);
}

static std::wstring JoinDir(const std::wstring& dir, const wchar_t* name) {
    return dir + L"\\" + name;
}

// ---- child mode: load + run the guest in-process ----
static int ChildRun(const wchar_t* guestPath, DWORD timeoutMs) {
    (void)timeoutMs;
    std::wstring dir = ExeDir();
    std::wstring layerPath = JoinDir(dir, L"ScarlettCompat.dll");
    HMODULE layer = LoadLibraryW(layerPath.c_str());
    if (!layer) {
        printf("[verify] FATAL: cannot load ScarlettCompat.dll (%lu)\n",
               GetLastError());
        return 0xE0000001;
    }
    auto scLoad = (ScLoadFn)GetProcAddress(layer, "ScLoadXboxPe");
    auto scRun = (ScRunFn)GetProcAddress(layer, "ScRunXboxPe");
    auto scFree = (ScFreeFn)GetProcAddress(layer, "ScFreeXboxPe");
    if (!scLoad || !scRun || !scFree) {
        printf("[verify] FATAL: layer exports missing\n");
        return 0xE0000001;
    }

    // The LoadedPe struct is defined in the layer; allocate a generous
    // opaque buffer (struct is ~600 bytes; 4 KB is safe).
    std::vector<uint8_t> peBuf(4096, 0);
    char err[1024] = {};
    std::string report;
    bool ok = scLoad(guestPath, peBuf.data(), err, sizeof(err));
    if (!ok) {
        report = "loader=FAIL\nloader_detail=" + std::string(err) + "\n";
        WriteFileStr(JoinDir(dir, L"load_report.txt"), report);
        printf("[verify] LOAD FAIL: %s\n", err);
        return 0xE0000002;
    }
    report = "loader=PASS\n";
    WriteFileStr(JoinDir(dir, L"load_report.txt"), report);
    printf("[verify] LOAD PASS\n");

    // Run the guest. Its entry point is a normal EXE entry: on return it
    // calls ExitProcess with the guest's exit code, so this process ends
    // here. A crash also ends this process with the exception code.
    DWORD exitCode = 0;
    bool ran = scRun(peBuf.data(), &exitCode);
    // Reached only if the entry returned without exiting the process
    // (e.g. a DLL guest). Report and exit with the guest's code.
    printf("[verify] RUN %s exit=0x%08x\n", ran ? "PASS" : "FAIL", exitCode);
    return ran ? (int)exitCode : 0xE0000003;
}

// ---- parent mode: spawn child, wait, collect reports ----
static int ParentRun(const wchar_t* guestPath, DWORD timeoutMs) {
    std::wstring exePath;
    {
        wchar_t buf[MAX_PATH];
        GetModuleFileNameW(nullptr, buf, MAX_PATH);
        exePath = buf;
    }
    // Guest dir becomes the child cwd, so guest_report.txt lands next to
    // the guest.
    std::wstring guestDir = guestPath;
    size_t p = guestDir.find_last_of(L"\\/");
    if (p != std::wstring::npos) guestDir = guestDir.substr(0, p);

    std::wstring cmd = L"\"" + exePath + L"\" --run \"" +
                       std::wstring(guestPath) + L"\"";
    STARTUPINFOW si = {};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi = {};
    if (!CreateProcessW(nullptr, &cmd[0], nullptr, nullptr, FALSE, 0,
                        nullptr, guestDir.c_str(), &si, &pi)) {
        printf("[verify] FATAL: CreateProcess failed (%lu)\n", GetLastError());
        return 1;
    }
    DWORD wait = WaitForSingleObject(pi.hProcess, timeoutMs);
    DWORD childCode = 0;
    bool timedOut = false;
    if (wait == WAIT_TIMEOUT) {
        timedOut = true;
        TerminateProcess(pi.hProcess, 0xE0000004);
        WaitForSingleObject(pi.hProcess, 5000);
    }
    GetExitCodeProcess(pi.hProcess, &childCode);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);

    std::string loadRep = ReadFileStr(JoinDir(guestDir, L"load_report.txt"));
    std::string guestRep =
        ReadFileStr(JoinDir(guestDir, L"guest_report.txt"));

    printf("\n===== ScarlettCompat verification =====\n");
    printf("guest:   %ls\n", guestPath);
    printf("child exit: 0x%08x%s\n", childCode,
           timedOut ? " (TIMEOUT)" : "");
    printf("--- loader ---\n%s",
           loadRep.empty() ? "loader=NO REPORT (child died during load)\n"
                           : loadRep.c_str());
    printf("--- guest subsystems ---\n%s",
           guestRep.empty()
               ? "guest=NO REPORT (guest died before writing results)\n"
               : guestRep.c_str());

    // Verdict: every reported line must be PASS.
    bool allPass = !timedOut && childCode == 0 &&
                   loadRep.find("loader=PASS") != std::string::npos;
    if (!guestRep.empty()) {
        // Each "name=PASS"/"name=FAIL" line counts.
        size_t pos = 0;
        while (pos < guestRep.size()) {
            size_t eol = guestRep.find('\n', pos);
            std::string line = guestRep.substr(
                pos, eol == std::string::npos ? eol : eol - pos);
            if (line.find("=FAIL") != std::string::npos) allPass = false;
            if (line.find("=PASS") == std::string::npos &&
                line.find('=') != std::string::npos &&
                line.find("_detail") == std::string::npos)
                allPass = false;
            if (eol == std::string::npos) break;
            pos = eol + 1;
        }
    } else {
        allPass = false;
    }
    printf("--- verdict: %s ---\n", allPass ? "ALL PASS" : "FAIL");
    printf("=======================================\n");

    std::string out = "child_exit=0x";
    char tmp[32];
    snprintf(tmp, sizeof(tmp), "%08x", childCode);
    out += tmp;
    out += timedOut ? " (TIMEOUT)\n" : "\n";
    out += loadRep;
    out += guestRep;
    out += std::string("verdict=") + (allPass ? "ALL PASS\n" : "FAIL\n");
    WriteFileStr(JoinDir(guestDir, L"scarlett_verify_report.txt"), out);
    return allPass ? 0 : 1;
}

int main(int argc, char** argv) {
    if (argc >= 3 && strcmp(argv[1], "--run") == 0) {
        wchar_t guest[MAX_PATH];
        MultiByteToWideChar(CP_UTF8, 0, argv[2], -1, guest, MAX_PATH);
        return ChildRun(guest, 60000);
    }
    if (argc < 2) {
        printf("usage: scarlett_verify.exe <guest_pe_path> [timeout_ms]\n");
        return 2;
    }
    wchar_t guest[MAX_PATH];
    MultiByteToWideChar(CP_UTF8, 0, argv[1], -1, guest, MAX_PATH);
    DWORD timeout = argc >= 3 ? (DWORD)atoi(argv[2]) : 60000;
    return ParentRun(guest, timeout);
}

#include <windows.h>
#include <iostream>
#include <fstream>
#include <vector>

int main(int argc, char** argv) {
    std::cout << "[MiamiTestApp:Services] Starting guest service verification...\n";

    // 1. Virtual Memory test
    void* pMem = VirtualAlloc(NULL, 1024 * 1024, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!pMem) {
        std::cerr << "[MiamiTestApp:Services] Memory allocation failed\n";
        return 1;
    }
    memset(pMem, 0xAB, 1024 * 1024);
    if (*reinterpret_cast<uint8_t*>(pMem) != 0xAB) {
        std::cerr << "[MiamiTestApp:Services] Memory readback mismatch\n";
        return 2;
    }
    VirtualFree(pMem, 0, MEM_RELEASE);
    std::cout << "[MiamiTestApp:Services] Memory allocation and readback: OK\n";

    // 2. File I/O test
    std::string testPath = "test_guest_io.tmp";
    std::ofstream out(testPath, std::ios::binary);
    if (!out.is_open()) {
        std::cerr << "[MiamiTestApp:Services] File write open failed\n";
        return 3;
    }
    const char msg[] = "Miami Guest I/O Validated";
    out.write(msg, sizeof(msg));
    out.close();

    std::ifstream in(testPath, std::ios::binary);
    if (!in.is_open()) {
        std::cerr << "[MiamiTestApp:Services] File read open failed\n";
        return 4;
    }
    char buf[64] = {0};
    in.read(buf, sizeof(msg));
    in.close();
    DeleteFileA(testPath.c_str());

    if (strcmp(buf, msg) != 0) {
        std::cerr << "[MiamiTestApp:Services] File content mismatch\n";
        return 5;
    }
    std::cout << "[MiamiTestApp:Services] File I/O write, read, and delete: OK\n";

    std::cout << "[MiamiTestApp:Services] All guest services verified successfully.\n";
    return 0;
}

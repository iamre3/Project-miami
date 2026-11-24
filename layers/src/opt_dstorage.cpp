// opt_dstorage.cpp — DirectStorage + BCPack (docs/OPTIMIZATION.md item 6).
// Async read queue (overlapped Win32 I/O on this machine — the desktop
// DirectStorage runtime is not installed here, see the header) plus a
// pluggable decompression stage: NONE (memcpy), ZLIB (RFC 1950/1951
// inflate, CPU), BCPACK_* (E_NOTIMPL — the rANS bitstream is not public).
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include "opt_dstorage.h"
#include "scarlett.h"

using namespace sc;

static ScOptDsPath g_dsPath = SC_OPT_DS_PATH_OVERLAPPED;
static bool g_dsInit = false;

extern "C" HRESULT ScOpt_DStorageInit(ScOptDsPath* path) {
    if (g_dsInit) {
        if (path) *path = g_dsPath;
        return S_OK;
    }
    // The desktop DirectStorage runtime is a separate install
    // (dstorage.dll + the SDK header). Detect it; the API wiring
    // needs the SDK header and is future work — the queue path below
    // is overlapped Win32 I/O either way on this machine.
    HMODULE ds = LoadLibraryA("dstorage.dll");
    if (ds) {
        Log("opt_dstorage",
            "dstorage.dll present but the DirectStorage API is not wired "
            "(needs the SDK header) — using overlapped I/O");
        FreeLibrary(ds);
        g_dsPath = SC_OPT_DS_PATH_OVERLAPPED;
    } else {
        g_dsPath = SC_OPT_DS_PATH_OVERLAPPED;
    }
    g_dsInit = true;
    if (path) *path = g_dsPath;
    Log("opt_dstorage", "storage queue initialized (path=overlapped)");
    return S_OK;
}

static HRESULT ReadOnce(HANDLE file, uint64_t offset, uint32_t size,
                        uint8_t* buf) {
    OVERLAPPED ov = {};
    ov.Offset = (DWORD)(offset & 0xFFFFFFFFULL);
    ov.OffsetHigh = (DWORD)(offset >> 32);
    ov.hEvent = CreateEvent(nullptr, TRUE, FALSE, nullptr);
    if (!ov.hEvent) return E_FAIL;
    uint32_t done = 0;
    while (done < size) {
        DWORD chunk = 0;
        BOOL ok = ReadFile(file, buf + done, size - done, &chunk, &ov);
        if (!ok && GetLastError() != ERROR_IO_PENDING) {
            CloseHandle(ov.hEvent);
            return HRESULT_FROM_WIN32(GetLastError());
        }
        if (!ok) {
            DWORD w = WaitForSingleObject(ov.hEvent, 30000);
            if (w != WAIT_OBJECT_0) {
                CancelIo(file);
                CloseHandle(ov.hEvent);
                return E_FAIL;
            }
            if (!GetOverlappedResult(file, &ov, &chunk, FALSE)) {
                CloseHandle(ov.hEvent);
                return HRESULT_FROM_WIN32(GetLastError());
            }
        }
        if (chunk == 0) {  // EOF before the requested size
            CloseHandle(ov.hEvent);
            return E_FAIL;
        }
        done += chunk;
        ov.Offset += chunk;  // advance for the next partial read
        // Note: OffsetHigh stays valid while offset < 4 GB in this use;
        // the guest test reads small files. Large-file correctness
        // would carry the offset into OffsetHigh on overflow.
        if (ov.Offset < chunk) ov.OffsetHigh++;
    }
    CloseHandle(ov.hEvent);
    return S_OK;
}

extern "C" HRESULT ScOpt_DStorageReadFile(const char* path, uint64_t offset,
                                          uint32_t size, uint8_t** data,
                                          uint32_t* bytesRead) {
    if (!g_dsInit || !path || size == 0 || !data || !bytesRead)
        return E_INVALIDARG;
    *data = nullptr;
    *bytesRead = 0;
    HANDLE file = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, nullptr,
                              OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        return HRESULT_FROM_WIN32(GetLastError());
    uint8_t* buf = (uint8_t*)malloc(size);
    if (!buf) {
        CloseHandle(file);
        return E_OUTOFMEMORY;
    }
    HRESULT hr = ReadOnce(file, offset, size, buf);
    CloseHandle(file);
    if (FAILED(hr)) {
        free(buf);
        return hr;
    }
    *data = buf;
    *bytesRead = size;
    return S_OK;
}

extern "C" HRESULT ScOpt_DStorageReadFileBatch(
    const char* path, const uint64_t* offsets, const uint32_t* sizes,
    uint32_t count, uint8_t** datas, uint32_t* bytesRead) {
    if (!g_dsInit || !path || !offsets || !sizes || count == 0 ||
        count > MAXIMUM_WAIT_OBJECTS || !datas || !bytesRead)
        return E_INVALIDARG;
    for (uint32_t i = 0; i < count; i++) {
        datas[i] = nullptr;
        bytesRead[i] = 0;
    }
    HANDLE file = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, nullptr,
                              OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        return HRESULT_FROM_WIN32(GetLastError());
    OVERLAPPED* ovs = (OVERLAPPED*)calloc(count, sizeof(OVERLAPPED));
    HANDLE* events = (HANDLE*)calloc(count, sizeof(HANDLE));
    if (!ovs || !events) {
        free(ovs);
        free(events);
        CloseHandle(file);
        return E_OUTOFMEMORY;
    }
    HRESULT hr = S_OK;
    uint32_t issued = 0;
    for (uint32_t i = 0; i < count; i++) {
        datas[i] = (uint8_t*)malloc(sizes[i]);
        if (!datas[i]) {
            hr = E_OUTOFMEMORY;
            break;
        }
        events[i] = CreateEvent(nullptr, TRUE, FALSE, nullptr);
        if (!events[i]) {
            hr = E_FAIL;
            break;
        }
        ovs[i].Offset = (DWORD)(offsets[i] & 0xFFFFFFFFULL);
        ovs[i].OffsetHigh = (DWORD)(offsets[i] >> 32);
        ovs[i].hEvent = events[i];
        DWORD chunk = 0;
        BOOL ok = ReadFile(file, datas[i], sizes[i], &chunk, &ovs[i]);
        if (!ok && GetLastError() != ERROR_IO_PENDING) {
            hr = HRESULT_FROM_WIN32(GetLastError());
            break;
        }
        issued++;
    }
    if (SUCCEEDED(hr)) {
        DWORD w = WaitForMultipleObjects(issued, events, TRUE, 30000);
        if (w < WAIT_OBJECT_0 || w >= WAIT_OBJECT_0 + issued) {
            CancelIo(file);
            hr = E_FAIL;
        }
    }
    if (SUCCEEDED(hr)) {
        for (uint32_t i = 0; i < issued; i++) {
            DWORD chunk = 0;
            if (!GetOverlappedResult(file, &ovs[i], &chunk, FALSE) ||
                chunk != sizes[i]) {
                hr = E_FAIL;
                break;
            }
            bytesRead[i] = chunk;
        }
    }
    for (uint32_t i = 0; i < count; i++) {
        if (events[i]) CloseHandle(events[i]);
        if (FAILED(hr) && datas[i]) {
            free(datas[i]);
            datas[i] = nullptr;
            bytesRead[i] = 0;
        }
    }
    free(ovs);
    free(events);
    CloseHandle(file);
    if (SUCCEEDED(hr))
        Log("opt_dstorage", "batch read: %u chunks issued concurrently",
            count);
    return hr;
}

// ---- inflate (RFC 1950/1951) ----
struct BitReader {
    const uint8_t* p = nullptr;
    size_t len = 0;
    size_t pos = 0;
    uint32_t bitbuf = 0;
    int bitcnt = 0;
    bool Fill() {
        while (bitcnt <= 24 && pos < len) {
            bitbuf |= (uint32_t)p[pos++] << bitcnt;
            bitcnt += 8;
        }
        return bitcnt > 0;
    }
    // Returns -1 on underflow.
    int Bits(int n) {
        if (n == 0) return 0;
        if (!Fill() && bitcnt < n) return -1;
        int v = (int)(bitbuf & ((n == 32) ? 0xFFFFFFFFu : ((1u << n) - 1)));
        bitbuf >>= n;
        bitcnt -= n;
        return v;
    }
};

struct Huffman {
    // Canonical codes: counts[len] = number of codes of that length;
    // symbols[] in code order; firstCode[len] = first code of that length.
    uint16_t counts[16] = {};
    uint16_t symbols[288] = {};
    uint16_t firstCode[16] = {};
    uint16_t firstSym[16] = {};
    bool Build(const uint8_t* lengths, int n) {
        memset(counts, 0, sizeof(counts));
        for (int i = 0; i < n; i++) {
            if (lengths[i] > 15) return false;
            counts[lengths[i]]++;
        }
        counts[0] = 0;
        uint16_t offs[16] = {};
        for (int len = 1; len < 16; len++) {
            offs[len] = (uint16_t)(offs[len - 1] + counts[len - 1]);
            firstSym[len] = offs[len];
        }
        for (int i = 0; i < n; i++) {
            if (lengths[i] != 0)
                symbols[offs[lengths[i]]++] = (uint16_t)i;
        }
        uint16_t code = 0;
        for (int len = 1; len < 16; len++) {
            code = (uint16_t)((code + counts[len - 1]) << 1);
            firstCode[len] = code;
        }
        return true;
    }
    // Decode one symbol. Returns -1 on underflow/invalid code.
    int Decode(BitReader* br) {
        uint32_t code = 0;
        for (int len = 1; len < 16; len++) {
            int b = br->Bits(1);
            if (b < 0) return -1;
            code = (code << 1) | (uint32_t)b;
            int count = counts[len];
            if (code - firstCode[len] < (uint32_t)count) {
                return symbols[firstSym[len] + (code - firstCode[len])];
            }
        }
        return -1;
    }
};

static const uint16_t kLenBase[29] = {
    3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31,
    35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
static const uint8_t kLenExtra[29] = {
    0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2,
    3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
static const uint16_t kDistBase[30] = {
    1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193,
    257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145,
    8193, 12289, 16385, 24577};
static const uint8_t kDistExtra[30] = {
    0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6,
    7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};

// Decode one deflate block with the given literal/length + distance tables.
static HRESULT InflateBlock(BitReader* br, Huffman* lit, Huffman* dist,
                            uint8_t* dst, size_t dstLen, size_t* outPos) {
    for (;;) {
        int sym = lit->Decode(br);
        if (sym < 0) return E_FAIL;
        if (sym < 256) {
            if (*outPos >= dstLen) return E_FAIL;
            dst[(*outPos)++] = (uint8_t)sym;
        } else if (sym == 256) {
            return S_OK;  // end of block
        } else if (sym <= 285) {
            int li = sym - 257;
            int extra = br->Bits(kLenExtra[li]);
            if (extra < 0) return E_FAIL;
            size_t len = kLenBase[li] + (size_t)extra;
            int dsym = dist->Decode(br);
            if (dsym < 0 || dsym > 29) return E_FAIL;
            int dextra = br->Bits(kDistExtra[dsym]);
            if (dextra < 0) return E_FAIL;
            size_t d = kDistBase[dsym] + (size_t)dextra;
            if (d > *outPos) return E_FAIL;  // distance past start
            if (*outPos + len > dstLen) return E_FAIL;
            for (size_t i = 0; i < len; i++) {
                dst[*outPos] = dst[*outPos - d];
                (*outPos)++;
            }
        } else {
            return E_FAIL;
        }
    }
}

static uint32_t Adler32(const uint8_t* data, size_t len) {
    uint32_t a = 1, b = 0;
    for (size_t i = 0; i < len; i++) {
        a = (a + data[i]) % 65521;
        b = (b + a) % 65521;
    }
    return (b << 16) | a;
}

static HRESULT InflateZlib(const uint8_t* src, size_t srcLen, uint8_t* dst,
                           size_t dstLen, size_t* outLen) {
    if (srcLen < 2) return E_INVALIDARG;
    // RFC 1950 header: CMF must be 0x78 (deflate, 32K window), FLG check
    // bits must make (CMF*256+FLG) % 31 == 0, FDICT must be 0.
    if ((src[0] & 0x0F) != 8) return E_FAIL;
    if (((src[0] << 8) + src[1]) % 31 != 0) return E_FAIL;
    if (src[1] & 0x20) return E_FAIL;  // preset dictionary: unsupported
    if (srcLen < 6) return E_FAIL;     // need at least header + adler32
    BitReader br;
    br.p = src + 2;
    br.len = srcLen - 2 - 4;  // exclude the trailing adler32
    size_t outPos = 0;
    for (;;) {
        int bfinal = br.Bits(1);
        int btype = br.Bits(2);
        if (bfinal < 0 || btype < 0) return E_FAIL;
        if (btype == 0) {
            // Stored block: align to a byte boundary, LEN/NLEN, copy.
            // Total bits consumed = pos*8 - bitcnt; drop the remaining
            // bits of the current byte, then rewind pos past the whole
            // bytes still sitting in bitbuf (bitcnt is a multiple of 8
            // after the alignment drop).
            int consumedMod8 = (int)(((size_t)br.pos * 8 - (size_t)br.bitcnt) % 8);
            int drop = (8 - consumedMod8) % 8;
            if (drop > 0 && br.Bits(drop) < 0) return E_FAIL;
            br.pos -= (size_t)(br.bitcnt / 8);
            br.bitcnt = 0;
            br.bitbuf = 0;
            if (br.pos + 4 > br.len) return E_FAIL;
            uint32_t len = br.p[br.pos] | ((uint32_t)br.p[br.pos + 1] << 8);
            uint32_t nlen = br.p[br.pos + 2] | ((uint32_t)br.p[br.pos + 3] << 8);
            br.pos += 4;
            if (len != ((~nlen) & 0xFFFF)) return E_FAIL;
            if (br.pos + len > br.len) return E_FAIL;
            if (outPos + len > dstLen) return E_FAIL;
            memcpy(dst + outPos, br.p + br.pos, len);
            br.pos += len;
            outPos += len;
        } else if (btype == 1) {
            // Fixed Huffman tables (RFC 1951 section 3.2.6).
            uint8_t litLen[288] = {};
            uint8_t distLen[30] = {};
            for (int i = 0; i < 144; i++) litLen[i] = 8;
            for (int i = 144; i < 256; i++) litLen[i] = 9;
            for (int i = 256; i < 280; i++) litLen[i] = 7;
            for (int i = 280; i < 288; i++) litLen[i] = 8;
            for (int i = 0; i < 30; i++) distLen[i] = 5;
            Huffman lit, dist;
            if (!lit.Build(litLen, 288) || !dist.Build(distLen, 30))
                return E_FAIL;
            HRESULT hr = InflateBlock(&br, &lit, &dist, dst, dstLen, &outPos);
            if (FAILED(hr)) return hr;
        } else if (btype == 2) {
            // Dynamic Huffman tables.
            int hlit = br.Bits(5);
            int hdist = br.Bits(5);
            int hclen = br.Bits(4);
            if (hlit < 0 || hdist < 0 || hclen < 0) return E_FAIL;
            hlit += 257;
            hdist += 1;
            hclen += 4;
            static const uint8_t kOrder[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10,
                                               5, 11, 4, 12, 3, 13, 2, 14, 1,
                                               15};
            uint8_t clLen[19] = {};
            for (int i = 0; i < hclen; i++) {
                int v = br.Bits(3);
                if (v < 0) return E_FAIL;
                clLen[kOrder[i]] = (uint8_t)v;
            }
            Huffman cl;
            if (!cl.Build(clLen, 19)) return E_FAIL;
            uint8_t lengths[288 + 30] = {};
            int total = hlit + hdist;
            int i = 0;
            while (i < total) {
                int sym = cl.Decode(&br);
                if (sym < 0) return E_FAIL;
                if (sym < 16) {
                    lengths[i++] = (uint8_t)sym;
                } else if (sym == 16) {
                    if (i == 0) return E_FAIL;
                    int rep = br.Bits(2);
                    if (rep < 0) return E_FAIL;
                    rep += 3;
                    if (i + rep > total) return E_FAIL;
                    uint8_t prev = lengths[i - 1];
                    for (int j = 0; j < rep; j++) lengths[i++] = prev;
                } else if (sym == 17) {
                    int rep = br.Bits(3);
                    if (rep < 0) return E_FAIL;
                    rep += 3;
                    if (i + rep > total) return E_FAIL;
                    for (int j = 0; j < rep; j++) lengths[i++] = 0;
                } else if (sym == 18) {
                    int rep = br.Bits(7);
                    if (rep < 0) return E_FAIL;
                    rep += 11;
                    if (i + rep > total) return E_FAIL;
                    for (int j = 0; j < rep; j++) lengths[i++] = 0;
                } else {
                    return E_FAIL;
                }
            }
            Huffman lit, dist;
            if (!lit.Build(lengths, hlit)) return E_FAIL;
            if (!dist.Build(lengths + hlit, hdist)) return E_FAIL;
            HRESULT hr = InflateBlock(&br, &lit, &dist, dst, dstLen, &outPos);
            if (FAILED(hr)) return hr;
        } else {
            return E_FAIL;  // reserved BTYPE
        }
        if (bfinal) break;
    }
    // Trailing adler32 (big-endian) must match the output.
    uint32_t want = ((uint32_t)src[srcLen - 4] << 24) |
                    ((uint32_t)src[srcLen - 3] << 16) |
                    ((uint32_t)src[srcLen - 2] << 8) |
                    (uint32_t)src[srcLen - 1];
    if (Adler32(dst, outPos) != want) return E_FAIL;
    *outLen = outPos;
    return S_OK;
}

extern "C" HRESULT ScOpt_DStorageDecompress(ScOptDsDecompress mode,
                                            const uint8_t* src, size_t srcLen,
                                            uint8_t* dst, size_t dstLen,
                                            size_t* outLen) {
    if (!src || !dst || !outLen) return E_INVALIDARG;
    *outLen = 0;
    switch (mode) {
        case SC_OPT_DS_DECOMP_NONE:
            if (dstLen < srcLen) return E_INVALIDARG;
            memcpy(dst, src, srcLen);
            *outLen = srcLen;
            return S_OK;
        case SC_OPT_DS_DECOMP_ZLIB:
            return InflateZlib(src, srcLen, dst, dstLen, outLen);
        case SC_OPT_DS_DECOMP_BCPACK_BC1:
        case SC_OPT_DS_DECOMP_BCPACK_BC2:
        case SC_OPT_DS_DECOMP_BCPACK_BC3:
        case SC_OPT_DS_DECOMP_BCPACK_BC4:
        case SC_OPT_DS_DECOMP_BCPACK_BC5:
        case SC_OPT_DS_DECOMP_BCPACK_BC6H:
        case SC_OPT_DS_DECOMP_BCPACK_BC7:
            Log("opt_dstorage",
                "BCPack decompress mode %d: E_NOTIMPL (rANS bitstream "
                "not public)",
                (int)mode);
            return E_NOTIMPL;
        default:
            return E_INVALIDARG;
    }
}

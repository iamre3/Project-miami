#include "graphics/shader_compiler.h"
#include <d3dcompiler.h>
#include <windows.h>

#pragma comment(lib, "d3dcompiler.lib")

namespace miami::gfx {

ShaderCompileOutput ShaderCompiler::CompileHlsl(const ShaderCompileDesc& desc) {
    ShaderCompileOutput out;

    const char* targetProfile = "vs_5_1";
    if (desc.stage == ShaderStage::Pixel) {
        targetProfile = "ps_5_1";
    } else if (desc.stage == ShaderStage::Compute) {
        targetProfile = "cs_5_1";
    }
    out.targetProfile = targetProfile;

    UINT flags = D3DCOMPILE_ENABLE_STRICTNESS;
    if (desc.enableDebug) {
        flags |= D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
    } else if (desc.optimize) {
        flags |= D3DCOMPILE_OPTIMIZATION_LEVEL3;
    }

    ID3DBlob* pBytecode = nullptr;
    ID3DBlob* pErrorBlob = nullptr;

    HRESULT hr = D3DCompile(
        desc.source.data(),
        desc.source.size(),
        desc.sourceFileName.c_str(),
        nullptr, // defines
        D3D_COMPILE_STANDARD_FILE_INCLUDE,
        desc.entryPoint.c_str(),
        targetProfile,
        flags,
        0,
        &pBytecode,
        &pErrorBlob
    );

    if (SUCCEEDED(hr) && pBytecode) {
        out.success = true;
        const uint8_t* pData = static_cast<const uint8_t*>(pBytecode->GetBufferPointer());
        size_t size = pBytecode->GetBufferSize();
        out.bytecode.assign(pData, pData + size);
        pBytecode->Release();
    } else {
        out.success = false;
        if (pErrorBlob) {
            out.errorMessage = static_cast<const char*>(pErrorBlob->GetBufferPointer());
            pErrorBlob->Release();
        } else {
            out.errorMessage = "Shader compilation failed with HRESULT 0x" + std::to_string(hr);
        }
    }

    if (pErrorBlob && out.success) {
        pErrorBlob->Release();
    }

    return out;
}

} // namespace miami::gfx

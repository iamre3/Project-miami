#pragma once
#include <string>
#include <vector>
#include <cstdint>

namespace miami::gfx {

enum class ShaderStage {
    Vertex,
    Pixel,
    Compute
};

struct ShaderCompileDesc {
    std::string source;
    std::string entryPoint = "main";
    ShaderStage stage = ShaderStage::Vertex;
    std::string sourceFileName = "shader.hlsl";
    bool enableDebug = false;
    bool optimize = true;
};

struct ShaderCompileOutput {
    bool success = false;
    std::vector<uint8_t> bytecode;
    std::string errorMessage;
    std::string targetProfile;
};

class ShaderCompiler {
public:
    static ShaderCompileOutput CompileHlsl(const ShaderCompileDesc& desc);
};

} // namespace miami::gfx

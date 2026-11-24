cbuffer FrameConstants : register(b0) {
    float4x4 u_ViewProj;
};

cbuffer ObjectConstants : register(b1) {
    float4x4 u_World;
    float4   u_Tint;
};

struct VSInput {
    float3 position : POSITION;
    float4 color    : COLOR;
    float2 uv       : TEXCOORD0;
};

struct PSInput {
    float4 position : SV_POSITION;
    float4 color    : COLOR;
    float2 uv       : TEXCOORD0;
};

PSInput VSMain(VSInput input) {
    PSInput output;
    float4 worldPos = mul(u_World, float4(input.position, 1.0f));
    output.position = mul(u_ViewProj, worldPos);
    output.color = input.color * u_Tint;
    output.uv = input.uv;
    return output;
}

Texture2D g_Texture : register(t0);
SamplerState g_Sampler : register(s0);

float4 PSColor(PSInput input) : SV_TARGET {
    return input.color;
}

float4 PSTexture(PSInput input) : SV_TARGET {
    return g_Texture.Sample(g_Sampler, input.uv) * input.color;
}

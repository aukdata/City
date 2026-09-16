// Far vehicle geometry is shared; only the world matrices change each frame.
cbuffer VSPerView : register(b1) { row_major float4x4 g_worldToProjected; }
cbuffer VehicleInstances : register(b4) { row_major float4x4 g_instanceWorld[256]; float4 g_instanceColors[256]; }
struct VSInput { float4 position : POSITION; float3 normal : NORMAL; float2 uv : TEXCOORD0; };
struct PSInput { float4 position : SV_POSITION; float3 worldPosition : TEXCOORD0; float2 uv : TEXCOORD1; float3 normal : TEXCOORD2; float4 paint : TEXCOORD3; };
PSInput VS(VSInput input)
{
    row_major float4x4 world = g_instanceWorld[(uint)input.uv.x];
    float4 position = mul(input.position, world);
    PSInput output;
    output.position = mul(position, g_worldToProjected);
    output.worldPosition = position.xyz;
    output.uv = float2(0, 0);
    output.paint = g_instanceColors[(uint)input.uv.x];
    output.normal = mul(input.normal, (float3x3)world);
    return output;
}

// A texture-independent ground pass for the production water GPU readback test.
struct PSInput { float4 position:SV_POSITION; float3 WorldPosition:TEXCOORD0; float2 UV:TEXCOORD1; float3 Normal:TEXCOORD2; };
float4 TerrainPS(PSInput input):SV_TARGET
{
    float light=0.43+0.57*max(0.0,dot(normalize(input.Normal),normalize(float3(-0.4,0.82,-0.35))));
    return float4(float3(0.30,0.36,0.16)*light,1.0);
}

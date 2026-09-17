// Screen-space coverage for real 28mm cables. Thin distant wires fade smoothly.
cbuffer VSPerView : register(b1) { row_major float4x4 g_worldToProjected; }
cbuffer VSPerObject : register(b2) { row_major float4x4 g_localToWorld; }
cbuffer CableView : register(b4) { float4 g_viewport; }
struct VSInput { float4 position : POSITION; float3 normal : NORMAL; float2 uv : TEXCOORD0; };
struct PSInput { float4 position : SV_POSITION; float2 uv : TEXCOORD0; float coverage : TEXCOORD1; };
PSInput Cable_VS(VSInput input)
{
	PSInput output;
	float4 world = mul(input.position, g_localToWorld);
	float4 clip = mul(world, g_worldToProjected);
	// Project the tangent homogeneously: dividing a rear endpoint by a clamped
	// positive w made a passing wire expand into foreground triangles.
	float4 direction = mul(mul(float4(input.normal, 0), g_localToWorld), g_worldToProjected);
	float2 tangent = (direction.xy * clip.w - clip.xy * direction.w) * g_viewport.xy;
	tangent /= max(length(tangent), 0.0001);
	float projectionScale = length(float3(g_worldToProjected[0][0], g_worldToProjected[1][0], g_worldToProjected[2][0]));
	float radius = input.uv.y * projectionScale * g_viewport.x * 0.5;
	// Keep both widths in homogeneous coordinates. No perspective divide is
	// needed, so endpoints on or behind the eye plane remain finite and thin.
	float width = max(0.75 * abs(clip.w), radius);
	clip.xy += float2(-tangent.y, tangent.x) * input.uv.x * width * 2 * g_viewport.zw;
	output.position = clip;
	output.uv = float2(input.uv.x, 0);
	output.coverage = saturate(radius / max(width, 0.0001));
	return output;
}
float4 Cable_PS(PSInput input) : SV_TARGET
{
	float alpha = input.coverage * (1 - smoothstep(0.45, 1.0, abs(input.uv.x)));
	return float4(0.015, 0.018, 0.02, alpha);
}

// Fixed tree meshes are shared across chunks and LOD changes.
cbuffer VSPerView : register(b1) { row_major float4x4 g_worldToProjected; }
struct TreeTransform { float4 positionWidth; float4 heightRotation; };
cbuffer NearTreeInstances : register(b4) { TreeTransform g_nearTrees[64]; }
cbuffer FarTreeInstances : register(b5) { TreeTransform g_farTrees[1024]; }
struct VSInput { float4 position : POSITION; float3 normal : NORMAL; float2 uv : TEXCOORD0; };
struct PSInput { float4 position : SV_POSITION; float3 worldPosition : TEXCOORD0; float2 uv : TEXCOORD1; float3 normal : TEXCOORD2; };
PSInput transformTree(VSInput input, TreeTransform tree)
{
	float3 scale = float3(tree.positionWidth.w, tree.heightRotation.x, tree.positionWidth.w);
	float cosine = tree.heightRotation.y, sine = tree.heightRotation.z;
	float3 local = input.position.xyz * scale;
	float3 world = float3(local.x * cosine + local.z * sine, local.y, -local.x * sine + local.z * cosine) + tree.positionWidth.xyz;
	float3 normal = normalize(input.normal * scale); // Match Siv3D MeshData::scale.
	PSInput output;
	output.position = mul(float4(world, 1), g_worldToProjected);
	output.worldPosition = world;
	output.normal = normalize(float3(normal.x * cosine + normal.z * sine, normal.y, -normal.x * sine + normal.z * cosine));
	output.uv = float2(0, 0); // Foliage and wood shading use world-space coordinates.
	return output;
}

PSInput NearVS(VSInput input) { return transformTree(input, g_nearTrees[(uint)input.uv.x]); }
PSInput FarVS(VSInput input) { return transformTree(input, g_farTrees[(uint)input.uv.x]); }

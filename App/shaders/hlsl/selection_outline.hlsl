//-----------------------------------------------
//
//	選択オブジェクトのアウトライン生成 2D PS
//	入力: 選択オブジェクトをソリッド白で描画したマスクテクスチャ
//	出力: マスクの輪郭のみをカラー付きで返し、内側は透明
//	方式: Dilate - Self（中央が 0 で周辺に 1 があれば縁）
//
//-----------------------------------------------

Texture2D		g_texture0 : register(t0);
SamplerState	g_sampler0 : register(s0);

namespace s3d
{
	struct PSInput
	{
		float4 position	: SV_POSITION;
		float4 color	: COLOR0;
		float2 uv		: TEXCOORD0;
	};
}

// Siv3D 既定の PS Constants
cbuffer PSConstants2D : register(b0)
{
	float4 g_colorAdd;
	float4 g_sdfParam;
	float4 g_sdfOutlineColor;
	float4 g_sdfShadowColor;
	float4 g_internal;
}

// アウトライン用パラメータ
cbuffer OutlineParams : register(b1)
{
	float4 g_texelSize;     // xy = 1/width, 1/height
	float4 g_outlineColor;  // rgba（線色）
	float4 g_outlineScale;  // x = thickness [px]
}

float4 PS(s3d::PSInput input) : SV_TARGET
{
	const float center = g_texture0.Sample(g_sampler0, input.uv).a;
	// 内側判定はアルファ（不透明 = 1.0）で行う — 陰影で RGB は暗くなるが α は一定
	if (center > 0.5)
	{
		// 内側は描画しない（下地の 3D が透けて見える）
		return float4(0, 0, 0, 0);
	}

	const float thickness = max(g_outlineScale.x, 1.0);
	const float2 off = g_texelSize.xy * thickness;

	// 8 近傍サンプル
	float hits = 0;
	hits += g_texture0.Sample(g_sampler0, input.uv + float2(-off.x, -off.y)).a;
	hits += g_texture0.Sample(g_sampler0, input.uv + float2( 0,     -off.y)).a;
	hits += g_texture0.Sample(g_sampler0, input.uv + float2( off.x, -off.y)).a;
	hits += g_texture0.Sample(g_sampler0, input.uv + float2(-off.x,  0     )).a;
	hits += g_texture0.Sample(g_sampler0, input.uv + float2( off.x,  0     )).a;
	hits += g_texture0.Sample(g_sampler0, input.uv + float2(-off.x,  off.y)).a;
	hits += g_texture0.Sample(g_sampler0, input.uv + float2( 0,      off.y)).a;
	hits += g_texture0.Sample(g_sampler0, input.uv + float2( off.x,  off.y)).a;

	const float edge = saturate(hits);
	return float4(g_outlineColor.rgb, g_outlineColor.a * edge);
}

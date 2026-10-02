#pragma once
#include <Siv3D.hpp>
#include <functional>

/// @brief Cached directional shadows and aerial perspective for the city renderer.
class CityLighting
{
public:
	/// @brief Select shadow allocation once, before the first rendered frame.
	bool initialize(FilePathView shaderPath = U"shaders/hlsl/city_forward.hlsl", bool shadowsEnabled = true);
	void update(const BasicCamera3D& camera, Vec3 focus, Vec3 sunDirection,
		double daylight, uint64 geometryRevision, const std::function<void(Vec3, double)>& drawCasters,
		const std::function<void(Vec3, double)>& drawDynamicCasters = {});
	void bind() const;
	[[nodiscard]] const PixelShader& shader() const { return m_forwardShader; }
	[[nodiscard]] const PixelShader& buildingShader() const { return m_buildingShader; }
	[[nodiscard]] const PixelShader& terrainShader() const { return m_terrainShader; }
	[[nodiscard]] const PixelShader& fieldShader() const { return m_fieldShader; }
	[[nodiscard]] const PixelShader& paddyShader() const { return m_paddyShader; }
	[[nodiscard]] const PixelShader& foliageShader() const { return m_foliageShader; }
	[[nodiscard]] double shadowMilliseconds() const { return m_shadowMilliseconds; }
	/// @brief 診断用: キャッシュ済みの実シャドウ深度を読み戻す。通常描画では呼ばない。
	void readStaticShadowDepth(Grid<float>& depth) const { m_shadowMap.read(depth); }
	[[nodiscard]] bool ready() const { return static_cast<bool>(m_forwardShader); }

private:
	struct Parameters
	{
		Mat4x4 worldToShadow = Mat4x4::Identity();
		Float4 shadowParameters{ 0, 0, 0, 0 };
		Float4 fogColorDensity{ 0, 0, 0, 0 };
		Float4 dynamicShadow{ 0, 0, 0, 0 };
		Float4 altitudeBands{ 850, 1700, 1900, 2200 };
		Float4 terrainVariation{ 35, 0, 0, 0 };
	};
	PixelShader m_buildingShader;
	PixelShader m_depthShader, m_forwardShader, m_terrainShader,m_fieldShader,m_paddyShader,m_foliageShader;
	RenderTexture m_shadowMap, m_dynamicShadowMap;
	ConstantBuffer<Parameters> m_parameters;
	Vec3 m_previousFocus{ Math::Inf, 0, 0 };
	Vec3 m_previousSun{ 0, 0, 0 };
	double m_previousExtent = 0.0;
	uint64 m_previousRevision = 0;
	double m_shadowMilliseconds = 0.0;
	bool m_attempted = false;
	bool m_shadowsEnabled = true;
};

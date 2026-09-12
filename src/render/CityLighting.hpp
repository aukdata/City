#pragma once
#include <Siv3D.hpp>
#include <functional>

/// @brief Cached directional shadows and aerial perspective for the city renderer.
class CityLighting
{
public:
	bool initialize(FilePathView shaderPath = U"shaders/hlsl/city_forward.hlsl");
	void update(const BasicCamera3D& camera, Vec3 focus, Vec3 sunDirection,
		double daylight, uint64 geometryRevision, const std::function<void(Vec3, double)>& drawCasters,
		const std::function<void(Vec3, double)>& drawDynamicCasters = {});
	void bind() const;
	[[nodiscard]] const PixelShader& shader() const { return m_forwardShader; }
	[[nodiscard]] const PixelShader& terrainShader() const { return m_terrainShader; }
	[[nodiscard]] const PixelShader& fieldShader() const { return m_fieldShader; }
	[[nodiscard]] const PixelShader& paddyShader() const { return m_paddyShader; }
	[[nodiscard]] const PixelShader& foliageShader() const { return m_foliageShader; }
	[[nodiscard]] double shadowMilliseconds() const { return m_shadowMilliseconds; }
	[[nodiscard]] bool ready() const { return static_cast<bool>(m_forwardShader); }

private:
	struct Parameters
	{
		Mat4x4 worldToShadow = Mat4x4::Identity();
		Float4 shadowParameters{ 0, 0, 0, 0 };
		Float4 fogColorDensity{ 0, 0, 0, 0 };
		Float4 dynamicShadow{ 0, 0, 0, 0 };
	};
	PixelShader m_depthShader, m_forwardShader, m_terrainShader,m_fieldShader,m_paddyShader,m_foliageShader;
	RenderTexture m_shadowMap, m_dynamicShadowMap;
	ConstantBuffer<Parameters> m_parameters;
	Vec3 m_previousFocus{ Math::Inf, 0, 0 };
	Vec3 m_previousSun{ 0, 0, 0 };
	double m_previousExtent = 0.0;
	uint64 m_previousRevision = 0;
	double m_shadowMilliseconds = 0.0;
	bool m_attempted = false;
};

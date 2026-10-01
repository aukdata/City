#include "CityLighting.hpp"
#include "ShaderAsset.hpp"
#include "../gen/GenerationSettings.hpp"

bool CityLighting::initialize(FilePathView shaderPath)
{
	if (m_attempted)
	{
		return ready();
	}
	const auto& settings = GenerationSettings::get();
	m_parameters->altitudeBands = Float4{static_cast<float>(settings.vegetation_maximumForestAltitude),
		static_cast<float>(settings.vegetation_treeLine),static_cast<float>(settings.vegetation_snowStart),static_cast<float>(settings.vegetation_snowFull)};
	m_parameters->terrainVariation = Float4{settings.vegetation_snowVariation,0,0,0};
	m_attempted = true;
	m_depthShader = ShaderAsset::pixel(shaderPath, U"Depth_PS");
	m_forwardShader = ShaderAsset::pixel(shaderPath, U"Shading_PS");
	m_buildingShader = ShaderAsset::pixel(shaderPath, U"Building_PS");
	m_terrainShader = ShaderAsset::pixel(shaderPath, U"Terrain_PS");
	m_fieldShader=ShaderAsset::pixel(shaderPath,U"Field_PS");
	m_paddyShader=ShaderAsset::pixel(shaderPath,U"Paddy_PS");
	m_foliageShader=ShaderAsset::pixel(shaderPath,U"Foliage_PS");
	if (!m_buildingShader || !m_depthShader || !m_forwardShader || !m_terrainShader || !m_fieldShader || !m_paddyShader || !m_foliageShader)
	{
		Logger << U"[CityLighting] Failed to compile " << shaderPath;
		m_forwardShader = PixelShader{};
		return false;
	}
	constexpr int kShadowResolution = 2048;
	m_shadowMap = RenderTexture{ Size{ kShadowResolution, kShadowResolution }, TextureFormat::R32_Float, HasDepth::Yes };
	m_dynamicShadowMap = RenderTexture{ m_shadowMap.size(), TextureFormat::R32_Float, HasDepth::Yes };
	return true;
}

void CityLighting::update(const BasicCamera3D& camera, Vec3 focus, Vec3 sunDirection,
	double daylight, uint64 geometryRevision, const std::function<void(Vec3, double)>& drawCasters,
	const std::function<void(Vec3, double)>& drawDynamicCasters)
{
	m_shadowMilliseconds = 0.0;
	if (!ready())
	{
		return;
	}
	constexpr double kMinExtent = 180.0;
	constexpr double kMaxExtent = 2400.0;
	const double extent = Clamp(camera.getEyePosition().distanceFrom(focus) * 1.8, kMinExtent, kMaxExtent);
	const double texelWorld = extent / m_shadowMap.width();
	focus.x = Round(focus.x / texelWorld) * texelWorld;
	focus.z = Round(focus.z / texelWorld) * texelWorld;
	const bool changed = focus.distanceFromSq(m_previousFocus) > texelWorld * texelWorld
		|| sunDirection.distanceFromSq(m_previousSun) > 0.000004
		|| Abs(extent - m_previousExtent) > texelWorld
		|| geometryRevision != m_previousRevision;
	const float day = static_cast<float>(Clamp(daylight, 0.0, 1.0));
	m_parameters->shadowParameters.w = day;
	const ColorF fog = ColorF{ 0.56, 0.65, 0.70 }.lerp(ColorF{ 0.025, 0.035, 0.065 }, 1.0 - day).removeSRGBCurve();
	constexpr float kFogDensity = 0.00004f;
	m_parameters->fogColorDensity = Float4{ static_cast<float>(fog.r), static_cast<float>(fog.g), static_cast<float>(fog.b), kFogDensity };
	if (changed)
	{
		const Stopwatch timer{ StartImmediately::Yes };
		const Vec3 sunPosition = focus + sunDirection * (extent * 2.0);
		const BasicCamera3D lightCamera{ m_shadowMap.size(), 30_deg, sunPosition, focus };
		// Reverse Z: depth clear is zero, and the test is GreaterEqual in Siv3D.
		const Mat4x4 projection{ DirectX::XMMatrixOrthographicLH(static_cast<float>(extent),
			static_cast<float>(extent), static_cast<float>(extent * 4.0), 1.0f) };
		m_parameters->worldToShadow = lightCamera.getView() * projection;
		const float texel = 1.0f / m_shadowMap.width();
		const float bias = static_cast<float>((texelWorld * 2.0 + 0.03) / (extent * 4.0));
		m_parameters->shadowParameters = Float4{ texel, texel, bias, day };
		const Mat4x4 oldCamera = Graphics3D::GetCameraTransform();
		const Float3 oldEye = Graphics3D::GetEyePosition();
		Graphics3D::SetPSTexture(1, none);
		{
			const ScopedRenderTarget3D target{ m_shadowMap.clear(ColorF{ 0.0 }) };
			const ScopedRenderStates3D states{ BlendState::Opaque, RasterizerState::SolidCullNone, DepthStencilState::DepthTestWrite };
			const ScopedCustomShader3D shader{ m_depthShader };
			Graphics3D::SetCameraTransform(m_parameters->worldToShadow, Float3{ sunPosition });
			drawCasters(focus, extent * 0.8);
		}
		Graphics3D::Flush();
		Graphics3D::SetCameraTransform(oldCamera, oldEye);
		m_previousFocus = focus;
		m_previousSun = sunDirection;
		m_previousExtent = extent;
		m_previousRevision = geometryRevision;
		m_shadowMilliseconds = timer.msF();
	}
	m_parameters->dynamicShadow.x = drawDynamicCasters ? 1.0f : 0.0f;
	if (drawDynamicCasters)
	{
		const Stopwatch timer{ StartImmediately::Yes };
		const Mat4x4 oldCamera = Graphics3D::GetCameraTransform();
		const Float3 oldEye = Graphics3D::GetEyePosition();
		Graphics3D::SetPSTexture(3, none);
		{
			const ScopedRenderTarget3D target{ m_dynamicShadowMap.clear(ColorF{ 0 }) };
			const ScopedRenderStates3D state{ BlendState::Opaque, RasterizerState::SolidCullNone, DepthStencilState::DepthTestWrite };
			const ScopedCustomShader3D shader{ m_depthShader };
			Graphics3D::SetCameraTransform(m_parameters->worldToShadow, Float3{ focus + sunDirection * (extent * 2) });
			drawDynamicCasters(focus, extent * 0.8);
		}
		Graphics3D::Flush();
		Graphics3D::SetCameraTransform(oldCamera, oldEye);
		m_shadowMilliseconds += timer.msF();
	}
}

void CityLighting::bind() const
{
	Graphics3D::SetPSConstantBuffer(4, m_parameters);
	Graphics3D::SetPSTexture(1, m_shadowMap);
	Graphics3D::SetPSTexture(3, m_dynamicShadowMap);
}

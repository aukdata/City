#pragma once
#include <Siv3D.hpp>

/// @brief Opt-in rendering policy; never changes the world, simulation or UI coordinates.
namespace RenderQuality
{
	/// @brief Reduce only the 3D target. HUD and picking keep the native scene size.
	inline Size targetSize(Size sceneSize, bool lowSpec)
	{
		constexpr double kLowSpecScale = 2.0 / 3.0;
		if (!lowSpec) { return sceneSize; }
		return Size{Max(1, static_cast<int>(Round(sceneSize.x * kLowSpecScale))),
			Max(1, static_cast<int>(Round(sceneSize.y * kLowSpecScale)))};
	}
	/// @brief Upscale the 3D image before native-resolution HUD and selection rendering.
	inline void present(const RenderTexture& scene, Size nativeSize)
	{
		Shader::LinearToScreen(scene, RectF{nativeSize}, TextureFilter::Linear);
	}
}

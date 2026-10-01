#pragma once
#include <Siv3D.hpp>

/// @brief Load equivalent shader entry points for Direct3D and OpenGL.
namespace ShaderAsset
{
	inline PixelShader pixel(FilePathView path, StringView entry)
	{
#if SIV3D_PLATFORM(WINDOWS)
		return PixelShader::HLSL(path, entry);
#else
		Array<ConstantBufferBinding> bindings;
		const String name = FileSystem::BaseName(path);
		if (name == U"city_forward" && entry != U"Depth_PS")
		{
			bindings = {{U"PSPerFrame", 0}, {U"PSPerView", 1}, {U"PSPerMaterial", 3}, {U"CityParameters", 4}};
			if (entry == U"VehiclePaint_PS") { bindings << ConstantBufferBinding{U"VehiclePaint", 5}; }
		}
		else if (name == U"selection_outline") { bindings << ConstantBufferBinding{U"OutlineParams", 1}; }
		return GLSL{FileSystem::ParentPath(path) + U"../glsl/{}_{}.frag"_fmt(name, entry), bindings};
#endif
	}
	inline VertexShader vertex(FilePathView path, StringView entry)
	{
#if SIV3D_PLATFORM(WINDOWS)
		return VertexShader::HLSL(path, entry);
#else
		Array<ConstantBufferBinding> bindings{{U"VSPerView", 1}};
		const String name = FileSystem::BaseName(path);
		if (name == U"tree_instances")
		{
			bindings << (entry == U"NearVS" ? ConstantBufferBinding{U"NearTreeInstances", 4} : ConstantBufferBinding{U"FarTreeInstances", 5});
		}
		else if (name == U"vehicle_instances") { bindings << ConstantBufferBinding{U"VehicleInstances", 4}; }
		else if (name == U"city_cable")
		{
			bindings << ConstantBufferBinding{U"VSPerObject", 2} << ConstantBufferBinding{U"CableView", 4};
		}
		return GLSL{FileSystem::ParentPath(path) + U"../glsl/{}_{}.vert"_fmt(name, entry), bindings};
#endif
	}
}

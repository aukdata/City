#pragma once
#include <Siv3D.hpp>
#include "../world/World.hpp"

/// @brief 地域全体を見渡す高度で使う地形。近傍チャンクの外側も海岸線・山脈を保つ。
namespace RegionalTerrain
{
	constexpr int kCells=512;
	constexpr double kMinimumClearance=4500;
	inline MeshData build(const World& world)
	{
		const float extent=static_cast<float>(WORLD_SIZE);
		auto data=MeshData::Grid(Float3{extent*.5f,0,extent*.5f},Float2{extent,extent},kCells,kCells,Float2{extent/32,extent/32});
		for(auto& vertex:data.vertices) { vertex.pos.y=world.sampleHeight(Clamp(vertex.pos.x,0.0f,extent-.01f),Clamp(vertex.pos.z,0.0f,extent-.01f)); }
		data.computeNormals();return data;
	}
}

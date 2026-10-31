#include "RoadRenderer.hpp"
#include "PowerLineGeometry.hpp"
#include "../gen/GenerationSettings.hpp"

/// @file
/// @brief 送電鉄塔と架空線の描画。路線全体を 2km 区画にまとめ、視点から遠い区画を省く。

namespace
{
	constexpr double kTileSize = 2000;
	constexpr double kDrawDistance = 14000;
	constexpr double kConductorRadius = .016;
}

void RoadRenderer::buildPowerLineTiles()
{
	m_powerLinesDirty = false;
	struct Pending { MeshData steel, insulator, concrete, cable; };
	HashTable<int64, Pending> tiles;
	const auto tileOf = [](double x, double z) { return (static_cast<int64>(Math::Floor(x / kTileSize)) << 32) ^ static_cast<uint32>(static_cast<int>(Math::Floor(z / kTileSize))); };
	const double sag = GenerationSettings::get().powerLines_conductorSag;
	for (const auto& line : m_powerLines)
	{
		for (size_t i = 0; i < line.towers.size(); ++i)
		{
			const auto& tower = line.towers[i];
			auto& tile = tiles[tileOf(tower.base.x, tower.base.z)];
			PowerLineGeometry::appendTower(tile.steel, tile.insulator, tile.concrete, tower, line.height);
			if (i + 1 == line.towers.size()) { continue; }
			const auto& next = line.towers[i + 1];
			const auto from = PowerLineGeometry::attachPoints(tower, line.height), to = PowerLineGeometry::attachPoints(next, line.height);
			// 鉄塔の向きが径間ごとに反転しないよう、右側の腕どうしを結ぶ。
			const Vec2 a{ tower.along.y, -tower.along.x }, b{ next.along.y, -next.along.x };
			const bool flipped = a.dot(b) < 0;
			auto& spanTile = tiles[tileOf((tower.base.x + next.base.x) * .5, (tower.base.z + next.base.z) * .5)];
			for (size_t k = 0; k < from.size(); ++k)
			{
				const size_t j = flipped ? (k < 6 ? (k + 3) % 6 : 13 - k) : k;
				PowerLineGeometry::appendConductor(spanTile.cable, from[k], to[j], k < 6 ? sag : sag * .8, kConductorRadius);
			}
		}
	}
	for (auto& [key, pending] : tiles)
	{
		const Vec2 center{ (static_cast<double>(key >> 32) + .5) * kTileSize, (static_cast<double>(static_cast<int>(static_cast<uint32>(key & 0xffffffff))) + .5) * kTileSize };
		m_powerLineTiles << PowerLineTile{ center, Mesh{ pending.steel }, Mesh{ pending.insulator }, Mesh{ pending.concrete }, Mesh{ pending.cable } };
	}
}

void RoadRenderer::drawPowerLines(Vec3 cameraPos)
{
	if (m_powerLinesDirty) { buildPowerLineTiles(); }
	if (m_powerLineTiles.isEmpty()) { return; }
	const Vec2 eye{ cameraPos.x, cameraPos.z };
	{
		const ScopedRenderStates3D state{ RasterizerState::SolidCullNone };
		for (const auto& tile : m_powerLineTiles)
		{
			if (tile.center.distanceFrom(eye) > kDrawDistance) { continue; }
			if (tile.steel) { tile.steel.draw(ColorF{ .60, .62, .62 }.removeSRGBCurve()); }
			if (tile.insulator) { tile.insulator.draw(ColorF{ .36, .30, .26 }.removeSRGBCurve()); }
			if (tile.concrete) { tile.concrete.draw(ColorF{ .66, .66, .62 }.removeSRGBCurve()); }
		}
	}
	Graphics3D::SetVSConstantBuffer(4, m_cableView);
	const ScopedCustomShader3D shader{ m_cableVS, m_cablePS };
	const ScopedRenderStates3D state{ BlendState::Default2D, RasterizerState::SolidCullNone, DepthStencilState::DepthTest };
	for (const auto& tile : m_powerLineTiles)
	{
		if (tile.cable && tile.center.distanceFrom(eye) <= kDrawDistance) { tile.cable.draw(ColorF{ 1 }); }
	}
}

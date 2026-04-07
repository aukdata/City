#pragma once
#include "../gen/MapGenerator.hpp"
#include "../gen/PlaceNameGenerator.hpp"
#include "../ui/Camera.hpp"
#include "../world/World.hpp"

/// @brief 地区地名をビルボードとして2D投影表示するレンダラ
class PlaceNameRenderer
{
public:
	/// @brief 地区地名を描画する（Shader::LinearToScreen の後に呼ぶこと）
	/// @param settlements  地区リスト（MapGenerator::settlements()）
	/// @param camera       ゲームカメラ
	/// @param world        高さサンプリング用ワールド
	void render(const Array<MapGenerator::Settlement>& settlements,
	            const GameCamera& camera,
	            const World& world) const;
};

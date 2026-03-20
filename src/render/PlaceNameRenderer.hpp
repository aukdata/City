#pragma once
#include "../gen/MapGenerator.hpp"
#include "../gen/PlaceNameGenerator.hpp"
#include "../ui/Camera.hpp"
#include "../world/World.hpp"

/// @brief 集落地名をビルボードとして2D投影表示するレンダラ
class PlaceNameRenderer
{
public:
	PlaceNameRenderer();

	/// @brief 集落地名を描画する（Shader::LinearToScreen の後に呼ぶこと）
	/// @param settlements  集落リスト（MapGenerator::settlements()）
	/// @param camera       ゲームカメラ
	/// @param world        高さサンプリング用ワールド
	void render(const Array<MapGenerator::Settlement>& settlements,
	            const GameCamera& camera,
	            const World& world) const;

private:
	Font m_font;
};

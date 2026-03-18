#pragma once
#include "../road/RoadNetwork.hpp"
#include "../world/World.hpp"

/// @brief 道路メッシュの描画クラス
class RoadRenderer
{
public:
	/// @brief 全エッジを描画する
	void render(const RoadNetwork& network, GameTime now, const World& world);

	/// @brief エッジのメッシュキャッシュを無効化する（道路変更時に呼ぶ）
	void markDirty(int edgeId);

private:
	/// @brief 単一エッジを描画する（ポリゴン帯 + 車線区画線）
	void drawEdge(const RoadEdge& edge, const CubicBezier& bezier, GameTime now, const World& world);

	/// @brief ベジェ曲線から道路ポリゴン帯の MeshData を生成する
	MeshData buildRoadMesh(const RoadEdge& edge, const CubicBezier& bezier, const World& world) const;

	/// @brief 車線区画線を描画する
	void drawLaneLines(const RoadEdge& edge, const CubicBezier& bezier, GameTime now) const;

	/// @brief 路面の ColorF を返す
	static ColorF roadSurfaceColor(RoadType rt);

	HashTable<int, Mesh> m_meshCache;  ///< エッジ ID → メッシュのキャッシュ
};

#pragma once
#include "../road/RoadNetwork.hpp"
#include "../world/World.hpp"
#include "../style/RoadStyleRegistry.hpp"

/// @brief 道路メッシュ・車線区画線の描画クラス
/// @details
///   各エッジはノード側を halfWidth 分だけ切り詰めて描画し、
///   ノードキャップ（交差点フィル）メッシュでつなぎ目を埋める。
class RoadRenderer
{
public:
	/// @brief スタイル定義 TOML をロードする（ゲーム起動時に一度呼ぶ）
	bool loadStyle(FilePathView tomlPath);

	/// @brief 全エッジ・ノードキャップを描画する
	void render(const RoadNetwork& network, GameTime now, const World& world);

	/// @brief エッジのメッシュキャッシュを無効化する（道路変更時に呼ぶ）
	void markDirty(int edgeId);

	/// @brief ノード接続数が変わるトポロジー変更を通知する（道路追加・削除時に呼ぶ）
	void markTopologyChanged();

private:
	// ---- キャッシュ構造 ----

	/// @brief 車線区画線の描画バッチ
	struct LaneLineBatch { ColorF color; Mesh mesh; };

	/// @brief エッジに適用されたマージン（端カット量）
	struct EdgeMargins { float a = 0.0f; float b = 0.0f; };

	// ---- 描画サブルーチン ----

	/// @brief 単一エッジを描画する（路面 + 車線区画線）
	void drawEdge(const RoadEdge& edge, const CubicBezier& bezier,
	              float marginA, float marginB, const World& world);

	/// @brief ノードキャップ（交差点フィル）を描画する
	void drawNodeCap(const RoadNetwork& network, int nodeId, const World& world);

	// ---- メッシュ生成 ----

	/// @brief 端カット済みの道路路面 MeshData を生成する
	MeshData buildRoadMesh(const RoadEdge& edge, const CubicBezier& bezier,
	                       const RoadStyle& style, const World& world,
	                       float marginA, float marginB) const;

	/// @brief 車線区画線・センターラインのメッシュバッチを生成する
	Array<LaneLineBatch> buildLaneLineBatches(const RoadEdge& edge, const CubicBezier& bezier,
	                                          const RoadStyle& style, const World& world,
	                                          float marginA, float marginB) const;

	/// @brief ノードキャップの MeshData を生成する（フィレット曲線 + 中心ファン）
	MeshData buildNodeCapMesh(const RoadNetwork& network, int nodeId, const World& world) const;

	// ---- ヘルパー ----

	/// @brief エッジの路肩込み片側幅を返す
	float halfWidthWithShoulder(const RoadEdge& edge) const;

	/// @brief 指定ノード端でのカットオフ量を返す（edge.cutoffA / cutoffB を参照）
	static float edgeMargin(const RoadEdge& edge, int nodeId);

	// ---- メンバ ----

	RoadStyleRegistry                    m_styleRegistry;
	HashTable<int, Mesh>                 m_meshCache;       ///< エッジ ID → 路面メッシュ
	HashTable<int, Array<LaneLineBatch>> m_laneCache;       ///< エッジ ID → 車線区画線
	HashTable<int, EdgeMargins>          m_marginCache;     ///< エッジ ID → 適用済みマージン
	HashTable<int, Mesh>                 m_nodeCapCache;    ///< ノード ID → キャップメッシュ
};

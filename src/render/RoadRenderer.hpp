#pragma once
#include "../road/RoadNetwork.hpp"
#include "../world/World.hpp"
#include "../style/RoadStyleRegistry.hpp"
#include <Siv3D/ViewFrustum.hpp>

/// @brief 通常 / LOD の2段メッシュペア
struct LodMeshPair { Mesh detail; Mesh lod; };

/// @brief 道路メッシュ・車線区画線の描画クラス
/// @details
///   各エッジはノード側を halfWidth 分だけ切り詰めて描画し、
///   ノードキャップ（交差点フィル）メッシュでつなぎ目を埋める。
class RoadRenderer
{
public:
	/// @brief LOD 切り替え距離 [m]
	static constexpr double kLodDist   = 400.0;
	static constexpr double kLodDistSq = kLodDist * kLodDist;

	/// @brief 道路の描画カット距離 [m]
	static constexpr double kDrawMaxDist   = 6000.0;
	static constexpr double kDrawMaxDistSq = kDrawMaxDist * kDrawMaxDist;

	/// @brief スタイル定義 TOML をロードする（ゲーム起動時に一度呼ぶ）
	bool loadStyle(FilePathView tomlPath);

	/// @brief 視錐台内のエッジ・ノードキャップを描画する
	void render(const RoadNetwork& network, GameTime now, const World& world,
	            const ViewFrustum& frustum, Vec3 cameraPos);

	/// @brief エッジのメッシュキャッシュを無効化する（道路変更時に呼ぶ）
	/// @param nodeA,nodeB  指定時はこれらのノードキャップのみ無効化する。
	///                     省略時は全ノードキャップを無効化する（重い）。
	void markDirty(int edgeId, int nodeA = -1, int nodeB = -1);

	/// @brief 全キャッシュをクリアする（道路追加・削除時に呼ぶ）
	void markTopologyChanged();

	/// @brief 指定ノード周辺のキャッシュのみ無効化する（部分的トポロジー変更用）
	void markTopologyChangedAt(int nodeId, const RoadNetwork& network);

private:
	// ---- キャッシュ構造 ----

	/// @brief 車線区画線の描画バッチ
	struct LaneLineBatch { ColorF color; Mesh mesh; };

	/// @brief エッジに適用されたマージン（端カット量）
	struct EdgeMargins { float a = 0.0f; float b = 0.0f; };

	// ---- 描画サブルーチン ----

	/// @brief 単一エッジを描画する（路面 + 車線区画線）
	void drawEdge(const RoadEdge& edge, const RoadNetwork& network,
	              float marginA, float marginB, const World& world, bool isClose);

	/// @brief ノードキャップ（交差点フィル）を描画する
	void drawNodeCap(const RoadNetwork& network, int nodeId, const World& world, bool isClose);

	// ---- メッシュ生成 ----

	/// @brief 端カット済みの道路路面 MeshData を生成する
	/// @param lodFactor  分割数のスケール係数（1.0=通常、0.25=LOD）
	MeshData buildRoadMesh(const RoadEdge& edge, const CubicBezier& bezier,
	                       const RoadStyle& style, const World& world,
	                       float marginA, float marginB, float lodFactor) const;

	/// @brief 車線区画線・センターラインのメッシュバッチを生成する
	Array<LaneLineBatch> buildLaneLineBatches(const RoadEdge& edge, const CubicBezier& bezier,
	                                          const RoadStyle& style, const World& world,
	                                          float marginA, float marginB) const;

	/// @brief ノードキャップの MeshData を生成する（フィレット曲線 + 中心ファン）
	/// @param div  フィレット曲線の分割数（6=通常、2=LOD）
	MeshData buildNodeCapMesh(const RoadNetwork& network, int nodeId, const World& world, int div) const;

	// ---- ヘルパー ----

	float halfWidthWithShoulder(const RoadEdge& edge) const;
	static float edgeMargin(const RoadEdge& edge, int nodeId);

	// ---- メンバ ----

	/// @brief エッジのバウンディング情報（カリング用キャッシュ）
	struct EdgeBounds { Float3 center; float radiusSq; };

	RoadStyleRegistry                    m_styleRegistry;
	HashTable<int, LodMeshPair>          m_meshCache;       ///< エッジ ID → 路面メッシュ(通常/LOD)
	HashTable<int, Array<LaneLineBatch>> m_laneCache;       ///< エッジ ID → 車線区画線
	HashTable<int, EdgeMargins>          m_marginCache;     ///< エッジ ID → 適用済みマージン
	HashTable<int, LodMeshPair>          m_nodeCapCache;    ///< ノード ID → キャップメッシュ(通常/LOD)
	HashTable<int, EdgeBounds>           m_boundsCache;     ///< エッジ ID → バウンディング情報
};

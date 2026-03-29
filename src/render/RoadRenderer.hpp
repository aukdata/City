#pragma once
#include "../road/RoadNetwork.hpp"
#include "../road/RoadPartRegistry.hpp"
#include "../world/World.hpp"
#include <Siv3D/ViewFrustum.hpp>

/// @brief 通常 / LOD の2段メッシュペア
struct LodMeshPair { Mesh detail; Mesh lod; };

/// @brief 部品メッシュ + 描画情報
struct PartMeshEntry
{
	LodMeshPair    meshPair;
	ColorF         color{ 0.35 };
	const Texture* texture = nullptr;  ///< null なら単色
};

/// @brief 道路メッシュ・車線区画線の描画クラス
class RoadRenderer
{
public:
	static constexpr double kLodDist   = 800.0;
	static constexpr double kLodDistSq = kLodDist * kLodDist;
	static constexpr double kDrawMaxDist   = 12000.0;
	static constexpr double kDrawMaxDistSq = kDrawMaxDist * kDrawMaxDist;

	/// @brief 道路部品アセットをロードする
	bool loadAssets();

	void render(const RoadNetwork& network, const World& world,
	            const ViewFrustum& frustum, Vec3 cameraPos);

	void invalidateEdgeCache(int edgeId, int nodeA = -1, int nodeB = -1);
	void invalidateAllCaches();
	void invalidateCachesAroundNode(int nodeId, const RoadNetwork& network);

	struct LaneLineBatch { ColorF color; Mesh mesh; };

private:
	struct EdgeMargins { float atNodeA = 0.0f; float atNodeB = 0.0f; };
	struct EdgeBounds { Float3 center; float radiusSq; };

	// ---- 描画 ----

	void drawEdge(const RoadEdge& edge, const RoadNetwork& network,
	              float marginA, float marginB, const World& world, bool isClose);
	void drawNodeCap(const RoadNetwork& network, int nodeId, const World& world, bool isClose);

	// ---- メッシュ生成 ----

	/// @brief 1部品分の帯メッシュを生成する（共通関数）
	/// @param offsetL  道路中心からの左端 [m]
	/// @param offsetR  道路中心からの右端 [m]
	/// @param heightOffset  路面基準からの高低差 [m]
	MeshData buildStripMesh(const CubicBezier& bezier, const World& world,
	                        float offsetL, float offsetR, float heightOffset,
	                        float sStart, float sEnd, float lodFactor) const;

	/// @brief 全部品のメッシュ配列を生成する
	/// @param flip  true ならオフセットを反転（A→B が正規方向と逆の場合）
	Array<PartMeshEntry> buildPartMeshes(const RoadEdge& edge, const CubicBezier& bezier,
	                                     const World& world,
	                                     float marginA, float marginB, bool flip);

	Array<LaneLineBatch> buildLaneLineBatches(const RoadEdge& edge, const CubicBezier& bezier,
	                                          const World& world,
	                                          float marginA, float marginB, bool flip) const;

	/// @brief 1部品幅でのフィレット曲線 MeshData を生成する
	MeshData buildNodeCapMeshForRange(const RoadNetwork& network, int nodeId,
	                                  const World& world, int div,
	                                  float partOffsetL, float partOffsetR, float heightOffset) const;

	/// @brief ノードキャップの全部品メッシュ配列を生成する
	Array<PartMeshEntry> buildNodeCapParts(const RoadNetwork& network, int nodeId, const World& world, int div);

	/// @brief ノードキャップ上の車線区画線を生成する
	Array<LaneLineBatch> buildNodeCapLaneLines(const RoadNetwork& network, int nodeId, const World& world) const;

	// ---- ヘルパー ----

	static float edgeMargin(const RoadEdge& edge, int nodeId);

	/// @brief エッジの A→B 方向が正規方向と逆かどうかを判定する
	/// @details 正規方向: nodeA の位置 < nodeB の位置（X優先、同値ならZ）
	///          逆なら true を返す → レンダリング時にオフセットを反転すべき
	static bool shouldFlipOffsets(const RoadEdge& edge, const RoadNetwork& network);

	// ---- メンバ ----

	RoadPartRegistry                          m_partRegistry;
	HashTable<int, Array<PartMeshEntry>>      m_partMeshCache;   ///< エッジ ID → 部品メッシュ配列
	HashTable<int, Array<LaneLineBatch>>      m_nodeCapLaneCache; ///< ノード ID → ノードキャップ車線区画線
	HashTable<int, Array<LaneLineBatch>>      m_laneCache;
	HashTable<int, EdgeMargins>               m_marginCache;
	HashTable<int, Array<PartMeshEntry>>      m_nodeCapCache;    ///< ノード ID → 部品メッシュ配列
	HashTable<int, EdgeBounds>                m_boundsCache;
};

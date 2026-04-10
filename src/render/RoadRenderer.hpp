#pragma once
#include "../road/RoadNetwork.hpp"
#include "../road/RoadPartRegistry.hpp"
#include "../sim/SimGraph.hpp"
#include "../traffic/SignalRegistry.hpp"
#include "../traffic/TrafficLight.hpp"
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

	/// @brief 信号機を描画する
	/// @param simGraph 旋回分類用に precomputed なエッジ接線角を取得する
	void drawSignals(const RoadNetwork& network, const SimGraph& simGraph,
	                 const World& world,
	                 const HashTable<int, TrafficLight>& trafficLights,
	                 GameTime gameNow, Vec3 cameraPos);

	/// @brief 信号レジストリへのアクセス
	const SignalRegistry& signalRegistry() const { return m_signalRegistry; }

	/// @brief 直近の render() で可視と判定されたエッジ ID の集合
	const HashSet<int>& visibleEdges() const { return m_visibleEdges; }

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
	                        float sStart, float sEnd, float lodFactor,
	                        bool useElevation = false) const;

	/// @brief 全部品のメッシュ配列を生成する
	Array<PartMeshEntry> buildPartMeshes(const RoadEdge& edge, const CubicBezier& bezier,
	                                     const World& world,
	                                     float marginA, float marginB);

	Array<LaneLineBatch> buildLaneLineBatches(const RoadEdge& edge, const CubicBezier& bezier,
	                                          const World& world,
	                                          float marginA, float marginB) const;

	/// @brief 1部品幅でのフィレット曲線 MeshData を生成する
	MeshData buildNodeCapMeshForRange(const RoadNetwork& network, int nodeId,
	                                  const World& world, int div,
	                                  float partOffsetL, float partOffsetR, float heightOffset) const;

	/// @brief ノードキャップの全部品メッシュ配列を生成する
	Array<PartMeshEntry> buildNodeCapParts(const RoadNetwork& network, int nodeId, const World& world, int div);

	/// @brief ノードキャップ上の車線区画線を生成する
	Array<LaneLineBatch> buildNodeCapLaneLines(const RoadNetwork& network, int nodeId, const World& world) const;

	// ---- ヘルパー ----

	/// @brief 部品の描画属性（色・高さオフセット・テクスチャ）
	struct PartVisual { ColorF color; float heightOff; const Texture* tex; };

	/// @brief RoadPart から描画属性を解決する（defId があればレジストリ参照、なければフォールバック）
	PartVisual getPartVisual(const RoadPart& part) const;

	static float edgeMargin(const RoadEdge& edge, int nodeId);

	/// @brief エッジのキャッシュ4種を一括消去する
	void eraseEdgeCaches(int edgeId);

	/// @brief ノードのキャップキャッシュ2種を一括消去する
	void eraseNodeCaches(int nodeId);

	// ---- 信号描画ヘルパー ----

	/// @brief 信号メッシュキャッシュ（メッシュ名 → Mesh）
	struct SignalMeshCache
	{
		HashTable<String, Mesh> meshes;
	};

	/// @brief 信号定義 ID → メッシュキャッシュ
	HashTable<String, SignalMeshCache> m_signalMeshCache;

	/// @brief 信号定義のメッシュを取得（キャッシュ付き）
	const Mesh* getSignalMesh(const String& defId, const String& meshName);

	// ---- メンバ ----

	SignalRegistry                            m_signalRegistry;
	RoadPartRegistry                          m_partRegistry;
	HashTable<int, Array<PartMeshEntry>>      m_partMeshCache;   ///< エッジ ID → 部品メッシュ配列
	HashTable<int, Array<LaneLineBatch>>      m_nodeCapLaneCache; ///< ノード ID → ノードキャップ車線区画線
	HashTable<int, Array<LaneLineBatch>>      m_laneCache;
	HashTable<int, EdgeMargins>               m_marginCache;
	HashTable<int, Array<PartMeshEntry>>      m_nodeCapCache;    ///< ノード ID → 部品メッシュ配列
	HashTable<int, EdgeBounds>                m_boundsCache;
	HashTable<int, Array<Mesh>>              m_pierMeshCache;   ///< エッジ ID → 橋脚メッシュ配列
	HashSet<int>                              m_visibleEdges;  ///< 直近 render() の可視エッジ集合
};

#pragma once
#include "../road/RoadNetwork.hpp"
#include "../road/RoadConstruction.hpp"
#include "../road/GuideSign.hpp"
#include "../road/ArrowMarkingRegistry.hpp"
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
	RoadPartType materialType = RoadPartType::Shoulder;
	bool asphalt = true; ///< 形状の種別ではなく路盤材質で舗装シェーダを選ぶ。
};

/// @brief 同マテリアルの LOD パーツを結合した描画バッチ（遠距離用）
struct PartLodBatch
{
	Mesh           mesh;
	ColorF         color{ 0.35 };
	const Texture* texture = nullptr;  ///< null なら単色
};

/// @brief 道路メッシュ・車線区画線の描画クラス
class RoadRenderer
{
public:
	static constexpr double kLodDist   = 1200.0;
	static constexpr double kPrepareDist = 1700.0;
	static constexpr double kLodDistSq = kLodDist * kLodDist;
	static constexpr double kDrawMaxDist   = 8000.0;
	static constexpr double kDrawMaxDistSq = kDrawMaxDist * kDrawMaxDist;

	/// @brief CPU cache construction performed in the latest render call.
	struct CacheBuildStats
	{
		int edges = 0, nodes = 0, markings = 0, furniture = 0, deferred = 0;
		double partsMs = 0, nodesMs = 0, markingsMs = 0, furnitureMs = 0, fallbackMs = 0;
		[[nodiscard]] double buildMilliseconds() const { return partsMs + nodesMs + markingsMs + furnitureMs; }
	};
	[[nodiscard]] const CacheBuildStats& cacheBuildStats() const { return m_cacheBuildStats; }

	/// @brief Limit new detailed caches per render call; infinity is the synchronous reference.
	/// @details Existing geometry always draws. A single build cannot be interrupted.
	void setCacheBuildBudget(double milliseconds) { m_cacheBuildBudgetMs = Max(0.01, milliseconds); }

	/// @brief 道路部品アセットをロードする
	bool loadAssets();
	/// @brief 読み込み中に地域全体の簡略路面を準備し、視点移動時の一斉生成を避ける。
	void preloadFallbacks(const RoadNetwork& network,const World& world);
	/// @brief Construction uses the paused/resumed game clock, never wall time.
	void setConstructionTime(GameTime now) { m_constructionNow = now; }

	void render(const RoadNetwork& network, const World& world,
	            const ViewFrustum& frustum, Vec3 cameraPos);

	/// @brief Reuse cached road structure, poles and signs in the static sun-depth pass.
	void renderShadowCasters(Vec3 focus, double radius);
	/// @brief Observe terrain edits before WorldRenderer consumes the dirty flags.
	void synchronizeTerrainChanges(const World& world, const RoadNetwork& network);
	[[nodiscard]] uint64 geometryRevision() const { return m_geometryRevision; }

	/// @brief 信号機を描画する
	/// @param simGraph 旋回分類用に precomputed なエッジ接線角を取得する
	void drawSignals(const RoadNetwork& network, const SimGraph& simGraph,
	                 const World& world,
	                 const HashTable<int, TrafficLight>& trafficLights,
	                 GameTime gameNow, Vec3 cameraPos);

	/// @brief 表示要求のある小型標識テクスチャを合成する（3D シーン描画前・2D パイプライン有効時に呼ぶこと）
	void prepareSignTextures();

	/// @brief 路線番号・道路名標識（3D ポール＋テクスチャ板）を描画する
	void drawRouteSigns(const RoadNetwork& network, const World& world, Vec3 cameraPos);

	/// @brief 案内標識テクスチャを事前合成する（3D 描画前・2D 有効時に呼ぶこと）
	/// @details plan/21_guide_sign_spec.md §4 参照
	void prepareGuideSignTextures(const RoadNetwork& network);

	/// @brief 案内標識のキャッシュ済みテクスチャを取得する（パネルプレビュー用）
	/// @return 見つからなければ nullptr
	const Texture* getGuideSignCachedTexture(const GuideSignPlacement& g) const;

	/// @brief 信号レジストリへのアクセス
	const SignalRegistry& signalRegistry() const { return m_signalRegistry; }

	/// @brief 直近の render() で可視と判定されたエッジ ID の集合
	const HashSet<int>& visibleEdges() const { return m_visibleEdges; }

	/// @brief 選択アウトライン用: 指定エッジのシルエットを描画する（キャッシュ済みメッシュを単色で再利用）
	void drawEdgeSilhouette(int edgeId, const RoadNetwork& network, const World& world,
	                         const ColorF& color);

	/// @brief 選択アウトライン用: 指定ノードキャップのシルエットを描画する
	void drawNodeSilhouette(int nodeId, const RoadNetwork& network, const World& world,
	                         const ColorF& color);

	/// @brief 選択アウトライン用: 指定ノードの信号機のシルエットを描画する
	void drawSignalSilhouette(int nodeId, const RoadNetwork& network, const World& world,
	                           const ColorF& color);

	/// @brief 選択アウトライン用: 指定 ID の案内標識のシルエットを描画する
	void drawGuideSignSilhouette(int signId, const RoadNetwork& network, const World& world,
	                              const ColorF& color);

	/// @brief Visible signal mesh depth for resolving an overlapping building pick.
	[[nodiscard]] Optional<double> signalHitDistance(int nodeId, const RoadNetwork& network,
		const World& world, const Ray& ray, Vec3 eye);
	/// @brief Visible guide sign mesh depth, using the rendering transforms and geometry.
	[[nodiscard]] Optional<double> guideSignHitDistance(int signId, const RoadNetwork& network,
		const World& world, const Ray& ray) const;

	void invalidateEdgeCache(int edgeId, int nodeA = -1, int nodeB = -1);
	void invalidateAllCaches();
	void invalidateRouteSigns(int routeId) { m_routeSignCache.erase(routeId); }
	void invalidateCachesAroundNode(int nodeId, const RoadNetwork& network);

	/// @brief Planned / UnderConstruction エッジをワイヤーフレームで描画する
	void drawEdgeWireframe(const RoadEdge& edge, const RoadNetwork& network,
	                       const World& world, ColorF color);

	/// @brief 交差点ノードのワイヤーフレーム（B パス）を描画する
	void drawNodeCapWireframe(const RoadNetwork& network, int nodeId, const World& world);

	/// @brief Planned / UnderConstruction エッジを含む全交差点のワイヤーフレームを描画する
	void renderWireframes(const RoadNetwork& network, const World& world,
	                      const ViewFrustum& frustum, Vec3 cameraPos);

	struct LaneLineBatch { ColorF color; Mesh mesh; bool cable = false; };

	/// @brief 市町村名の参照元を変更し、境界標識の位置を再生成する。
	void setMunicipalityLookup(std::function<String(Vec2)> lookup)
	{
		m_municipalityLookup = std::move(lookup);
		m_signCache.clear();
	}

private:
	/// @brief ポール＋看板 1基分の描画情報
	/// @details 種別ごとに OBJ メッシュ + テクスチャで描画。テクスチャは type + auxNumber から
	///   描画時に引き当てる（TextureAsset / m_signTextures）。
	struct SignDraw
	{
		Mat4x4       poleMat;                 ///< ポールに適用（Scale(1,h,1) * Translate(groundPos)）
		Mat4x4       boardMat;                ///< 看板に適用（RotateY(yaw) * Translate(boardCenter)）
		Vec3         poleTop;                 ///< ポール頂上のワールド座標（デバッグ目印用）
		RoadSignType type      = RoadSignType::None;
		int          auxNumber = 0;
		String       label;
	};

	/// @brief 案内標識 1基分の描画情報
	struct GuideSignDraw
	{
		Mat4x4 poleMat;
		Mat4x4 boardMat;
		Vec3   poleTop;
		Mesh   boardMesh,backMesh;    ///< 板寸法ごとに生成される平面メッシュ
		uint64 texKey = 0;   ///< m_guideSignTexCache のキー
	};

	CacheBuildStats m_cacheBuildStats;
	double m_cacheBuildBudgetMs = Math::Inf;
	struct VisibleRoad
	{
		int id;
		float distanceSquared;
		bool node, close;
		bool prepare = false;
	};
	Array<VisibleRoad> m_visibleRoads;
	HashTable<int, Array<PartMeshEntry>> m_fallbackEdges;
	HashSet<int> m_drawnFallbackEdges;
	HashTable<int, PartMeshEntry> m_fallbackNodes;
	void prepareFallbackEdge(const RoadEdge& edge,const RoadNetwork& network,const World& world);
	void prepareFallbackNode(int nodeId,const RoadNetwork& network,const World& world);
	HashTable<int64,uint64> m_preloadedHeights;
	[[nodiscard]] bool canBuildCache();
	void drawFallbackEdge(const RoadEdge& edge, const RoadNetwork& network, const World& world);
	void drawFallbackNode(int nodeId, const RoadNetwork& network, const World& world);
	struct EdgeMargins { float atNodeA = 0.0f; float atNodeB = 0.0f; };
	struct EdgeBounds { Float3 center; float radiusSq; Array<Vec3> samples; };
	[[nodiscard]] EdgeBounds edgeBounds(const RoadNetwork& network, int edgeId) const;

	// ---- 描画 ----

	void drawEdge(const RoadEdge& edge, const RoadNetwork& network,
	              float marginA, float marginB, const World& world, bool isClose, bool prepareDetail = false);
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

	/// @brief テーパー対応ストリップメッシュ生成（A/B 端で左右オフセットが異なる場合）
	/// @param offsetA_L  A 端（s=0）の左端 [m]
	/// @param offsetA_R  A 端（s=0）の右端 [m]
	/// @param offsetB_L  B 端（s=totalLength）の左端 [m]
	/// @param offsetB_R  B 端（s=totalLength）の右端 [m]
	MeshData buildStripMeshTapered(const CubicBezier& bezier, const World& world,
	                               float offsetA_L, float offsetA_R,
	                               float offsetB_L, float offsetB_R,
	                               float heightOffset,
	                               float sStart, float sEnd, float lodFactor,
	                               float sideSkirtDrop = 0.0f,
	                               bool useElevation = false) const;

	/// @brief 全部品のメッシュ配列を生成する
	Array<PartMeshEntry> buildPartMeshes(const RoadNetwork& network, const RoadEdge& edge, const CubicBezier& bezier,
	                                     const World& world,
	                                     float marginA, float marginB);

	/// @brief 生活感を出す道路沿い設備（車・電柱・側溝・小看板）メッシュを生成する
	Array<LaneLineBatch> buildStreetFurnitureBatches(const RoadEdge& edge, const CubicBezier& bezier,
	                                                const World& world,
	                                                float marginA, float marginB) const;

	Array<LaneLineBatch> buildLaneLineBatches(const RoadEdge& edge, const CubicBezier& bezier,
	                                          const World& world,
	                                          float marginA, float marginB) const;

	/// @brief エッジ単位の道路標示を canonical 配置へ正規化する
	Array<RoadMarkingPlacement> collectEdgeRoadMarkings(const RoadNetwork& network, const RoadEdge& edge) const;

	/// @brief ノード単位の道路標示を canonical 配置へ正規化する
	Array<RoadMarkingPlacement> collectNodeRoadMarkings(const RoadNetwork& network, int nodeId, bool isClose) const;

	/// @brief canonical 配置配列からエッジ道路標示を描画バッチへ変換する
	Array<LaneLineBatch> buildEdgeRoadMarkingsFromPlacements(const RoadNetwork& network, const RoadEdge& edge, const CubicBezier& bezier,
	                                                         const World& world, float marginA, float marginB,
	                                                         const Array<RoadMarkingPlacement>& placements) const;

	/// @brief canonical 配置配列からノード道路標示を描画バッチへ変換する
	Array<LaneLineBatch> buildNodeRoadMarkingsFromPlacements(const RoadNetwork& network, int nodeId, const World& world,
	                                                         const Array<RoadMarkingPlacement>& placements) const;

	/// @brief エッジ単位の道路標示を統合生成する
	Array<LaneLineBatch> buildEdgeRoadMarkingBatches(const RoadNetwork& network, const RoadEdge& edge, const CubicBezier& bezier,
	                                                    const World& world, float marginA, float marginB) const;

	/// @brief ノード単位の道路標示を統合生成する
	Array<LaneLineBatch> buildNodeRoadMarkingBatches(const RoadNetwork& network, int nodeId, const World& world, bool isClose) const;

	/// @brief ノードキャップの輪郭線分を生成する（全エッジ対象、ワイヤーフレーム B パス用）
	/// @return 線分の始点・終点ペア配列（Y は cap 面より 0.015 上げて Z ファイト回避）
	Array<std::pair<Vec3, Vec3>> buildNodeCapWireLines(const RoadNetwork& network, int nodeId,
	                                                   const World& world) const;

	/// @brief ノードキャップの全部品メッシュ配列を生成する
	/// @param onlyOpenEdges true なら Open/Existing のエッジのみを使う（A パス用）
	Array<PartMeshEntry> buildNodeCapParts(const RoadNetwork& network, int nodeId,
	                                       const World& world, int div,
	                                       bool onlyOpenEdges = true);

	/// @brief ノードキャップ上の車線区画線を生成する
	Array<LaneLineBatch> buildNodeCapLaneLines(const RoadNetwork& network, int nodeId, const World& world) const;

	/// @brief ノード境界の停止線メッシュ配列を生成する（Stop / Signal 制御の Entry 側のみ）
	Array<LaneLineBatch> buildStopLineBatches(const RoadNetwork& network, int nodeId, const World& world) const;

	/// @brief Joint (Blend) ノードの車線区画線を生成する
	Array<LaneLineBatch> buildJointBlendLaneLines(const RoadNetwork& network, int nodeId,
	                                              const RoadNode& node, const World& world) const;

	/// @brief ノードに進入する各車線の路面標示矢印メッシュを生成する
	/// @details plan/07_road_lane_spec.md §10 参照。
	///   配置: ノード境界（cutoff）から進行方向と逆向きに kArrowOffset_m
	///   種別: RoadArrow::InferType による LaneConnection 自動推論
	Array<LaneLineBatch> buildLaneArrowMeshes(const RoadNetwork& network, int nodeId,
	                                          const World& world) const;

	/// @brief 1エッジ分の道路標識変換情報を生成する
	/// @details plan/07_road_lane_spec.md §11 参照。RoadEdge.signs の各エントリに
	///   ポール・看板それぞれの Mat4x4 変換を生成する。メッシュはキャッシュ済み OBJ を利用。
	Array<SignDraw> buildEdgeSignMeshes(const RoadNetwork& network, int edgeId,
	                                    const World& world) const;

	/// @brief 1路線分の路線番号・道路名標識の描画情報を生成する
	/// @details 路線のアンカーを両方向に配置し、テクスチャは実際の描画時に要求する。
	Array<SignDraw> buildRouteSignDraws(const RoadRoute& route, const RoadNetwork& network,
	                                    const World& world) const;

	/// @brief ポール+看板の Mat4x4 変換を計算する共通ヘルパー
	/// @details ポール OBJ は実寸で設計されている前提（スケールしない）。
	///   看板位置は (offsetX, offsetY, offsetZ) のポール基底からの相対 3D オフセット。
	/// @param arcLen        Bezier 上の弧長位置
	/// @param lateralOffset 道路中心からの横方向オフセット [m]（A→B 右向きが正）
	/// @param boardFacesTan true なら看板正面が +tangent 方向、false なら -tangent 方向
	/// @param boardOffsetX/Y/Z  ポール基底からの看板中心オフセット [m]（local X=横, Y=上, Z=長手方向）
	static bool computeSignTransforms(const CubicBezier& bezier, const World& world,
	                                  float arcLen, float lateralOffset,
	                                  bool boardFacesTan,
	                                  float boardOffsetX, float boardOffsetY, float boardOffsetZ,
	                                  const RoadEdge& edge,
	                                  Mat4x4& outPole, Mat4x4& outBoard,
	                                  Vec3& outPoleTop);

	/// @brief 看板メッシュを取得（未ロードなら遅延生成）
	const Mesh* getSignBoardMesh(RoadSignType type);

	/// @brief SignDraw 配列を統一的に描画する（ポール + 看板）
	void drawSigns(const Array<SignDraw>& draws);

	/// @brief 通常描画と選択表示で共用する案内標識1基の位置・形状。
	Optional<GuideSignDraw> buildGuideSignDraw(const GuideSignPlacement& sign,
		const RoadNetwork& network, const World& world) const;
	bool ensureGuidePoleMesh();

	/// @brief 案内標識の描画情報を 1 エッジ分構築する
	Array<GuideSignDraw> buildEdgeGuideSignDraws(const RoadNetwork& network, int edgeId,
	                                             const World& world) const;

	/// @brief 案内標識を描画する（ポール + 板）
	void drawGuideSigns(const Array<GuideSignDraw>& draws);

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

	// ---- 信号アタッチメントジオメトリキャッシュ ----

	/// @brief 信号アタッチメントの静的ジオメトリ（変換行列）キャッシュ
	struct SignalAttachGeomCache
	{
		Mat4x4 baseMat = Mat4x4::Identity();
		bool   valid   = false;  ///< false なら描画スキップ（edge/bez が無効）
	};
	/// @brief ノード ID → アタッチメント順の変換行列キャッシュ（道路変更時に無効化）
	HashTable<int, Array<SignalAttachGeomCache>> m_signalAttachGeomCache;

	/// @brief 1 交差点の信号アタッチメント変換行列キャッシュを構築する
	/// @details attachment 数と一致していれば何もしない。道路変更で無効化されると再構築される。
	void ensureSignalAttachGeomCache(const RoadNode& node, const RoadNetwork& network,
	                                 const World& world, bool elevated,
	                                 Array<SignalAttachGeomCache>& cacheArr) const;

	// ---- 信号描画ヘルパー ----

	/// @brief 1交差点分の進入エッジ別・旋回別信号状態サマリー
	struct EdgeSignalSummary
	{
		bool hasStraight = false, straightGreen = false;
		bool hasLeft     = false, leftGreen     = false;
		bool hasRight    = false, rightGreen    = false;
	};

	/// @brief 交差点の LaneConnection を進入エッジ別 × 旋回別に集計する
	/// @param node          対象ノード
	/// @param simGraph      旋回分類用 SimGraph
	/// @param tl            交通信号（nullptr なら全方向青扱い）
	/// @return edgeId → EdgeSignalSummary のテーブル
	static HashTable<int, EdgeSignalSummary> buildEdgeSignalSummaries(
		const RoadNode& node,
		const SimGraph& simGraph,
		const TrafficLight* tl);

	/// @brief 交差点ごとの EdgeSignalSummary キャッシュ（フェーズ変化時のみ再構築）
	struct SignalSummaryCache
	{
		HashTable<int, EdgeSignalSummary> summaries;
		int lastPhaseIdx = -2;  ///< -2 = 未初期化（強制再構築）
	};

	/// @brief 信号メッシュキャッシュ（メッシュ名 → Mesh）
	struct SignalMeshCache
	{
		HashTable<String, Mesh> meshes;
	};

	/// @brief ノード ID → 信号サマリーキャッシュ
	HashTable<int, SignalSummaryCache> m_signalSummaryCache;

	/// @brief 信号定義 ID → メッシュキャッシュ
	HashTable<String, SignalMeshCache> m_signalMeshCache;

	/// @brief 信号定義のメッシュを取得（キャッシュ付き）
	const Mesh* getSignalMesh(const String& defId, const String& meshName, int lod = 0);

	// ---- Construction meshes are rebuilt only at 5% stage boundaries. ----
	struct ConstructionCache
	{
		int key = -1;
		Array<PartMeshEntry> surfaces;
		Array<LaneLineBatch> details;
		Array<std::pair<int,Mat4x4>> machines;
	};
	GameTime m_constructionNow = 0;
	HashTable<int, ConstructionCache> m_constructionCache;
	PixelShader m_constructionEarthPS, m_constructionAggregatePS;
	PixelShader m_asphaltPS, m_pavementPS;
	Texture m_asphaltNormal;
	void drawSurface(const PartMeshEntry& entry, const Mesh& mesh) const;
	Texture m_constructionSoilNormal, m_constructionGravelNormal;
	Texture m_constructionSoil, m_constructionGravel, m_constructionConcrete;
	std::array<Model,4> m_constructionModels;
	std::array<Model,4> m_constructionLodModels;
	void drawConstruction(const RoadEdge& edge, const RoadNetwork& network, const World& world, bool close);

	// ---- メンバ ----

	ArrowMarkingRegistry                      m_arrowMarkingRegistry;
	SignalRegistry                            m_signalRegistry;
	RoadPartRegistry                          m_partRegistry;
	HashTable<int, Array<PartMeshEntry>>      m_partMeshCache;      ///< エッジ ID → 部品メッシュ配列（詳細 + LOD）
	HashTable<int, Array<PartLodBatch>>       m_partLodBatchCache;  ///< エッジ ID → 遠距離用 combined LOD バッチ
	HashTable<int64, Array<LaneLineBatch>>    m_markingCacheByNode; ///< (node ID, LOD) → 統合道路標示
	HashTable<int, Array<LaneLineBatch>>      m_markingCacheByEdge; ///< エッジ ID → 統合道路標示
	HashTable<int, Array<LaneLineBatch>>      m_streetFurnitureCache;
	uint64 m_geometryRevision = 0;
	HashSet<int64> m_dirtyTerrainObserved;
	VertexShader m_cableVS;
	PixelShader m_cablePS;
	struct CableView { Float4 viewport{ 1, 1, 1, 1 }; };
	ConstantBuffer<CableView> m_cableView;
	HashTable<int, EdgeMargins>               m_marginCache;
	HashTable<int, Array<PartMeshEntry>>              m_nodeCapCache;        ///< ノード ID → 部品メッシュ配列（Open/Existing エッジのみ）
	HashTable<int, Array<std::pair<Vec3, Vec3>>>      m_nodeCapWireCache;    ///< ノード ID → ワイヤーフレーム輪郭線分配列（B パス用）
	HashTable<int, EdgeBounds>                m_boundsCache;
	HashTable<int, Array<Mesh>>              m_pierMeshCache;   ///< エッジ ID → 橋脚メッシュ配列
	HashTable<int, Array<SignDraw>>           m_signCache;          ///< エッジ ID → 道路標識変換情報
	HashSet<int>                              m_visibleEdges;     ///< 直近 render() の可視エッジ集合
	std::function<String(Vec2)> m_municipalityLookup;
	HashTable<String, RenderTexture> m_signTextures;
	struct SignTextureContent
	{
		RoadSignType type;
		int auxNumber;
		String label;
	};
	HashTable<String, SignTextureContent> m_pendingSignTextures;
	HashTable<int, Array<SignDraw>>           m_routeSignCache;    ///< route ID → 路線番号・道路名標識描画情報
	HashTable<uint64, RenderTexture>          m_guideSignTexCache;    ///< パネル内容ハッシュ → 合成テクスチャ（ランタイムのみ、セッション毎に再生成）
	bool                                      m_guideSignTexAllReady = false; ///< 全案内標識テクスチャ準備済みフラグ（true なら毎フレームのループをスキップ）
	HashTable<int, Array<GuideSignDraw>>      m_guideSignCache;    ///< エッジ ID → 案内標識描画情報
	std::array<Optional<Mesh>, 2> m_signPoleLods, m_guidePoleLods;
	Optional<Mesh>                            m_signPoleMesh;       ///< RoadSign 共通ポール（OBJ ロード、実寸）
	Optional<Mesh>                            m_guidePoleMesh;     ///< 案内標識用ポール（2 本柱フレーム、OBJ ロード）
	HashTable<String, Mesh>                   m_signBoardBacks;
	HashTable<String, Mesh>                   m_signBoardMeshes;   ///< 看板メッシュ（形状 OBJ パス別・遅延生成、カテゴリ内の同形状は共有）
};

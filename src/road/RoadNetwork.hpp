#pragma once
#include "RoadTypes.hpp"
#include "BezierUtil.hpp"

class World;

/// @brief 道路グラフ管理クラス
/// @details エッジ・ノードの追加/削除・交差検出を担う
class RoadNetwork
{
public:
	/// @brief ノードを追加し、割り当てた id を返す
	int addNode(Vec3 pos, NodeType type = NodeType::Endpoint);

	/// @brief エッジを追加し、割り当てた id を返す
	/// @param nodeA    始点ノード id
	/// @param nodeB    終点ノード id
	/// @param ctrlA    ベジェ制御点A（始点側）
	/// @param ctrlB    ベジェ制御点B（終点側）
	/// @param numLanes 総車線数（双方向合計）
	Optional<int> addEdge(int nodeA, int nodeB,
	                      Vec3 ctrlA, Vec3 ctrlB,
	                      RoadType rt   = RoadType::LocalRoad,
	                      int numLanes  = 2);

	/// @brief エッジを削除する（id を -1 にマーク）
	void removeEdge(int edgeId);

	/// @brief ノードを削除する（id を -1 にマーク）
	void removeNode(int nodeId);

	/// @brief 既存 ID を保持したままノードを追加する（チャンクリロード用）
	/// @details 同 id が既に存在する場合は何もしない。m_nextNodeId を自動拡張する
	void addNodeRaw(const RoadNode& node);

	/// @brief 既存 ID を保持したままエッジを追加する（チャンクリロード用）
	/// @details 同 id が既に存在する場合は何もしない。m_nextEdgeId を自動拡張する
	void addEdgeRaw(const RoadEdge& edge);

	/// @brief 次割当 ID を直接設定する（セーブロード時の復元用）
	void setNextIds(int nextNodeId, int nextEdgeId)
	{
		if (nextNodeId > m_nextNodeId) m_nextNodeId = nextNodeId;
		if (nextEdgeId > m_nextEdgeId) m_nextEdgeId = nextEdgeId;
	}

	int nextNodeId() const { return m_nextNodeId; }
	int nextEdgeId() const { return m_nextEdgeId; }

	/// @brief id でエッジを取得する（存在しなければ nullptr）
	RoadEdge*       getEdge(int id);
	const RoadEdge* getEdge(int id) const;

	/// @brief id でノードを取得する（存在しなければ nullptr）
	RoadNode*       getNode(int id);
	const RoadNode* getNode(int id) const;

	const Array<RoadEdge>& edges() const { return m_edges; }
	const Array<RoadNode>& nodes() const { return m_nodes; }

	/// @brief 指定位置に近いノードを探す
	/// @param radius 探索半径 [m]
	Optional<int> findNodeNear(Vec3 pos, float radius = 10.0f) const;

	/// @brief 指定位置に最も近いエッジと弧長位置を探す（XZ 平面距離）
	/// @return {edgeId, arcLength} のペア。なければ none
	Optional<std::pair<int, float>> findEdgeNearDetailed(Vec3 pos, float maxDist = 15.0f) const;

	/// @brief 指定位置に最も近いエッジを探す（findEdgeNearDetailed のラッパー）
	Optional<int> findEdgeNear(Vec3 pos, float maxDist = 15.0f) const
	{
		auto r = findEdgeNearDetailed(pos, maxDist);
		return r ? Optional<int>{r->first} : none;
	}

	/// @brief テンプレートの属性（speedLimit/parts/lanes）をエッジにコピーする
	void applyEdgeTemplate(int edgeId, const RoadEdge& tmpl);

	/// @brief 2本のエッジを持つノードを溶解し、1本のエッジに統合する
	/// @return 統合後のエッジ ID。失敗時 none
	Optional<int> dissolveNode(int nodeId);

	/// @brief エッジを弧長位置で2分割し、分割点に新ノードを作成する
	/// @param edgeId  分割対象エッジ
	/// @param arcLength 分割する弧長位置 [m]
	/// @return 新ノードの ID。失敗時 -1
	int splitEdgeAt(int edgeId, float arcLength);

	/// @brief 既存エッジとの交差を処理しながらエッジを追加する
	/// @return 追加されたエッジの id（上限超過時は none）
	Optional<int> addEdgeWithIntersection(int nodeA, int nodeB,
	                                      Vec3 ctrlA, Vec3 ctrlB,
	                                      RoadType rt, int numLanes);

	/// @brief ベジェ曲線を取得する（エッジ id が有効なら）
	Optional<CubicBezier> getBezier(int edgeId) const;

	/// @brief エッジに TempOp を追加する（自動的に変更フラグを立てる）
	void addTempOp(int edgeId, TempOp op);

	/// @brief 期限切れの TempOp を全エッジから削除する
	/// @return 変化があった場合 true
	bool clearExpiredTempOps(GameTime now);

	/// @brief 交差点（接続数 3 以上）のベジェ制御点を整列する
	/// @details 各交差点で、隣接エッジ（他端点を結んだ直線のなす角で判定）の
	///   なす角が 12.5° 未満のペアに対して、交差点側の制御点を調整する。
	///   調整後の制御点方向は隣接エッジ方向から 12.5° 離れた方向とし、
	///   制御点長は交差点〜他端点の XZ 距離の 1/2 とする。
	bool spreadIntersectionTangents();

	// ── RoadObject ──

	/// @brief オブジェクトを追加し、割り当てた id を返す
	int addObject(RoadObject obj);

	/// @brief オブジェクトを削除する
	void removeObject(int objectId);

	/// @brief 指定エッジに属する全オブジェクトを削除する
	void removeObjectsByEdge(int edgeId);

	/// @brief id でオブジェクトを取得する
	RoadObject*       getObject(int id);
	const RoadObject* getObject(int id) const;

	const Array<RoadObject>& objects() const { return m_objects; }

	/// @brief ノードの接続エッジのいずれかが高架かどうかを返す
	[[nodiscard]] bool isNodeElevated(int nodeId) const;

	/// @brief エッジの両端ノード高さから useElevation を自動判定・更新する
	void updateEdgeElevation(int edgeId, const World& world);

	/// @brief 高架エッジに橋脚を自動配置する
	void generatePiersForEdge(int edgeId, const World& world);

	// ── ポスト処理 ──

	/// @brief 鋭角交差ポスト処理（ネットワーク作成直後に呼ぶ）
	/// @details 同一ノードで minAngleDeg 未満の角を成すエッジペアを検出し、
	///   幅が狭い方のエッジを、そのノードの隣接ノードのうち最も近いものに付け替える。
	/// @param minAngleDeg 許容する最小交差角 [deg]（デフォルト 12.5°）
	bool fixSharpAngles(float minAngleDeg = 12.5f);

	/// @brief 同一ノードペアを持つ重複エッジを削除する
	/// @details 両端が同じノードペアのエッジが複数ある場合、道幅が小さい方を削除する。
	///   同幅のときは seed を用いた決定的ハッシュで一方を選択する。
	/// @param seed 乱数シード（ゲームシードをそのまま渡す）
	bool removeDuplicateEdges(uint64 seed);

	/// @brief Node なし交差解消（fixSharpAngles の直後に呼ぶ）
	/// @details エッジ同士がノードを共有せずに交差している箇所を検出し、
	///   交差点に Intersection ノードを生成して両エッジを分割する。
	///   新規ノードが既存ノードと 40 m 以内なら既存ノードへマージする。
	/// @param sinceEdgeId  この ID 以上のエッジのみを判定対象にする（0 なら全エッジ）。
	///   前回ポスト処理以降に追加されたエッジだけをチェックする用途で使う。
	bool resolveIntersections(int sinceEdgeId = 0);

	/// @brief 接続数 2 の全ノードで曲線を滑らかにする（Phase 3.5 ポスト処理用）
	void smoothAllCurves();

	/// @brief 短すぎるエッジを結合する
	/// @param minLength この長さ以下のエッジを削除し、両端ノードを結合する [m]
	/// @return 結合したエッジ数
	int mergeShortEdges(float minLength = 30.0f);

	/// @brief 指定ノードに接続する全エッジの cutoffA/cutoffB を再計算する
	void updateNodeCutoffs(int nodeId);

	/// @brief 指定ノードの LaneConnection を再構築する
	void rebuildLaneConnections(int nodeId);

	/// @brief 既存�� LaneConnection のベジェパスだ���を再計算する（接続構造は変えない）
	void updateLaneConnectionPaths(int nodeId);

	/// @brief 直進ペ���ベースのデフォルト信号フェーズを生成���る
	Array<SignalPhaseDef> buildDefaultSignalPhases(int nodeId) const;

	/// @brief 2ノードのカットオフと LaneConnection をまとめて再計算する
	void rebuildNodeConnectivity(int nodeA, int nodeB)
	{
		updateNodeCutoffs(nodeA);
		updateNodeCutoffs(nodeB);
		rebuildLaneConnections(nodeA);
		rebuildLaneConnections(nodeB);
	}

	/// @brief 指定エッジの自動生成 RoadSign を再構築する
	/// @details
	///   既存 signs のうち autoGenerated=true のものを削除し、
	///   `RoadSign::InferAutoForEdge()` で生成し直す。
	///   手動配置（autoGenerated=false）のエントリは保持される。
	void recomputeAutoSignsForEdge(int edgeId);

	/// @brief 全エッジの自動生成 RoadSign を再構築する（ロード時用）
	void recomputeAllAutoSigns();

	// ===== RoadRoute API =====
	// 詳細は plan/22_road_route_spec.md 参照

	/// @brief 道路路線を追加する
	/// @param kind   路線種別
	/// @param name   正式名称（空文字なら自動命名）
	/// @param edgeIds 構成エッジ（起点→終点の順）
	/// @param number 番号（0 ならデフォルト値。kind が番号系なら auto 採番）
	/// @return 割当路線 ID
	int addRoute(RoadRouteKind kind, String name, Array<int> edgeIds, int number = 0);

	/// @brief 路線を削除する（id を -1 にマーク、edge 側の逆引きからも除去）
	void removeRoute(int routeId);

	/// @brief 路線を取得（存在しなければ nullptr）
	RoadRoute*       getRoute(int id);
	const RoadRoute* getRoute(int id) const;

	/// @brief 全路線リスト（tombstone 含む）
	const Array<RoadRoute>& routes() const { return m_routes; }

	/// @brief kind に応じたデフォルト名を生成する（空 route から呼び出す）
	/// @details 番号系 kind は kind 内でユニークな 1〜400 の番号を割当
	///   Expressway/Named は固定文字列（v1）
	String generateAutoRouteName(RoadRouteKind kind, int* outNumber = nullptr) const;

	/// @brief kind に応じたデフォルト表示色を返す
	static ColorF defaultRouteColor(RoadRouteKind kind);

	/// @brief エッジ側の逆引き routeIds を再構築する（ロード時）
	void rebuildEdgeRouteIndex();

	/// @brief デフォルトの車線セットを生成する
	static Array<Lane> buildDefaultLanes(int numLanes, RoadType rt);

	/// @brief RoadType に応じたデフォルトの部品配列を生成・設定する
	static void buildDefaultParts(RoadEdge& edge);

	/// @brief 2 接続ノードで曲線が滑らかに繋がるよう制御点を補正する
	/// @param newEdgeId 新たに追加したエッジの id
	/// @param midNodeId 補正対象のノード id
	void smoothCurveAt(int newEdgeId, int midNodeId);

private:
	Array<RoadEdge> m_edges;
	Array<RoadNode> m_nodes;
	HashTable<int, int> m_edgeIdToIdx;   ///< edge ID → m_edges インデックス (O(1) ルックアップ)
	HashTable<int, int> m_nodeIdToIdx;   ///< node ID → m_nodes インデックス (O(1) ルックアップ)
	Array<int>          m_freeEdgeSlots; ///< m_edges 内の再利用可能スロット
	Array<int>          m_freeNodeSlots; ///< m_nodes 内の再利用可能スロット
	int m_nextEdgeId = 0;
	int m_nextNodeId = 0;

	// ── RoadObject ──
	Array<RoadObject>   m_objects;
	HashTable<int, int> m_objectIdToIdx;
	Array<int>          m_freeObjectSlots;
	int                 m_nextObjectId = 0;

	// ── RoadRoute ──
	Array<RoadRoute>    m_routes;
	HashTable<int, int> m_routeIdToIdx;
	Array<int>          m_freeRouteSlots;
	int                 m_nextRouteId = 0;

	int routeIndex(int id) const
	{
		const auto it = m_routeIdToIdx.find(id);
		return (it != m_routeIdToIdx.end()) ? it->second : -1;
	}

	/// @brief removeEdge の整合性フック: 該当 edge を含む routes から除去 or 分割する
	void onEdgeRemovedFromRoutes(int edgeId);

	int objectIndex(int id) const
	{
		const auto it = m_objectIdToIdx.find(id);
		return (it != m_objectIdToIdx.end()) ? it->second : -1;
	}

	/// @brief エッジ id からインデックスを返す（-1 なら存在しない）
	int edgeIndex(int id) const;
	int nodeIndex(int id) const;

};

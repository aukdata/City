#pragma once
#include "RoadTypes.hpp"
#include "BezierUtil.hpp"

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
	/// @details
	///   カットオフ量 = そのノードにつながる最も幅広の道路の幅 × 1.5
	///   接続エッジが 1 本以下（端点）なら 0 を設定する。
	///   addEdge / removeEdge 後に自動で呼ばれる。
	void updateNodeCutoffs(int nodeId);

private:
	Array<RoadEdge> m_edges;
	Array<RoadNode> m_nodes;
	HashTable<int, int> m_edgeIdToIdx;   ///< edge ID → m_edges インデックス (O(1) ルックアップ)
	HashTable<int, int> m_nodeIdToIdx;   ///< node ID → m_nodes インデックス (O(1) ルックアップ)
	Array<int>          m_freeEdgeSlots; ///< m_edges 内の再利用可能スロット
	Array<int>          m_freeNodeSlots; ///< m_nodes 内の再利用可能スロット
	int m_nextEdgeId = 0;
	int m_nextNodeId = 0;

	/// @brief エッジ id からインデックスを返す（-1 なら存在しない）
	int edgeIndex(int id) const;
	int nodeIndex(int id) const;

	/// @brief デフォルトの車線セットを生成する
	static Array<Lane> buildDefaultLanes(int numLanes, RoadType rt);

	/// @brief 2 接続ノードで曲線が滑らかに繋がるよう制御点を補正する
	/// @param newEdgeId 新たに追加したエッジの id
	/// @param midNodeId 補正対象のノード id
	/// @details
	///   条件: midNodeId の接続数が 2 かつ PrevRoad と NewRoad のなす角が 90° 以上。
	///   動作: NewRoad の midNode 側制御点 (CPN) を、
	///         PrevRoad の midNode 側制御点 (CPP) と midNode を結ぶ直線上で
	///         midNode から NewRoad 両端間の直線距離の 1/2 の位置に移動する。
	void smoothCurveAt(int newEdgeId, int midNodeId);
};

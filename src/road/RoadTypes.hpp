#pragma once
#include "../time/GameClock.hpp"
#include "RoadEnums.hpp"
#include "RoadPartTypes.hpp"

// ===== 列挙型 =====

/// @brief 運用状態: 現在の交通への供用状態（頻繁に変化）
enum class OpState : uint8
{
	Open,        ///< 供用中
	Provisional, ///< 暫定供用
	Closed,      ///< 閉鎖
	Reserved,    ///< 将来供用のため確保
};

/// @brief 向き
enum class LaneDir : uint8
{
	Forward,   ///< A → B
	Backward,  ///< B → A
};

/// @brief 機能種別
enum class LaneType : uint8
{
	Normal,
	Overtaking,
	Acceleration,
	Deceleration,
	TurnLeft,
	TurnRight,
	Bus,
	ParkingBay,
	EmergencyStop,
};

/// @brief 区画線種別
enum class LineType : uint8
{
	None,           ///< 線なし
	SolidWhite,     ///< 白実線（車線変更禁止）
	DashedWhite,    ///< 白破線（車線変更可）
	SolidYellow,    ///< 黄実線（追い越し禁止）
	DoubleYellow,   ///< 黄二重線
};

/// @brief ノード種別
enum class NodeType : uint8
{
	Endpoint,      ///< 端点（1本接続）
	Joint,         ///< 継ぎ目（2本接続）
	Intersection,  ///< 交差点（3本以上、isThrough なし）
	Diverge,       ///< 分岐合流（3本以上、isThrough 2本）
};

/// @brief ノードでの遷移方式（Joint 時のみ有効）
/// @details 17_road_node_spec.md 参照
enum class NodeTransition : uint8
{
	Blend,    ///< 部品を滑らかにモーフィング（車線減少・幅変化）
	Abrupt,   ///< ノード中心で不連続に切替（延伸端・道路種別境界）
};

/// @brief エッジのノードへの接続情報
/// @details 17_road_node_spec.md 参照
struct EdgeAttachment
{
	int   edgeId = -1;
	float lateralOffset = 0.0f;  ///< ノード中心からの横方向オフセット [m]（エッジ外向き接線に対して右が正）
	bool  isThrough = false;     ///< Diverge ノード専用: 本線エッジなら true
};

/// @brief 道路種別
enum class RoadType : uint8
{
	LocalRoad,    ///< 一般道（市道・町道）
	Arterial,     ///< 幹線道路（国道・県道）
	Expressway,   ///< 高速道路（自動車専用道）
	Highway,      ///< 有料道路
};

/// @brief エッジ状態
enum class EdgeState : uint8
{
	Existing,
	Planned,
	UnderConstruction,
	Open,
	Closed,
};

/// @brief 道路計画の状態
enum class PlanState : uint8
{
	Planning,
	Approved,
	UnderConstruction,
	Complete,
};

// ===== Lane =====

/// @brief 車線データ
struct Lane
{
	// --- 幾何（A端・B端で異なる位置 → テーパー車線対応） ---
	float   offsetA_L = 0.0f;              ///< A端: 道路中心からの左端 [m]
	float   offsetA_R = 0.0f;              ///< A端: 道路中心からの右端 [m]
	float   offsetB_L = 0.0f;              ///< B端: 道路中心からの左端 [m]
	float   offsetB_R = 0.0f;              ///< B端: 道路中心からの右端 [m]

	// --- 運用 ---
	LaneDir   dir = LaneDir::Forward;      ///< 走行方向
	OpState   op  = OpState::Open;         ///< 供用状態

	// --- 車線変更 ---
	bool      canChangeLaneLeft  = false;  ///< 左隣の車線への変更が可能か
	bool      canChangeLaneRight = false;  ///< 右隣の車線への変更が可能か

	// --- 区画線 ---
	LineType  lineLeft  = LineType::None;  ///< 左側の区画線種別
	LineType  lineRight = LineType::None;  ///< 右側の区画線種別

	// --- ゲームプレイ ---
	float     nominalWidth = 3.5f;         ///< 公称幅 [m]（容量計算・UI表示用）
	LaneType  type = LaneType::Normal;     ///< 機能種別
};


// ===== TempOp =====

/// @brief TempOp による個別車線の運用上書き
struct LaneOpOverride
{
	int     laneIndex;
	LaneDir newDir;
	OpState newOp;
};

/// @brief 一時的な運用変更の種別
enum class TempOpKind : uint8
{
	Construction,   ///< 工事（CrossingClose > Construction > Event の優先度）
	CrossingClose,  ///< 踏切閉鎖
	Event,          ///< 祭り・交通規制など
};

/// @brief 一時的な運用変更
struct TempOp
{
	TempOpKind            kind;
	Array<LaneOpOverride> overrides;
	GameTime              start = 0.0;
	GameTime              end   = 0.0;
	String                reason;
};

// ===== PlannedChange =====

/// @brief 将来の計画的変化における部品の変更仕様
struct PartChange
{
	int                  partIndex;
	Optional<BuildState> newBuild;
};

/// @brief 将来の計画的変化における車線の変更仕様
struct LaneChange
{
	int                  laneIndex;
	Optional<LaneDir>    newDir;
	Optional<OpState>    newOp;
};

/// @brief PlannedChange の発動トリガー
enum class Trigger : uint8
{
	OnConstruction,
	OnOpen,
};

/// @brief 将来の計画的変化
struct PlannedChange
{
	int                planId;
	Trigger            trigger;
	Array<PartChange>  partChanges;
	Array<LaneChange>  laneChanges;
};

// ===== RoadEdge =====

/// @brief 道路エッジ（ベジェ曲線の道路区間）
struct RoadEdge
{
	int       id       = -1;
	int       nodeA    = -1;
	int       nodeB    = -1;
	Vec3      ctrlA;                          ///< ベジェ制御点A
	Vec3      ctrlB;                          ///< ベジェ制御点B
	RoadType  roadType  = RoadType::LocalRoad;
	float     speedLimit = 60.0f;            ///< 制限速度 [km/h]
	float     length     = 0.0f;            ///< 弧長 [m]
	int       planId     = -1;              ///< 所属 RoadPlan（-1 = 既存道路）

	/// @brief nodeA 端でのカットオフ量 [m]（描画メッシュをノード手前で切る距離）
	/// @note RoadNetwork::updateNodeCutoffs() で自動計算される。0 = カットなし（端点）
	float     cutoffA   = 0.0f;
	/// @brief nodeB 端でのカットオフ量 [m]
	float     cutoffB   = 0.0f;

	/// @brief 物理構造（道路部品の配列、左端から右端の順）
	Array<RoadPart>      parts;

	Array<Lane>          lanes;
	Array<TempOp>        tempOps;
	Array<PlannedChange> planned;
	Array<Array<int>>    laneVehicles;      ///< 車線ごとの vehicleId リスト

	EdgeState edgeState = EdgeState::Open;
	float     congestion = 0.0f;

	// 経路探索サポート（Phase 2 で使用）
	int borderNodeA = -1;
	int borderNodeB = -1;

	/// @brief TempOp を適用した有効な車線状態を返す
	/// @param i     車線インデックス
	/// @param now   現在のゲーム時刻
	Lane effectiveLane(int i, GameTime now) const
	{
		Lane L = lanes[i];
		// 優先度順にソート: CrossingClose < Construction < Event（値が小さいほど優先）
		auto ops = tempOps;
		ops.sort_by([](const TempOp& a, const TempOp& b)
		{
			// CrossingClose=1 > Construction=0 > Event=2
			// 優先度: CrossingClose > Construction > Event
			const auto priority = [](TempOpKind k) -> int
			{
				switch (k)
				{
				case TempOpKind::CrossingClose: return 0;
				case TempOpKind::Construction:  return 1;
				case TempOpKind::Event:         return 2;
				}
				return 3;
			};
			return priority(a.kind) < priority(b.kind);
		});
		for (const auto& op : ops)
		{
			if (op.start <= now && now <= op.end)
			{
				for (const auto& ov : op.overrides)
				{
					if (ov.laneIndex == i)
					{
						L.dir = ov.newDir;
						L.op  = ov.newOp;
					}
				}
			}
		}
		return L;
	}

	/// @brief 指定方向の走行可能な車線インデックス一覧を返す
	Array<int> openLanes(LaneDir dir, GameTime now) const
	{
		Array<int> result;
		for (int i = 0; i < static_cast<int>(lanes.size()); ++i)
		{
			const Lane L = effectiveLane(i, now);
			if (isRoadbedBuilt() && (L.op == OpState::Open || L.op == OpState::Provisional) && L.dir == dir)
				result << i;
		}
		return result;
	}

	/// @brief 道路の総幅を返す [m]（parts ベース）
	float totalWidth() const
	{
		float minOff = 1e9f, maxOff = -1e9f;
		for (const auto& p : parts)
		{
			minOff = Min(minOff, p.offset);
			maxOff = Max(maxOff, p.offset + p.width);
		}
		return (minOff < maxOff) ? (maxOff - minOff) : 0.0f;
	}

	/// @brief 路盤パーツが建設済みかどうか
	[[nodiscard]]
	bool isRoadbedBuilt() const
	{
		for (const auto& part : parts)
		{
			if (part.type == RoadPartType::Roadbed && part.build == BuildState::Built)
				return true;
		}
		return false;
	}
};

/// @brief 車線が走行可能か（新シグネチャ: RoadEdge + 車線インデックス）
/// @details 路盤パーツの BuildState + 車線の OpState で判定
inline bool isPassable(const RoadEdge& edge, int laneIndex)
{
	if (laneIndex < 0 || laneIndex >= static_cast<int>(edge.lanes.size()))
		return false;
	const auto& lane = edge.lanes[laneIndex];
	return edge.isRoadbedBuilt()
		&& (lane.op == OpState::Open || lane.op == OpState::Provisional);
}

// ===== RoadNode =====

/// @brief 道路ノード（端点・継ぎ目・交差点・分岐合流）
/// @details 17_road_node_spec.md 参照
struct RoadNode
{
	int                    id = -1;
	Vec3                   position;
	NodeType               type       = NodeType::Endpoint;
	NodeTransition         transition = NodeTransition::Blend;
	Array<EdgeAttachment>  attachments;

	/// @brief 接続エッジ ID 一覧を返す（旧 edgeIds 互換）
	[[nodiscard]] Array<int> edgeIds() const
	{
		Array<int> ids;
		ids.reserve(attachments.size());
		for (const auto& a : attachments) ids << a.edgeId;
		return ids;
	}

	/// @brief エッジを追加する
	void addEdge(int edgeId)
	{
		attachments << EdgeAttachment{ edgeId };
	}

	/// @brief エッジを削除する
	void removeEdge(int edgeId)
	{
		attachments.remove_if([edgeId](const EdgeAttachment& a) { return a.edgeId == edgeId; });
	}

	/// @brief 指定エッジの接続情報を取得する（なければ nullptr）
	[[nodiscard]] EdgeAttachment* getAttachment(int edgeId)
	{
		for (auto& a : attachments)
			if (a.edgeId == edgeId) return &a;
		return nullptr;
	}
	[[nodiscard]] const EdgeAttachment* getAttachment(int edgeId) const
	{
		for (const auto& a : attachments)
			if (a.edgeId == edgeId) return &a;
		return nullptr;
	}
};

// ===== RoadPlan =====

/// @brief 道路計画
struct RoadPlan
{
	int        id = -1;
	String     name;
	String     originName;
	String     destName;
	RoadType   roadType  = RoadType::LocalRoad;
	Array<int> edgeIds;
	float      totalCost   = 0.0f;
	float      totalLength = 0.0f;         ///< 総延長 [km]
	PlanState  state = PlanState::Planning;

	Optional<GameTime> constructionStart;
	Optional<GameTime> completionDate;
};

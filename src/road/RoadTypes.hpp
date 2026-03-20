#pragma once
#include "../time/GameClock.hpp"

// ===== 列挙型 =====

/// @brief 物理状態: 路盤・構造物の建設状態（原則不可逆）
enum class BuildState : uint8
{
	NotBuilt,            ///< 路盤なし（計画のみ）
	UnderConstruction,   ///< 施工中（路盤未完成）
	Built,               ///< 路盤完成（供用可能）
	StubEnd,             ///< 延伸端・イカの耳
};

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
	StubReserved,
};

/// @brief ノード種別
enum class NodeType : uint8
{
	Intersection,  ///< 交差点
	TJunction,     ///< T字路
	Endpoint,      ///< 端点
	IC,            ///< インターチェンジ
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
	// 物理軸（原則変更されない）
	int        index = 0;                  ///< 左端=0 の物理位置（不変）
	BuildState build = BuildState::NotBuilt;
	LaneType   type  = LaneType::Normal;
	float      width = 3.5f;              ///< 車線幅 [m]

	// 運用軸（頻繁に変わりうる）
	LaneDir    dir   = LaneDir::Forward;
	OpState    op    = OpState::Open;
};

/// @brief 車線が走行可能か（build==Built && op==Open||Provisional）
inline bool isPassable(const Lane& lane)
{
	return lane.build == BuildState::Built
		&& (lane.op == OpState::Open || lane.op == OpState::Provisional);
}

// ===== TempOp =====

/// @brief TempOp による個別車線の運用上書き
struct LaneOpOverride
{
	int     index;
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

/// @brief 将来の計画的変化における個別車線の変更仕様
struct LaneChange
{
	int                  index;
	Optional<BuildState> newBuild;
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
	int               planId;
	Trigger           trigger;
	Array<LaneChange> changes;
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
					if (ov.index == i)
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
			if (isPassable(L) && L.dir == dir)
				result << i;
		}
		return result;
	}

	/// @brief 総車線幅を返す [m]
	float totalWidth() const
	{
		float w = 0.0f;
		for (const auto& lane : lanes) w += lane.width;
		return w;
	}
};

// ===== RoadNode =====

/// @brief 道路ノード（交差点・端点）
struct RoadNode
{
	int        id = -1;
	Vec3       position;
	NodeType   type = NodeType::Endpoint;
	Array<int> edgeIds;                    ///< 接続するエッジの id リスト
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

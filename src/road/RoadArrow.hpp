#pragma once
#include "RoadTypes.hpp"

class RoadNetwork;

/// @brief 路面標示矢印の種別
/// @details 詳細は plan/07_road_lane_spec.md §10 参照
enum class RoadArrowType : uint8
{
	None,             ///< 矢印なし
	Straight,         ///< 直進 (規制標示 204(2) ht2 相当)
	Left,             ///< 左折 (規制標示 204(1) ht1 相当)
	Right,            ///< 右折 (ht1 を上下反転)
	StraightLeft,     ///< 直進+左折 (規制標示 204(3) ht3 相当)
	StraightRight,    ///< 直進+右折 (ht3 を上下反転)
	LeftRight,        ///< 左折+右折 (将来)
	All,              ///< 直進+左折+右折 (将来)
	UTurn,            ///< U ターン (将来)
};

/// @brief 路面標示矢印メッシュの生成・自動推論
/// @details 全ての公開関数は純粋関数（メンバ状態なし）。
namespace RoadArrow
{
	// ===== 寸法定数 =====

	/// @brief 全長（進行方向）[m]
	constexpr double kArrowLength_m = 5.0;

	/// @brief 直進矢印の縦寸法（perpendicular）[m]
	/// @details JIS 規格 4.5m
	constexpr double kArrowWidthStraight_m = 4.5;

	/// @brief 左折/右折矢印の縦寸法 [m]
	/// @details 出典: TBD（リファレンス画像由来の暫定値、要 JIS 確認）
	constexpr double kArrowWidthTurn_m = 2.2;

	/// @brief 直進+左/右折矢印の縦寸法 [m]
	/// @details 直進部分の幅と一致
	constexpr double kArrowWidthCombined_m = 4.5;

	/// @brief ノード境界から矢印中心までのオフセット [m]
	constexpr double kArrowOffsetFromNode_m = 8.0;

	// ===== API =====

	/// @brief 種別から 2D Polygon を生成する（ローカル座標）
	/// @details ローカル座標規約:
	///   - X 軸: 進行方向（矢印先端 = +X、テール = -X）
	///   - Y 軸: 横方向（perpendicular）
	///   - 単位: メートル
	///   左折系（Left, StraightLeft）: 矢じり先端は +Y 方向
	///   右折系（Right, StraightRight）: 矢じり先端は -Y 方向
	/// @param type 矢印種別
	/// @return 構築済み Polygon。未対応種別 / None なら空 Polygon
	Polygon CreateContour(RoadArrowType type);

	/// @brief 種別から 3D MeshData を生成する（ローカル座標、Y=0 平面）
	/// @details normal は +Y 固定。CreateContour() の三角形分割を MeshData に詰める。
	/// @param type 矢印種別
	/// @return MeshData。未対応 / None なら空 MeshData
	MeshData CreateMesh(RoadArrowType type);

	/// @brief LaneConnection 群から矢印種別を自動推論する
	/// @details
	///   1. 指定レーンが当該ノードへの entry でなければ None
	///   2. node.laneConnections から fromEdgeId/fromLaneIndex 一致を抽出
	///   3. UTurn はトポロジ判定 (toEdgeId == fromEdgeId) 優先
	///   4. それ以外は TrafficCommon::classifyTurnByAngles (45° ルール) で分類
	///   5. 出口の方向集合 → ArrowType
	/// @param network 道路ネットワーク
	/// @param edgeId 矢印を表示する車線が属するエッジ
	/// @param laneIndex 車線インデックス
	/// @param towardNodeId 進行方向側のノード ID
	/// @return 推論された矢印種別。接続なし or 非 entry なら None
	RoadArrowType InferType(const RoadNetwork& network,
	                        int edgeId, int laneIndex, int towardNodeId);
}

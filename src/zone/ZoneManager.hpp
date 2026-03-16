
#pragma once
#include "Building.hpp"
#include "../world/World.hpp"
#include "../road/RoadNetwork.hpp"
#include "../economy/Economy.hpp"

/// @brief ゾーン塗り・建物自動生成・月次評価を担うクラス（05_zoning_spec.md §5）
class ZoneManager
{
public:
	// ----- ゾーン操作 -----

	/// @brief 指定ワールド座標を中心にブラシでゾーンを塗る
	/// @param brushRadius セル単位の半径（0 = 1セルのみ）
	void paintZone(World& world, Vec3 worldPos, ZoneType zone, int brushRadius = 2);

	/// @brief 2点で指定した矩形範囲にゾーンを塗る
	void paintZoneRect(World& world, Vec3 a, Vec3 b, ZoneType zone);

	/// @brief ワールド座標のゾーン種別を返す
	ZoneType getZone(const World& world, Vec3 worldPos) const;

	// ----- 月次更新 -----

	/// @brief 毎ゲーム月に呼ぶ（建物生成・成長・経済収支適用）
	void monthlyUpdate(World& world, const RoadNetwork& network,
	                   double gameNow, Economy& economy);

	// ----- 統計 -----

	/// @brief 全アクティブチャンクの住宅収容人口合計を返す
	int totalHousingCapacity(const World& world) const;

	// ----- オーバーレイ描画 -----

	bool showOverlay = false;  ///< Tab キーでトグル

	/// @brief 3D シーンにゾーンオーバーレイを描画する（update 後・UI 前に呼ぶ）
	void renderOverlay(const World& world) const;

private:
	// ----- 座標変換ヘルパー -----

	/// @brief ワールド座標 → (チャンク座標, セル座標) に変換する
	static std::pair<Point, Point> worldToCell(Vec3 worldPos);

	/// @brief セルの中心ワールド座標を返す
	static Vec3 cellToWorld(Point chunkCoord, Point cellCoord);

	// ----- 発展スコア -----

	/// @brief セルの発展スコア（0.0〜1.0）を計算する
	/// @details 道路アクセス係数をメインに算出（Phase 3: 簡易版）
	float calcDevelopmentScore(Point chunkCoord, int cx, int cy,
	                           const RoadNetwork& network) const;

	// ----- 建物生成 -----

	/// @brief ゾーン種別に応じた初期建物（stage=0）を返す
	Building spawnBuilding(ZoneType zone, double gameNow) const;

	/// @brief スコアが十分であれば成長段階を1上げる（成長したら true）
	bool tryGrowBuilding(Building& b, ZoneType zone, float score) const;

	// 最大成長段階（BuildingType × ゾーン）
	static int maxStage(BuildingType t, ZoneType zone);
};

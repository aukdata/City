
#pragma once
#include "Building.hpp"
#include "../world/World.hpp"
#include "../road/RoadNetwork.hpp"
#include "../time/GameClock.hpp"
#include "../gameplay/CitySimulation.hpp"

/// @brief 月次の用途別開発需要
struct ZoneDevelopmentDemand
{
	double residential = 0.0;
	double commercial = 0.0;
	double industrial = 0.0;
};

/// @brief 都市統計から用途別の月次開発需要を計算する
ZoneDevelopmentDemand calculateZoneDevelopmentDemand(int population, const CitySnapshot& snapshot);

/// @brief 月次ゾーン更新の変更件数
struct ZoneMonthlyUpdateResult
{
	int spawnedBuildings = 0;
	int upgradedBuildings = 0;
	int removedBuildings = 0;
};

/// @brief ゾーン塗り・建物自動生成・月次評価を担うクラス（05_zoning_spec.md §5）
class ZoneManager
{
public:
	// ZoneManager はゾーン編集、統計計算、建物初期化をまとめて扱う薄いゲームプレイ層とする。
	// ----- ゾーン操作 -----

	/// @brief 指定ワールド座標を中心にブラシでゾーンを塗る
	/// @param brushRadius セル単位の半径（0 = 1セルのみ）
	void paintZone(World& world, Vec3 worldPos, ZoneType zone, int brushRadius = 2);

	/// @brief 2点で指定した矩形範囲にゾーンを塗る
	void paintZoneRect(World& world, Vec3 a, Vec3 b, ZoneType zone);

	/// @brief ワールド座標のゾーン種別を返す
	ZoneType getZone(const World& world, Vec3 worldPos) const;

	// ----- 統計 -----

	/// @brief 全ワールドの住宅収容人口合計を返す
	int totalHousingCapacity(const World& world) const;

	/// @brief 全ワールドのゾーンを決定論的に月次評価する
	ZoneMonthlyUpdateResult updateMonthly(World& world, const RoadNetwork& network,
		const ZoneDevelopmentDemand& demand, GameTime gameNow, int64 monthIndex) const;

	// ----- オーバーレイ描画 -----

	bool showOverlay = false;  ///< Tab キーでトグル

	/// @brief 3D シーンにゾーンオーバーレイを描画する（update 後・UI 前に呼ぶ）
	void renderOverlay(const World& world) const;

	// ----- 建物生成 -----

	/// @brief ゾーン種別に応じた初期建物を返す
	Building spawnBuilding(ZoneType zone, double gameNow) const;

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

	/// @brief 乱数状態に依存しない月次生成用の建物選択
	Building spawnBuildingDeterministic(ZoneType zone, GameTime gameNow, uint32 roll) const;
};

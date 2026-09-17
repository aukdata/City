
#pragma once
#include "Building.hpp"
#include "../world/World.hpp"
#include "../road/RoadNetwork.hpp"
#include "../time/GameClock.hpp"
#include "../gameplay/CitySimulation.hpp"
#include "../gen/ParcelRoadIndex.hpp"

/// @brief 用途別開発需要
struct ZoneDevelopmentDemand
{
	double residential = 0.0;
	double commercial = 0.0;
	double industrial = 0.0;
};

/// @brief 都市統計から用途別の開発需要を計算する
ZoneDevelopmentDemand calculateZoneDevelopmentDemand(int population, const CitySnapshot& snapshot);

/// @brief 塗った空き区画の開発状態。
enum class ZoneDevelopmentState : uint8 { Checking, Developing, NeedsRoad, NeedsSpace, UnsuitableTerrain };
struct ZoneDevelopmentSummary
{
	int checking = 0, developing = 0, needsRoad = 0, needsSpace = 0, unsuitableTerrain = 0;
	int completed = 0;
};
struct ZoneDevelopmentResult
{
	int buildings = 0, housing = 0, residential = 0, commercial = 0, industrial = 0;
};

/// @brief ゾーン塗り・道路沿いの開発を担うクラス（05_zoning_spec.md §5）
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

	/// @brief 塗った空き区画を少しずつ開発する。秒数は標準速度基準で、停止中は0。
	ZoneDevelopmentResult updateDevelopment(World& world, const RoadNetwork& network,
		const ZoneDevelopmentDemand& demand, double simulationSeconds, GameTime gameNow, const TrainNetwork* railway=nullptr);
	[[nodiscard]] ZoneDevelopmentSummary developmentSummary() const;
	[[nodiscard]] JSON developmentSnapshot() const;
	[[nodiscard]] JSON saveState(const World& world) const;
	void restoreState(const JSON& snapshot, World& world);
	/// @brief 開発途中の区画と進捗を復元する。既存建物・ゾーンが一致する区画だけを採用。
	void restoreDevelopment(const JSON& snapshot, const World& world);

	// ----- オーバーレイ描画 -----

	bool showOverlay = false;  ///< Tab キーでトグル

	/// @brief 3D シーンにゾーンオーバーレイを描画する（update 後・UI 前に呼ぶ）
	void renderOverlay(const World& world) const;

private:
	struct DevelopmentPlot
	{
		Point chunk, cell;
		ZoneType zone = ZoneType::Unzoned;
		double progress = 0, checkedAt = 0, nextCheck = 0;
		ZoneDevelopmentState state = ZoneDevelopmentState::Checking;
	};
	Optional<ParcelRoadIndex> m_railwayCorridor;
	size_t m_railwayEdgeCount = 0;
	HashSet<Point> m_editedCells;
	Array<DevelopmentPlot> m_development;
	HashTable<int64, size_t> m_developmentIndex;
	size_t m_developmentCursor = 0;
	double m_developmentTime = 0;
	int m_completed = 0;
	void paintCell(Chunk& chunk, Point cell, ZoneType zone);
	void removeDevelopment(size_t index);
	static std::pair<Point, Point> worldToCell(Vec3 worldPos);
};

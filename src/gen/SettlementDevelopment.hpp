#pragma once
#include "MapGenerator.hpp"
#include "AgriculturalLayout.hpp"

/// @brief 集落の土地利用・建物・敷地を生成する。画面や描画資産を所有しない。
class SettlementDevelopment
{
public:
	struct Validation
	{
		bool passed;
		String summary;
	};

	SettlementDevelopment(World& world, RoadNetwork& network, const TrainNetwork& trains,
		const Array<MapGenerator::Settlement>& districts, uint64 seed)
		: m_world{ world }, m_network{ network }, m_trainNetwork{ trains }, m_districts{ districts }, m_seed{ seed } {}

	void applyZonesGlobal();
	/// @brief 建物、敷地、接道方向の順に生成し、街の制約を検証する。
	Validation placeInitialBuildings(bool preserveLandPatches = false);
	void generateLandPatches(bool preserveExisting = false);
	void refreshBuildingAnglesFromEdges();
	Validation validateGeneratedCityConstraints() const;
	/// @brief 既存の読み込み入口。処理を移すだけで移行範囲は拡張しない。
	void migrateLegacyBuildingFrontageReferences();

private:
	Array<AgriculturalLayout::Frame> agriculturalFrames() const;
	World& m_world;
	RoadNetwork& m_network;
	const TrainNetwork& m_trainNetwork;
	const Array<MapGenerator::Settlement>& m_districts;
	uint64 m_seed;
};

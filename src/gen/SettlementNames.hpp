#pragma once
#include "MapGenerator.hpp"

/// @brief 内部の地名語幹を保持し、集落の規模に応じた表示名と読みを作る。
namespace SettlementNames
{
	inline String suffix(MapGenerator::SettlementKind kind)
	{
		return kind==MapGenerator::SettlementKind::RegionalCity ? U"市"
			: kind==MapGenerator::SettlementKind::LocalTown ? U"町" : U"村";
	}
	inline String name(const MapGenerator::Settlement& town) { const String ending=suffix(town.kind);return town.name.ends_with(ending) ? town.name : town.name+ending; }
	inline String reading(const MapGenerator::Settlement& town)
	{
		if (town.reading.isEmpty() || town.name.ends_with(suffix(town.kind))) { return town.reading; }
		return town.reading+(town.kind==MapGenerator::SettlementKind::RegionalCity ? U"-shi"
			: town.kind==MapGenerator::SettlementKind::LocalTown ? U"-machi" : U"-mura");
	}
}

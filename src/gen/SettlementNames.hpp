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
		if (town.reading.isEmpty())
		{
			return {};
		}
		// 語幹が「村」で終わる場合も英語の行政種別は省略しない。
		return town.reading + (town.kind == MapGenerator::SettlementKind::RegionalCity	 ? U" City"
								  : town.kind == MapGenerator::SettlementKind::LocalTown ? U" Towm"
																						 : U" Vill.");
	}
}

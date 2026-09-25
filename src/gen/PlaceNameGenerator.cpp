#include "GenerationSettings.hpp"
# include "../../stdafx.h"
#include "PlaceNameGenerator.hpp"

// ─────────────────────────────────────────────────────────────────────────────
// 内部定数：カテゴリ重み（バイオームベースのため均等配分）
// 列順: River, Mountain, Plain, Coast, General
// ─────────────────────────────────────────────────────────────────────────────

namespace
{
	

	/// @brief TOML 配列から String 配列を読む
	Array<String> readStringArray(const TOMLValue& v)
	{
		Array<String> result;
		if (v.isEmpty()) return result;
		const auto arr = v.arrayView();
		result.reserve(v.arrayCount());
		for (const auto& elem : arr)
			result << elem.getOr<String>(U"");
		result.remove_if([](const String& s) { return s.isEmpty(); });
		return result;
	}
}

// ─────────────────────────────────────────────────────────────────────────────
// PlaceNameGenerator 実装
// ─────────────────────────────────────────────────────────────────────────────

bool PlaceNameGenerator::load(FilePathView tomlPath)
{
	const TOMLReader toml{ tomlPath };
	if (!toml)
		return false;

	const char32_t* kNames[5] = { U"River", U"Mountain", U"Plain", U"Coast", U"General" };
	for (int i = 0; i < 5; ++i)
	{
		const auto sec = toml[kNames[i]];
		m_words[i].prefix     = readStringArray(sec[U"prefix"]);
		m_words[i].prefixYomi = readStringArray(sec[U"prefix_yomi"]);
		m_words[i].suffix     = readStringArray(sec[U"suffix"]);
		m_words[i].suffixYomi = readStringArray(sec[U"suffix_yomi"]);
	}

	m_loaded = true;
	return true;
}

PlaceNameDB PlaceNameGenerator::generate(int settlementCount, uint64 mapSeed) const
{
	PlaceNameDB db;
	if (!m_loaded || settlementCount <= 0)
		return db;

	for (int i = 0; i < settlementCount; ++i)
	{
		uint64 state = hashCombine(mapSeed, static_cast<uint64>(i));

		const PlaceCategory cat    = pickCategory(state);
		const auto [name, reading] = generateOne(cat, db, state);

		db.settlementNames[i]    = name;
		db.settlementReadings[i] = PlaceNameFormat::capitalizeReading(reading);
	}

	return db;
}

PlaceNameDB PlaceNameGenerator::generateWithBiomes(
	int settlementCount, const Array<BiomeType>& biomes, uint64 mapSeed) const
{
	PlaceNameDB db;
	if (!m_loaded || settlementCount <= 0)
		return db;

	for (int i = 0; i < settlementCount; ++i)
	{
		uint64 state = hashCombine(mapSeed, static_cast<uint64>(i));

		const BiomeType biome = (i < static_cast<int>(biomes.size()))
			? biomes[i] : BiomeType::Plain;
		const PlaceCategory cat    = pickCategoryForBiome(biome, state);
		const auto [name, reading] = generateOne(cat, db, state);

		db.settlementNames[i]    = name;
		db.settlementReadings[i] = PlaceNameFormat::capitalizeReading(reading);
	}

	return db;
}

// ─────────────────────────────────────────────────────────────────────────────
// 内部ヘルパー
// ─────────────────────────────────────────────────────────────────────────────

PlaceCategory PlaceNameGenerator::pickCategory(uint64& state) const
{
	const int w[5] = { GenerationSettings::get().placeNames_default_river, GenerationSettings::get().placeNames_default_mountain, GenerationSettings::get().placeNames_default_plain, GenerationSettings::get().placeNames_default_coast, GenerationSettings::get().placeNames_default_general };
	int        sum = 0;
	for (int k = 0; k < 5; ++k) sum += w[k];
	if (sum <= 0) return PlaceCategory::General;

	int r   = static_cast<int>(randIndex(state, static_cast<size_t>(sum)));
	int cum = 0;
	for (int k = 0; k < 5; ++k)
	{
		cum += w[k];
		if (r < cum)
			return static_cast<PlaceCategory>(k);
	}
	return PlaceCategory::General;
}

PlaceCategory PlaceNameGenerator::pickCategoryForBiome(BiomeType biome, uint64& state) const
{
	// バイオーム別重み: { River, Mountain, Plain, Coast, General }
	const int kBiomeWeights[][5] = {
		/* Ocean */ { GenerationSettings::get().placeNames_Ocean_river, GenerationSettings::get().placeNames_Ocean_mountain, GenerationSettings::get().placeNames_Ocean_plain, GenerationSettings::get().placeNames_Ocean_coast, GenerationSettings::get().placeNames_Ocean_general },
		/* Lake */ { GenerationSettings::get().placeNames_Lake_river, GenerationSettings::get().placeNames_Lake_mountain, GenerationSettings::get().placeNames_Lake_plain, GenerationSettings::get().placeNames_Lake_coast, GenerationSettings::get().placeNames_Lake_general },
		/* CoastalPlain */ { GenerationSettings::get().placeNames_CoastalPlain_river, GenerationSettings::get().placeNames_CoastalPlain_mountain, GenerationSettings::get().placeNames_CoastalPlain_plain, GenerationSettings::get().placeNames_CoastalPlain_coast, GenerationSettings::get().placeNames_CoastalPlain_general },
		/* CoastalHill */ { GenerationSettings::get().placeNames_CoastalHill_river, GenerationSettings::get().placeNames_CoastalHill_mountain, GenerationSettings::get().placeNames_CoastalHill_plain, GenerationSettings::get().placeNames_CoastalHill_coast, GenerationSettings::get().placeNames_CoastalHill_general },
		/* Plain */ { GenerationSettings::get().placeNames_Plain_river, GenerationSettings::get().placeNames_Plain_mountain, GenerationSettings::get().placeNames_Plain_plain, GenerationSettings::get().placeNames_Plain_coast, GenerationSettings::get().placeNames_Plain_general },
		/* Basin */ { GenerationSettings::get().placeNames_Basin_river, GenerationSettings::get().placeNames_Basin_mountain, GenerationSettings::get().placeNames_Basin_plain, GenerationSettings::get().placeNames_Basin_coast, GenerationSettings::get().placeNames_Basin_general },
		/* Hill */ { GenerationSettings::get().placeNames_Hill_river, GenerationSettings::get().placeNames_Hill_mountain, GenerationSettings::get().placeNames_Hill_plain, GenerationSettings::get().placeNames_Hill_coast, GenerationSettings::get().placeNames_Hill_general },
		/* Foothill */ { GenerationSettings::get().placeNames_Foothill_river, GenerationSettings::get().placeNames_Foothill_mountain, GenerationSettings::get().placeNames_Foothill_plain, GenerationSettings::get().placeNames_Foothill_coast, GenerationSettings::get().placeNames_Foothill_general },
		/* Plateau */ { GenerationSettings::get().placeNames_Plateau_river, GenerationSettings::get().placeNames_Plateau_mountain, GenerationSettings::get().placeNames_Plateau_plain, GenerationSettings::get().placeNames_Plateau_coast, GenerationSettings::get().placeNames_Plateau_general },
		/* Mountain */ { GenerationSettings::get().placeNames_Mountain_river, GenerationSettings::get().placeNames_Mountain_mountain, GenerationSettings::get().placeNames_Mountain_plain, GenerationSettings::get().placeNames_Mountain_coast, GenerationSettings::get().placeNames_Mountain_general },
		/* MountainRange */ { GenerationSettings::get().placeNames_MountainRange_river, GenerationSettings::get().placeNames_MountainRange_mountain, GenerationSettings::get().placeNames_MountainRange_plain, GenerationSettings::get().placeNames_MountainRange_coast, GenerationSettings::get().placeNames_MountainRange_general },
	};

	const int idx = Clamp(static_cast<int>(biome), 0,
		static_cast<int>(std::size(kBiomeWeights)) - 1);
	const int* w = kBiomeWeights[idx];
	int sum = 0;
	for (int k = 0; k < 5; ++k) sum += w[k];
	if (sum <= 0) return PlaceCategory::General;

	int r   = static_cast<int>(randIndex(state, static_cast<size_t>(sum)));
	int cum = 0;
	for (int k = 0; k < 5; ++k)
	{
		cum += w[k];
		if (r < cum)
			return static_cast<PlaceCategory>(k);
	}
	return PlaceCategory::General;
}

std::pair<String, String> PlaceNameGenerator::generateOne(PlaceCategory cat, PlaceNameDB& db, uint64& state) const
{
	const int ci   = static_cast<int>(cat);
	const auto& wl = m_words[ci];

	if (wl.prefix.isEmpty() || wl.suffix.isEmpty())
		return { U"不明", U"fumei" };

	// 10% の確率で 3 文字地名（prefix+prefix+suffix または prefix+suffix+suffix）
	const bool threeChar         = (randIndex(state, GenerationSettings::get().placeNames_threeCharacterPeriod) == 0);
	const bool prefixPrefixSuffix = threeChar && (randIndex(state, 2) == 0);

	constexpr int kMaxRetry = 200;
	for (int attempt = 0; attempt < kMaxRetry; ++attempt)
	{
		String name, reading;

		if (threeChar)
		{
			if (prefixPrefixSuffix)
			{
				// prefix1 + prefix2 + suffix
				const size_t pi1 = randIndex(state, wl.prefix.size());
				const size_t pi2 = randIndex(state, wl.prefix.size());
				const size_t si  = randIndex(state, wl.suffix.size());
				// 隣接する同一漢字を排除
				if (wl.prefix[pi1] == wl.prefix[pi2] || wl.prefix[pi2] == wl.suffix[si])
					continue;
				name    = wl.prefix[pi1] + wl.prefix[pi2] + wl.suffix[si];
				reading = wl.prefixYomi[pi1] + wl.prefixYomi[pi2] + wl.suffixYomi[si];
			}
			else
			{
				// prefix + suffix1 + suffix2
				const size_t pi  = randIndex(state, wl.prefix.size());
				const size_t si1 = randIndex(state, wl.suffix.size());
				const size_t si2 = randIndex(state, wl.suffix.size());
				if (wl.prefix[pi] == wl.suffix[si1] || wl.suffix[si1] == wl.suffix[si2])
					continue;
				name    = wl.prefix[pi] + wl.suffix[si1] + wl.suffix[si2];
				reading = wl.prefixYomi[pi] + wl.suffixYomi[si1] + wl.suffixYomi[si2];
			}
		}
		else
		{
			// 通常 2 文字地名
			const size_t pi = randIndex(state, wl.prefix.size());
			const size_t si = randIndex(state, wl.suffix.size());
			if (wl.prefix[pi] == wl.suffix[si])
				continue;
			name    = wl.prefix[pi] + wl.suffix[si];
			reading = wl.prefixYomi[pi] + wl.suffixYomi[si];
		}

		if (!db.usedNames.contains(name))
		{
			db.usedNames.emplace(name);
			return { name, reading };
		}
	}

	// 枯渇時フォールバック：全カテゴリの prefix+suffix を試行
	for (int c = 0; c < 5; ++c)
	{
		const auto& wl2 = m_words[c];
		if (wl2.prefix.isEmpty() || wl2.suffix.isEmpty()) continue;
		for (int attempt = 0; attempt < 100; ++attempt)
		{
			const size_t pi = randIndex(state, wl2.prefix.size());
			const size_t si = randIndex(state, wl2.suffix.size());
			const String name = wl2.prefix[pi] + wl2.suffix[si];
			if (!db.usedNames.contains(name))
			{
				db.usedNames.emplace(name);
				return { name, wl2.prefixYomi[pi] + wl2.suffixYomi[si] };
			}
		}
	}

	// 最終フォールバック：前節 + 連番
	for (int n = 2; n <= 999; ++n)
	{
		const size_t pi   = randIndex(state, wl.prefix.size());
		const String name = wl.prefix[pi] + Format(n);
		if (!db.usedNames.contains(name))
		{
			db.usedNames.emplace(name);
			return { name, wl.prefixYomi[pi] + Format(n) };
		}
	}

	return { U"不明", U"fumei" };
}

uint64 PlaceNameGenerator::nextRand(uint64& state)
{
	// xorshift64
	state ^= state << 13;
	state ^= state >> 7;
	state ^= state << 17;
	return state;
}

size_t PlaceNameGenerator::randIndex(uint64& state, size_t n)
{
	if (n == 0) return 0;
	return static_cast<size_t>(nextRand(state) % static_cast<uint64>(n));
}

uint64 PlaceNameGenerator::hashCombine(uint64 a, uint64 b)
{
	// SplitMix64 ベースのハッシュ合成
	a ^= b + 0x9e3779b97f4a7c15ULL + (a << 6) + (a >> 2);
	a = (a ^ (a >> 30)) * 0xbf58476d1ce4e5b9ULL;
	a = (a ^ (a >> 27)) * 0x94d049bb133111ebULL;
	return a ^ (a >> 31);
}

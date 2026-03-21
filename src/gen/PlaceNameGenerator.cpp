# include "../../stdafx.h"
#include "PlaceNameGenerator.hpp"

// ─────────────────────────────────────────────────────────────────────────────
// 内部定数：地形タイプ別カテゴリ重み（11_placename_spec.md §2 テーブル）
// 列順: River, Mountain, Plain, Coast, General
// ─────────────────────────────────────────────────────────────────────────────

namespace
{
	constexpr int kWeights[4][5] =
	{
		/* Basin    */ { 30, 40, 10,  0, 20 },
		/* Coastal  */ { 15,  5, 30, 30, 20 },
		/* RiverFan */ { 40, 10, 30,  0, 20 },
		/* Hills    */ { 10, 40, 20,  0, 30 },
	};

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

PlaceNameDB PlaceNameGenerator::generate(int settlementCount, TerrainType terrain, uint64 mapSeed) const
{
	PlaceNameDB db;
	if (!m_loaded || settlementCount <= 0)
		return db;

	for (int i = 0; i < settlementCount; ++i)
	{
		// 地区ごとに独立したシードを生成する
		uint64 state = hashCombine(mapSeed, static_cast<uint64>(i));

		const PlaceCategory cat    = pickCategory(terrain, state);
		const auto [name, reading] = generateOne(cat, db, state);

		db.settlementNames[i]    = name;
		db.settlementReadings[i] = reading;
	}

	return db;
}

// ─────────────────────────────────────────────────────────────────────────────
// 内部ヘルパー
// ─────────────────────────────────────────────────────────────────────────────

PlaceCategory PlaceNameGenerator::pickCategory(TerrainType terrain, uint64& state) const
{
	const int* w   = kWeights[static_cast<int>(terrain)];
	int        sum = 0;
	for (int k = 0; k < 5; ++k) sum += w[k];

	// Coast の重みが 0 の地形でも安全に動作するよう sum > 0 を保証
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
	const bool threeChar         = (randIndex(state, 10) == 0);
	const bool prefixPrefixSuffix = threeChar && (randIndex(state, 2) == 0);

	constexpr int kMaxRetry = 40;
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

	// 枯渇時フォールバック：前節 + 連番
	for (int n = 2; n <= 99; ++n)
	{
		const size_t pi   = randIndex(state, wl.prefix.size());
		const String name = wl.prefix[pi] + Format(n);
		if (!db.usedNames.contains(name))
		{
			db.usedNames.emplace(name);
			return { name, wl.prefixYomi[pi] + Format(n) };
		}
	}

	const size_t pi = randIndex(state, wl.prefix.size());
	return { wl.prefix[pi], wl.prefixYomi[pi] };
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

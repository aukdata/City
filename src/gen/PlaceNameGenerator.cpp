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
		m_words[i].prefix = readStringArray(sec[U"prefix"]);
		m_words[i].suffix = readStringArray(sec[U"suffix"]);
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
		// 集落ごとに独立したシードを生成する
		uint64 state = hashCombine(mapSeed, static_cast<uint64>(i));

		const PlaceCategory cat = pickCategory(terrain, state);
		const String name       = generateOne(cat, db, state);

		db.settlementNames[i] = name;
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

String PlaceNameGenerator::generateOne(PlaceCategory cat, PlaceNameDB& db, uint64& state) const
{
	const int ci = static_cast<int>(cat);
	const auto& wl = m_words[ci];

	if (wl.prefix.isEmpty() || wl.suffix.isEmpty())
		return U"不明";

	// 前節を決定（変えない）
	const String prefix = wl.prefix[randIndex(state, wl.prefix.size())];

	// 後節を決定（重複時は別の後節を選び直す）
	constexpr int kMaxRetry = 20;
	for (int attempt = 0; attempt < kMaxRetry; ++attempt)
	{
		const String suffix = wl.suffix[randIndex(state, wl.suffix.size())];
		const String name   = prefix + suffix;
		if (!db.usedNames.contains(name))
		{
			db.usedNames.emplace(name);
			return name;
		}
	}

	// 全後節が枯渇した場合：前節に連番サフィックスを付けて強制解決
	for (int n = 2; n <= 99; ++n)
	{
		const String name = prefix + Format(n);
		if (!db.usedNames.contains(name))
		{
			db.usedNames.emplace(name);
			return name;
		}
	}

	return prefix;  // 最終フォールバック
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

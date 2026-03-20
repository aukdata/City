#pragma once
#include "TerrainType.hpp"

// ─────────────────────────────────────────────────────────────────────────────
// 地名カテゴリ（11_placename_spec.md §2）
// ─────────────────────────────────────────────────────────────────────────────

/// @brief 語根カテゴリ
enum class PlaceCategory : uint8
{
	River    = 0,  ///< 川・水系
	Mountain = 1,  ///< 山・丘・高地
	Plain    = 2,  ///< 平野・田・原
	Coast    = 3,  ///< 海・湾・浜
	General  = 4,  ///< 地形に依らない汎用
};

// ─────────────────────────────────────────────────────────────────────────────
// 地名データベース（11_placename_spec.md §6）
// ─────────────────────────────────────────────────────────────────────────────

/// @brief ランタイム地名データベース
struct PlaceNameDB
{
	/// @brief 集落インデックス → 集落地名
	HashTable<int, String> settlementNames;

	/// @brief 施設 ID → 施設名（駅・IC・信号所）
	HashTable<int, String> facilityNames;

	/// @brief 道路計画 ID → 道路名
	HashTable<int, String> roadNames;

	/// @brief 重複管理セット
	HashSet<String> usedNames;

	// ---- 派生地名生成ヘルパー（11_placename_spec.md §3） ----

	/// @brief 集落名を返す（未登録なら空文字列）
	String settlementName(int idx) const
	{
		const auto it = settlementNames.find(idx);
		return (it != settlementNames.end()) ? it->second : U"";
	}

	/// @brief 駅名を生成する
	/// @param settlementIdx 集落インデックス
	/// @param variant 0="{S}駅" 1="{S}中央駅" 2="{S}市駅" 3="{S}口駅" 4="新{S}駅"
	String makeStationName(int settlementIdx, int variant = 0) const
	{
		const String s = settlementName(settlementIdx);
		if (s.isEmpty()) return U"";
		switch (variant)
		{
		case 1:  return s + U"中央駅";
		case 2:  return s + U"市駅";
		case 3:  return s + U"口駅";
		case 4:  return U"新" + s + U"駅";
		default: return s + U"駅";
		}
	}

	/// @brief IC名を生成する
	/// @param variant 0="{S}IC" 1="{S}東IC" 2="{S}西IC"
	String makeICName(int settlementIdx, int variant = 0) const
	{
		const String s = settlementName(settlementIdx);
		if (s.isEmpty()) return U"";
		switch (variant)
		{
		case 1:  return s + U"東IC";
		case 2:  return s + U"西IC";
		default: return s + U"IC";
		}
	}

	/// @brief 道路名を生成する（バイパス or 街道）
	String makeRoadName(int settlementIdx, bool isBypass = true) const
	{
		const String s = settlementName(settlementIdx);
		if (s.isEmpty()) return U"";
		return s + (isBypass ? U"バイパス" : U"街道");
	}

	/// @brief 信号所名を生成する
	String makeSignalName(int settlementIdx) const
	{
		const String s = settlementName(settlementIdx);
		return s.isEmpty() ? U"" : s + U"信号場";
	}
};

// ─────────────────────────────────────────────────────────────────────────────
// 地名生成エンジン（11_placename_spec.md §3）
// ─────────────────────────────────────────────────────────────────────────────

/// @brief 地名生成クラス。外部 TOML から語根辞書を読み込み、シード値から確定的に地名を生成する。
class PlaceNameGenerator
{
public:
	/// @brief 語根辞書を TOML ファイルから読み込む
	/// @param tomlPath 語根辞書 TOML のパス
	/// @return 読み込み成功なら true
	bool load(FilePathView tomlPath);

	/// @brief 集落地名を一括生成して PlaceNameDB を構築する
	/// @param settlementCount 集落数
	/// @param terrain         マップ全体の地形タイプ
	/// @param mapSeed         マップシード値（同じシード→同じ地名）
	PlaceNameDB generate(int settlementCount, TerrainType terrain, uint64 mapSeed) const;

private:
	/// @brief カテゴリ別の前節・後節リスト
	struct WordList
	{
		Array<String> prefix;
		Array<String> suffix;
	};

	/// @brief 5 カテゴリ分の語根 (インデックス = PlaceCategory)
	WordList m_words[5];

	bool m_loaded = false;

	// ---- 内部ヘルパー ----

	/// @brief 地形タイプに応じたカテゴリ重みでカテゴリを選ぶ
	PlaceCategory pickCategory(TerrainType terrain, uint64& state) const;

	/// @brief 地名を 1 件生成して usedNames に追加する（衝突時は suffix を変える）
	String generateOne(PlaceCategory cat, PlaceNameDB& db, uint64& state) const;

	/// @brief xorshift64 ベースの軽量 RNG（state を更新しながら次の乱数を返す）
	static uint64 nextRand(uint64& state);

	/// @brief [0, n) の一様乱数
	static size_t randIndex(uint64& state, size_t n);

	/// @brief シード合成（集落ごとに独立したシードを作る）
	static uint64 hashCombine(uint64 a, uint64 b);
};

#pragma once
#include "../road/RoadTypes.hpp"
#include "RoadStyle.hpp"

/// @brief 道路種別ごとの外観スタイルを管理するレジストリ
/// TOML ファイルからロードし、RoadType をキーにスタイルを提供する。
///
/// @par 将来拡張
/// 同じパターンで FenceStyleRegistry, GuardRailStyleRegistry 等を実装できる。
/// TOML パーシング共通ユーティリティは将来 LinearStyleLoader に切り出すことを想定。
class RoadStyleRegistry
{
public:
	/// @brief 道路種別ごとの TOML ファイルが置かれたディレクトリからロードする
	/// @param dirPath ディレクトリパス（例: "assets/roads"）
	///   各ファイル名は RoadType 名に対応: LocalRoad.toml / Arterial.toml 等
	/// @return 1 ファイル以上ロード成功なら true
	bool load(FilePathView dirPath);

	/// @brief 道路種別に対応するスタイルを返す（未登録なら fallback を返す）
	const RoadStyle& get(RoadType rt) const;

private:
	HashTable<uint8, RoadStyle> m_styles;  ///< RoadType を uint8 にキャストしてインデックス
	RoadStyle                   m_fallback; ///< 未登録種別のデフォルトスタイル
};

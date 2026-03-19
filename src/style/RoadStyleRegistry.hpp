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
	/// @brief TOML ファイルからスタイルをロードする
	/// @param tomlPath ファイルパス（実行時 WD からの相対パスまたは絶対パス）
	/// @return ロード成功なら true
	bool load(FilePathView tomlPath);

	/// @brief 道路種別に対応するスタイルを返す（未登録なら fallback を返す）
	const RoadStyle& get(RoadType rt) const;

private:
	HashTable<uint8, RoadStyle> m_styles;  ///< RoadType を uint8 にキャストしてインデックス
	RoadStyle                   m_fallback; ///< 未登録種別のデフォルトスタイル
};

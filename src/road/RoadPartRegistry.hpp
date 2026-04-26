#pragma once
#include "RoadPartTypes.hpp"
#include "ObjParser.hpp"

/// @brief 道路部品アセットの管理レジストリ
/// @details App/assets/road_parts/ 内の TOML+OBJ ペアを読み込み、
///          defId で検索できるように管理する。
class RoadPartRegistry
{
public:
	// 道路断面を構成する部品定義とメッシュを defId で引ける形に集約して管理する。

	/// @brief 指定ディレクトリ内の全 TOML+OBJ を読み込む
	/// @param dirPath アセットディレクトリパス（例: "assets/road_parts"）
	/// @return 1件以上読み込めれば true
	bool load(FilePathView dirPath);

	/// @brief defId で部品定義を検索
	/// @return 見つからなければフォールバック定義を返す
	[[nodiscard]]
	const RoadPartDef& get(StringView defId) const;

	/// @brief defId で部品モデルを検索
	/// @return 見つからなければ空モデルを返す
	[[nodiscard]]
	const RoadPartModel& getModel(StringView defId) const;

	/// @brief 登録済みの defId 一覧を返す
	[[nodiscard]]
	Array<String> defIds() const;

	/// @brief 登録数を返す
	[[nodiscard]]
	size_t size() const { return m_entries.size(); }

private:

	struct Entry
	{
		RoadPartDef   def;
		RoadPartModel model;
	};

	HashTable<String, Entry> m_entries;

	RoadPartDef   m_fallbackDef;
	RoadPartModel m_fallbackModel;

	/// @brief 1つの TOML ファイルからエントリを読み込む
	Optional<Entry> loadEntry(FilePathView tomlPath, FilePathView baseDir);
};

#pragma once
#include "RoadArrow.hpp"

/// @brief 路面標示矢印アセットの管理レジストリ
/// @details App/assets/road_markings/ 内の TOML を読み込み、
///          RoadArrowType でメッシュを検索できるように管理する。
class ArrowMarkingRegistry
{
public:

	/// @brief 指定ディレクトリ内の全 TOML を読み込む
	/// @param dirPath アセットディレクトリパス（例: "assets/road_markings"）
	/// @return 1件以上読み込めれば true
	bool load(FilePathView dirPath);

	/// @brief 矢印種別からメッシュデータを取得する
	/// @return 見つからなければ nullptr
	[[nodiscard]]
	const MeshData* getMesh(RoadArrowType type) const;

	/// @brief 登録数を返す
	[[nodiscard]]
	size_t size() const { return m_entries.size(); }

private:

	struct Entry
	{
		MeshData mesh;
	};

	HashTable<uint8, Entry> m_entries;

	/// @brief TOML の id 文字列から RoadArrowType へ変換
	static Optional<RoadArrowType> parseType(StringView id);

	/// @brief Polygon から MeshData を構築する
	static MeshData buildMesh(const Polygon& poly);
};

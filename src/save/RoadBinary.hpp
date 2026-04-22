#pragma once
#include "../road/RoadNetwork.hpp"

/// @brief chunks/{cx}_{cy}/roads.bin の読み書きユーティリティ
/// @details リトルエンディアン固定バイナリ（14_save_spec.md §6 参照）
class RoadBinary
{
public:
	static constexpr uint32 kMagic   = 0x004E4452u;  ///< "RDN\0"（リトルエンディアン）
	static constexpr uint16 kVersion = 12; ///< v12: constructionStartTime を追加。v7〜v11 互換読み込み対応

	/// @brief roads.bin を書き出す
	/// @param path     出力ファイルパス
	/// @param cx, cy   チャンク座標
	/// @param nodes    保存するノード列（tombstone id < 0 は自動除外）
	/// @param edges    保存するエッジ列（tombstone id < 0 は自動除外）
	/// @return 成功なら true
	static bool write(const FilePath& path, int32 cx, int32 cy,
	                  const Array<RoadNode>& nodes,
	                  const Array<RoadEdge>& edges);

	/// @brief roads.bin を読み込み、ノード・エッジを追記する
	/// @note  読み込んだノードの edgeIds はクリア済み（addEdgeRaw で再構築するため）
	/// @return 成功なら true
	static bool read(const FilePath& path,
	                 Array<RoadNode>& outNodes,
	                 Array<RoadEdge>& outEdges);

	/// @brief RoadNetwork 全体を単一ファイルに書き出す
	static bool writeGlobal(const FilePath& path, const RoadNetwork& network);

	/// @brief 単一ファイルから RoadNetwork を復元する
	static bool readGlobal(const FilePath& path, RoadNetwork& network);
};

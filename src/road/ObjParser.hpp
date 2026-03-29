#pragma once
#include "RoadPartTypes.hpp"

/// @brief Wavefront OBJ パーサー（道路部品用）
/// @details OBJ ファイルを CPU 上の MeshData として読み込む。
///          Siv3D の Model クラスと異なり、頂点データを直接操作可能。
///          対応行: o, v, vt, vn, f（三角面/四角面）
namespace ObjParser
{
	/// @brief OBJ ファイルをパースし、オブジェクト名ごとに PartModelData に分割する
	/// @param path OBJ ファイルのパス
	/// @return オブジェクト名付きメッシュデータの配列。失敗時は空配列
	[[nodiscard]]
	Array<PartModelData> parse(FilePathView path);

	/// @brief パース結果から RoadPartModel を構築する
	/// @param meshes parse() の戻り値
	/// @param symmetric true の場合、outer が存在しなければ inner をX軸ミラーして生成
	/// @return 構築された RoadPartModel
	[[nodiscard]]
	RoadPartModel buildModel(const Array<PartModelData>& meshes, bool symmetric = false);
}

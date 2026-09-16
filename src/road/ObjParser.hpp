#pragma once
#include "RoadPartTypes.hpp"

/// @brief Wavefront OBJ パーサー（道路部品用）
/// @details OBJ ファイルを CPU 上の MeshData として読み込む。
///          Siv3D の Model クラスと異なり、頂点データを直接操作可能。
///          対応行: o, v, vt, vn, f（三角面/四角面）
namespace ObjParser
{
	// 道路部品用 OBJ は CPU メッシュとして読み込み、後段で断面部品モデルへ再構成する。
	/// @brief OBJ ファイルをパースし、オブジェクト名ごとに PartModelData に分割する
	/// @param path OBJ ファイルのパス
	/// @param nativeMaterials true なら材質名で分割し、Siv3D Model と同じ左手座標・UVに変換（法線付きOBJ用）。
	/// @return オブジェクト名付きメッシュデータの配列。失敗時は空配列
	[[nodiscard]]
	Array<PartModelData> parse(FilePathView path, bool nativeMaterials = false);

	/// @brief パース結果から RoadPartModel を構築する
	/// @param meshes parse() の戻り値
	/// @param symmetric true の場合、outer が存在しなければ inner をX軸ミラーして生成
	/// @return 構築された RoadPartModel
	[[nodiscard]]
	RoadPartModel buildModel(const Array<PartModelData>& meshes, bool symmetric = false);
}

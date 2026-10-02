#pragma once
#include <Siv3D.hpp>

class World;

/// @brief 開発状態を、地形・道路とは独立した完全なスナップショットとして保存する。
/// @details 固定幅リトルエンディアン形式。省略セルは Unzoned と既定の空建物に戻す。
/// Version 1: ヘッダー20 bytes (magic/u32, version/u16, worldChunks/u16, chunkSize/u32,
/// heightCells/u16, zoneCells/u16, chunkCount/u32)。チャンクヘッダー17 bytes
/// (cx/u16, cy/u16, urban/u8, zoneCount/u32, buildingCount/u32, patchCount/u32)。
/// 続いて zoneCount 個の (cell/u16, zone/u8)、buildingCount 個の
/// (cell/u16, type/u8, builtAt/f64, angle/f32, edgeId/i32, edgeT/f32, offsetX/f32, offsetZ/f32)、
/// patchCount 個の (id/i32, type/u8, elevation/f32, material/u32, parcelKey/i64, vertexCount/u32,
/// 頂点の x/f64, y/f64) をこの順に保持する。セル番号は行優先。
class DevelopmentSnapshot
{
public:
	static constexpr uint32 kMagic = 0x31564544u; ///< "DEV1"
	static constexpr uint16 kVersion = 1;

	/// @brief 全生成済みチャンクの開発状態を書き、再読込で内容を検証する。
	[[nodiscard]] static bool write(const FilePath& path, const World& world);

	/// @brief 全検証の成功後だけ開発状態を置換する。対象の地形チャンク集合は一致が必要。
	/// @note 地形・道路・シミュレーション状態は変更しない。失敗時は world を変更しない。
	[[nodiscard]] static bool read(const FilePath& path, World& world);

	/// @brief ファイル全体を再検証し、既定セルを含む全保存フィールドの一致を確認する。
	[[nodiscard]] static bool matches(const FilePath& path, const World& world);
};

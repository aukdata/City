#pragma once

/// @brief シーン識別子
enum class SceneState { Title, Game };

/// @brief シーン間で共有するデータ
struct SceneData
{
	uint64 seed        = 20260316ULL;
	bool   sandboxMode = true;   ///< サンドボックスモード（道路形状を自由に編集）
	bool   isNewGame   = true;   ///< true: 新規生成、false: セーブロード
	String saveName;             ///< セーブ名（ロード時のみ使用）
};

/// @brief SceneManager の型エイリアス
using App = SceneManager<SceneState, SceneData>;

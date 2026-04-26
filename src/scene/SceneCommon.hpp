#pragma once

/// @brief シーン識別子
enum class SceneState { Title, Game };

/// @brief シーン間で共有するデータ
struct SceneData
{
	// タイトル画面で決めた開始条件をここへ集約し、GameScene への遷移時にまとめて引き渡す。
	uint64 seed        = 20260316ULL;
	bool   sandboxMode = true;   ///< サンドボックスモード（道路形状を自由に編集）
	bool   isNewGame   = true;   ///< true: 新規生成、false: セーブロード
	String saveName;             ///< セーブ名（ロード時のみ使用）
};

/// @brief SceneManager の型エイリアス
using App = SceneManager<SceneState, SceneData>;

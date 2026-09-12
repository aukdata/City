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
	bool   captureFirstPerson = false; ///< 同じ街角を歩行目線で比較撮影する。
	bool   captureTransport = false; ///< Streets, railway, bridges and full-screen map render review.
	bool   auditRoadIntegrity = false; ///< 道路生成の段階データを出力して終了（画像なし）
	bool   captureConstruction = false; ///< 工事の実画面と自動撤去を検証
	bool   captureRoadPlanUx = false; ///< 道路計画の入力状態を再現して実画面を撮影する
	bool   captureCityRenders = false; ///< true: 提出用に実ゲームレンダを自動撮影して終了
	bool   captureRoadRenders = false; ///< true: 幹線交差点を近景・上空から自動検証
	bool   benchmarkStreaming = false; ///< Deterministic camera flight with per-frame CPU measurements.
	bool   syncTerrain = false; ///< Reference path for terrain streaming A/B measurements.
	int    captureNode = -1; ///< Optional exact junction for render regression on saved maps.
	int    inspectNode = -1; ///< 起動後に指定した交差点へカメラを移動
	String saveName;             ///< セーブ名（ロード時のみ使用）
};

/// @brief SceneManager の型エイリアス
using App = SceneManager<SceneState, SceneData>;

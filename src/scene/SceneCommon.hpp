#pragma once
#include "../gen/GenerationOptions.hpp"
#include "../render/RenderDistance.hpp"

/// @brief シーン識別子
enum class SceneState { Title, Game };

/// @brief シーン間で共有するデータ
struct SceneData
{
	GenerationOptions generation; ///< 生成前に選んだ要素。ロード時は保存側の設定で復元する。
	// タイトル画面で決めた開始条件をここへ集約し、GameScene への遷移時にまとめて引き渡す。
	uint64 seed        = 20260316ULL;
	double effectVolume = .6; ///< ローカル設定から復元する効果音音量。都市セーブとは独立。
	double renderDistance = RenderDistance::kDefault; ///< 建物・木の追加描画距離 [m]。0 は従来の表示。
	bool   lowSpec = false; ///< Opt-in reduced-resolution 3D without MSAA or cast shadows.
	bool   sandboxMode = true;   ///< サンドボックスモード（道路形状を自由に編集）
	bool   isNewGame   = true;   ///< true: 新規生成、false: セーブロード
	bool   captureFirstPerson = false; ///< 同じ街角を歩行目線で比較撮影する。
	bool   captureTransportObjects = false;
	bool   captureTransport = false; ///< Streets, railway, bridges and full-screen map render review.
	bool   auditRoadIntegrity = false; ///< 道路生成の段階データを出力して終了（画像なし）
	bool   captureConstruction = false; ///< 工事の実画面と自動撤去を検証
	bool   captureRoadPlanUx = false; ///< 道路計画の入力状態を再現して実画面を撮影する
	bool   captureCityRenders = false; ///< true: 提出用に実ゲームレンダを自動撮影して終了
	String captureFolder = U"city_generation"; ///< Screenshot 下の撮影先。
	bool   captureRoadRenders = false; ///< true: 幹線交差点を近景・上空から自動検証
	bool   syncRoads = false; ///< Synchronous road-cache reference for controlled benchmarks.
	bool   benchmarkNavigation = false; ///< Repeat walking transitions, turns and map jumps.
	bool   benchmarkStreaming = false; ///< Deterministic camera flight with per-frame CPU measurements.
	FilePath playtestCommands; ///< 明示指定されたローカル操作コマンドだけを読む。
	bool   playtest = false; ///< Opt-in live input/state/frame diagnostics without image output.
	bool   syncTerrain = false; ///< Reference path for terrain streaming A/B measurements.
	int    captureNode = -1; ///< Optional exact junction for render regression on saved maps.
	int    inspectNode = -1; ///< 起動後に指定した交差点へカメラを移動
	String saveName;             ///< セーブ名（ロード時のみ使用）
};

/// @brief SceneManager の型エイリアス
using App = SceneManager<SceneState, SceneData>;

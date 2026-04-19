#pragma once
#include "RoadTypes.hpp"

class RoadNetwork;

/// @brief 案内標識のレイアウト定数（auto-layout + WYSIWYG 変換で共有）
/// @details renderSign / buildDirectionDistance_Elements / buildDirectionArrow_Elements で
///   同じ値を参照する。
namespace GuideSignLayout
{
	// DirectionDistance（106: 左↑ + 右に地名 km）
	namespace DirectionDistance
	{
		constexpr float ArrowColRatio   = 0.22f;   ///< 矢印カラム幅比（盤面幅に対して）
		constexpr float RowHeightM      = 0.70f;   ///< 1行高さ [m]（実物基準）
		constexpr float PaddingM        = 0.15f;   ///< 上下パディング [m]（実物基準）
	}
	// DirectionArrow（108: 十字）
	namespace DirectionArrow
	{
		constexpr float ArrowTopY       = 0.26f;   ///< 直進矢印の先端 Y 比
		constexpr float ArrowBotY       = 0.93f;   ///< 矢印末端 Y 比
		constexpr float ArmCy           = 0.575f;  ///< 水平アーム Y 比
		constexpr float LeftArmEndX     = 0.28f;   ///< 左アーム先端 X 比（inset 基準 +）
		constexpr float UpTextCy        = 0.12f;   ///< 上部地名 Y 比
		constexpr float UpTextH         = 0.13f;   ///< 上部地名高さ比
	}
}

/// @brief 案内標識（方面系: 106 / 108の2）
/// @details plan/21_guide_sign_spec.md 参照。
///   RoadSign（規制標識）とは別系統で、板サイズ可変・複数エントリを扱う。
namespace GuideSign
{
	// ===== 寸法定数 =====

	constexpr float  kPoleHeight_m     = 3.5f;   ///< ポール高さ既定値 [m]
	constexpr float  kSideMargin_m     = 0.5f;   ///< 路肩外側へのマージン [m]
	constexpr double kBoardRowHeight_m = 0.35;   ///< 1 行分の高さ [m]
	constexpr double kBoardPadding_m   = 0.15;   ///< 上下パディング [m]
	constexpr double kBoardCharWidth_m = 0.45;   ///< 文字幅推定 [m/文字]
	constexpr double kBoardMinWidth_m  = 2.0;    ///< 看板最小幅 [m]

	// 自動配置パラメータ（106）
	constexpr float  kAutoPlaceArcOffset_m   = 150.0f;  ///< ノード端からの距離 [m]
	constexpr int    kMaxEntriesPerPanel     = 3;       ///< 最大エントリ数
	constexpr float  kMaxSearchDistance_km   = 50.0f;   ///< 探索打ち切り距離
	constexpr int    kMaxSearchDepth         = 30;      ///< 探索打ち切り深度（エッジ本数）

	// 自動配置パラメータ（108の2）
	constexpr float  kArrowPlaceArcOffset_m  = 100.0f;  ///< 交差点手前距離 [m]
	constexpr int    kArrowMinAttachments    = 3;       ///< 交差点とみなす接続数

	// ===== 矢印形状定数 [px] =====
	constexpr double kArrowShaftWidth  = 180.0 * 0.18;   ///< シャフト幅
	constexpr double kArrowHeadWidth   = 180.0 * 0.36;   ///< 矢頭底辺幅
	constexpr double kArrowHeadHeight  = 180.0 * 0.23;   ///< 矢頭高さ

	// ===== API =====

	/// @brief 板メッシュの寸法（m 単位）
	struct BoardSize { double width; double height; };

	/// @brief 上書き指定を加味した板寸法を返す（elements の内容から推定）
	BoardSize computeBoardSizeFor(const GuideSignPlacement& g);

	/// @brief プロシージャル平面四角形のメッシュ（看板本体）
	/// @details ローカル座標: 原点=板中心、X 右・Y 上・Z 前面法線(+Z)。
	///   UV: 左上=(0,0), 右下=(1,1)。
	MeshData CreateBoardMesh(double width, double height);

	/// @brief 案内標識用ポール（2 本柱フレーム構造）を OBJ から読み込む
	/// @details 単位高さ 1.0 で設計されており、実行時に Scale(1, poleHeight, 1) で伸縮する
	///   形状詳細は chore/generate_guide_pole_obj.py と assets/signs/guide/guide_pole.obj 参照
	MeshData CreatePoleMesh();

	/// @brief ポール OBJ に対応するメタデータ（assets/signs/guide/guide_pole.json）
	/// @details OBJ は実寸 [m] で設計され、スケールされない。
	///   board は看板中心のポール基底からの相対 3D オフセット。
	///
	///   座標系（ポールローカル）:
	///     X: 道路に対して横方向（車線と直交）
	///     Y: 垂直（上向き）。ポール接地 = 0
	///     Z: 道路の長手方向（driver の進行方向と逆＝手前に伸ばす軸）
	struct PoleMetadata
	{
		float offsetX = 0.0f;    ///< 看板中心の横オフセット [m]
		float offsetY = 3.3f;    ///< 看板中心の高さ [m]（接地基準）
		float offsetZ = 0.06f;   ///< 看板中心の長手方向オフセット [m]
	};

	/// @brief ポールメタデータを JSON からロードする（シングルトンキャッシュ）
	const PoleMetadata& poleMetadata();

	/// @brief poleMetadata() のキャッシュを破棄（次回呼び出し時に JSON から再ロード）
	void reloadPoleMetadata();

	/// @brief 案内標識テクスチャのピクセルサイズを決める（m 単位 → px）
	Size guideSignTexSize(double widthM, double heightM);

	/// @brief 案内標識の内容を 2D キャンバスに描画する（合成）
	/// @details 呼び出し前に ScopedRenderTarget2D 等でターゲットを設定すること。
	void renderContents(const GuideSignPlacement& g, const Size& texSize,
	                    const Font& fontJa, const Font& fontNum);

	/// @brief 1エッジ分の自動生成 DirectionDistance 標識を作る
	Array<GuideSignPlacement> InferAutoForEdge(const RoadEdge& edge, const RoadNetwork& network);
}

#pragma once
#include "../gen/MapGenerator.hpp"
#include "../ui/Camera.hpp"
#include "../ui/PanelManager.hpp"
#include "../road/RoadNetwork.hpp"
#include "../world/World.hpp"
#include "../asset/AssetRegistrar.hpp"

/// @brief 画面右上に表示するミニマップレンダラ
class MinimapRenderer
{
public:
	// 地形と道路の事前生成テクスチャを持ち、小マップ表示と拡大パネル表示の両方を担当する。
	/// @brief 地形テクスチャを生成する（ロード完了後に1回呼ぶ）
	void buildTerrainTexture(const World& world);

	/// @brief 道路オーバーレイテクスチャを全道路から再構築する
	void updateRoadOverlay(const RoadNetwork& network, const World& world);

	/// @brief 指定ノード周辺の道路オーバーレイだけ差分更新する
	void updateRoadOverlayAround(const Array<int>& dirtyNodeIds,
	                              const RoadNetwork& network);

	/// @brief 入力処理（クリックで拡大パネル表示）。毎フレーム render の前に呼ぶ
	void update(PanelManager& panels);

	/// @brief 右上の小さいミニマップを描画する（毎フレーム）
	void render(const GameCamera& camera,
	            const Array<MapGenerator::Settlement>& settlements) const;

	/// @brief 拡大パネル内のコンテンツを描画する
	void drawExpandedPanel(PanelManager& panels,
	                       const GameCamera& camera,
	                       const Array<MapGenerator::Settlement>& settlements) const;

private:
	static constexpr int kMapSize     = 256;  ///< テクスチャ解像度 [px]
	static constexpr int kDisplaySize = 200;  ///< 縮小時の表示サイズ [px]
	static constexpr int kMargin      = 12;   ///< 画面端からのマージン [px]

	DynamicTexture m_terrainTex;  ///< 地形テクスチャ
	DynamicTexture m_roadTex;     ///< 道路オーバーレイテクスチャ
	Image          m_roadImage;   ///< 道路オーバーレイ画像（差分更新用）
	Font           m_font = FontAsset(Asset::CJK14);  ///< 地名用フォント

	/// @brief ワールド範囲（テクスチャ生成時に確定）
	float m_worldMinX = 0, m_worldMinZ = 0;
	float m_worldMaxX = 0, m_worldMaxZ = 0;

	/// @brief 指定描画領域に対してワールド座標 → 画面座標に変換
	Vec2 worldToScreen(float wx, float wz, const RectF& rect) const;

	/// @brief ワールド座標 → テクスチャピクセル座標に変換
	Point worldToPixel(float wx, float wz) const;

	/// @brief 右上の小さいミニマップの描画領域を返す
	RectF smallRect() const;

	/// @brief マップ＋オーバーレイ＋地名＋カメラを描画する共通処理
	void drawMapContent(const RectF& rect,
	                    const GameCamera& camera,
	                    const Array<MapGenerator::Settlement>& settlements,
	                    bool showLabels) const;
};

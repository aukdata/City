#pragma once
#include "../gen/MapGenerator.hpp"
#include "../ui/Camera.hpp"
#include "../ui/PanelManager.hpp"
#include "../road/RoadNetwork.hpp"
#include "../world/World.hpp"

/// @brief 画面右上に表示するミニマップレンダラ
class MinimapRenderer
{
public:
	MinimapRenderer();

	/// @brief 地形テクスチャを生成する（ロード完了後に1回呼ぶ）
	void buildTerrainTexture(const World& world);

	/// @brief 道路オーバーレイテクスチャを更新する（道路変更時に呼ぶ）
	void updateRoadOverlay(const RoadNetwork& network, const World& world);

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
	Font           m_font;        ///< 地名用フォント

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

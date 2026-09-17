#pragma once
#include "../gen/MapGenerator.hpp"
#include "../ui/Camera.hpp"
#include "../ui/TownBillboards.hpp"

/// @brief 俯瞰では街の上へ地名を投影し、徒歩・運転では現在地を上部へ固定する。
class PlaceNameRenderer
{
	mutable Array<TownBillboards::Label> m_labels;
public:
	/// @brief 描画したラベルの検証用スナップショット。
	const Array<TownBillboards::Label>& labels() const { return m_labels; }
	void render(const Array<MapGenerator::Settlement>& settlements,const GameCamera& camera, const World& world, const Array<RectF>& hudBounds) const;
};

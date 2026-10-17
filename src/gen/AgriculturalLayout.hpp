#pragma once
#include "GenerationSettings.hpp"
#include "../world/World.hpp"
#include "../road/RoadNetwork.hpp"
#include "../railway/TrainNetwork.hpp"

/// @brief 耕作適地に既存道と両端接続する農道を計画し、民家の近くにまとまったほ場を生成する。
namespace AgriculturalLayout
{
	constexpr uint32 kManagedField=0x80000000u;
	/// @brief 密度計算に使う集落中心と、町割の参照情報。農道とほ場の向きは実際の接道先に従う。
	struct Frame
	{
		Vec2 center{0,0};
		Vec2 origin{0,0};
		Vec2 axisX{1,0}, axisZ{0,1};
		Vec2 plotSize{GenerationSettings::get().agriculture_plotLength,GenerationSettings::get().agriculture_plotWidth};
		Vec2 plannedHalfExtent{0,0}; ///< 計画的に造成したニュータウンの範囲。内側の空き地には田畑を作らない。
		Vec2 townHalfExtent{0,0}; ///< 町割の範囲。内側では道路沿いの奥へ何列も田を伸ばさない。
		Vec2 worldPoint(Vec2 point) const { return origin+axisX*point.x+axisZ*point.y; }
		Vec2 localPoint(Vec2 point) const { const Vec2 delta=point-origin; return {delta.dot(axisX),delta.dot(axisZ)}; }
	};
	struct Stats
	{
		int candidates=0, fields=0, paddies=0, tracks=0, drains=0, disconnected=0, homes=0, connections=0;
	};
	/// @brief 建物より先に、一般道路に接続する農道と沿道の民家を確保する。
	Stats prepare(World& world,RoadNetwork& network,uint64 seed,const Array<Frame>& frames={},const TrainNetwork* railway=nullptr);
	Stats generate(World& world,RoadNetwork& network,uint64 seed,const Array<Frame>& frames={},bool prepareAccess=true,const TrainNetwork* railway=nullptr);
}

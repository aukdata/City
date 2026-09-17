#pragma once
#include "RoadTypes.hpp"

/// @brief 共通路盤上の交通断面。材質は部品、走行資格・方向は車線が所有する。
namespace TransportCrossSection
{
	inline constexpr float kTrackSpacing = 3.6f;
	inline constexpr float kRailTop = .17f;

	/// @brief バラスト・スラブ、または車道を併設した軌道断面を設定する。
	inline void railway(RoadEdge& edge, bool doubleTrack = true, bool slab = false, bool street = false)
	{
		edge.parts.clear(); edge.lanes.clear();
		const float halfWidth = street ? 7.4f : (doubleTrack ? 4.2f : 2.4f);
		RoadPart bed; bed.type = RoadPartType::Roadbed;
		bed.defId = street ? U"roadbed_asphalt" : slab ? U"roadbed_slab" : U"roadbed_ballast";
		bed.offsetA_L = bed.offsetB_L = -halfWidth; bed.offsetA_R = bed.offsetB_R = halfWidth;
		edge.parts << bed;
		const auto append = [&](float center, LaneDir direction, LaneType type, bool both)
		{
			Lane lane; lane.type = type; lane.dir = direction; lane.bidirectional = both;
			lane.nominalWidth = 3.2f;
			lane.offsetA_L = lane.offsetB_L = center - 1.6f;
			lane.offsetA_R = lane.offsetB_R = center + 1.6f;
			edge.lanes << lane;
		};
		if (street) { append(-5.4f, LaneDir::Forward, LaneType::Normal, false); }
		append(doubleTrack ? -kTrackSpacing * .5f : 0, LaneDir::Forward, LaneType::Rail, !doubleTrack);
		if (doubleTrack) { append(kTrackSpacing * .5f, LaneDir::Backward, LaneType::Rail, false); }
		if (street) { append(5.4f, LaneDir::Backward, LaneType::Normal, false); }
		edge.laneVehicles.resize(edge.lanes.size());
	}

	/// @brief 併用道路ではレール頭部を舗装面に埋め込む。
	inline float railTop(const RoadEdge& edge) { return edge.hasRoadLanes() ? .012f : kRailTop; }

	/// @brief 進行方向に使える軌道。普通車線を代替として返すことはない。
	inline int railLane(const RoadEdge& edge, bool forward)
	{
		const auto lanes = edge.openLanes(forward ? LaneDir::Forward : LaneDir::Backward, 0, TransportMode::Rail);
		return lanes.isEmpty() ? -1 : lanes.front();
	}

	/// @brief 路盤中心の弧長から実際の軌道中心へ投影する。描画と列車姿勢で共用する。
	inline Vec3 lanePosition(const RoadEdge& edge, const CubicBezier& curve, float arc, int index)
	{
		const float fraction = curve.totalLength > 0 ? Clamp(arc / curve.totalLength, 0.0f, 1.0f) : 0;
		return curve.positionAt(arc) + tangentToRight(curve.tangentAt(arc)) * edge.lanes[index].centerAt(fraction);
	}
}

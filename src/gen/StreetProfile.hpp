#pragma once
#include "../road/RoadNetwork.hpp"

/// @brief 自動生成専用の断面。地域の役割を車道・歩道・路肩・標示へ一貫して反映する。
namespace GeneratedStreet
{
	enum class Role : uint8 { FarmAccess, Village, Local, OneWay, ResidentialWalkways, Collector, MainArterial, Regional };
	struct Profile
	{
		int lanes=2;
		float laneWidth=2.75f;
		float speed=30;
		float walkwayLeft=0, walkwayRight=0;
		float shoulder=0.35f;
		LineType center=LineType::None;
		bool edgeLines=false;
		bool arterial=false;
	};

	inline Profile describe(Role role)
	{
		Profile result;
		switch (role)
		{
		case Role::FarmAccess: result.laneWidth=2.2f; result.speed=20; break;
		case Role::Village: result.laneWidth=2.5f; break;
		case Role::Local: break;
		case Role::OneWay: result.lanes=1; result.laneWidth=3.25f; result.shoulder=.75f; result.edgeLines=true; break;
		case Role::ResidentialWalkways: result.walkwayLeft=result.walkwayRight=1.8f; break;
		case Role::Collector:
			result.laneWidth=3.0f; result.speed=40; result.walkwayLeft=result.walkwayRight=2.0f;
			result.center=LineType::DashedWhite; result.arterial=true; break;
		case Role::MainArterial:
			result.lanes=4; result.laneWidth=3.25f; result.speed=50; result.walkwayLeft=result.walkwayRight=3.0f;
			result.center=LineType::SolidWhite; result.arterial=true; break;
		case Role::Regional:
			result.laneWidth=3.0f; result.speed=50; result.shoulder=.75f;
			result.center=LineType::DashedWhite; result.edgeLines=true; result.arterial=true; break;
		}
		return result;
	}

	/// @brief 無区画の双方向道路でも車両経路は左右2本で保持する。見た目の2車線標示とは区別する。
	inline void apply(RoadEdge& edge, Profile profile, bool reverse=false)
	{
		edge.roadType=profile.arterial ? RoadType::Arterial : RoadType::LocalRoad;
		edge.speedLimit=profile.speed;
		edge.lanes=RoadNetwork::buildDefaultLanes(profile.lanes,edge.roadType);
		const float halfRoadbed=profile.lanes*profile.laneWidth*.5f;
		for (int index=0;index<profile.lanes;++index)
		{
			auto& lane=edge.lanes[index];
			lane.offsetA_L=lane.offsetB_L=-halfRoadbed+index*profile.laneWidth;
			lane.offsetA_R=lane.offsetB_R=lane.offsetA_L+profile.laneWidth;
			lane.nominalWidth=profile.laneWidth;
			if (profile.lanes==1) { lane.dir=reverse ? LaneDir::Backward : LaneDir::Forward; }
			const int center=profile.lanes/2;
			lane.lineLeft=index==0 ? (profile.edgeLines ? LineType::SolidWhite : LineType::None)
				: (index==center ? profile.center : LineType::DashedWhite);
			lane.lineRight=index==profile.lanes-1 ? (profile.edgeLines ? LineType::SolidWhite : LineType::None)
				: (index+1==center ? profile.center : LineType::DashedWhite);
		}
		edge.parts.clear();
		const auto part=[&](RoadPartType type, float left, float width, StringView definition)
		{
			RoadPart value; value.type=type; value.defId=definition; value.build=BuildState::Built;
			value.offsetA_L=value.offsetB_L=left; value.offsetA_R=value.offsetB_R=left+width;
			edge.parts << value;
		};
		part(RoadPartType::Roadbed,-halfRoadbed,halfRoadbed*2,U"roadbed_asphalt");
		for (const int side : {-1,1})
		{
			const float walkway=side<0 ? profile.walkwayLeft : profile.walkwayRight;
			float distance=halfRoadbed;
			const auto strip=[&](RoadPartType type,float width,StringView definition)
			{
				part(type,side<0 ? -distance-width : distance,width,definition); distance+=width;
			};
			strip(RoadPartType::Shoulder,profile.shoulder,U"roadbed_asphalt");
			strip(RoadPartType::RoadsideGutter,.32f,U"roadside_gutter_concrete");
			if (walkway>0)
			{
				strip(RoadPartType::Curb,.18f,U"curb_concrete");
				strip(RoadPartType::Sidewalk,walkway,U"sidewalk_tile");
			}
		}
		RoadPart pole; pole.type=RoadPartType::UtilityPole; pole.defId=U"utility_pole_concrete";
		pole.offsetA_L=pole.offsetA_R=pole.offsetB_L=pole.offsetB_R=halfRoadbed+profile.shoulder+.32f+(profile.walkwayRight>0 ? .65f : .28f);
		pole.placement=RoadPartPlacement::RepeatAlongEdge; pole.envelopeRole=RoadPartEnvelopeRole::RoadOwnedObject;
		pole.repeatSpacing=profile.arterial ? 38.0f : 32.0f; pole.repeatJitter=2;
		pole.useDefinitionRepeatSpacing=false; pole.useDefinitionRepeatJitter=false; pole.build=BuildState::Built;
		edge.parts << pole;
	}
}

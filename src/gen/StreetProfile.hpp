#pragma once
#include "GenerationSettings.hpp"
#include "../road/RoadNetwork.hpp"

/// @brief 自動生成専用の断面。地域の役割を車道・歩道・路肩・標示へ一貫して反映する。
namespace GeneratedStreet
{
	enum class Role : uint8 { FarmAccess, Village, Local, OneWay, ResidentialWalkways, Collector, MainArterial, Regional, Mountain };
	struct Profile
	{
		int lanes=0;
		float laneWidth=0;
		float speed=0;
		float walkwayLeft=0, walkwayRight=0;
		float shoulder=0;
		LineType center=LineType::None;
		bool edgeLines=false;
		bool arterial=false;
	};

	inline Profile describe(Role role)
	{
		static constexpr LineType markings[]{LineType::None,LineType::DashedWhite,LineType::SolidWhite,LineType::SolidYellow};
		Profile result;
		const auto& config = GenerationSettings::get();
		switch (role)
		{
		case Role::FarmAccess:
			result.lanes=config.streetProfiles_farmAccess_lanes; result.laneWidth=config.streetProfiles_farmAccess_laneWidth; result.speed=config.streetProfiles_farmAccess_speed;
			result.walkwayLeft=result.walkwayRight=config.streetProfiles_farmAccess_walkway; result.shoulder=config.streetProfiles_farmAccess_shoulder;
			result.center=markings[config.streetProfiles_farmAccess_center]; result.edgeLines=config.streetProfiles_farmAccess_edgeLines!=0; result.arterial=config.streetProfiles_farmAccess_arterial!=0; break;
		case Role::Village:
			result.lanes=config.streetProfiles_village_lanes; result.laneWidth=config.streetProfiles_village_laneWidth; result.speed=config.streetProfiles_village_speed;
			result.walkwayLeft=result.walkwayRight=config.streetProfiles_village_walkway; result.shoulder=config.streetProfiles_village_shoulder;
			result.center=markings[config.streetProfiles_village_center]; result.edgeLines=config.streetProfiles_village_edgeLines!=0; result.arterial=config.streetProfiles_village_arterial!=0; break;
		case Role::Local:
			result.lanes=config.streetProfiles_local_lanes; result.laneWidth=config.streetProfiles_local_laneWidth; result.speed=config.streetProfiles_local_speed;
			result.walkwayLeft=result.walkwayRight=config.streetProfiles_local_walkway; result.shoulder=config.streetProfiles_local_shoulder;
			result.center=markings[config.streetProfiles_local_center]; result.edgeLines=config.streetProfiles_local_edgeLines!=0; result.arterial=config.streetProfiles_local_arterial!=0; break;
		case Role::OneWay:
			result.lanes=config.streetProfiles_oneWay_lanes; result.laneWidth=config.streetProfiles_oneWay_laneWidth; result.speed=config.streetProfiles_oneWay_speed;
			result.walkwayLeft=result.walkwayRight=config.streetProfiles_oneWay_walkway; result.shoulder=config.streetProfiles_oneWay_shoulder;
			result.center=markings[config.streetProfiles_oneWay_center]; result.edgeLines=config.streetProfiles_oneWay_edgeLines!=0; result.arterial=config.streetProfiles_oneWay_arterial!=0; break;
		case Role::ResidentialWalkways:
			result.lanes=config.streetProfiles_residentialWalkways_lanes; result.laneWidth=config.streetProfiles_residentialWalkways_laneWidth; result.speed=config.streetProfiles_residentialWalkways_speed;
			result.walkwayLeft=result.walkwayRight=config.streetProfiles_residentialWalkways_walkway; result.shoulder=config.streetProfiles_residentialWalkways_shoulder;
			result.center=markings[config.streetProfiles_residentialWalkways_center]; result.edgeLines=config.streetProfiles_residentialWalkways_edgeLines!=0; result.arterial=config.streetProfiles_residentialWalkways_arterial!=0; break;
		case Role::Collector:
			result.lanes=config.streetProfiles_collector_lanes; result.laneWidth=config.streetProfiles_collector_laneWidth; result.speed=config.streetProfiles_collector_speed;
			result.walkwayLeft=result.walkwayRight=config.streetProfiles_collector_walkway; result.shoulder=config.streetProfiles_collector_shoulder;
			result.center=markings[config.streetProfiles_collector_center]; result.edgeLines=config.streetProfiles_collector_edgeLines!=0; result.arterial=config.streetProfiles_collector_arterial!=0; break;
		case Role::MainArterial:
			result.lanes=config.streetProfiles_mainArterial_lanes; result.laneWidth=config.streetProfiles_mainArterial_laneWidth; result.speed=config.streetProfiles_mainArterial_speed;
			result.walkwayLeft=result.walkwayRight=config.streetProfiles_mainArterial_walkway; result.shoulder=config.streetProfiles_mainArterial_shoulder;
			result.center=markings[config.streetProfiles_mainArterial_center]; result.edgeLines=config.streetProfiles_mainArterial_edgeLines!=0; result.arterial=config.streetProfiles_mainArterial_arterial!=0; break;
		case Role::Regional:
			result.lanes=config.streetProfiles_regional_lanes; result.laneWidth=config.streetProfiles_regional_laneWidth; result.speed=config.streetProfiles_regional_speed;
			result.walkwayLeft=result.walkwayRight=config.streetProfiles_regional_walkway; result.shoulder=config.streetProfiles_regional_shoulder;
			result.center=markings[config.streetProfiles_regional_center]; result.edgeLines=config.streetProfiles_regional_edgeLines!=0; result.arterial=config.streetProfiles_regional_arterial!=0; break;
		case Role::Mountain:
			result.lanes=config.streetProfiles_mountain_lanes; result.laneWidth=config.streetProfiles_mountain_laneWidth; result.speed=config.streetProfiles_mountain_speed;
			result.walkwayLeft=result.walkwayRight=config.streetProfiles_mountain_walkway; result.shoulder=config.streetProfiles_mountain_shoulder;
			result.center=markings[config.streetProfiles_mountain_center]; result.edgeLines=config.streetProfiles_mountain_edgeLines!=0; result.arterial=config.streetProfiles_mountain_arterial!=0; break;
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
			strip(RoadPartType::RoadsideGutter,GenerationSettings::get().streetProfiles_gutterWidth,U"roadside_gutter_concrete");
			if (walkway>0)
			{
				strip(RoadPartType::Curb,GenerationSettings::get().streetProfiles_curbWidth,U"curb_concrete");
				strip(RoadPartType::Sidewalk,walkway,U"sidewalk_tile");
			}
		}
		RoadPart pole; pole.type=RoadPartType::UtilityPole; pole.defId=U"utility_pole_concrete";
		pole.offsetA_L=pole.offsetA_R=pole.offsetB_L=pole.offsetB_R=halfRoadbed+profile.shoulder+GenerationSettings::get().streetProfiles_gutterWidth+(profile.walkwayRight>0 ? GenerationSettings::get().streetProfiles_walkwayPoleOffset : GenerationSettings::get().streetProfiles_shoulderPoleOffset);
		pole.placement=RoadPartPlacement::RepeatAlongEdge; pole.envelopeRole=RoadPartEnvelopeRole::RoadOwnedObject;
		pole.repeatSpacing=profile.arterial ? GenerationSettings::get().streetProfiles_arterialPoleSpacing : GenerationSettings::get().streetProfiles_localPoleSpacing; pole.repeatJitter=GenerationSettings::get().streetProfiles_poleJitter;
		pole.useDefinitionRepeatSpacing=false; pole.useDefinitionRepeatJitter=false; pole.build=BuildState::Built;
		edge.parts << pole;
	}
}

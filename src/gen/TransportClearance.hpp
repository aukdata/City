#pragma once
#include "GenerationSettings.hpp"
#include "../road/RoadNetwork.hpp"
#include "../world/World.hpp"

/// @brief Road elevation and width samples indexed in 32 m cells for railway crossings.
class TransportClearance
{
public:
	TransportClearance(const RoadNetwork* roads,const World& world)
	{
		if (!roads) { return; }
		for (const auto& edge : roads->edges())
		{
			if (edge.id<0 || !edge.hasRoadLanes()) { continue; }
			const auto curve=roads->getBezier(edge.id); if (!curve) { continue; }
			const int count=Max(1,static_cast<int>(std::ceil(curve->totalLength/6)));
			for (int i=0;i<=count;++i)
			{
				const Vec3 point=curve->positionAt(curve->totalLength*i/count);
				const double elevation=edge.useElevation ? point.y : world.sampleHeight(static_cast<float>(point.x),static_cast<float>(point.z));
				m_samples[key(static_cast<int>(point.x/32),static_cast<int>(point.z/32))] << Sample{{point.x,point.z},elevation,edge.totalWidth()*.5+GenerationSettings::get().crossings_railRoadLateralClearance};
				m_maximumRadius=Max(m_maximumRadius,edge.totalWidth()*.5+GenerationSettings::get().crossings_railRoadLateralClearance);
			}
		}
	}
	double minimumRailHeight(Vec2 point,double margin=0) const { return interval(point,margin).second; }
	double maximumUnderpassHeight(Vec2 point,double margin=0) const { return interval(point,margin).first; }
	std::pair<double,double> interval(Vec2 point,double margin=0) const
	{
		double result=-1e9,under=1e9;
		const int x=static_cast<int>(point.x/32),z=static_cast<int>(point.y/32);
		const int range=static_cast<int>(Ceil((margin + m_maximumRadius) / 32));
		for (int dz=-range;dz<=range;++dz)
		{
			for (int dx=-range;dx<=range;++dx)
			{
				const auto found=m_samples.find(key(x+dx,z+dz)); if (found==m_samples.end()) { continue; }
				for (const auto& sample : found->second)
				{
					if (point.distanceFromSq(sample.position)<Square(sample.radius+margin)) { result=Max(result,sample.height+GenerationSettings::get().crossings_railOverRoadClearance); under=Min(under,sample.height-GenerationSettings::get().crossings_railUnderRoadClearance); }
				}
			}
		}
		return {under,result};
	}
private:
	double m_maximumRadius=0;
	struct Sample { Vec2 position; double height; double radius; };
	static int64 key(int x,int z) { return static_cast<int64>(x)*0x100000000LL+static_cast<uint32>(z); }
	HashTable<int64,Array<Sample>> m_samples;
};

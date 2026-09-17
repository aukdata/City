#include "TransportLandscape.hpp"
#include "../world/World.hpp"
#include "../road/RoadEnvironment.hpp"

Array<ParcelGeometry::Quad> TransportLandscape::footprints(const TrainNetwork& railway,const RoadNetwork& roads,const World& world)
{
	auto result=RailwaySite::landscapeFootprints(railway,world);
	for (const auto& edge : roads.edges())
	{
		// Ground roads already supply their complete roadbed footprint through terrain cutting.
		if (edge.id<0 || !edge.hasRoadLanes() || !edge.isRoadbedBuilt() || (!edge.useElevation && !edge.tunnel)) { continue; }
		const auto curve=roads.getBezier(edge.id);if (!curve) { continue; }
		const int count=Max(1,static_cast<int>(Ceil(curve->totalLength/8)));
		for (int i=0;i<count;++i)
		{
			const Vec3 a=curve->positionAt(curve->totalLength*i/count),b=curve->positionAt(curve->totalLength*(i+1)/count);
			if (RoadEnvironment::coveredAt(world,(a+b)*.5,true,RoadEnvironment::kRoadTunnelCrown)) { continue; }
			Vec3 along=b-a;along.y=0;const double length=along.length();if (length<.01) { continue; }along/=length;
			result<<RailwaySite::rectangle({a,along,{along.z,0,-along.x}},-edge.totalWidth()*.5-.5,edge.totalWidth()*.5+.5,-.5,length+.5);
		}
	}
	return result;
}

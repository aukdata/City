#include "GenerationSettings.hpp"
#include "SettlementFringe.hpp"
#include "StreetProfile.hpp"
#include "../debug/DebugLog.hpp"

namespace SettlementFringe
{
	namespace
	{
		
		

		Vec2 horizontal(Vec3 point) { return {point.x,point.z}; }

		bool fitsTerrain(const World& world, Line line)
		{
			const int steps=Max(2,static_cast<int>(Ceil(line.length()/GenerationSettings::get().fringe_terrainStep)));
			double previous=world.sampleHeight(static_cast<float>(line.begin.x),static_cast<float>(line.begin.y));
			for (int step=0;step<=steps;++step)
			{
				const Vec2 point=line.begin.lerp(line.end,static_cast<double>(step)/steps);
				const double height=world.sampleHeight(static_cast<float>(point.x),static_cast<float>(point.y));
				if (point.x<0 || point.y<0 || point.x>=WORLD_SIZE || point.y>=WORLD_SIZE
					|| height<world.waterSurfaceHeight(point.x,point.y)+GenerationSettings::get().fringe_waterFreeboard
					|| Abs(height-previous)>line.length()/steps*GenerationSettings::get().fringe_maximumGrade) { return false; }
				previous=height;
			}
			return true;
		}

		/// @brief 未分割の交差や近接した平行道路を作らない。共有する始終点だけを許す。
		bool clearOfRoads(const RoadNetwork& network, Line proposed, int startNode, int endNode)
		{
			for (const auto& edge : network.edges())
			{
				if (edge.id<0) { continue; }
				const auto curve=network.getBezier(edge.id);
				if (!curve) { continue; }
				const int steps=Max(2,static_cast<int>(Ceil(curve->totalLength/GenerationSettings::get().fringe_terrainStep)));
				for (int step=0;step<=steps;++step)
				{
					const Vec2 point=horizontal(curve->positionAt(curve->totalLength*step/steps));
					const bool sharedStart=edge.nodeA==startNode || edge.nodeB==startNode;
					const bool sharedEnd=edge.nodeA==endNode || edge.nodeB==endNode;
					if ((sharedStart && point.distanceFrom(proposed.begin)<GenerationSettings::get().fringe_sharedNodeRadius)
						|| (sharedEnd && point.distanceFrom(proposed.end)<GenerationSettings::get().fringe_sharedNodeRadius)) { continue; }
					const Vec2 span=proposed.end-proposed.begin;
					const double t=Clamp((point-proposed.begin).dot(span)/span.lengthSq(),0.0,1.0);
					if (point.distanceFrom(proposed.begin+span*t)<GenerationSettings::get().fringe_roadClearance) { return false; }
				}
			}
			return true;
		}
	}

	void generate(MapGenerator::Settlement& settlement, const World& world, RoadNetwork& network)
	{
		auto& plan=settlement.plan;
		if (plan.origin==UrbanMorphology::Origin::Rural) { return; }
		plan.fringeStreets.clear();
		const auto local=[&](Vec2 point)
		{
			const Vec2 delta=point-settlement.center;
			return Vec2{delta.dot(settlement.gridAxisX),delta.dot(settlement.gridAxisZ)};
		};
		const auto global=[&](Vec2 point)
		{
			return settlement.center+settlement.gridAxisX*point.x+settlement.gridAxisZ*point.y;
		};
		const auto record=[&](Vec2 a,Vec2 b) { plan.fringeStreets << Line{local(a),local(b)}; };
		const auto position=[&](Vec2 point)
		{
			return Vec3{point.x,world.sampleHeight(static_cast<float>(point.x),static_cast<float>(point.y)),point.y};
		};
		// Existing regional streets carry ribbon development beyond the old town limit.
		const double kRibbonDepth=plan.scale==0 ? GenerationSettings::get().fringe_cityRibbonDepth : GenerationSettings::get().fringe_townRibbonDepth;
		for (const auto& edge : network.edges())
		{
			if (edge.id<0 || edge.useElevation || edge.roadType!=RoadType::Arterial) { continue; }
			const auto curve=network.getBezier(edge.id);
			if (!curve) { continue; }
			const int steps=Max(1,static_cast<int>(Ceil(curve->totalLength/GenerationSettings::get().fringe_ribbonSampleStep)));
			for (int step=0;step<steps;++step)
			{
				const Vec2 a=horizontal(curve->positionAt(curve->totalLength*step/steps));
				const Vec2 b=horizontal(curve->positionAt(curve->totalLength*(step+1)/steps));
				const Vec2 middle=local((a+b)*.5);
				if (!UrbanMorphology::inCore(plan,middle,GenerationSettings::get().fringe_coreMargin) && UrbanMorphology::inCore(plan,middle,kRibbonDepth)) { record(a,b); }
			}
		}
		if (plan.frontageRoads) { return; }
		int neighborhoods=0;
		for (int side=0;side<4;++side)
		{
			const bool alongX=side<2;
			const double sign=side%2==0 ? -1.0 : 1.0;
			const Vec2 outward=alongX ? Vec2{0,sign} : Vec2{sign,0};
			const Vec2 along=alongX ? Vec2{1,0} : Vec2{0,1};
			const double extent=alongX ? plan.halfExtent.y : plan.halfExtent.x;
			Array<std::pair<double,int>> anchors;
			for (const auto& node : network.nodes())
			{
				if (node.id<0 || node.attachments.isEmpty()) { continue; }
				const Vec2 point=local(horizontal(node.position));
				if (Abs(point.dot(outward)-extent)<.1 && Abs(point.dot(along))<(alongX ? plan.halfExtent.x : plan.halfExtent.y)-GenerationSettings::get().fringe_cornerMargin)
				{
					anchors << std::make_pair(point.dot(along),node.id);
				}
			}
			anchors.sort_by([](const auto& a,const auto& b) { return a.first<b.first; });
			for (size_t index=0;index+2<anchors.size();index+=GenerationSettings::get().fringe_anchorStride)
			{
				int first=anchors[index].second,last=anchors[index+2].second;
				const uint64 salt=UrbanMorphology::mix(plan.salt+side*137+index*71);
				// Fewer streets continue into each outer belt; the spaces between them remain countryside.
				const int tiers=plan.scale==0 ? (salt%GenerationSettings::get().fringe_cityShallowPeriod==0 ? 1 : (salt%GenerationSettings::get().fringe_cityDeepPeriod==0 ? GenerationSettings::get().fringe_cityMaximumTiers : GenerationSettings::get().fringe_normalTiers)) : (salt%GenerationSettings::get().fringe_townShallowPeriod==0 ? 1 : GenerationSettings::get().fringe_normalTiers);
				for (int tier=0;tier<tiers;++tier)
				{
					const double depth=(GenerationSettings::get().fringe_baseDepth+static_cast<double>((salt>>(tier*4))%4)*GenerationSettings::get().fringe_depthVariation+tier*GenerationSettings::get().fringe_tierDepthIncrement)*(plan.scale==0 ? 1.0 : GenerationSettings::get().fringe_townDepthScale);
					const double skew=tier==0 ? static_cast<double>((salt>>16)%GenerationSettings::get().fringe_skewSteps)-GenerationSettings::get().fringe_skewOffset : 0;
					const Vec2 a=local(horizontal(network.getNode(first)->position));
					const Vec2 b=local(horizontal(network.getNode(last)->position));
					const Array<Vec2> points{global(a),global(a+outward*depth+along*skew),
						global(b+outward*(depth+GenerationSettings::get().fringe_secondLegDepthIncrement)+along*skew),global(b)};
					bool accepted=true;
					for (size_t piece=1;piece<points.size();++piece)
					{
						const Line line{points[piece-1],points[piece]};
						accepted &= fitsTerrain(world,line) && clearOfRoads(network,line,piece==1 ? first : -1,piece==3 ? last : -1);
					}
					if (!accepted) { break; }
					Array<int> nodes{first,network.addNode(position(points[1]),NodeType::Joint),
						network.addNode(position(points[2]),NodeType::Joint),last};
					for (size_t piece=1;piece<points.size();++piece)
					{
						const Vec3 begin=position(points[piece-1]),end=position(points[piece]);
						if (const auto id=network.addEdge(nodes[piece-1],nodes[piece],begin.lerp(end,1.0/3),begin.lerp(end,2.0/3),RoadType::LocalRoad,2))
						{
							GeneratedStreet::apply(*network.getEdge(*id),GeneratedStreet::describe(GeneratedStreet::Role::Local));
							record(points[piece-1],points[piece]);
						}
					}
					first=nodes[1]; last=nodes[2];
					++neighborhoods;
				}
			}
		}
		DBG_LOG(U"[SettlementFringe] neighborhoods={} frontageSegments={}"_fmt(neighborhoods,plan.fringeStreets.size()));
	}
}

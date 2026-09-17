#pragma once
#include "GenerationSettings.hpp"
#include "MapGenerator.hpp"
#include "RoadPathfinder.hpp"
#include "TransportClearance.hpp"
#include "RailCostProfile.hpp"
#include "RoadAlignment.hpp"
#include "../railway/RailTimetable.hpp"
#include "../railway/RailDepotBuilder.hpp"
#include "../debug/DebugLog.hpp"

/// @brief 近接駅の接続ごとに複数の地形回廊と縦断施工費を比較して線形を選ぶ。
namespace RailwayAlignment
{
	struct CandidateAudit { int variant; bool profile; double radius; double grade; };
	struct Audit { Array<CandidateAudit> candidates; };
	inline void generate(TrainNetwork& network,World& world,const Array<MapGenerator::Settlement>& towns,RoadNetwork* roads=nullptr, Audit* audit=nullptr)
	{
		const TransportClearance crossings{roads,world};
		network=TrainNetwork{};
		network.bind(roads);
		struct StationCandidate { Vec3 position; String name; };
		Array<StationCandidate> candidates;
		Array<int> stations; HashTable<int,Vec3> axes; HashTable<int,int> builtStations;
		// Candidate IDs are separate from the live graph. Only feasible corridors create stations.
		const auto buildStation=[&](int candidate)
		{
			if (!builtStations.contains(candidate))
			{
				builtStations[candidate]=network.addStation(candidates[candidate].position,candidates[candidate].name);
			}
			return builtStations[candidate];
		};
		for (const auto& town : towns)
		{
			if (!town.plan.station) { continue; }
			const Vec2 point=town.center+town.gridAxisX*town.plan.station->x+town.gridAxisZ*town.plan.station->y;
			double required=Max(world.sampleHeight(static_cast<float>(point.x),static_cast<float>(point.y))+GenerationSettings::get().railway_stationBaseElevation,crossings.minimumRailHeight(point,GenerationSettings::get().railway_stationRoadMargin)+GenerationSettings::get().railway_stationRoadClearance);
			for (int i=0;i<16;++i) for (const double distance : {GenerationSettings::get().railway_stationSurveyRadius0,GenerationSettings::get().railway_stationSurveyRadius1,GenerationSettings::get().railway_stationSurveyRadius2})
			{
				const Vec2 around=point+Vec2{Cos(i*Math::TwoPi/16),Sin(i*Math::TwoPi/16)}*distance;
				required=Max(required,crossings.minimumRailHeight(around,GenerationSettings::get().railway_stationSurveyRoadMargin)+GenerationSettings::get().railway_stationRoadClearance-distance*GenerationSettings::get().railway_stationApproachGrade);
			}
			required=Min(required,world.sampleHeight(static_cast<float>(point.x),static_cast<float>(point.y))+GenerationSettings::get().railway_stationMaximumElevation);
			const double elevation=std::ceil(required/.5)*.5;
			const int id=static_cast<int>(candidates.size());
			candidates << StationCandidate{{point.x,elevation,point.y},town.name};
			stations << id; axes[id]={town.gridAxisX.x,0,town.gridAxisX.y};
		}
		if (stations.size()<2) { return; }
		HashSet<int> connected{stations.front()}; HashSet<uint64> rejected; int lines=0,failures=0;
		const auto pairKey=[](int a,int b) { return (static_cast<uint64>(Min(a,b))<<32)|static_cast<uint32>(Max(a,b)); };
		while (connected.size()<stations.size())
		{
			int from=-1,to=-1; double nearest=1e30;
			for (const int a : stations) if (connected.contains(a)) for (const int b : stations) if (!connected.contains(b))
			{
				if (rejected.contains(pairKey(a,b))) { continue; }
				const double distance=candidates[a].position.distanceFromSq(candidates[b].position);
				if (distance<nearest) { nearest=distance; from=a; to=b; }
			}
			if (from<0)
			{
				// A sea or mountain barrier can separate railway regions without invalidating the city.
				for (const int candidate : stations)
				{
					if (connected.contains(candidate)) { continue; }
					connected.insert(candidate);
					DBG_LOG(U"[RailwaySeparateRegion] candidate={} position={} reason=no-feasible-corridor"_fmt(candidate,candidates[candidate].position));
					break;
				}
				continue;
			}
			const Vec3 start=candidates[from].position,end=candidates[to].position;
			const Vec2 delta{end.x-start.x,end.z-start.z};
			const double kStationStraightLead=GenerationSettings::get().railway_stationStraightLead;
			const Vec3 fromAxis=axes[from]*(axes[from].dot(end-start)>=0 ? 1.0 : -1.0);
			const Vec3 toAxis=axes[to]*(axes[to].dot(end-start)>=0 ? 1.0 : -1.0);
			const Vec3 startGate=start+fromAxis*kStationStraightLead,endGate=end-toAxis*kStationStraightLead;
			Array<Vec3> bestPoints; Array<double> bestHeights; double bestCost=Math::Inf; int chosen=-1;
			for (int alternative=0;alternative<5;++alternative)
			{
				const double padding=alternative<3 ? GenerationSettings::get().railway_corridorMargin : GenerationSettings::get().railway_wideCorridorMargin;
				const Vec2 offset{Max(0.0,Min(start.x,end.x)-padding),Max(0.0,Min(start.z,end.z)-padding)};
				const Vec2 upper{Min(static_cast<double>(WORLD_SIZE),Max(start.x,end.x)+padding),Min(static_cast<double>(WORLD_SIZE),Max(start.z,end.z)+padding)};
				RoadPathfinder finder; finder.setRailwayRouting(true);
				finder.setConstructionCost([&](Vec2 point,double ground)
				{
					const double t=Clamp((point-Vec2{start.x,start.z}).dot(delta)/Max(1.0,delta.lengthSq()),0.0,1.0);
					const double reference=Math::Lerp(start.y,end.y,t);
					const double coefficient=alternative==0 ? GenerationSettings::get().railway_corridorCostWeight0 : alternative==1 ? GenerationSettings::get().railway_corridorCostWeight1 : alternative==2 ? GenerationSettings::get().railway_corridorCostWeight2 : GenerationSettings::get().railway_corridorCostWeight3;
					const double water=world.waterSurfaceHeight(point.x,point.y);
					return 1+RailCostProfile::unitCost(reference-ground)*coefficient+(ground<water+1 ? GenerationSettings::get().railway_waterCorridorCost : 0.0);
				});
				finder.setup(world,offset,Max(2,static_cast<int>((upper.x-offset.x)/GenerationSettings::get().railway_routingCell)),Max(2,static_cast<int>((upper.y-offset.y)/GenerationSettings::get().railway_routingCell)),GenerationSettings::get().railway_routingCell);
				const auto path=finder.findPath(finder.worldToGrid(static_cast<float>(startGate.x),static_cast<float>(startGate.z)),finder.worldToGrid(static_cast<float>(endGate.x),static_cast<float>(endGate.z)));
				auto coarse=finder.samplePath(path,2); if (coarse.size()<2) { continue; }
				coarse.front()=startGate; coarse.back()=endGate;
				for (int pass=0;pass<24+alternative*12;++pass) { auto next=coarse; for (size_t i=1;i+1<coarse.size();++i) { next[i]=coarse[i]*.5+(coarse[i-1]+coarse[i+1])*.25; } coarse=std::move(next); }
				coarse.insert(coarse.begin(),start); coarse<<end;
				Array<Vec3> points;
				for (size_t i=0;i+1<coarse.size();++i)
				{
					const Vec3 direction=coarse[i+1]-coarse[i]; const double handle=Vec2{direction.x,direction.z}.length()/3;
					Vec3 a=i>0 ? coarse[i+1]-coarse[i-1] : axes[from]*(axes[from].dot(direction)>=0 ? 1.0 : -1.0);
					Vec3 b=i+2<coarse.size() ? coarse[i+2]-coarse[i] : axes[to]*(axes[to].dot(direction)>=0 ? 1.0 : -1.0); a.y=b.y=0;
					if (i==0) { a=b=fromAxis; } else if (i==1) { a=fromAxis; }
					if (i+2==coarse.size()) { a=b=toAxis; } else if (i+3==coarse.size()) { b=toAxis; }
					const CubicBezier curve{coarse[i],coarse[i]+a.normalized()*handle,coarse[i+1]-b.normalized()*handle,coarse[i+1]};
					const int count=Max(1,static_cast<int>(std::ceil(curve.totalLength/GenerationSettings::get().railway_profileSampleLength)));
					for (int sample=0;sample<count;++sample) { points << curve.positionAt(curve.totalLength*sample/count); }
				}
				points << end;
				if (alternative == 4)
				{
					// 駅の接線を固定した大きな曲線も、地形追従候補と同じ費用・制約で比較する。
					const double arm = Vec2{endGate.x-startGate.x,endGate.z-startGate.z}.length()/3;
					const Array<CubicBezier> corridor{
						{start,start+(startGate-start)/3,start+(startGate-start)*2/3,startGate},
						{startGate,startGate+fromAxis*arm,endGate-toAxis*arm,endGate},
						{endGate,endGate+(end-endGate)/3,endGate+(end-endGate)*2/3,end}};
					points.clear();
					for (const auto& curve : corridor)
					{
						const int count = Max(1,static_cast<int>(Ceil(curve.totalLength/GenerationSettings::get().railway_profileSampleLength)));
						for (int i=0;i<count;++i) { points << curve.positionAt(curve.totalLength*i/count); }
					}
					points << end;
				}

				// 地形・費用を評価する前に曲率を収める。平面を後から動かして縦断評価を無効にしない。
				if (points.size() > 6)
				{
					for (int pass=0;pass<GenerationSettings::get().railway_smoothingPasses;++pass)
					{
						bool curved = false;
						const auto fitted = RoadAlignment::fit(points);
						for (size_t i=0;i<fitted.size();++i)
						{
							const double radius = (i<2 || i+2>=fitted.size()) ? RailwaySite::kStationMinimumRadius : GenerationSettings::get().railway_minimumRadius+TransportCrossSection::kTrackSpacing*.5;
							curved |= fitted[i].minimumHorizontalRadius() < radius*1.02;
						}
						if (!curved) { break; }
						auto softened = points;
						for (size_t i=2;i+2<points.size();++i) { softened[i] = points[i].lerp((points[i-1]+points[i+1])*.5,.45); }
						points = std::move(softened);
					}
				}
				if (points.any([&](Vec3 point)
				{
					return point.x<0 || point.z<0 || point.x>=WORLD_SIZE || point.z>=WORLD_SIZE
						|| !world.getChunk({static_cast<int>(point.x)/CHUNK_SIZE,static_cast<int>(point.z)/CHUNK_SIZE});
				})) { continue; }
				Array<RailCostProfile::Sample> samples;
				for (size_t i=0;i<points.size();++i)
				{
					const auto& point=points[i]; const Vec2 position{point.x,point.z};
					const double ground=world.sampleHeight(static_cast<float>(point.x),static_cast<float>(point.z));
					double minimum=Max(GenerationSettings::get().railway_minimumAltitude,ground-GenerationSettings::get().railway_maximumTunnelDepth),maximum=ground+GenerationSettings::get().railway_maximumViaductHeight,roadClearance=-1e9,underpass=1e9;
					for (int dz=-1;dz<=1;++dz) for (int dx=-1;dx<=1;++dx)
					{
						const Vec2 around=position+Vec2{dx*GenerationSettings::get().railway_crossingSideSample,dz*GenerationSettings::get().railway_crossingSideSample}; const double terrain=world.sampleHeight(static_cast<float>(around.x),static_cast<float>(around.y)),water=world.waterSurfaceHeight(around.x,around.y);
						const auto corridor=crossings.interval(around,GenerationSettings::get().railway_crossingMargin);
						roadClearance=Max(roadClearance,corridor.second+GenerationSettings::get().railway_crossingExtraClearance); underpass=Min(underpass,corridor.first-GenerationSettings::get().railway_crossingExtraClearance);
						if (terrain<water+1) { roadClearance=Max(roadClearance,water+GenerationSettings::get().railway_waterClearance); underpass=Min(underpass,terrain-GenerationSettings::get().railway_waterTunnelCover); }
					}
					if (Vec2{point.x-start.x,point.z-start.z}.length()<=RailwaySite::kPlatformLength+GenerationSettings::get().railway_platformLevelMargin) { minimum=maximum=start.y; }
					if (Vec2{point.x-end.x,point.z-end.z}.length()<=RailwaySite::kPlatformLength+GenerationSettings::get().railway_platformLevelMargin) { minimum=maximum=end.y; }
					samples << RailCostProfile::Sample{position,ground,minimum,maximum,roadClearance,underpass};
				}
				const auto profile=RailCostProfile::solve(samples,start.y,end.y,GenerationSettings::get().railway_maximumGrade*.97);
				if (audit) { audit->candidates << CandidateAudit{alternative,profile.feasible,Math::Inf,0}; }
				DBG_LOG(U"[RailCostCandidate] line={} variant={} feasible={} cost={:.0f} points={}"_fmt(lines,alternative,profile.feasible,profile.cost,points.size()));
				if (profile.feasible && profile.cost<bestCost)
				{
					for (size_t i=0;i<points.size();++i) { points[i].y=profile.heights[i]; }
					const auto curves = RoadAlignment::fit(points);
					bool legal = curves.size()+1 == points.size();
					if (audit)
					{
						for (const auto& curve : curves)
						{
							audit->candidates.back().radius = Min(audit->candidates.back().radius,curve.minimumHorizontalRadius());
							for (int i=0;i<=32;++i) { const Vec3 t=curve.tangent(i/32.0f); audit->candidates.back().grade=Max(audit->candidates.back().grade,Abs(t.y)/Max(1e-8,Vec2{t.x,t.z}.length())); }
						}
					}
					for (const auto& curve : curves) { legal &= RoadAlignment::respectsLimits(curve,RoadType::LocalRoad,TransportMode::Rail); }
					// 線路中心だけでなく、内側軌道にも最小半径の余裕を確保する。
					for (const auto& curve : curves) { legal &= curve.minimumHorizontalRadius()+.001 >= GenerationSettings::get().railway_minimumRadius+TransportCrossSection::kTrackSpacing*.5; }
					if (legal) { bestCost=profile.cost; bestPoints=std::move(points); bestHeights=profile.heights; chosen=alternative; }
					else { DBG_LOG(U"[RailAlignmentRejected] variant={} reason=radius-or-grade"_fmt(alternative)); }
				}
			}
			if (bestPoints.isEmpty()) { ++failures; rejected.insert(pairKey(from,to)); DBG_LOG(U"[RailCostFailure] from={} to={}"_fmt(from,to)); continue; }
			connected.insert(to);
			const int firstStation=buildStation(from),lastStation=buildStation(to);
			Array<int> ids{firstStation};
			for (size_t i=0;i<bestPoints.size();++i) { bestPoints[i].y=bestHeights[i]; if (i>0 && i+1<bestPoints.size()) { ids << network.addNode(bestPoints[i]); } } ids << lastStation;
			const auto curves = RoadAlignment::fit(bestPoints);
			for (size_t i=0;i<curves.size();++i)
			{
				const auto& curve = curves[i];
				const int id = network.addEdge(ids[i],ids[i+1],curve.p1,curve.p2,80,true);
				if (auto* edge = network.getEdge(id))
				{
					for (int sample=0;sample<=8;++sample)
					{
						const Vec3 point = curve.evaluate(sample/8.0f);
						const double height = point.y-world.sampleHeight(static_cast<float>(point.x),static_cast<float>(point.z));
						edge->useElevation |= height>3; edge->tunnel |= height < -5;
					}
					if (edge->useElevation) { network.infrastructure().generatePiersForEdge(id,world); }
				}
			}
			DBG_LOG(U"[RailCostChosen] line={} variant={} cost={:.0f} sections={}"_fmt(lines,chosen,bestCost,bestPoints.size()-1));
			network.addSchedule(RailTimetable::makeDefault(network,firstStation,lastStation));
			++lines;
		}
		// Shallow cuts are physical earthworks, not track meshes hidden inside the original terrain.
		HashTable<int64,float> cutting;
		for (const auto& edge : network.edges())
		{
			const auto curve=network.getBezier(edge.id);
			for (float arc=0;arc<=curve->totalLength;arc+=8)
			{
				const Vec3 point=curve->positionAt(arc); const int gx=static_cast<int>(point.x/16),gz=static_cast<int>(point.z/16);
				for (int z=Max(0,gz-1);z<=Min(WORLD_CHUNKS*HEIGHT_CELLS,gz+2);++z) for (int x=Max(0,gx-1);x<=Min(WORLD_CHUNKS*HEIGHT_CELLS,gx+2);++x)
				{
					if (Vec2{x*16.0,z*16.0}.distanceFrom({point.x,point.z})>22) { continue; }
					const float ground=world.sampleHeight(static_cast<float>(x*16),static_cast<float>(z*16));
					const float target=static_cast<float>(point.y-.35); if (ground<=target || ground-target>5) { continue; }
					const int64 key=static_cast<int64>(z)*65536+x; if (!cutting.contains(key) || cutting[key]>target) { cutting[key]=target; }
				}
			}
		}
		for (const auto& [key,height] : cutting) { world.setGridHeight(static_cast<int>(key%65536),static_cast<int>(key/65536),height); }
		for (int z=0;z<WORLD_CHUNKS;++z) for (int x=0;x<WORLD_CHUNKS;++x) { if (auto* chunk=world.getChunk({x,z});chunk && chunk->meshDirty) { chunk->updateHeightBounds(); } }
		Array<double> clearances; double maximumGrade=0; int conflicts=0,underWater=0;
		for (const auto& edge : network.edges())
		{
			const auto curve=network.getBezier(edge.id); const int count=Max(2,static_cast<int>(std::ceil(curve->totalLength/6)));
			for (int i=0;i<=count;++i)
			{
				const Vec3 point=curve->evaluate(static_cast<float>(i)/count),tangent=curve->tangent(static_cast<float>(i)/count);
				clearances << point.y-world.sampleHeight(static_cast<float>(point.x),static_cast<float>(point.z));
				maximumGrade=Max(maximumGrade,Abs(tangent.y)/Max(.001,Vec2{tangent.x,tangent.z}.length()));
				const auto crossing=crossings.interval({point.x,point.z}); conflicts+=point.y<crossing.second-.1 && point.y>crossing.first+.1; underWater+=world.sampleHeight(static_cast<float>(point.x),static_cast<float>(point.z))<world.waterSurfaceHeight(point.x,point.z)+1 && point.y<world.waterSurfaceHeight(point.x,point.z) && point.y>world.sampleHeight(static_cast<float>(point.x),static_cast<float>(point.z))-8;
			}
		}
		clearances.sort();
		if (!clearances.isEmpty()) { DBG_LOG(U"[RailHeight] samples={} median={} p95={} maximum={}"_fmt(clearances.size(),clearances[clearances.size()/2],clearances[clearances.size()*95/100],clearances.back())); }
		DBG_LOG(U"[RailwayAudit] maxGrade={} roadConflicts={} underWater={} failedConnections={}"_fmt(maximumGrade,conflicts,underWater,stations.size()-builtStations.size()));
		int stationViolations=0;
		for (const int candidate : stations)
		{
			if (!builtStations.contains(candidate))
			{
				DBG_LOG(U"[RailwayStationOmitted] candidate={} name={} position={} reason=no-feasible-corridor"_fmt(candidate,candidates[candidate].name,candidates[candidate].position));
				continue;
			}
			const int station=builtStations[candidate];
			const double radius=RailwaySite::stationMinimumRadius(network,station);
			stationViolations+=radius<RailwaySite::kStationMinimumRadius;
			DBG_LOG(U"[StationCurvature] station={} minimumRadius={}"_fmt(station,radius));
		}
		DBG_LOG(U"[StationCurvature] violations={}"_fmt(stationViolations));
		if (roads) { RailDepotBuilder::generate(network,world,*roads); }
		DBG_LOG(U"[RailwayAlignment] stations={} lines={} sections={} rejectedCorridors={}"_fmt(builtStations.size(),lines,network.edges().size(),failures));
	}
}

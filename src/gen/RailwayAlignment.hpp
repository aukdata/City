#pragma once
#include "GenerationSettings.hpp"
#include "MapGenerator.hpp"
#include "RoadPathfinder.hpp"
#include "TransportClearance.hpp"
#include "RailCostProfile.hpp"
#include "UrbanFacilities.hpp"
#include "RoadAlignment.hpp"
#include "../railway/RailTimetable.hpp"
#include "../railway/RailDepotBuilder.hpp"
#include "../debug/DebugLog.hpp"

/// @brief 近接駅の接続ごとに複数の地形回廊と縦断施工費を比較して線形を選ぶ。
namespace RailwayAlignment
{
	struct CandidateAudit { int variant; bool profile; double radius; double grade; };
	/// @brief turnarounds は駅の反対端から回り込んだ接続数、sharedStationEnds は反対端が空いているのに 2 本以上の線路が出る駅端の数（通り抜けると折り返す）。
	struct Audit { Array<CandidateAudit> candidates; int turnarounds=0; int sharedStationEnds=0; };
	inline void generate(TrainNetwork& network,World& world,const Array<MapGenerator::Settlement>& towns,RoadNetwork* roads=nullptr, Audit* audit=nullptr)
	{
		const TransportClearance crossings{roads,world};
		network=TrainNetwork{};
		network.bind(roads);
		struct StationCandidate { Vec3 position; String name; StationKind kind; };
		Array<StationCandidate> candidates;
		Array<int> stations; HashTable<int,Vec3> axes; HashTable<int,int> builtStations;
		// Candidate IDs are separate from the live graph. Only feasible corridors create stations.
		const auto buildStation=[&](int candidate)
		{
			if (!builtStations.contains(candidate))
			{
				builtStations[candidate]=network.addStation(candidates[candidate].position,candidates[candidate].name);
				network.getNode(builtStations[candidate])->stationKind=candidates[candidate].kind;
			}
			return builtStations[candidate];
		};
		Array<Array<int>> townStations; HashTable<int,int> townOf;
		for (const auto& town : towns)
		{
			townStations.emplace_back();
			const auto positions=UrbanStructure::stationPositions(town.plan);
			for (size_t stationIndex=0;stationIndex<positions.size();++stationIndex)
			{
				const Vec2 local=positions[stationIndex];
				const Vec2 point=town.center+town.gridAxisX*local.x+town.gridAxisZ*local.y;
				double required=Max(world.sampleHeight(static_cast<float>(point.x),static_cast<float>(point.y))+GenerationSettings::get().railway_stationBaseElevation,crossings.minimumRailHeight(point,GenerationSettings::get().railway_stationRoadMargin)+GenerationSettings::get().railway_stationRoadClearance);
				for (int i=0;i<16;++i) for (const double distance : {GenerationSettings::get().railway_stationSurveyRadius0,GenerationSettings::get().railway_stationSurveyRadius1,GenerationSettings::get().railway_stationSurveyRadius2})
				{
					const Vec2 around=point+Vec2{Cos(i*Math::TwoPi/16),Sin(i*Math::TwoPi/16)}*distance;
					required=Max(required,crossings.minimumRailHeight(around,GenerationSettings::get().railway_stationSurveyRoadMargin)+GenerationSettings::get().railway_stationRoadClearance-distance*GenerationSettings::get().railway_stationApproachGrade);
				}
				required=Min(required,world.sampleHeight(static_cast<float>(point.x),static_cast<float>(point.y))+GenerationSettings::get().railway_stationMaximumElevation);
				const double elevation=std::ceil(required/.5)*.5;
				const int id=static_cast<int>(candidates.size());
				candidates << StationCandidate{{point.x,elevation,point.y},stationIndex==0 ? town.name : U"{}第{}地区"_fmt(town.name,stationIndex+1),town.plan.scale==0 && stationIndex==0 ? StationKind::Terminal : StationKind::Local};
				stations << id; axes[id]={town.gridAxisX.x,0,town.gridAxisX.y}; townOf[id]=static_cast<int>(townStations.size()-1); townStations.back() << id;
			}
		}
		if (stations.size()<2) { UrbanFacilities::generateSubways(world,network,towns); return; }
		HashSet<int> connected; HashSet<uint64> rejected,strictRejected;
		constexpr int kMaximumStrictAttempts=3; int strictFailures=0; int lines=0,failures=0;
		// 近い順に3駅から通せない駅は、山や海で隔てられた別の地域とみなす。遠い駅からの探索を繰り返して生成を長引かせない。
		constexpr int kMaximumTargetAttempts=3;
		HashTable<int,int> targetFailures;
		const auto pairKey=[](int a,int b) { return (static_cast<uint64>(Min(a,b))<<32)|static_cast<uint32>(Max(a,b)); };
		// 駅の両端（0: 軸の正側、1: 負側）から出ている線路の数。同じ端から 2 本出すと、通り抜ける列車が駅で折り返す（スイッチバック）。
		HashTable<int,std::array<int,2>> endUse;
		const auto facingEnd=[&](int candidate,Vec2 target)
		{
			const Vec3 position=candidates[candidate].position;
			return axes[candidate].x*(target.x-position.x)+axes[candidate].z*(target.y-position.z)>=0 ? 0 : 1;
		};
		const auto usedEnd=[&](int candidate,int side) { return endUse.contains(candidate) ? endUse[candidate][side] : 0; };
		// 回り込みのループは最小半径の円より外を通す。駅構内へ戻る経路は探索で避ける。
		const double kTurnaroundRadius=GenerationSettings::get().railway_minimumRadius*1.6+TransportCrossSection::kTrackSpacing;
		// 駅で折り返さない接続を優先する。面した端が使用済みの駅からの接続は、ループ 1 周分ほど遠いものとして比べる。
		const double kReversalPenalty=Math::Pi*GenerationSettings::get().railway_minimumRadius*2+GenerationSettings::get().railway_stationStraightLead*2;
		int turnarounds=0,reversingJunctions=0,insertions=0;
		// 駅間の線形計画。網へ敷設する前に、途中駅の挿入で計画を差し替えられるようにする。
		struct Plan { int from=-1,to=-1,fromEnd=0,toEnd=0; Array<Vec3> points; bool turnaround=false; int variant=-1; double cost=0; };
		Array<Plan> plans; Array<bool> livePlans;
		bool lastProfileFeasible=false; ///< 直前の計画で、縦断勾配が成立する候補が 1 つでもあったか（平面線形だけの不成立か）
		const auto recordPlan=[&](const Plan& plan,int sign) { endUse[plan.from][plan.fromEnd]+=sign; endUse[plan.to][plan.toEnd]+=sign; };
		// 2 つの候補を fromEnd・toEnd の駅端で結ぶ線形を比較して計画する。成立しなければ none。
		const auto planEnds=[&](int from,int to,int fromEnd,int toEnd,bool turnToTarget=false) -> Optional<Plan>
		{
				const Vec3 start=candidates[from].position,end=candidates[to].position;
				const bool portalEnd=candidates[to].kind==StationKind::Portal;
				const Vec2 delta{end.x-start.x,end.z-start.z};
				const double kStationStraightLead=GenerationSettings::get().railway_stationStraightLead;
				// fromAxis は出発駅から出ていく向き、toAxis は到着駅へ入る向き。
				const Vec3 fromAxis=axes[from]*(fromEnd==0 ? 1.0 : -1.0);
				const Vec3 toAxis=axes[to]*(toEnd==0 ? -1.0 : 1.0);
				const Vec3 startGate=start+fromAxis*kStationStraightLead,endGate=end-toAxis*kStationStraightLead;
				// 相手に背を向けた駅端から出入りするときは、引き上げ線の先で相手の方へ向き直る円弧（ループ）を描いて駅の周りを回り込む。
				// 戻り値は円弧上の標本（引き上げ線の端を除く）と円弧を出る向き。向き直る必要がなければ空。
				const auto turnLead=[&](Vec3 gate,Vec3 axis,Vec3 target)
				{
					Array<Vec3> arc;
					Vec2 position{gate.x,gate.z},heading=Vec2{axis.x,axis.z}.normalized();
					const Vec2 goal{target.x,target.z};
					// 通常は相手に背を向けるときだけ回り込む。turnToTarget では、引き上げ線の端で大きく向きを変える場合も円弧で向き直る。
					if (heading.dot((goal-position).normalized())>=(turnToTarget ? std::cos(Math::ToRadians(30.0)) : 0.0)) { return std::pair{arc,Vec3{heading.x,0,heading.y}}; }
					const double side=heading.x*(goal.y-position.y)-heading.y*(goal.x-position.x)>=0 ? 1.0 : -1.0;
					constexpr double kStep=10;
					const double turn=side*kStep/kTurnaroundRadius,sampleLength=GenerationSettings::get().railway_profileSampleLength;
					double travelled=0,turned=0;
					while (heading.dot((goal-position).normalized())<std::cos(Math::ToRadians(30.0)) && turned<Math::ToRadians(210.0))
					{
						heading=Vec2{heading.x*std::cos(turn)-heading.y*std::sin(turn),heading.x*std::sin(turn)+heading.y*std::cos(turn)};
						position+=heading*kStep; travelled+=kStep; turned+=Abs(turn);
						if (travelled>=sampleLength) { arc << Vec3{position.x,gate.y,position.y}; travelled=0; }
					}
					if (travelled>0) { arc << Vec3{position.x,gate.y,position.y}; }
					// 円弧の先に直線を置き、探索経路の曲線が円弧へ急に折れ込まないようにする。
					for (double run=sampleLength;run<=kStationStraightLead+1;run+=sampleLength) { arc << Vec3{position.x+heading.x*run,gate.y,position.y+heading.y*run}; }
					return std::pair{arc,Vec3{heading.x,0,heading.y}};
				};
				const auto [startArc,startHeading]=turnLead(startGate,fromAxis,end);
				auto [endArc,endHeading]=turnLead(endGate,-toAxis,start);
				if (portalEnd) { endArc.clear(); endHeading=-toAxis; }
				const bool fromTurnaround=!startArc.isEmpty(),toTurnaround=!endArc.isEmpty();
				// 経路探索と曲線は routeStart から routeEnd まで。駅から引き上げ線・円弧までは固定の線形を前後に付ける。
				const Vec3 routeStart=fromTurnaround ? startArc.back() : startGate,routeEnd=toTurnaround ? endArc.back() : endGate;
				const Vec3 routeStartAxis=startHeading,routeEndAxis=-endHeading;
				Array<Vec3> startLead,endLead;
				{
					const int count=Max(1,static_cast<int>(std::ceil(kStationStraightLead/GenerationSettings::get().railway_profileSampleLength)));
					for (int i=0;i<count;++i) { startLead << start.lerp(startGate,static_cast<double>(i)/count); }
					if (fromTurnaround) { startLead << startGate; for (size_t i=0;i+1<startArc.size();++i) { startLead << startArc[i]; } }
					if (toTurnaround) { for (size_t i=endArc.size()-1;i-->0;) { endLead << endArc[i]; } }
					for (int i=0;i<count;++i) { endLead << endGate.lerp(end,static_cast<double>(i)/count); }
					endLead << end;
				}
				// 円弧で回り込む駅の周りは、探索経路が駅構内へ戻らないよう高くする。
				const auto turnaroundCost=[&](Vec2 point)
				{
					double cost=0;
					if (fromTurnaround && point.distanceFrom({start.x,start.z})<kTurnaroundRadius) { cost+=60; }
					if (toTurnaround && point.distanceFrom({end.x,end.z})<kTurnaroundRadius) { cost+=60; }
					return cost;
				};
				Array<Vec3> bestPoints; Array<double> bestHeights; double bestCost=Math::Inf; int chosen=-1;
				lastProfileFeasible=false;
				for (int alternative=0;alternative<5;++alternative)
				{
					Array<Vec3> points=startLead;
					// 解析曲線候補は探索経路を使わないため、破棄されていた5回目の探索を省く。
					if (alternative < 4)
					{
						const double padding = alternative < 3 ? GenerationSettings::get().railway_corridorMargin
															   : GenerationSettings::get().railway_wideCorridorMargin;
						const Vec2 offset{Max(0.0, Min(start.x, end.x) - padding), Max(0.0, Min(start.z, end.z) - padding)};
						const Vec2 upper{Min(static_cast<double>(WORLD_SIZE), Max(start.x, end.x) + padding),
							Min(static_cast<double>(WORLD_SIZE), Max(start.z, end.z) + padding)};
						RoadPathfinder finder;
						finder.setRailwayRouting(true);
						finder.setConstructionCost([&](Vec2 point, double ground)
						{
							const double t =
								Clamp((point - Vec2{start.x, start.z}).dot(delta) / Max(1.0, delta.lengthSq()), 0.0, 1.0);
							const double reference = Math::Lerp(start.y, end.y, t);
							const double coefficient =
								alternative == 0   ? GenerationSettings::get().railway_corridorCostWeight0
								: alternative == 1 ? GenerationSettings::get().railway_corridorCostWeight1
								: alternative == 2 ? GenerationSettings::get().railway_corridorCostWeight2
												   : GenerationSettings::get().railway_corridorCostWeight3;
							const double water = world.waterPlanningHeight(point.x, point.y);
							return 1 + RailCostProfile::unitCost(reference - ground) * coefficient +
								   (ground < water + 1 ? GenerationSettings::get().railway_waterCorridorCost : 0.0) + turnaroundCost(point);
						});
						finder.setup(world, offset,
							Max(2, static_cast<int>((upper.x - offset.x) / GenerationSettings::get().railway_routingCell)),
							Max(2, static_cast<int>((upper.y - offset.y) / GenerationSettings::get().railway_routingCell)),
							GenerationSettings::get().railway_routingCell);
						const auto path = finder.findPath(
							finder.worldToGrid(static_cast<float>(routeStart.x), static_cast<float>(routeStart.z)),
							finder.worldToGrid(static_cast<float>(routeEnd.x), static_cast<float>(routeEnd.z)));
						auto coarse = finder.samplePath(path, 2);
						if (coarse.size() < 2)
						{
							continue;
						}
						coarse.front() = routeStart;
						coarse.back() = routeEnd;
						for (int pass = 0; pass < 24 + alternative * 12; ++pass)
						{
							auto next = coarse;
							for (size_t i = 1; i + 1 < coarse.size(); ++i)
							{
								next[i] = coarse[i] * .5 + (coarse[i - 1] + coarse[i + 1]) * .25;
							}
							coarse = std::move(next);
						}
						// 引き上げ線の端で向きを大きく変える探索点は、最小半径で曲がり切れる距離まで間引く。
						const double kTurnRadius=GenerationSettings::get().railway_minimumRadius*1.4;
						const auto required=[&](Vec3 gate,Vec3 axis,Vec3 next)
						{
							const Vec2 heading=Vec2{axis.x,axis.z}.normalized(),direction=Vec2{next.x-gate.x,next.z-gate.z};
							const double angle=std::acos(Clamp(heading.dot(direction.normalized()),-1.0,1.0));
							return direction.length()>=2*kTurnRadius*std::sin(angle*.5)*1.5;
						};
						while (coarse.size()>2 && !required(coarse.front(),routeStartAxis,coarse[1])) { coarse.erase(coarse.begin()+1); }
						while (coarse.size()>2 && !required(coarse.back(),-routeEndAxis,coarse[coarse.size()-2])) { coarse.erase(coarse.end()-2); }
						for (size_t i = 0; i + 1 < coarse.size(); ++i)
						{
							const Vec3 direction = coarse[i + 1] - coarse[i];
							const double handle = Vec2{direction.x, direction.z}.length() / 3;
							Vec3 a = i > 0 ? coarse[i + 1] - coarse[i - 1] : routeStartAxis;
							Vec3 b = i + 2 < coarse.size() ? coarse[i + 2] - coarse[i] : routeEndAxis;
							a.y = b.y = 0;
							const CubicBezier curve{coarse[i], coarse[i] + a.normalized() * handle,
								coarse[i + 1] - b.normalized() * handle, coarse[i + 1]};
							const int count = Max(1,
								static_cast<int>(
									std::ceil(curve.totalLength / GenerationSettings::get().railway_profileSampleLength)));
							for (int sample = 0; sample < count; ++sample)
							{
								points << curve.positionAt(curve.totalLength * sample / count);
							}
						}
					}
					if (alternative == 4)
					{
						// 駅の接線を固定した大きな曲線も、地形追従候補と同じ費用・制約で比較する。
						const double arm = Vec2{routeEnd.x-routeStart.x,routeEnd.z-routeStart.z}.length()/3;
						const CubicBezier curve{routeStart,routeStart+routeStartAxis*arm,routeEnd-routeEndAxis*arm,routeEnd};
						const int count = Max(1,static_cast<int>(Ceil(curve.totalLength/GenerationSettings::get().railway_profileSampleLength)));
						for (int i=0;i<count;++i) { points << curve.positionAt(curve.totalLength*i/count); }
					}
					points.append(endLead);

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
						double minimum=Max(GenerationSettings::get().railway_minimumAltitude,ground-GenerationSettings::get().railway_maximumTunnelDepth*(portalEnd ? 1000.0 : 1.0)),maximum=ground+GenerationSettings::get().railway_maximumViaductHeight,roadClearance=-1e9,underpass=1e9;
						for (int dz=-1;dz<=1;++dz) for (int dx=-1;dx<=1;++dx)
						{
							const Vec2 around=position+Vec2{dx*GenerationSettings::get().railway_crossingSideSample,dz*GenerationSettings::get().railway_crossingSideSample}; const double terrain=world.sampleHeight(static_cast<float>(around.x),static_cast<float>(around.y)),water=world.waterPlanningHeight(around.x,around.y);
							const auto corridor=crossings.interval(around,GenerationSettings::get().railway_crossingMargin);
							roadClearance=Max(roadClearance,corridor.second+GenerationSettings::get().railway_crossingExtraClearance); underpass=Min(underpass,corridor.first-GenerationSettings::get().railway_crossingExtraClearance);
							if (terrain<water+1) { roadClearance=Max(roadClearance,water+GenerationSettings::get().railway_waterClearance); underpass=Min(underpass,terrain-GenerationSettings::get().railway_waterTunnelCover); }
						}
						if (Vec2{point.x-start.x,point.z-start.z}.length()<=RailwaySite::kPlatformLength+GenerationSettings::get().railway_platformLevelMargin) { minimum=maximum=start.y; }
						if (!portalEnd && Vec2{point.x-end.x,point.z-end.z}.length()<=RailwaySite::kPlatformLength+GenerationSettings::get().railway_platformLevelMargin) { minimum=maximum=end.y; }
						samples << RailCostProfile::Sample{position,ground,minimum,maximum,roadClearance,underpass};
					}
					// 外部接続は山地の地図端へもトンネルで抜けられるよう土被りの上限を外す（上の minimum）。
					// 外部接続の端点（ホームを持たない）は高さを固定せず、地図端で取れる高さから最も安い縦断を選ぶ。
					Array<double> endHeights{end.y};
					if (portalEnd)
					{
						const auto& last=samples.back();
						for (int k=0;k<=8;++k) { endHeights << Math::Lerp(Max(last.minimum,last.roadClearance),Min(last.maximum,last.underpass),k/8.0); }
						endHeights << Clamp(start.y,last.minimum,last.maximum);
					}
					RailCostProfile::Result profile;
					for (const double endHeight : endHeights)
					{
						auto attempt=RailCostProfile::solve(samples,start.y,endHeight,GenerationSettings::get().railway_maximumGrade*.97);
						if (attempt.feasible && (!profile.feasible || attempt.cost<profile.cost)) { profile=std::move(attempt); }
					}
					if (audit) { audit->candidates << CandidateAudit{alternative,profile.feasible,Math::Inf,0}; }
					DBG_LOG(U"[RailCostCandidate] line={} variant={} feasible={} cost={:.0f} points={}"_fmt(lines,alternative,profile.feasible,profile.cost,points.size()));
					lastProfileFeasible|=profile.feasible;
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
						// 回り込みが駅構内を横切らないこと（引き上げ線より先の区間が駅の近くへ戻らない）。
						if (fromTurnaround || toTurnaround)
						{
							double travelled=0; const double total=[&] { double length=0; for (size_t i=1;i<points.size();++i) { length+=Vec2{points[i].x-points[i-1].x,points[i].z-points[i-1].z}.length(); } return length; }();
							for (size_t i=1;i<points.size();++i)
							{
								travelled+=Vec2{points[i].x-points[i-1].x,points[i].z-points[i-1].z}.length();
								const Vec2 flat{points[i].x,points[i].z};
								const double clearance=kStationStraightLead*.75;
								if (fromTurnaround && travelled>kStationStraightLead*1.5 && flat.distanceFrom({start.x,start.z})<clearance) { legal=false; }
								if (toTurnaround && total-travelled>kStationStraightLead*1.5 && flat.distanceFrom({end.x,end.z})<clearance) { legal=false; }
							}
						}
						// 線路中心だけでなく、内側軌道にも最小半径の余裕を確保する。
						for (const auto& curve : curves) { legal &= curve.minimumHorizontalRadius()+.001 >= GenerationSettings::get().railway_minimumRadius+TransportCrossSection::kTrackSpacing*.5; }
						if (legal) { bestCost=profile.cost; bestPoints=std::move(points); bestHeights=profile.heights; chosen=alternative; }
						else
						{
							double radius=Math::Inf,grade=0; size_t worst=0;
							for (size_t i=0;i<curves.size();++i)
							{
								if (curves[i].minimumHorizontalRadius()<radius) { radius=curves[i].minimumHorizontalRadius(); worst=i; }
								for (int k=0;k<=16;++k) { const Vec3 t=curves[i].tangent(k/16.0f); grade=Max(grade,Abs(t.y)/Max(1e-8,Vec2{t.x,t.z}.length())); }
							}
							DBG_LOG(U"[RailAlignmentRejected] variant={} reason=radius-or-grade radius={:.0f} at={}/{} grade={:.4f}"_fmt(alternative,radius,worst,curves.size(),grade));
						}
					}
				}
				if (bestPoints.isEmpty()) { DBG_LOG(U"[RailCostFailure] from={} to={}"_fmt(from,to)); return none; }
				for (size_t i=0;i<bestPoints.size();++i) { bestPoints[i].y=bestHeights[i]; }
				DBG_LOG(U"[RailCostPlanned] from={} to={} variant={} cost={:.0f} sections={} turnaround={}"_fmt(candidates[from].name,candidates[to].name,chosen,bestCost,bestPoints.size()-1,fromTurnaround || toTurnaround));
				const bool turnaround=fromEnd!=facingEnd(from,{end.x,end.z}) || (!portalEnd && toEnd!=facingEnd(to,{start.x,start.z}));
				return Plan{from,to,fromEnd,toEnd,std::move(bestPoints),turnaround,chosen,bestCost};
		};
		// 計画した線形を網へ敷設し、両端の駅 ID を返す。
		const auto commit=[&](const Plan& plan)
		{
			if (candidates[plan.to].kind==StationKind::Portal) { candidates[plan.to].position.y=plan.points.back().y; }
			const int firstStation=buildStation(plan.from),lastStation=buildStation(plan.to);
			Array<int> ids{firstStation};
			for (size_t i=1;i+1<plan.points.size();++i) { ids << network.addNode(plan.points[i]); }
			ids << lastStation;
			const auto curves = RoadAlignment::fit(plan.points);
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
			turnarounds+=plan.turnaround;
			DBG_LOG(U"[RailCostChosen] line={} variant={} cost={:.0f} sections={} turnaround={}"_fmt(lines,plan.variant,plan.cost,plan.points.size()-1,plan.turnaround));
			return std::pair{firstStation,lastStation};
		};
		const auto flatPosition=[&](int candidate) { return Vec2{candidates[candidate].position.x,candidates[candidate].position.z}; };
		const auto add=[&](Plan plan) { recordPlan(plan,1); plans << std::move(plan); livePlans << true; };
		// station の end 端を使う計画のうち、target がその線の途中（両端の駅の間）にあるもの。target を途中駅として挿入できる。
		const auto insertionCandidates=[&](int station,int end,int target)
		{
			Array<size_t> result;
			if (candidates[target].kind==StationKind::Portal) { return result; }
			const Vec2 x=flatPosition(station),t=flatPosition(target);
			for (size_t k=0;k<plans.size();++k)
			{
				if (!livePlans[k]) { continue; }
				const auto& plan=plans[k];
				const bool atFrom=plan.from==station && plan.fromEnd==end,atTo=plan.to==station && plan.toEnd==end;
				if (!atFrom && !atTo) { continue; }
				const int other=atFrom ? plan.to : plan.from;
				if (candidates[other].kind==StationKind::Portal) { continue; }
				const Vec2 y=flatPosition(other);
				if (x.distanceFrom(t)+t.distanceFrom(y)<=x.distanceFrom(y)*1.35 && facingEnd(target,x)!=facingEnd(target,y)) { result << k; }
			}
			return result;
		};
		// 駅で折り返さない端を選んで計画する。面した端が使用済みなら、その端の線の途中駅として挿入するか、反対端からループで回り込む。
		// 成立しなければ到着駅の反対端へ回り込み、最後に相手に面した端を共用する分岐駅にする。成立した計画を plans に加える。
		// Avoid は駅で折り返さない接続だけ、Only は Avoid で試し終えた後に折り返す接続だけを試す。
		enum class Reversal { Avoid, Allow, Only };
		const auto connect=[&](int from,int to,Reversal reversal) -> bool
		{
			const Vec2 start=flatPosition(from),end=flatPosition(to);
			const int fromFacing=facingEnd(from,end),toFacing=facingEnd(to,start);
			if (usedEnd(from,fromFacing)>0 && reversal!=Reversal::Only)
			{
				for (const size_t k : insertionCandidates(from,fromFacing,to))
				{
					const Plan line=plans[k];
					const bool atFrom=line.from==from;
					const int other=atFrom ? line.to : line.from,otherEnd=atFrom ? line.toEnd : line.fromEnd;
					auto first=planEnds(from,to,fromFacing,toFacing);
					if (!first) { continue; }
					auto second=planEnds(to,other,facingEnd(to,flatPosition(other)),otherEnd);
					if (!second) { continue; }
					livePlans[k]=false; recordPlan(line,-1);
					add(std::move(*first)); add(std::move(*second));
					++insertions;
					DBG_LOG(U"[RailStationInserted] station={} between={} and={}"_fmt(candidates[to].name,candidates[from].name,candidates[other].name));
					return true;
				}
			}
			// 両駅の端の組を、共用する駅端（折り返し）が少なく、回り込みが少ない順に試す。
			Array<std::tuple<int,int,int>> options;
			for (const int first : {0,1}) for (const int second : {0,1})
			{
				if (candidates[to].kind==StationKind::Portal && second!=toFacing) { continue; }
				const int shared=(usedEnd(from,first)>0)+(usedEnd(to,second)>0),turns=(first!=fromFacing)+(second!=toFacing);
				// 反対端が空いているのに同じ端を共用すると、通り抜ける列車が駅で折り返す。許可されたときだけ試す。
				const bool reverses=(usedEnd(from,first)>0 && usedEnd(from,1-first)==0) || (candidates[to].kind!=StationKind::Portal && usedEnd(to,second)>0 && usedEnd(to,1-second)==0);
				if (reverses && reversal==Reversal::Avoid) { continue; }
				options << std::tuple{shared*10+turns+(reverses ? 100 : 0),first,second};
			}
			options.sort();
			int attempts=0;
			for (const auto& [score,first,second] : options)
			{
				if (++attempts>3) { break; }
				// Only では、Avoid で試し終えた折り返さない組を数えるだけで計画し直さない。
				if (reversal==Reversal::Only && score<100) { continue; }
				auto plan=planEnds(from,to,first,second);
				// 地形・高さで縦断が成立しない区間は、駅端や円弧を変えても通せないので打ち切る。
				if (!plan && !lastProfileFeasible) { break; }
				// 地図端への延長線は長く探索が重いので、円弧で向き直る再計画を省く。
				if (!plan && candidates[to].kind!=StationKind::Portal) { plan=planEnds(from,to,first,second,true); }
				if (plan)
				{
					add(std::move(*plan));
					if ((usedEnd(from,first)>1 && usedEnd(from,1-first)==0) || (candidates[to].kind!=StationKind::Portal && usedEnd(to,second)>1 && usedEnd(to,1-second)==0))
					{
						++reversingJunctions;
						DBG_LOG(U"[RailReversingJunction] from={} to={} reason=no-feasible-loop"_fmt(candidates[from].name,candidates[to].name));
					}
					return true;
				}
			}
			return false;
		};
		// 同じ町の駅は町の軸に沿って並べ、先に 1 本の線でつなぐ。町の外からの線は並びの両端の駅へ入るので、町の中の駅で折り返さない。
		HashTable<int,int> groupOf; Array<Array<int>> groups;
		for (auto members : townStations)
		{
			if (members.isEmpty()) { continue; }
			const Vec3 axis=axes[members.front()];
			members.sort_by([&](int a,int b) { return axis.dot(candidates[a].position)<axis.dot(candidates[b].position); });
			groups.push_back({members.front()});
			for (size_t i=1;i<members.size();++i)
			{
				const int previous=members[i-1],current=members[i];
				auto plan=planEnds(previous,current,facingEnd(previous,flatPosition(current)),facingEnd(current,flatPosition(previous)));
				if (plan) { add(std::move(*plan)); groups.back() << current; }
				else { groups.push_back({current}); }
			}
		}
		for (size_t g=0;g<groups.size();++g) { for (const int member : groups[g]) { groupOf[member]=static_cast<int>(g); } }
		const auto join=[&](int candidate) { for (const int member : groups[groupOf[candidate]]) { connected.insert(member); } };
		join(stations.front());
		// 町のどれかの駅が既に網につながっていれば、残りの駅は同じ町の駅からだけつなぐ。町の外から並行する 2 本目の線を引かない。
		const auto parallelLine=[&](int a,int b)
		{
			return townOf[a]!=townOf[b] && townStations[townOf[b]].any([&](int member) { return member!=b && connected.contains(member); });
		};
		while (connected.size()<stations.size())
		{
			int from=-1,to=-1; double nearest=1e30;
			for (const int a : stations) if (connected.contains(a)) for (const int b : stations) if (!connected.contains(b))
			{
				if (rejected.contains(pairKey(a,b)) || strictRejected.contains(pairKey(a,b)) || parallelLine(a,b)) { continue; }
				const Vec2 flatA=flatPosition(a),flatB=flatPosition(b);
				const int facing=facingEnd(a,flatB);
				const bool reverses=(usedEnd(a,facing)>0 && insertionCandidates(a,facing,b).isEmpty()) || usedEnd(b,facingEnd(b,flatA))>0;
				const double distance=flatA.distanceFrom(flatB)+(reverses ? kReversalPenalty : 0.0);
				if (distance<nearest) { nearest=distance; from=a; to=b; }
			}
			// 駅で折り返さない接続がどの組でも成立しないときだけ、折り返しを許して最も近い組を敷設する。
			// 折り返さない接続の失敗が続く場合も、生成を長引かせないよう折り返しを許す。
			bool allowReversal=false;
			if (from<0 || strictFailures>=kMaximumStrictAttempts)
			{
				from=to=-1; nearest=1e30;
				allowReversal=true;
				for (const int a : stations) if (connected.contains(a)) for (const int b : stations) if (!connected.contains(b))
				{
					if (rejected.contains(pairKey(a,b)) || parallelLine(a,b)) { continue; }
					const double distance=flatPosition(a).distanceFrom(flatPosition(b));
					if (distance<nearest) { nearest=distance; from=a; to=b; }
				}
			}
			if (from<0)
			{
				// A sea or mountain barrier can separate railway regions without invalidating the city.
				for (const int candidate : stations)
				{
					if (connected.contains(candidate)) { continue; }
					join(candidate);
					DBG_LOG(U"[RailwaySeparateRegion] candidate={} position={} reason=no-feasible-corridor"_fmt(candidate,candidates[candidate].position));
					break;
				}
				continue;
			}
			if (!connect(from,to,!allowReversal ? Reversal::Avoid : strictRejected.contains(pairKey(from,to)) ? Reversal::Only : Reversal::Allow))
			{
				if (!allowReversal) { strictRejected.insert(pairKey(from,to)); ++strictFailures; continue; }
				++failures; rejected.insert(pairKey(from,to));
				if (++targetFailures[to]>=kMaximumTargetAttempts)
				{
					join(to);
					DBG_LOG(U"[RailwaySeparateRegion] candidate={} position={} reason=attempts-exhausted"_fmt(to,candidates[to].position));
				}
				continue;
			}
			strictFailures=0;
			join(to);
		}
		for (size_t k=0;k<plans.size();++k)
		{
			if (!livePlans[k]) { continue; }
			const auto built=commit(plans[k]);
			network.addSchedule(RailTimetable::makeDefault(network,built.first,built.second));
			++lines;
		}
		// ---- 外部接続: 路線網の両端から地図端へ線路を延ばし、地図外と直通する列車を走らせる ----
		// 端点は駅施設を持たない発着点（StationKind::Portal）。運行間隔は街の人口から決める（RailTimetable::applyOutsideDemand）。
		if (GenerationSettings::get().railway_outsideConnections>0 && !builtStations.empty())
		{
			const auto& settings=GenerationSettings::get();
			Array<int> built;
			for (const auto& entry : builtStations) { if (network.getNode(entry.second)->stationKind!=StationKind::Underground) { built << entry.first; } }
			built.sort();
			const auto flatOf=[&](int candidate) { return Vec2{candidates[candidate].position.x,candidates[candidate].position.z}; };
			int first=built.isEmpty() ? -1 : built.front(),second=first; double longest=-1;
			for (const int a : built) for (const int b : built)
			{
				const double distance=flatOf(a).distanceFromSq(flatOf(b));
				if (distance>longest) { longest=distance; first=a; second=b; }
			}
			// 地図端の陸地で、駅との高低差が小さい点を延長線の近くから探す。
			const auto borderPoint=[&](Vec2 from,Vec2 direction) -> Optional<Vec2>
			{
				const double inset=settings.railway_outsideBorderInset;
				double reach=Math::Inf;
				if (Abs(direction.x)>1e-6) { reach=Min(reach,((direction.x>0 ? WORLD_SIZE-inset : inset)-from.x)/direction.x); }
				if (Abs(direction.y)>1e-6) { reach=Min(reach,((direction.y>0 ? WORLD_SIZE-inset : inset)-from.y)/direction.y); }
				if (!std::isfinite(reach) || reach<=0) { return none; }
				const Vec2 hit=from+direction*reach;
				const bool vertical=Abs(hit.x-inset)<1 || Abs(hit.x-(WORLD_SIZE-inset))<1;
				const Vec2 along=vertical ? Vec2{0,1} : Vec2{1,0};
				const double origin=world.sampleHeight(static_cast<float>(from.x),static_cast<float>(from.y));
				const auto land=[&](Vec2 p) { return world.sampleHeight(static_cast<float>(p.x),static_cast<float>(p.y))>world.waterPlanningHeight(p.x,p.y)+2; };
				Optional<Vec2> best; double bestScore=Math::Inf;
				for (const double limit : {300.0,Math::Inf})
				{
					for (double offset=0;offset<=12000;offset+=250) for (const double sign : {1.0,-1.0})
					{
						const Vec2 point=hit+along*offset*sign;
						if (point.x<inset-1 || point.y<inset-1 || point.x>WORLD_SIZE-inset+1 || point.y>WORLD_SIZE-inset+1 || !land(point) || !land(point-direction*400)) { continue; }
						const double rise=Abs(world.sampleHeight(static_cast<float>(point.x),static_cast<float>(point.y))-origin);
						const double score=offset+rise*30;
						if (rise<limit && score<bestScore) { bestScore=score; best=point; }
					}
					if (best) { break; }
				}
				return best;
			};
			Array<std::pair<int,Vec2>> ends;
			if (first>=0 && first!=second)
			{
				ends << std::pair{second,(flatOf(second)-flatOf(first)).normalized()} << std::pair{first,(flatOf(first)-flatOf(second)).normalized()};
			}
			else if (first>=0)
			{
				Vec2 outward=flatOf(first)-Vec2{WORLD_SIZE*.5,WORLD_SIZE*.5};
				if (outward.lengthSq()<1) { outward={1,0}; }
				ends << std::pair{first,outward.normalized()};
			}
			int portals=0;
			for (const auto& [from,direction] : ends)
			{
				if (portals>=settings.railway_outsideConnections) { break; }
				// 延長方向とその左右 30°・60° の方向から地図端の候補を集め、3km 以上離れたものを順に試す。
				Array<std::pair<Vec2,Vec2>> points;
				for (const double degrees : {0.0,30.0,-30.0,60.0,-60.0})
				{
					const double angle=Math::ToRadians(degrees);
					const Vec2 turned{direction.x*std::cos(angle)-direction.y*std::sin(angle),direction.x*std::sin(angle)+direction.y*std::cos(angle)};
					const auto point=borderPoint(flatOf(from),turned);
					if (!point || points.any([&](const auto& p) { return p.first.distanceFrom(*point)<3000; })) { continue; }
					points << std::pair{*point,turned};
				}
				// 駅で折り返さないよう、空いている端の側にある候補を先に試す。
				const int freeEnd=usedEnd(from,0)>0 && usedEnd(from,1)==0 ? 1 : usedEnd(from,1)>0 && usedEnd(from,0)==0 ? 0 : -1;
				if (freeEnd>=0) { points.stable_sort_by([&](const auto& a,const auto& b) { return (facingEnd(from,a.first)==freeEnd)>(facingEnd(from,b.first)==freeEnd); }); }
				if (points.isEmpty()) { DBG_LOG(U"[RailwayOutside] skipped station={} reason=no-border-land"_fmt(candidates[from].name)); continue; }
				const int id=static_cast<int>(candidates.size());
				candidates << StationCandidate{};
				Optional<std::pair<int,int>> link;
				// 折り返さない接続は空いている端の側の 2 地点まで試し、成立しなければ全地点で折り返しを許す。
				for (const bool allowReversal : {false,true}) for (size_t index=0;index<points.size();++index)
				{
					if (link || (!allowReversal && index>=2)) { break; }
					const auto& [point,turned]=points[index];
					// 方面名は線路が抜ける地図端の向き。
					const double toWest=point.x,toEast=WORLD_SIZE-point.x,toNorth=point.y,toSouth=WORLD_SIZE-point.y,nearest=Min({toWest,toEast,toNorth,toSouth});
					const String compass=nearest==toEast ? U"東" : nearest==toWest ? U"西" : nearest==toSouth ? U"南" : U"北";
					const double ground=world.sampleHeight(static_cast<float>(point.x),static_cast<float>(point.y));
					candidates[id]=StationCandidate{{point.x,ground+settings.railway_stationBaseElevation,point.y},U"{}方面"_fmt(compass),StationKind::Portal};
					axes[id]=Vec3{turned.x,0,turned.y};
					if (connect(from,id,!allowReversal ? Reversal::Avoid : index<2 ? Reversal::Only : Reversal::Allow)) { link=commit(plans.back()); }
					if (link) { break; }
					DBG_LOG(U"[RailwayOutside] retry station={} point={} reason=no-feasible-corridor"_fmt(candidates[from].name,point));
				}
				if (!link) { DBG_LOG(U"[RailwayOutside] skipped station={} reason=no-feasible-corridor tried={}"_fmt(candidates[from].name,points.size())); continue; }
				++portals; ++lines;
				// 地図外から路線網の反対側の端まで、途中の駅にすべて停車する直通列車。
				const int portalNode=link->second,farNode=builtStations[from==first ? second : first];
				TrainSchedule schedule;
				schedule.outside=true;
				const auto routeEdges=network.findRoute(portalNode,farNode);
				Array<int> stops{portalNode};
				int cursor=portalNode;
				for (const int edgeId : routeEdges)
				{
					const auto* edge=network.getEdge(edgeId);
					cursor=edge->nodeA==cursor ? edge->nodeB : edge->nodeA;
					const auto* node=network.getNode(cursor);
					if (node && node->isPassengerStation() && !stops.contains(cursor)) { stops << cursor; }
				}
				if (stops.size()<2) { stops << builtStations[from]; }
				// ダイヤの停車駅は 12 駅まで。多い場合は両端を残して間引く。
				if (stops.size()>12)
				{
					Array<int> thinned;
					for (int i=0;i<12;++i) { thinned << stops[static_cast<size_t>(Math::Round(i*(stops.size()-1)/11.0))]; }
					stops=std::move(thinned);
				}
				for (const int stop : stops) { schedule.stops << StopEntry{stop,stop==portalNode ? 0.0f : 1.0f}; }
				const auto* last=network.getNode(stops.back());
				schedule.name=U"{}直通 {}行"_fmt(candidates[id].name,last ? last->name : String{});
				if (network.getNode(portalNode)->position.distanceFromSq(last->position)>Square(12000.0)) { schedule.type=TrainType::Express; }
				schedule.headwaySec=RailTimetable::outsideHeadwayMinutes(0);
				schedule.firstDepartureMinute=5*60; schedule.lastDepartureMinute=23*60+30;
				const String error=RailTimetable::validate(network,schedule);
				if (!error.isEmpty()) { DBG_LOG(U"[RailwayOutside] schedule rejected name={} reason={}"_fmt(schedule.name,error)); continue; }
				network.addSchedule(schedule);
				DBG_LOG(U"[RailwayOutside] portal={} station={} position={} stops={} headwayMinutes={}"_fmt(candidates[id].name,candidates[from].name,candidates[id].position,stops.size(),schedule.headwaySec));
			}
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
				const auto crossing=crossings.interval({point.x,point.z}); conflicts+=point.y<crossing.second-.1 && point.y>crossing.first+.1; underWater+=world.sampleHeight(static_cast<float>(point.x),static_cast<float>(point.z))<world.waterPlanningHeight(point.x,point.z)+1 && point.y<world.waterPlanningHeight(point.x,point.z) && point.y>world.sampleHeight(static_cast<float>(point.x),static_cast<float>(point.z))-8;
			}
		}
		clearances.sort();
		if (!clearances.isEmpty()) { DBG_LOG(U"[RailHeight] samples={} median={} p95={} maximum={}"_fmt(clearances.size(),clearances[clearances.size()/2],clearances[clearances.size()*95/100],clearances.back())); }
		DBG_LOG(U"[RailwayAudit] maxGrade={} roadConflicts={} underWater={} failedConnections={}"_fmt(maximumGrade,conflicts,underWater,stations.count_if([&](int candidate) { return !builtStations.contains(candidate); })));
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
		UrbanFacilities::generateSubways(world,network,towns);
		int sharedEnds=0;
		for (const auto& entry : endUse) { sharedEnds+=(entry.second[0]>1 && entry.second[1]==0)+(entry.second[1]>1 && entry.second[0]==0); }
		if (audit) { audit->turnarounds=turnarounds; audit->sharedStationEnds=sharedEnds; }
		DBG_LOG(U"[RailwayAlignment] stations={} lines={} sections={} rejectedCorridors={} turnarounds={} insertions={} sharedStationEnds={} reversingJunctions={}"_fmt(builtStations.size(),lines,network.edges().size(),failures,turnarounds,insertions,sharedEnds,reversingJunctions));
	}
}

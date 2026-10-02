#pragma once
#include "MapGenerator.hpp"
#include "RoadAlignment.hpp"
#include "RoadAutoPlace.hpp"
#include "StreetProfile.hpp"
#include "UrbanDeparture.hpp"
#include "../debug/DebugLog.hpp"

/// @brief Older hamlet lanes retained within later, collector-connected urban infill.
namespace UrbanMosaic
{
	inline constexpr int kRequestedCores=3;
	struct Core
	{
		Vec2 center;
		Vec2 axis;
		double halfLength = 0, halfDepth = 0;
		Vec3 gateway;
	};
	struct Layout
	{
		Array<Core> cores;
		Array<Line> lanes, seamLanes;
		int rejected = 0, routes = 0, seamConnections = 0, transactionCopies = 0, gatewayAttempts = 0, closeDepartures = 0, rejoinedLoops = 0, unbuiltPaths = 0;
		int coreTrials=0, blockedPaths=0, fitFailures=0, joinFailures=0, detourFailures=0;
		double length = 0;

		bool containsCore(Vec2 point) const
		{
			for (const auto& core : cores)
			{
				const Vec2 delta=point-core.center,side{-core.axis.y,core.axis.x};
				if (Abs(delta.dot(core.axis))<core.halfLength && Abs(delta.dot(side))<core.halfDepth) { return true; }
			}
			return false;
		}
		/// @brief Only seam-crossing insertions need a whole-network rollback snapshot.
		bool crossesInherited(Vec2 a,Vec2 b) const
		{
			const auto near=[&](const Line& lane)
			{
				// Retained lane chords are <=10m; padding includes their curved road envelope.
				return Max(a.x,b.x)+16>=Min(lane.begin.x,lane.end.x) && Max(lane.begin.x,lane.end.x)+16>=Min(a.x,b.x)
					&& Max(a.y,b.y)+16>=Min(lane.begin.y,lane.end.y) && Max(lane.begin.y,lane.end.y)+16>=Min(a.y,b.y);
			};
			return lanes.any(near) || seamLanes.any(near);
		}
		/// @brief Reserve successful inherited plots/lanes; modern collectors are never passed here.
		bool reserves(Vec2 a, Vec2 b) const
		{
			constexpr double kLaneMargin = 13.0;
			const int samples = Max(2,static_cast<int>(Ceil(a.distanceFrom(b)/10.0)));
			for (int sample = 0; sample <= samples; ++sample)
			{
				const Vec2 point = a.lerp(b,static_cast<double>(sample)/samples);
				for (const auto& core : cores)
				{
					const Vec2 delta = point-core.center, side{-core.axis.y,core.axis.x};
					if (Abs(delta.dot(core.axis))<core.halfLength && Abs(delta.dot(side))<core.halfDepth) { return true; }
				}
				for (const auto& lane : lanes)
				{
					const Vec2 span = lane.end-lane.begin;
					const double t = Clamp((point-lane.begin).dot(span)/Max(.01,span.lengthSq()),0.0,1.0);
					if (point.distanceFrom(lane.begin+span*t)<kLaneMargin && Abs((b-a).normalized().dot(span.normalized()))>Cos(25_deg)) { return true; }
				}
			}
			return false;
		}
	};

	inline bool enabled(const UrbanMorphology::Plan& plan)
	{
		using Type = UrbanStructure::Type;
		return plan.scale == 0 && !plan.frontageRoads && plan.origin != UrbanMorphology::Origin::Industrial
			&& plan.origin != UrbanMorphology::Origin::Planned && plan.origin != UrbanMorphology::Origin::Rural
			&& plan.structure != Type::None && plan.structure != Type::PlannedGrid && plan.structure != Type::HistoricGrid
			&& Min(plan.halfExtent.x,plan.halfExtent.y)>=300;
	}

	inline bool inherited(const RoadNetwork& network,const RoadEdge& edge)
	{
		for (const int routeId : edge.routeIds)
		{
			const auto* route=network.getRoute(routeId);
			if (route && (route->name.contains(U"旧村道") || route->name.contains(U"旧集落連絡道"))) { return true; }
		}
		return false;
	}

	/// @brief Splitting/snapping cannot bypass the same physical limits used by the planner.
	inline bool validNewGeometry(const RoadNetwork& roads,int firstEdge)
	{
		for (const auto& edge : roads.edges())
		{
			if (edge.id<firstEdge || !edge.hasRoadLanes()) { continue; }
			const auto curve=roads.getBezier(edge.id);
			if (!curve || !RoadAlignment::respectsLimits(*curve,edge.roadType)) { return false; }
		}
		return true;
	}

	/// @brief Terminate a new infill street on an older lane, forming a real seam T-junction.
	inline bool connectInfill(Layout& layout,const MapGenerator::Settlement& town,const World& world,
		RoadNetwork& roads,int a,int b,Vec2 localA,Vec2 localB)
	{
		const bool insideA=layout.containsCore(localA),insideB=layout.containsCore(localB);
		if (insideA==insideB) { return false; }
		const int outside=insideA ? b : a;
		// This modern node already reaches an old lane. A second approach from the
		// same gateway can run alongside it and enclose an unusable sliver.
		for (const auto& attachment : roads.getNode(outside)->attachments)
		{
			const auto* edge=roads.getEdge(attachment.edgeId);
			if (edge && inherited(roads,*edge)) { return false; }
		}
		const Vec2 desiredLocal=insideA ? localA : localB;
		const Vec2 desired=town.center+town.gridAxisX*desiredLocal.x+town.gridAxisZ*desiredLocal.y;
		const Vec3 start=roads.getNode(outside)->position;
		Optional<Vec3> goal;
		double best=Square(100.0);
		for (const auto& edge : roads.edges())
		{
			if (edge.id<0 || !inherited(roads,edge)) { continue; }
			const auto curve=roads.getBezier(edge.id); if (!curve) { continue; }
			const int samples=Max(2,static_cast<int>(Ceil(curve->totalLength/8)));
			for (int sample=0;sample<=samples;++sample)
			{
				const float arc=curve->totalLength*sample/samples;
				const Vec3 point=curve->positionAt(arc);
				const Vec2 delta{point.x-start.x,point.z-start.z};
				if (delta.length()<18 || delta.length()>190) { continue; }
				const Vec3 tangent=curve->tangentAt(arc);
				if (Abs(delta.normalized().dot(Vec2{tangent.x,tangent.z}.normalized()))>Cos(28_deg)) { continue; }
				const double distance=desired.distanceFromSq({point.x,point.z});
				if (distance<best) { best=distance;goal=point; }
			}
		}
		if (!goal) { return false; }
		const int samples=Max(2,static_cast<int>(Ceil(start.distanceFrom(*goal)/10.0)));
		for (int sample=0;sample<=samples;++sample)
		{
			const Vec3 point=start.lerp(*goal,static_cast<double>(sample)/samples);
			const Vec2 delta{point.x-town.center.x,point.z-town.center.y};
			const Vec2 local{delta.dot(town.gridAxisX),delta.dot(town.gridAxisZ)};
			if (UrbanMorphology::isReservedGreen(town.plan,local) || (town.plan.civic && town.plan.civic->contains(local))) { return false; }
		}
		const auto alignment=RoadAlignment::fitTerrain(world,{start,*goal},RoadType::LocalRoad);
		if (!alignment || RoadAlignment::maximumClearance(world,alignment->curves)>2.5) { return false; }
		++layout.transactionCopies;
		RoadNetwork proposed=roads;
		RoadEdge profile; GeneratedStreet::apply(profile,GeneratedStreet::describe(GeneratedStreet::Role::Local));
		const int firstEdge=proposed.nextEdgeId();
		Array<int> added=RoadAutoPlace::buildAlignment(proposed,world,alignment->curves,profile,.5f);
		if (added.isEmpty()) { return false; }
		proposed.resolveIntersections(firstEdge,&added);
		if (!validNewGeometry(proposed,firstEdge)) { return false; }
		for (const int id : added) { proposed.getEdge(id)->edgeState=EdgeState::Existing; }
		for (const int id : added)
		{
			const auto curve=proposed.getBezier(id); if (!curve) { continue; }
			const int count=Max(2,static_cast<int>(Ceil(curve->totalLength/10.0)));
			const auto local=[&](Vec3 point)
			{
				const Vec2 delta{point.x-town.center.x,point.z-town.center.y};
				return Vec2{delta.dot(town.gridAxisX),delta.dot(town.gridAxisZ)};
			};
			for (int sample=0;sample<count;++sample)
			{
				layout.seamLanes << Line{local(curve->positionAt(curve->totalLength*sample/count)),local(curve->positionAt(curve->totalLength*(sample+1)/count))};
			}
		}
		roads=std::move(proposed); ++layout.seamConnections;
		return true;
	}

	/// @brief Build inherited lanes on a proposed graph before regular local streets are admitted.
	inline Layout build(const MapGenerator::Settlement& town, const World& world, RoadNetwork& roads)
	{
		Layout result;
		if (!enabled(town.plan)) { return result; }
		const auto& plan = town.plan;
		const auto local = [&](Vec3 p)
		{
			const Vec2 delta{p.x-town.center.x,p.z-town.center.y};
			return Vec2{delta.dot(town.gridAxisX),delta.dot(town.gridAxisZ)};
		};
		const auto position = [&](Vec2 p)
		{
			const Vec2 global = town.center+town.gridAxisX*p.x+town.gridAxisZ*p.y;
			return Vec3{global.x,world.sampleHeight(static_cast<float>(global.x),static_cast<float>(global.y)),global.y};
		};
		const auto allowed = [&](Vec2 p)
		{
			if (!UrbanMorphology::inCore(plan,p,-12) || UrbanMorphology::isReservedGreen(plan,p)
				|| (plan.civic && plan.civic->contains(p)) || (plan.industry.w>0 && plan.industry.contains(p))) { return false; }
			for (const auto& center : plan.centers)
			{
				if (center.rail && Abs(p.x-center.position.x)<48 && Abs(p.y-center.position.y)<32) { return false; }
			}
			const Vec3 worldPoint = position(p);
			return worldPoint.y>=world.waterSurfaceHeight(worldPoint.x,worldPoint.z)+2.0;
		};
		Array<int> anchors;
		for (const auto& node : roads.nodes())
		{
			if (node.id<0 || node.attachments.isEmpty() || !UrbanMorphology::inCore(plan,local(node.position),-8)) { continue; }
			bool road = false;
			for (const auto& attachment : node.attachments)
			{
				const auto* edge = roads.getEdge(attachment.edgeId);
				road |= edge && edge->hasRoadLanes();
			}
			if (road) { anchors << node.id; }
		}
		// A hamlet must join the connected collector backbone, not an isolated clipped stub.
		const HashSet<int> candidates{anchors.begin(),anchors.end()};
		HashSet<int> seen;
		Array<int> largest;
		for (const int start : anchors)
		{
			if (!seen.insert(start).second) { continue; }
			Array<int> component{start};
			for (size_t cursor=0;cursor<component.size();++cursor)
			{
				for (const auto& attachment : roads.getNode(component[cursor])->attachments)
				{
					const auto* edge=roads.getEdge(attachment.edgeId);
					if (!edge || !edge->hasRoadLanes()) { continue; }
					const int next=edge->nodeA==component[cursor] ? edge->nodeB : edge->nodeA;
					if (candidates.contains(next) && seen.insert(next).second) { component << next; }
				}
			}
			if (component.size()>largest.size()) { largest=std::move(component); }
		}
		anchors=std::move(largest);
		const double extent = Min(plan.halfExtent.x,plan.halfExtent.y);
		const double halfLength = Min(290.0,extent*.26), halfDepth = Min(205.0,extent*.19);
		const auto anchor = [&](const RoadNetwork& network,Vec2 desired,Optional<int> exclude = none)->Optional<int>
		{
			Optional<int> best;
			double distance = Square(Max(180.0,halfLength));
			for (const int id : anchors)
			{
				if (exclude && id==*exclude) { continue; }
				const auto* node = network.getNode(id);
				if (!node || node->attachments.isEmpty() || node->attachments.size()>=5) { continue; }
				const double candidate = local(node->position).distanceFromSq(desired);
				if (candidate<distance) { distance=candidate; best=id; }
			}
			return best;
		};
		HashSet<int> backboneNodes{anchors.begin(),anchors.end()}; Array<CubicBezier> backboneCurves;
		for (const auto& edge : roads.edges())
		{
			if (edge.id>=0 && edge.hasRoadLanes() && backboneNodes.contains(edge.nodeA) && backboneNodes.contains(edge.nodeB))
			{
				if (const auto curve=roads.getBezier(edge.id)) { backboneCurves << *curve; }
			}
		}
		/// Retain a small set of distinct backbone gateways, including true mid-edge T joins.
		const auto gatewayCandidates=[&](const RoadNetwork& network,Vec2 desired,Vec3 towardInterior)
		{
			struct Candidate { Vec3 position; double distance; bool nearParallel; }; Array<Candidate> candidates;
			const double radius=Max(180.0,halfLength),maximum=Square(radius);
			Array<int> localNodes;
			for (const auto& node : network.nodes())
			{
				if (node.id<0) { continue; } const Vec2 delta=local(node.position)-desired;
				if (Abs(delta.x)<=radius+1 && Abs(delta.y)<=radius+1) { localNodes << node.id; }
			}
			const auto liveNodeNear=[&](Vec3 point)->const RoadNode*
			{
				const RoadNode* best=nullptr; float distance=.5f;
				for (const int id : localNodes)
				{
					const auto* node=network.getNode(id);
					const float x=static_cast<float>(node->position.x-point.x),z=static_cast<float>(node->position.z-point.z);
					const float candidate=std::sqrt(x*x+z*z);
					if (candidate<=distance) { distance=candidate; best=node; }
				}
				return best;
			};
			const auto parallelAtNode=[&](const RoadNode& node)
			{
				const Vec2 delta{towardInterior.x-node.position.x,towardInterior.z-node.position.z};
				if (delta.lengthSq()<1e-8) { return true; }
				const Vec2 direction=delta.normalized(); bool nearParallel=false;
				for (const auto& attachment : node.attachments)
				{
					const auto* edge=network.getEdge(attachment.edgeId); if (!edge || !edge->hasRoadLanes()) { continue; }
					const Vec3 arm=(edge->nodeA==node.id ? edge->ctrlA : edge->ctrlB)-node.position;
					const Vec2 outward{arm.x,arm.z};
					nearParallel |= outward.lengthSq()>1e-8 && direction.dot(outward.normalized())>Cos(30_deg);
				}
				return nearParallel;
			};
			const auto append=[&](Vec3 position,bool nearParallel)
			{
				const double distance=local(position).distanceFromSq(desired); if (distance>=maximum) { return; }
				candidates << Candidate{position,distance,nearParallel};
			};
			for (const int id : anchors)
			{
				const auto* node=network.getNode(id); if (!node || node->attachments.isEmpty() || node->attachments.size()>=5) { continue; }
				append(node->position,parallelAtNode(*node));
			}
			for (const auto& curve : backboneCurves)
			{
				Optional<Vec3> closest; Vec2 tangent{0,0}; double best=maximum;
				// Leave room around the original collector junctions; existing nodes remain candidates.
				for (float arc=18;arc<curve.totalLength-18;arc+=4)
				{
					const Vec3 point=curve.positionAt(arc); const double distance=local(point).distanceFromSq(desired);
					if (distance<best) { best=distance; closest=point; const Vec3 forward=curve.tangentAt(arc); tangent={forward.x,forward.z}; }
				}
				if (closest && tangent.lengthSq()>1e-8)
				{
					const Vec2 delta{towardInterior.x-closest->x,towardInterior.z-closest->z};
					if (const auto* node=liveNodeNear(*closest))
					{
						if (!node->attachments.isEmpty() && node->attachments.size()<5) { append(node->position,parallelAtNode(*node)); }
					}
					else if (delta.lengthSq()>1e-8) { append(*closest,Abs(delta.normalized().dot(tangent.normalized()))>Cos(30_deg)); }
				}
			}
			candidates.sort_by([](const Candidate& a,const Candidate& b)
			{
				// Prefer a compatible T/clear departure within the existing bounded radius.
				// This is ordering only; the fitted-curve check still decides acceptance.
				if (a.nearParallel!=b.nearParallel) { return !a.nearParallel; }
				if (a.distance!=b.distance) { return a.distance<b.distance; }
				if (a.position.x!=b.position.x) { return a.position.x<b.position.x; }
				return a.position.z<b.position.z;
			});
			Array<Vec3> result;
			for (const auto& candidate : candidates)
			{
				if (result.any([&](Vec3 point) { return point.distanceFrom(candidate.position)<18; })) { continue; }
				result << candidate.position; if (result.size()==4) { break; }
			}
			return result;
		};
		RoadEdge laneTemplate;
		GeneratedStreet::apply(laneTemplate,GeneratedStreet::describe(GeneratedStreet::Role::Village));
		const auto addPath = [&](RoadNetwork& network,const Array<Vec3>& points,StringView name,
			Array<Line>& retained,double& length,Optional<Vec3>* gatewayOutput,Optional<int>* routeIdentity,bool collector)->bool
		{
			for (size_t i=1;i<points.size();++i)
			{
				const int samples = Max(2,static_cast<int>(Ceil(points[i-1].distanceFrom(points[i])/12.0)));
				for (int sample=0;sample<=samples;++sample)
				{
					if (!allowed(local(points[i-1].lerp(points[i],static_cast<double>(sample)/samples)))) { ++result.blockedPaths; return false; }
				}
			}
			RoadEdge pathTemplate=laneTemplate;
			if (collector) { GeneratedStreet::apply(pathTemplate,GeneratedStreet::describe(GeneratedStreet::Role::Collector)); }
			const auto alignment = RoadAlignment::fitTerrain(world,points,pathTemplate.roadType);
			if (!alignment || RoadAlignment::maximumClearance(world,alignment->curves)>2.5) { ++result.fitFailures; return false; }
			if (!collector && UrbanDeparture::findConflict(network,alignment->curves,pathTemplate)) { ++result.closeDepartures; return false; }
			Array<Line> lines;
			double pathLength = 0;
			for (const auto& curve : alignment->curves)
			{
				const int samples = Max(2,static_cast<int>(Ceil(curve.totalLength/10.0)));
				for (int sample=0;sample<samples;++sample)
				{
					const Vec3 a = curve.positionAt(curve.totalLength*sample/samples);
					const Vec3 b = curve.positionAt(curve.totalLength*(sample+1)/samples);
					if (!allowed(local(a)) || !allowed(local(b))) { ++result.blockedPaths; return false; }
					lines << Line{local(a),local(b)};
				}
				pathLength += curve.totalLength;
			}
			if (pathLength>points.front().distanceFrom(points.back())*2.2) { ++result.detourFailures; return false; }
			// Only one short-lived path transaction is retained at a time.
			++result.transactionCopies;
			RoadNetwork transaction = network;
			const int firstEdge=transaction.nextEdgeId();
			Array<int> added = RoadAutoPlace::buildAlignment(transaction,world,alignment->curves,pathTemplate,.5f);
			if (added.isEmpty()) { ++result.joinFailures; return false; }
			transaction.resolveIntersections(firstEdge,&added);
			if (!validNewGeometry(transaction,firstEdge)) { ++result.joinFailures; return false; }
			if (!collector && UrbanDeparture::findRejoinedConflict(transaction,added)) { ++result.rejoinedLoops; return false; }
			for (const int id : added) { transaction.getEdge(id)->edgeState=EdgeState::Existing; }
			const int routeId=transaction.addRoute(RoadRouteKind::CityRoute,String{name},added);
			if (routeIdentity) { *routeIdentity=routeId; }
			network=std::move(transaction);
			if (gatewayOutput)
			{
				// The historical connector joins the established village entrance. A midpoint
				// target duplicated the primary street's approach and enclosed long slivers.
				const Vec3 toward=position(plan.oldCore);
				*gatewayOutput=points.front().distanceFromSq(toward)<points.back().distanceFromSq(toward) ? points.front() : points.back();
			}
			retained.append(lines); length+=pathLength;
			return true;
		};
		const Array<Vec2> sites{{-.50,.38},{.40,.48},{.55,-.40},{-.45,-.46},{.08,.59},{-.58,.02}};
		for (size_t trial=0;trial<sites.size() && result.cores.size()<kRequestedCores;++trial)
		{
			++result.coreTrials;
			const auto rejectedSite=[&](StringView reason) { DBG_LOG(U"[UrbanMosaicSite] town={} trial={} reason={}"_fmt(town.name,trial+1,reason)); };
			const Vec2 normalized = sites[(trial+plan.salt%sites.size())%sites.size()];
			const Vec2 center{normalized.x*plan.halfExtent.x,normalized.y*plan.halfExtent.y};
			if (!allowed(center)) { rejectedSite(U"terrain/reserved/boundary"); continue; }
			if (center.distanceFrom(plan.oldCore)<halfLength*.7) { rejectedSite(U"existing old center"); continue; }
			if (result.cores.any([&](const Core& core) { return core.center.distanceFrom(center)<2.0*Sqrt(Square(halfLength)+Square(halfDepth))+40.0; })) { rejectedSite(U"overlaps accepted old core"); continue; }
			bool station = false;
			for (const auto& existing : plan.centers) { station |= existing.rail && existing.position.distanceFrom(center)<Max(110.0,halfDepth*.65); }
			if (station) { rejectedSite(U"station reservation"); continue; }
			const Vec2 axis = (plan.oldCore-center).normalized(), side{-axis.y,axis.x};
			// Access-directed orientations form coherent old districts, not random node jitter.
			if (Min(Abs(axis.x),Abs(axis.y))<Sin(9_deg)) { rejectedSite(U"no distinct access-directed orientation"); continue; }
			++result.transactionCopies;
			RoadNetwork proposed = roads;
			Array<Line> inherited;
			double inheritedLength = 0;
			Optional<Vec3> gateway;
			Optional<int> primaryRoute;
			int paths = 0, branchPaths = 0;
			for (int path=0;path<5;++path)
			{
				const bool cross = path>=3;
				const Vec2 direction = cross ? side : axis, normal = cross ? -axis : side;
				const double reach = cross ? halfDepth : halfLength;
				const double offset = cross ? (path==3 ? -.37 : .35)*halfLength
					: (path==0 ? 0 : (path==1 ? -.53 : .56)*halfDepth);
				const Vec2 origin = center+(cross ? axis : side)*offset;
				const double startSide = cross && path==4 ? 1.0 : -1.0;
				const double bend=reach*(path%2==0 ? .075 : -.06);
				const Vec2 firstInterior=cross ? origin+direction*(reach*.42*startSide)+normal*bend : origin-direction*(reach*.35)+normal*bend;
				const Vec2 lastInterior=origin+direction*(reach*.30)-normal*(bend*.35);
				const auto starts=gatewayCandidates(proposed,origin+direction*(startSide*(reach+36)),position(firstInterior));
				if (starts.isEmpty()) { ++result.unbuiltPaths; continue; }
				Array<Vec3> finishes;
				if (cross && primaryRoute)
				{
					// Older side lanes meet the spine as offset T-junctions, not another full grid.
					Optional<Vec3> finish; double best=Square(60.0);
					for (const int id : proposed.getRoute(*primaryRoute)->edgeIds)
					{
						const auto curve=proposed.getBezier(id); if (!curve) { continue; }
						const int samples=Max(2,static_cast<int>(Ceil(curve->totalLength/8.0)));
						for (int sample=0;sample<=samples;++sample)
						{
							const Vec3 point=curve->positionAt(curve->totalLength*sample/samples);
							const double distance=local(point).distanceFromSq(origin);
							if (distance<best) { best=distance; finish=point; }
						}
					}
					if (finish) { finishes << *finish; }
				}
				else if (!cross) { finishes=gatewayCandidates(proposed,origin+direction*(reach+36),position(lastInterior)); }
				if (finishes.isEmpty()) { ++result.unbuiltPaths; continue; }
				// Eight bounded combinations cover both endpoint alternatives without an unbounded search.
				const Array<Point> attempts{{0,0},{1,0},{0,1},{1,1},{2,0},{0,2},{3,0},{0,3}};
				bool builtPath=false;
				for (const Point attempt : attempts)
				{
					if (attempt.x>=static_cast<int>(starts.size()) || attempt.y>=static_cast<int>(finishes.size())) { continue; }
					const Vec3 a=starts[attempt.x],finish=finishes[attempt.y]; if (a.distanceFrom(finish)<100) { continue; }
					++result.gatewayAttempts;
					const Array<Vec3> points=cross
						? Array<Vec3>{a,position(origin+direction*(reach*.42*startSide)+normal*bend),finish}
						: Array<Vec3>{a,position(origin-direction*(reach*.35)+normal*bend),position(origin+direction*(reach*.30)-normal*(bend*.35)),finish};
					if (addPath(proposed,points,U"{}旧村道{}-{}"_fmt(town.name,trial+1,path+1),inherited,inheritedLength,path==0 ? &gateway : nullptr,path==0 ? &primaryRoute : nullptr,false))
					{
						++paths; branchPaths+=cross; builtPath=true; break;
					}
				}
				if (!builtPath) { ++result.unbuiltPaths; }
			}
			if (paths<4 || branchPaths<1 || !gateway) { ++result.rejected; rejectedSite(U"insufficient feasible connected lanes"); continue; }
			roads=std::move(proposed);
			result.cores << Core{center,axis,halfLength,halfDepth,*gateway};
			result.lanes.append(inherited); result.length+=inheritedLength; result.routes+=paths;
		}
		// Older settlement-to-settlement lanes remain as diagonals through the later infill.
		Array<Vec3> connected;
		if (const auto root = anchor(roads,plan.oldCore)) { connected << roads.getNode(*root)->position; }
		for (const auto& core : result.cores)
		{
			if (connected.isEmpty()) { connected << core.gateway; continue; }
			Vec3 nearest = connected.front();
			for (const Vec3 point : connected) { if (point.distanceFromSq(core.gateway)<nearest.distanceFromSq(core.gateway)) { nearest=point; } }
			const Vec2 a=local(nearest),b=local(core.gateway),span=b-a;
			if (span.length()<100) { connected << core.gateway; continue; }
			const Vec2 normal=Vec2{-span.y,span.x}.normalized();
			++result.transactionCopies;
			RoadNetwork proposed = roads;
			Array<Line> inherited;
			double length = 0;
			const Array<Vec3> points{nearest,position(a.lerp(b,.48)+normal*Min(28.0,span.length()*.025)),core.gateway};
			if (addPath(proposed,points,U"{}旧集落連絡道{}"_fmt(town.name,connected.size()),inherited,length,nullptr,nullptr,true))
			{
				roads=std::move(proposed); result.lanes.append(inherited); result.length+=length; ++result.routes;
			}
			else { ++result.rejected; }
			connected << core.gateway;
		}
		DBG_LOG(U"[UrbanMosaic] town={} requestedCores={} coreTrials={} cores={} inheritedRoutes={} inheritedMeters={:.1f} rejected={} gatewayAttempts={} closeDepartures={} rejoinedLoops={} unbuiltPaths={} blockedPaths={} fitFailures={} joinFailures={} detourFailures={}"_fmt(
			town.name,kRequestedCores,result.coreTrials,result.cores.size(),result.routes,result.length,result.rejected,result.gatewayAttempts,result.closeDepartures,result.rejoinedLoops,result.unbuiltPaths,result.blockedPaths,result.fitFailures,result.joinFailures,result.detourFailures));
		return result;
	}
}

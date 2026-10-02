#pragma once
#include "StreetBlocks.hpp"
#include "../world/World.hpp"

/// @brief Read-only design check for long, unusably narrow approaches at shared gateways.
namespace UrbanDeparture
{
	struct Conflict
	{
		bool atFinish=false;
		int existingEdge=-1;
		double closeLength=0,requiredSeparation=0;
	};
	namespace Detail
	{
		inline constexpr double kGatewayRadius=.5;
		inline constexpr double kSampleStep=2;
		inline constexpr double kTraceLimit=320;
		inline constexpr int kTraceEdgeLimit=128;
		struct Sample { Vec3 point; Vec2 direction; RoadGeometry::LateralRange range; };
		struct Gateway { int edge=-1; float arc=0; bool forward=true; };
		struct ReachedNode { int node=-1; size_t sample=0; };
		inline Vec2 flat(Vec3 point) { return {point.x,point.z}; }
		inline RoadGeometry::LateralRange oriented(RoadGeometry::LateralRange range,bool forward)
		{
			if (!forward) { const float left=range.left; range.left=-range.right; range.right=-left; }
			return range;
		}
		inline Optional<Sample> sample(const CubicBezier& curve,const RoadEdge& edge,float arc,bool forward)
		{
			const auto range=oriented(RoadGeometry::structuralRangeAt(edge,arc/Max(.001f,curve.totalLength)),forward);
			const Vec2 tangent=flat(curve.tangentAt(arc))*(forward ? 1 : -1);
			if (!range.valid || tangent.lengthSq()<1e-12) { return none; }
			return Sample{curve.positionAt(arc),tangent.normalized(),range};
		}
		inline Optional<Vec2> outward(const RoadNetwork& network,const RoadEdge& edge,int node)
		{
			const auto* endpoint=network.getNode(node); if (!endpoint) { return none; }
			const Vec2 direction=flat((edge.nodeA==node ? edge.ctrlA : edge.ctrlB)-endpoint->position);
			if (direction.lengthSq()<1e-12) { return none; }
			return direction.normalized();
		}
		inline bool usable(const RoadEdge& edge,const RoadEdge& proposed)
		{
			return edge.id>=0 && edge.hasRoadLanes() && !edge.tunnel && !proposed.tunnel
				&& edge.useElevation==proposed.useElevation;
		}
		/// @brief Match geometric cross sections in travel order, including reversed pieces.
		inline bool sameProfile(const RoadEdge& first,bool firstForward,const RoadEdge& second,bool secondForward)
		{
			if (first.roadType!=second.roadType || first.lanes.size()!=second.lanes.size()
				|| first.useElevation!=second.useElevation || first.tunnel!=second.tunnel) { return false; }
			const float firstT=firstForward ? 1.0f : 0.0f,secondT=secondForward ? 0.0f : 1.0f;
			const auto equal=[](const auto& a,const auto& b)
			{
				return a.valid && b.valid && Abs(a.left-b.left)<=.1 && Abs(a.right-b.right)<=.1;
			};
			return equal(oriented(RoadGeometry::structuralRangeAt(first,firstT),firstForward),oriented(RoadGeometry::structuralRangeAt(second,secondT),secondForward))
				&& equal(oriented(RoadGeometry::roadbedRangeAt(first,firstT),firstForward),oriented(RoadGeometry::roadbedRangeAt(second,secondT),secondForward));
		}
		inline float closestArc(const CubicBezier& curve,Vec3 point)
		{
			const auto distance=[&](float arc) { return flat(curve.positionAt(arc)-point).lengthSq(); };
			constexpr int kSamples=32; int best=0; double minimum=distance(0);
			for (int i=1;i<=kSamples;++i)
			{
				const double value=distance(curve.totalLength*i/kSamples);
				if (value<minimum) { minimum=value; best=i; }
			}
			float low=curve.totalLength*Max(0,best-1)/kSamples,high=curve.totalLength*Min(kSamples,best+1)/kSamples;
			for (int i=0;i<18;++i)
			{
				const float a=low+(high-low)/3,b=high-(high-low)/3;
				if (distance(a)<distance(b)) { high=b; } else { low=a; }
			}
			return (low+high)*.5f;
		}
		inline Array<Gateway> gateways(const RoadNetwork& network,Vec3 point,const RoadEdge& proposed)
		{
			Array<Gateway> result;
			// Match RoadAutoPlace's node-first endpoint resolution. Shared nodes have
			// several outward arms; a genuine through continuation uses the signed dot.
			if (const auto id=network.findNodeNear(point,static_cast<float>(kGatewayRadius)))
			{
				for (const auto& attachment : network.getNode(*id)->attachments)
				{
					const auto* edge=network.getEdge(attachment.edgeId); if (!edge || !usable(*edge,proposed)) { continue; }
					const auto curve=network.getBezier(edge->id); if (!curve || curve->totalLength<.1f) { continue; }
					const bool forward=edge->nodeA==*id;
					result << Gateway{edge->id,forward ? 0.0f : curve->totalLength,forward};
				}
				return result;
			}
			for (const auto& edge : network.edges())
			{
				if (!usable(edge,proposed)) { continue; }
				const auto curve=network.getBezier(edge.id); if (!curve || curve->totalLength<.1f) { continue; }
				if (point.x<Min({curve->p0.x,curve->p1.x,curve->p2.x,curve->p3.x})-kGatewayRadius
					|| point.x>Max({curve->p0.x,curve->p1.x,curve->p2.x,curve->p3.x})+kGatewayRadius
					|| point.z<Min({curve->p0.z,curve->p1.z,curve->p2.z,curve->p3.z})-kGatewayRadius
					|| point.z>Max({curve->p0.z,curve->p1.z,curve->p2.z,curve->p3.z})+kGatewayRadius) { continue; }
				const float arc=closestArc(*curve,point); const Vec3 projected=curve->positionAt(arc);
				if (flat(projected-point).length()>kGatewayRadius || Abs(projected.y-point.y)>1) { continue; }
				if (arc>.01f) { result << Gateway{edge.id,arc,false}; }
				if (arc<curve->totalLength-.01f) { result << Gateway{edge.id,arc,true}; }
			}
			return result;
		}
		/// @brief Follow only a unique same-profile near-straight arm, across real subdivisions.
		inline Array<Sample> trace(const RoadNetwork& network,Gateway gateway,const RoadEdge& proposed,
			const HashSet<int>* excluded=nullptr,Array<ReachedNode>* reached=nullptr)
		{
			Array<Sample> result; HashSet<int> visited; double distance=0;
			for (int piece=0;piece<kTraceEdgeLimit && distance<kTraceLimit;++piece)
			{
				if ((excluded && excluded->contains(gateway.edge)) || !visited.insert(gateway.edge).second) { break; }
				const auto* edge=network.getEdge(gateway.edge); const auto curve=network.getBezier(gateway.edge);
				if (!edge || !curve || !usable(*edge,proposed)) { break; }
				const auto head=sample(*curve,*edge,gateway.arc,gateway.forward); if (!head) { break; }
				if (result.isEmpty()) { result << *head; }
				const double available=gateway.forward ? curve->totalLength-gateway.arc : gateway.arc;
				const double length=Min(available,kTraceLimit-distance);
				for (double travelled=Min(kSampleStep,length);travelled>0;travelled=Min(travelled+kSampleStep,length))
				{
					const float arc=static_cast<float>(gateway.arc+(gateway.forward ? travelled : -travelled));
					const auto point=sample(*curve,*edge,arc,gateway.forward); if (!point) { return result; }
					result << *point;
					if (travelled>=length) { break; }
				}
				distance+=length;
				if (length<available) { break; }
				const int node=gateway.forward ? edge->nodeB : edge->nodeA;
				if (reached) { *reached << ReachedNode{node,result.size()-1}; }
				if (distance>=kTraceLimit) { break; }
				const auto incoming=outward(network,*edge,node); if (!incoming) { break; }
				Optional<Gateway> next; bool ambiguous=false;
				for (const auto& attachment : network.getNode(node)->attachments)
				{
					const auto* candidate=network.getEdge(attachment.edgeId);
					if (!candidate || visited.contains(candidate->id) || (excluded && excluded->contains(candidate->id)) || !usable(*candidate,proposed)) { continue; }
					const bool forward=candidate->nodeA==node;
					const auto direction=outward(network,*candidate,node);
					if (!direction || (-*incoming).dot(*direction)<Cos(10_deg) || !sameProfile(*edge,gateway.forward,*candidate,forward)) { continue; }
					const auto candidateCurve=network.getBezier(candidate->id); if (!candidateCurve || candidateCurve->totalLength<.1f) { continue; }
					if (next) { ambiguous=true; break; }
					next=Gateway{candidate->id,forward ? 0.0f : candidateCurve->totalLength,forward};
				}
				if (ambiguous || !next) { break; }
				gateway=*next;
			}
			return result;
		}
		inline Optional<Sample> proposedSample(const Array<CubicBezier>& curves,const RoadEdge& profile,double distance,bool atFinish)
		{
			for (size_t index=0;index<curves.size();++index)
			{
				const auto& curve=curves[atFinish ? curves.size()-1-index : index];
				if (distance<=curve.totalLength)
				{
					return sample(curve,profile,static_cast<float>(atFinish ? curve.totalLength-distance : distance),!atFinish);
				}
				distance-=curve.totalLength;
			}
			return none;
		}
		struct Nearest { Sample sample; double distance=0; };
		inline Optional<Nearest> nearest(const Array<Sample>& points,Vec3 position)
		{
			Optional<Nearest> result;
			for (size_t i=1;i<points.size();++i)
			{
				const auto& a=points[i-1]; const auto& b=points[i]; const Vec2 span=flat(b.point-a.point);
				if (span.lengthSq()<1e-12) { continue; }
				const double t=Clamp(flat(position-a.point).dot(span)/span.lengthSq(),0.0,1.0);
				const Vec3 point=a.point.lerp(b.point,t); const double distance=flat(point-position).length();
				if (result && result->distance<=distance) { continue; }
				RoadGeometry::LateralRange range;
				range.valid=true; range.left=static_cast<float>(Math::Lerp(a.range.left,b.range.left,t)); range.right=static_cast<float>(Math::Lerp(a.range.right,b.range.right,t));
				result=Nearest{Sample{point,span.normalized(),range},distance};
			}
			return result;
		}
		inline double facing(const Sample& sample,Vec2 toward)
		{
			const Vec2 right{sample.direction.y,-sample.direction.x};
			return right.dot(toward)>=0 ? Max(0.0,static_cast<double>(sample.range.right)) : Max(0.0,-static_cast<double>(sample.range.left));
		}
		/// @brief Apply one shared duration/spacing rule to actual oriented road samples.
		template <class Sampler>
		inline Optional<Conflict> closeDeparture(const Array<Sample>& corridor,const Sample& first,
			Sampler&& at,int existingEdge,bool atFinish)
		{
			if (corridor.size()<2 || first.direction.dot(corridor.front().direction)<=Cos(30_deg)) { return none; }
			const auto& settings=GenerationSettings::get();
			const double homeClearance=2*(buildingFootprintXZ(BuildingType::Detached)*.5+settings.development_footprintMargin+settings.parcels_roadMargin);
			const double mouth=Max(6.0,Max(static_cast<double>(first.range.width()),static_cast<double>(corridor.front().range.width()))*.5+3.0);
			double closeLength=0,maximumRequired=0;
			for (double distance=mouth;distance<=kTraceLimit;distance+=kSampleStep)
			{
				const auto current=at(distance); if (!current) { break; }
				const auto nearby=nearest(corridor,current->point); if (!nearby) { break; }
				if (flat(current->point-corridor.back().point).dot(corridor.back().direction)>.1) { break; }
				if (Abs(current->point.y-nearby->sample.point.y)>1 || current->direction.dot(nearby->sample.direction)<=Cos(30_deg)) { break; }
				const Vec2 between=flat(current->point-nearby->sample.point);
				const double required=facing(nearby->sample,between)+facing(*current,-between)+homeClearance;
				if (nearby->distance+.1>=required) { break; }
				maximumRequired=Max(maximumRequired,required); closeLength=distance-mouth;
				if (closeLength>Max(24.0,2*maximumRequired)) { return Conflict{atFinish,existingEdge,closeLength,maximumRequired}; }
			}
			return none;
		}

	}
	/// @brief Flag only a sustained, same-level close departure from a shared gateway.
	/// @details This is a bounded frontage-design preference, not an impossibility proof.
	/// It never changes the network, fitted curves, profiles, IDs, or route membership.
	inline Optional<Conflict> findConflict(const RoadNetwork& network,const Array<CubicBezier>& curves,const RoadEdge& proposed)
	{
		if (curves.isEmpty() || !proposed.hasRoadLanes() || proposed.tunnel) { return none; }
		for (const bool atFinish : {false,true})
		{
			const auto first=Detail::proposedSample(curves,proposed,0,atFinish); if (!first) { continue; }
			for (const auto& gateway : Detail::gateways(network,first->point,proposed))
			{
				const auto* existing=network.getEdge(gateway.edge); const auto curve=network.getBezier(gateway.edge);
				if (!existing || !curve) { continue; }
				const auto initial=Detail::sample(*curve,*existing,gateway.arc,gateway.forward);
				if (!initial || first->direction.dot(initial->direction)<=Cos(30_deg)) { continue; }
				const auto corridor=Detail::trace(network,gateway,proposed); if (corridor.size()<2) { continue; }
				if (const auto conflict=Detail::closeDeparture(corridor,*first,[&](double distance)
				{
					return Detail::proposedSample(curves,proposed,distance,atFinish);
				},gateway.edge,atFinish)) { return conflict; }
			}
		}
		return none;
	}
	struct RejoinedConflict
	{
		int startNode=-1,rejoinNode=-1;
		Conflict departure;
	};
	namespace Detail
	{
		struct RejoinedRun { Array<Gateway> pieces; int finish=-1; size_t corridorEnd=0; };
		/// @brief Follow candidate ownership only, accepting a rejoin at an exact existing-corridor node.
		inline Optional<RejoinedRun> rejoinedRun(const RoadNetwork& network,Gateway start,int startNode,
			const HashSet<int>& candidates,const Array<ReachedNode>& reached)
		{
			RejoinedRun result; HashSet<int> visited; double length=0;
			for (int piece=0;piece<kTraceEdgeLimit;++piece)
			{
				if (!candidates.contains(start.edge) || !visited.insert(start.edge).second) { return none; }
				const auto* edge=network.getEdge(start.edge); const auto curve=network.getBezier(start.edge);
				if (!edge || !curve || !edge->hasRoadLanes() || edge->tunnel || curve->totalLength<.1f) { return none; }
				length+=curve->totalLength; if (length>kTraceLimit) { return none; }
				result.pieces << start;
				const int node=start.forward ? edge->nodeB : edge->nodeA;
				if (node==startNode) { return none; }
				for (const auto& junction : reached)
				{
					if (junction.node==node) { result.finish=node; result.corridorEnd=junction.sample; return result; }
				}
				Optional<Gateway> next;
				for (const auto& attachment : network.getNode(node)->attachments)
				{
					if (!candidates.contains(attachment.edgeId) || visited.contains(attachment.edgeId)) { continue; }
					const auto* other=network.getEdge(attachment.edgeId); const auto otherCurve=network.getBezier(attachment.edgeId);
					if (!other || !otherCurve || !usable(*other,*edge)) { return none; }
					if (next) { return none; }
					const bool forward=other->nodeA==node;
					if (!sameProfile(*edge,start.forward,*other,forward)) { return none; }
					next=Gateway{other->id,forward ? 0.0f : otherCurve->totalLength,forward};
				}
				if (!next) { return none; } start=*next;
			}
			return none;
		}
		inline Optional<Sample> runSample(const RoadNetwork& network,const RejoinedRun& run,double distance)
		{
			for (const auto& piece : run.pieces)
			{
				const auto* edge=network.getEdge(piece.edge); const auto curve=network.getBezier(piece.edge);
				if (!edge || !curve) { return none; }
				if (distance<=curve->totalLength)
				{
					return sample(*curve,*edge,static_cast<float>(piece.forward ? distance : curve->totalLength-distance),piece.forward);
				}
				distance-=curve->totalLength;
			}
			return none;
		}
	}
	/// @brief Reject a new close departure only after it rejoins the same existing corridor.
	/// @details Pass every live candidate descendant returned by resolveIntersections. Existing
	/// host descendants may have newer IDs too; ownership cannot be inferred from an ID threshold.
	/// Candidate edges never participate in the existing-corridor trace. A lone X crossing,
	/// ambiguous continuation, or proximity without a second shared node proves no rejoined loop.
	inline Optional<RejoinedConflict> findRejoinedConflict(const RoadNetwork& network,const Array<int>& candidateEdges)
	{
		if (candidateEdges.isEmpty()) { return none; }
		const HashSet<int> candidates{candidateEdges.begin(),candidateEdges.end()};
		Array<int> nodes; HashSet<int> seen;
		for (const int id : candidateEdges)
		{
			const auto* edge=network.getEdge(id); if (!edge) { return none; }
			for (const int node : {edge->nodeA,edge->nodeB}) { if (seen.insert(node).second) { nodes << node; } }
		}
		for (const int nodeId : nodes)
		{
			const auto* node=network.getNode(nodeId); if (!node) { return none; }
			for (const auto& added : node->attachments)
			{
				if (!candidates.contains(added.edgeId)) { continue; }
				const auto* proposed=network.getEdge(added.edgeId); const auto curve=network.getBezier(added.edgeId);
				if (!proposed || !curve || !proposed->hasRoadLanes() || proposed->tunnel) { continue; }
				const bool forward=proposed->nodeA==nodeId;
				const Detail::Gateway start{proposed->id,forward ? 0.0f : curve->totalLength,forward};
				const auto first=Detail::sample(*curve,*proposed,start.arc,forward); if (!first) { continue; }
				for (const auto& attachment : node->attachments)
				{
					if (candidates.contains(attachment.edgeId)) { continue; }
					const auto* existing=network.getEdge(attachment.edgeId); const auto existingCurve=network.getBezier(attachment.edgeId);
					if (!existing || !existingCurve || !Detail::usable(*existing,*proposed)) { continue; }
					const bool oldForward=existing->nodeA==nodeId;
					const Detail::Gateway gateway{existing->id,oldForward ? 0.0f : existingCurve->totalLength,oldForward};
					const auto initial=Detail::sample(*existingCurve,*existing,gateway.arc,oldForward);
					if (!initial || first->direction.dot(initial->direction)<=Cos(30_deg)) { continue; }
					Array<Detail::ReachedNode> reached;
					auto corridor=Detail::trace(network,gateway,*proposed,&candidates,&reached);
					const auto run=Detail::rejoinedRun(network,start,nodeId,candidates,reached); if (!run) { continue; }
					corridor.resize(run->corridorEnd+1);
					if (const auto conflict=Detail::closeDeparture(corridor,*first,[&](double distance)
					{
						return Detail::runSample(network,*run,distance);
					},existing->id,false)) { return RejoinedConflict{nodeId,run->finish,*conflict}; }
				}
			}
		}
		return none;
	}
}

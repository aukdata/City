#pragma once
#include "GenerationSettings.hpp"
#include "../road/RoadNetwork.hpp"
#include "../road/RoadGeometry.hpp"
#include "ParcelRoadIndex.hpp"

/// @brief Actual enclosed street faces, including merged civic grounds and new alley subdivisions.
namespace StreetBlocks
{
	struct Block
	{
		Array<Vec2> outline; Array<int> edges; Vec2 center{0,0}; RectF bounds; double area=0;
		bool contains(Vec2 point) const
		{
			if (!bounds.contains(point)) { return false; }
			bool inside=false;
			for (size_t i=0,j=outline.size()-1;i<outline.size();j=i++)
			{
				const Vec2 a=outline[i],b=outline[j];
				if ((a.y>point.y)!=(b.y>point.y) && point.x<(b.x-a.x)*(point.y-a.y)/(b.y-a.y)+a.x) { inside=!inside; }
			}
			return inside;
		}
	};
	inline Array<Block> collect(const RoadNetwork& network,double minimumArea=-1)
	{
		Array<Block> blocks; HashSet<int64> visited;
		for (const auto& initial : network.edges())
		{
			if (initial.id<0) { continue; }
			for (const int first : {initial.nodeA,initial.nodeB})
			{
				Block block; int id=initial.id,from=first;
				const int64 start=static_cast<int64>(id)*2+(from==initial.nodeB);
				if (visited.contains(start)) { continue; }
				bool closed=false;
				for (size_t step=0;step<=network.edges().size()*2;++step)
				{
					const auto* edge=network.getEdge(id); const bool forward=edge->nodeA==from;
					const int64 key=static_cast<int64>(id)*2+(!forward);
					if (step>0 && key==start) { closed=true; break; }
					if (!visited.insert(key).second) { break; }
					block.edges << id;
					const auto curve=network.getBezier(id);
					const int count=Max(1,static_cast<int>(std::ceil(curve->totalLength/20)));
					for (int sample=0;sample<count;++sample)
					{
						const Vec3 point=curve->evaluate(forward ? static_cast<float>(sample)/count : 1-static_cast<float>(sample)/count);
						block.outline << Vec2{point.x,point.z};
					}
					const int to=forward ? edge->nodeB : edge->nodeA;
					const Vec3 reverse=curve->tangent(forward ? 1.0f : 0.0f)*(forward ? -1 : 1);
					const double reverseAngle=Math::Atan2(reverse.z,reverse.x);
					int next=-1; double best=Math::TwoPi+1;
					for (const auto& attachment : network.getNode(to)->attachments)
					{
						const auto* candidate=network.getEdge(attachment.edgeId);
						if (!candidate) { continue; }
						const auto other=network.getBezier(candidate->id);
						const Vec3 direction=other->tangent(candidate->nodeA==to ? 0.0f : 1.0f)*(candidate->nodeA==to ? 1 : -1);
						double turn=std::fmod(reverseAngle-Math::Atan2(direction.z,direction.x)+Math::TwoPi,Math::TwoPi);
						if (candidate->id==id) { turn=Math::TwoPi; }
						if (turn<best) { best=turn; next=candidate->id; }
					}
					if (next<0) { break; }
					from=to; id=next;
				}
				if (!closed || block.outline.size()<3) { continue; }
				Vec2 lower{1e9,1e9},upper{-1e9,-1e9};
				for (size_t i=0;i<block.outline.size();++i)
				{
					const Vec2 a=block.outline[i],b=block.outline[(i+1)%block.outline.size()];
					block.area+=(a.x*b.y-b.x*a.y)*.5; block.center+=a;
					lower.x=Min(lower.x,a.x); lower.y=Min(lower.y,a.y); upper.x=Max(upper.x,a.x); upper.y=Max(upper.y,a.y);
				}
				block.center/=static_cast<double>(block.outline.size()); block.bounds={lower,upper-lower};
				if (block.area>(minimumArea>=0 ? minimumArea : GenerationSettings::get().parcels_minimumBlockArea)) { blocks << std::move(block); }
			}
		}
		return blocks;
	}
	/// @brief Continuous fitted pieces share frontage while retaining distant junction exclusions.
	inline Vec2 frontageSpan(const RoadNetwork& network,int edgeId)
	{
		const auto* edge=network.getEdge(edgeId); const auto curve=network.getBezier(edgeId);
		if (!edge || !curve) { return {1,0}; }
		const auto continuation=[&](int current,int nodeId)->Optional<int>
		{
			const auto* first=network.getEdge(current); const auto* node=network.getNode(nodeId);
			if (!first || !node || node->attachments.size()!=2) { return none; }
			const int otherId=node->attachments.front().edgeId==current ? node->attachments.back().edgeId : node->attachments.front().edgeId;
			const auto* other=network.getEdge(otherId);
			if (!other || !other->hasRoadLanes() || other->roadType!=first->roadType
				|| Abs(RoadGeometry::structuralWidth(*first)-RoadGeometry::structuralWidth(*other))>.1) { return none; }
			const Vec3 firstDirection=(first->nodeA==nodeId ? first->ctrlA : first->ctrlB)-node->position;
			const Vec3 secondDirection=(other->nodeA==nodeId ? other->ctrlA : other->ctrlB)-node->position;
			const Vec2 a{firstDirection.x,firstDirection.z},b{secondDirection.x,secondDirection.z};
			if (a.lengthSq()<=1e-12 || b.lengthSq()<=1e-12 || a.normalized().dot(b.normalized())>=-Cos(10_deg)) { return none; }
			return otherId;
		};
		const auto exclusion=[&](int endpoint)
		{
			int current=edgeId,nodeId=endpoint; double distance=0; HashSet<int> visited{edgeId};
			for (;;)
			{
				const auto* currentEdge=network.getEdge(current);
				const auto next=continuation(current,nodeId);
				if (!next)
				{
					const double cutoff=currentEdge->nodeA==nodeId ? currentEdge->cutoffA : currentEdge->cutoffB;
					return Max(.25,cutoff+9.0-distance);
				}
				if (!visited.insert(*next).second) { return .25; }
				current=*next; currentEdge=network.getEdge(current);
				const auto nextCurve=network.getBezier(current);
				if (!currentEdge || !nextCurve) { return Math::Inf; }
				distance+=nextCurve->totalLength;
				nodeId=currentEdge->nodeA==nodeId ? currentEdge->nodeB : currentEdge->nodeA;
			}
		};
		return {exclusion(edge->nodeA),curve->totalLength-exclusion(edge->nodeB)};
	}
	/// @brief Shared bounded search schedule for coarse, fine, and refined frontage passes.
	struct FrontageSampling
	{
		Vec2 span{1,0};
		float arcStep=6;
		double firstSetback=0,lastSetback=0;
		static constexpr double kSetbackStep=.25;
	};
	inline constexpr int kFrontagePassCount=3;
	inline FrontageSampling frontageSampling(const RoadNetwork& network,int edgeId,int pass)
	{
		FrontageSampling result;
		const auto* edge=network.getEdge(edgeId); const auto curve=network.getBezier(edgeId);
		if (!edge || !curve || pass<0 || pass>=kFrontagePassCount) { return result; }
		result.span=pass==0 ? Vec2{edge->cutoffA+9.0,curve->totalLength-edge->cutoffB-9.0} : frontageSpan(network,edgeId);
		result.arcStep=pass==0 ? 6.0f : pass==1 ? 1.5f : .5f;
		result.firstSetback=pass==0 ? GenerationSettings::get().development_suburbanSetback : GenerationSettings::get().development_minimumRoadSetback;
		result.lastSetback=pass==2 ? Max(result.firstSetback,6.0) : result.firstSetback;
		return result;
	}
	/// @brief Available-center superset using the placement validator's exact road ribbons.
	/// @details Every rotated square of half-size r contains a disk of radius r. Only empty
	/// subtraction proves that no square can fit; a surviving center never proves a fit.
	inline Optional<double> freeCenterArea(const RoadNetwork& network,const Block& block,double footprintRadius,const ParcelRoadIndex* obstacleIndex=nullptr)
	{
		if (block.outline.size()<3) { return none; }
		const Vec2 origin=block.outline.front(); Array<Vec2> outline;
		for (const Vec2 point : block.outline) { outline << point-origin; }
		const Polygon face{outline}; if (!face) { return none; }
		Array<Polygon> regions{face};
		for (const int id : block.edges)
		{
			const auto* edge=network.getEdge(id); const auto curve=network.getBezier(id);
			if (!edge || !curve || !edge->isRoadbedBuilt() || edge->tunnel || edge->useElevation) { return none; }
			if (Vec2{curve->p1.x-curve->p0.x,curve->p1.z-curve->p0.z}.lengthSq()<1e-12
				|| Vec2{curve->p2.x-curve->p3.x,curve->p2.z-curve->p3.z}.lengthSq()<1e-12) { return none; }
			Array<Vec2> controls;
			for (const Vec3 point : {curve->p0,curve->p1,curve->p2,curve->p3}) { controls << Vec2{point.x-origin.x,point.z-origin.y}; }
			// The curved/chord boundary difference lies in this edge's convex hull.
			// Add only those local hulls, not a hull spanning concave corners of the whole face.
			const Polygon controlHull=Geometry2D::ConvexHull(controls);
			if (controlHull) { regions << controlHull; }
		}
		Optional<ParcelRoadIndex> ownedIndex;
		if (!obstacleIndex) { ownedIndex.emplace(network,true); obstacleIndex=&*ownedIndex; }
		Vec2 low=regions.front().boundingRect().tl(),high=regions.front().boundingRect().br();
		for (const auto& region : regions)
		{
			const auto bounds=region.boundingRect(); low.x=Min(low.x,bounds.x); low.y=Min(low.y,bounds.y);
			high.x=Max(high.x,bounds.x+bounds.w); high.y=Max(high.y,bounds.y+bounds.h);
		}
		bool unsupported=false;
		const RectF query=RectF{low+origin,high-low}.stretched(footprintRadius+.001);
		obstacleIndex->forEachNearby(query,[&](const ParcelGeometry::Quad& quad)
		{
			if (unsupported || regions.isEmpty()) { return; }
			Array<Vec2> points; for (const Vec2 point : quad) { points << point-origin; }
			const Polygon ribbon=Geometry2D::ConvexHull(points);
			if (!ribbon) { unsupported=true; return; }
			// Use the exact road, track and site quads already registered by placement.
			// A slightly smaller inscribed disk keeps legal touching conservative.
			const double radius=Max(0.0,footprintRadius-.001);
			const Polygon obstacle=radius>0 ? ribbon.calculateRoundBuffer(radius) : ribbon;
			if (!obstacle) { unsupported=true; return; }
			Array<Polygon> remaining;
			for (const auto& region : regions) { remaining.append(Geometry2D::Subtract(region,obstacle)); }
			regions=std::move(remaining);
		});
		if (unsupported) { return none; }
		if (regions.isEmpty()) { return 0.0; }
		double area=0; for (const auto& region : regions) { area+=region.area(); }
		// A tiny nonempty region is unresolved, never a geometric impossibility.
		return Max(area,1e-12);
	}
	/// @brief 河川迂回と町割の間に生じた、住宅幅を確保できない細長い面を隣接街区へ統合する。
	inline int mergeNarrowFaces(RoadNetwork& network)
	{
		int removed=0;
		for (int pass=0;pass<3;++pass)
		{
			HashSet<int> remove;
			for (const auto& block : collect(network))
			{
				if (block.area>GenerationSettings::get().parcels_narrowBlockArea) { continue; }
				struct Boundary { Vec2 a,b; double width; };
				Array<Boundary> boundaries;
				for (const int id : block.edges)
				{
					const auto* edge=network.getEdge(id);const auto curve=network.getBezier(id);
					const int count=Max(1,static_cast<int>(std::ceil(curve->totalLength/4)));
					for (int i=0;i<count;++i) { const Vec3 a=curve->evaluate(static_cast<float>(i)/count),b=curve->evaluate(static_cast<float>(i+1)/count);boundaries<<Boundary{{a.x,a.z},{b.x,b.z},edge->totalWidth()*.5}; }
				}
				bool fits=false;
				for (double z=block.bounds.y+1;z<block.bounds.br().y && !fits;z+=2)
					for (double x=block.bounds.x+1;x<block.bounds.br().x && !fits;x+=2)
					{
						const Vec2 p{x,z};if (!block.contains(p)) { continue; }
						bool clear=true;
						for (const auto& boundary : boundaries)
						{
							const Vec2 delta=boundary.b-boundary.a;const double t=Clamp((p-boundary.a).dot(delta)/Max(.001,delta.lengthSq()),0.0,1.0);
							if (p.distanceFrom(boundary.a+delta*t)<boundary.width+GenerationSettings::get().parcels_minimumBuildingHalfWidth) { clear=false;break; }
						}
						fits=clear;
					}
				if (fits) { continue; }
				int selected=-1;double best=1e30;
				for (const int id : block.edges)
				{
					const auto* edge=network.getEdge(id);
					if (!edge->routeIds.isEmpty()) { continue; }
					if (edge->tunnel || edge->useElevation || network.getNode(edge->nodeA)->attachments.size()<3 || network.getNode(edge->nodeB)->attachments.size()<3) { continue; }
					const double score=edge->totalWidth()*GenerationSettings::get().parcels_roadWidthPreservationWeight+edge->length;
					if (score<best) { best=score;selected=id; }
				}
				if (selected>=0) { remove.insert(selected); }
			}
			if (remove.empty()) { break; }
			// Recompute faces after each batch. Removing a cycle edge preserves connected access.
			Array<int> ids{remove.begin(),remove.end()};ids.sort();
			for (const int id : ids)
			{
				const auto* edge=network.getEdge(id);if (!edge) { continue; }const int a=edge->nodeA,b=edge->nodeB;
				if (network.getNode(a)->attachments.size()<3 || network.getNode(b)->attachments.size()<3) { continue; }
				network.removeEdge(id);network.rebuildNodeConnectivity(a,b);++removed;
			}
		}
		return removed;
	}

}

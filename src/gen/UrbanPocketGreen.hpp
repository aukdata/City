#pragma once
#include "StreetBlocks.hpp"
#include "../world/World.hpp"
#include "../world/ZoneGrid.hpp"

/// @brief Deliberate small public greens at inherited/modern street corners, not housing coverage.
namespace UrbanPocketGreen
{
	/// @brief Clip a bounded pocket to usable land; larger vacant blocks remain unresolved.
	inline Optional<Array<LandPatch>> create(const World& world,const RoadNetwork& roads,const TrainNetwork& trains,const StreetBlocks::Block& block)
	{
		if (block.outline.size()<3 || block.area>1200 || Max(block.bounds.w,block.bounds.h)>100) { return none; }
		bool inherited=false,modern=false;
		for (const int id : block.edges)
		{
			const auto* edge=roads.getEdge(id); if (!edge || edge->useElevation || edge->tunnel) { return none; }
			bool older=false;
			for (const int routeId : edge->routeIds)
			{
				const auto* route=roads.getRoute(routeId);
				older |= route && (route->name.contains(U"旧村道") || route->name.contains(U"旧集落連絡道"));
			}
			inherited |= older; modern |= !older;
		}
		if (!inherited || !modern) { return none; }
		const Vec2 origin=block.outline.front(); Array<Vec2> outline;
		for (const Vec2 point : block.outline) { outline << point-origin; }
		const Polygon face{outline}; if (!face) { return none; }
		Array<Polygon> regions{face};
		const auto subtract=[&](const Polygon& obstacle)
		{
			Array<Polygon> remaining;
			for (const auto& region : regions) { remaining.append(Geometry2D::Subtract(region,obstacle)); }
			regions=std::move(remaining);
		};
		for (const auto& edge : roads.edges())
		{
			if (edge.id<0 || !edge.isRoadbedBuilt()) { continue; }
			const auto curve=roads.getBezier(edge.id); if (!curve || curve->totalLength<.1f) { continue; }
			const auto firstRange=RoadGeometry::structuralRangeAt(edge,0),lastRange=RoadGeometry::structuralRangeAt(edge,1);
			if (!firstRange.valid || !lastRange.valid) { return none; }
			const double padding=Max({Abs(firstRange.left),Abs(firstRange.right),Abs(lastRange.left),Abs(lastRange.right)})+GenerationSettings::get().parcels_roadMargin+.16;
			const RectF bounds{Min({curve->p0.x,curve->p1.x,curve->p2.x,curve->p3.x})-padding,
				Min({curve->p0.z,curve->p1.z,curve->p2.z,curve->p3.z})-padding,
				Max({curve->p0.x,curve->p1.x,curve->p2.x,curve->p3.x})-Min({curve->p0.x,curve->p1.x,curve->p2.x,curve->p3.x})+padding*2,
				Max({curve->p0.z,curve->p1.z,curve->p2.z,curve->p3.z})-Min({curve->p0.z,curve->p1.z,curve->p2.z,curve->p3.z})+padding*2};
			if (!bounds.intersects(block.bounds)) { continue; }
			if (edge.useElevation || edge.tunnel) { return none; }
			bool valid=true;
			ParcelRoadGeometry::forEachRibbon(edge,*curve,[&](const ParcelGeometry::Quad& quad)
			{
				Array<Vec2> points; for (const Vec2 point : quad) { points << point-origin; }
				const Polygon ribbon=Geometry2D::ConvexHull(points);
				if (!ribbon) { valid=false; return; }
				const Polygon buffered=ribbon.calculateRoundBuffer(.15);
				if (!buffered) { valid=false; return; }
				subtract(buffered);
			});
			if (!valid) { return none; }
		}
		// Existing railway landscape exclusions include tracks, stations and depots.
		for (const auto& site : RailwaySite::landscapeFootprints(trains,world))
		{
			Array<Vec2> points; for (const Vec2 point : site) { points << point-origin; }
			const Polygon shape=Geometry2D::ConvexHull(points);
			if (!shape || shape.intersects(face)) { return none; }
		}
		const double padding=maximumBuildingFootprint()+16;
		const int x0=Max(0,static_cast<int>(Floor((block.bounds.x-padding)/16))),x1=Min(WORLD_CHUNKS*ZONE_CELLS-1,static_cast<int>(Ceil((block.bounds.x+block.bounds.w+padding)/16)));
		const int z0=Max(0,static_cast<int>(Floor((block.bounds.y-padding)/16))),z1=Min(WORLD_CHUNKS*ZONE_CELLS-1,static_cast<int>(Ceil((block.bounds.y+block.bounds.h+padding)/16)));
		for (int z=z0;z<=z1;++z) for (int x=x0;x<=x1;++x)
		{
			const Point coord{x/ZONE_CELLS,z/ZONE_CELLS}; const auto* chunk=world.getChunk(coord); if (!chunk) { continue; }
			const auto& building=chunk->buildingGrid[{x%ZONE_CELLS,z%ZONE_CELLS}];
			if (building.type==BuildingType::None || building.type==BuildingType::Farmland) { continue; }
			const Vec2 center=ZoneGrid::cellCenterXZ(coord,x%ZONE_CELLS,z%ZONE_CELLS)+Vec2{building.offsetX,building.offsetZ};
			Array<Vec2> points;
			for (const Vec2 point : ParcelGeometry::footprint(center,buildingFootprintXZ(building.type)*.5+.35,building.angle)) { points << point-origin; }
			const Polygon occupied=Geometry2D::ConvexHull(points); if (occupied) { subtract(occupied); }
		}
		// Existing private parcels and public land are never overwritten by this treatment.
		for (int z=z0/ZONE_CELLS;z<=z1/ZONE_CELLS;++z) for (int x=x0/ZONE_CELLS;x<=x1/ZONE_CELLS;++x)
		{
			const auto* chunk=world.getChunk({x,z}); if (!chunk) { continue; }
			for (const auto& patch : chunk->landPatches)
			{
				Array<Vec2> points; for (const Vec2 point : patch.polygon) { points << point-origin; }
				const Polygon existing{points}; if (existing && existing.boundingRect().intersects(face.boundingRect())) { subtract(existing); }
			}
		}
		double area=0;
		for (const auto& region : regions) { area+=region.area(); }
		// Design limits for a small landscaped corner, not a claim that housing is impossible.
		if (area<12 || area>250) { return none; }
		Array<LandPatch> result;
		for (const auto& region : regions)
		{
			if (region.num_holes()>0 || Max(region.boundingRect().w,region.boundingRect().h)>55) { return none; }
			if (region.area()<1) { continue; }
			const Polygon usable=region.calculateRoundBuffer(-1.5);
			if (!usable || usable.area()<4) { return none; }
			const Polygon global=region.movedBy(origin); const RectF box=global.boundingRect();
			const Point first{static_cast<int>(Floor(box.x/CHUNK_SIZE)),static_cast<int>(Floor(box.y/CHUNK_SIZE))};
			const Point last{static_cast<int>(Floor((box.x+box.w)/CHUNK_SIZE)),static_cast<int>(Floor((box.y+box.h)/CHUNK_SIZE))};
			if (first!=last || !world.getChunk(first)) { return none; }
			double low=Math::Inf,high=-Math::Inf;
			const auto dry=[&](Vec2 point)
			{
				const double height=world.sampleHeight(point.x,point.y); low=Min(low,height); high=Max(high,height);
				return height>world.waterSurfaceHeight(point.x,point.y)+1;
			};
			for (size_t i=0;i<global.outer().size();++i)
			{
				const Vec2 a=global.outer()[i],b=global.outer()[(i+1)%global.outer().size()];
				const int count=Max(1,static_cast<int>(Ceil(a.distanceFrom(b))));
				for (int sample=0;sample<=count;++sample) { if (!dry(a.lerp(b,static_cast<double>(sample)/count))) { return none; } }
			}
			// Conservatively include every terrain-grid corner in the pocket's bounding cells.
			// Bilinear terrain extrema cannot hide between these corners.
			for (double z=Floor(box.y/16)*16;z<=Ceil((box.y+box.h)/16)*16;z+=16)
			for (double x=Floor(box.x/16)*16;x<=Ceil((box.x+box.w)/16)*16;x+=16)
			{
				if (!dry({x,z})) { return none; }
			}
			for (double z=box.y;z<=box.y+box.h;z+=1) for (double x=box.x;x<=box.x+box.w;x+=1)
			{
				if (global.contains(Vec2{x,z}) && !dry({x,z})) { return none; }
			}
			if (high-low>2.5) { return none; }
			LandPatch patch; patch.type=LandPatchType::GardenSoil; patch.polygon=global.outer(); patch.sourceParcelKey=-1;
			result << std::move(patch);
		}
		return result.isEmpty() ? none : Optional<Array<LandPatch>>{std::move(result)};
	}
}

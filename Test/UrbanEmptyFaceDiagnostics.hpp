#pragma once
#include "src/gen/SettlementDevelopment.hpp"
#include "src/gen/StreetBlocks.hpp"
#include "src/gen/ParcelRoadIndex.hpp"
#include "src/world/ZoneGrid.hpp"

/// @brief Read-only plan views and finite candidate searches for unresolved street faces.
namespace UrbanEmptyFaceDiagnostics
{
	inline void capture(const FilePath& directory,uint64 seed,const MapGenerator::Settlement& town,
		const World& world,const RoadNetwork& roads,const SettlementDevelopment::Validation& validation,
		const TrainNetwork* trains=nullptr,const String& label=U"")
	{
		Array<JSON> faces;
		if (validation.unfilledBlocks.isArray()) { for (const auto& face : validation.unfilledBlocks.arrayView()) { faces << face; } }
		if (validation.landscapedPockets.isArray()) { for (const auto& face : validation.landscapedPockets.arrayView()) { faces << face; } }
		ParcelRoadIndex index{roads,true}; if (trains) { index.addRailway(*trains); }
		const ParcelRoadIndex roadOnly{roads,true,false}; RoadNetwork emptyNetwork; ParcelRoadIndex railOnly{emptyNetwork}; if (trains) { railOnly.addRailway(*trains); } const Font font{16};
		for (size_t faceIndex=0;faceIndex<faces.size();++faceIndex)
		{
			const auto& source=faces[faceIndex]; StreetBlocks::Block face;
			Vec2 lower{Math::Inf,Math::Inf},upper{-Math::Inf,-Math::Inf};
			for (const auto& entry : source[U"outline"].arrayView())
			{
				const Vec2 point{entry[0].get<double>(),entry[1].get<double>()}; face.outline << point;
				lower.x=Min(lower.x,point.x); lower.y=Min(lower.y,point.y); upper.x=Max(upper.x,point.x); upper.y=Max(upper.y,point.y);
			}
			face.bounds={lower,upper-lower}; face.center={source[U"center"][0].get<double>(),source[U"center"][1].get<double>()};
			for (const auto& entry : source[U"boundaries"].arrayView()) { face.edges << entry[U"id"].get<int>(); }
			const RectF view=face.bounds.stretched(22); const Vec2 middle=view.center();
			const double scale=Min(680.0/view.w,640.0/view.h);
			const auto screen=[&](Vec2 point) { return Vec2{384,408}+(point-middle)*scale; };
			const auto polygon=[&](const auto& points)
			{
				Array<Vec2> translated; for (const Vec2 point : points) { translated << screen(point); } return Polygon{translated};
			};
			JSON report=source; report[U"seed"]=seed; report[U"faceIndex"]=faceIndex;
			for (const int id : face.edges)
			{
				const auto* edge=roads.getEdge(id); JSON boundary; boundary[U"edgeId"]=id;
				boundary[U"roadType"]=static_cast<int>(edge->roadType); boundary[U"designGrade"]=edge->designGrade; boundary[U"nodes"]=Array<int>{edge->nodeA,edge->nodeB};
				for (const int routeId : edge->routeIds) { if (const auto* route=roads.getRoute(routeId)) { boundary[U"routes"].push_back(route->name); } }
				report[U"boundaryRoles"].push_back(boundary);
			}

			struct Site { Vec2 center; ParcelGeometry::Quad footprint,padded; };
			Array<Site> sites;
			for (int z=0;z<WORLD_CHUNKS;++z) for (int x=0;x<WORLD_CHUNKS;++x)
			{
				const Point coord{x,z}; const auto* chunk=world.getChunk(coord); if (!chunk) { continue; }
				for (int row=0;row<ZONE_CELLS;++row) for (int col=0;col<ZONE_CELLS;++col)
				{
					const auto& building=chunk->buildingGrid[{col,row}]; if (building.type==BuildingType::None) { continue; }
					const Vec2 center=ZoneGrid::cellCenterXZ(coord,col,row)+Vec2{building.offsetX,building.offsetZ};
					const double half=buildingFootprintXZ(building.type)*.5;
					if (!view.stretched(half*2).contains(center)) { continue; }
					const Site site{center,ParcelGeometry::footprint(center,half,building.angle),ParcelGeometry::footprint(center,half+.25,building.angle)};
					sites << site; JSON item; item[U"center"]=Array<double>{center.x,center.y}; item[U"angle"]=building.angle; item[U"half"]=half; item[U"type"]=static_cast<int>(building.type);
					for (const Vec2 point : site.padded) { item[U"paddedFootprint"].push_back(Array<double>{point.x,point.y}); }
					report[U"occupiedSites"].push_back(item);
				}
			}
			Array<ParcelGeometry::Quad> ribbons,railRibbons;
			for (const auto& edge : roads.edges())
			{
				if (edge.id<0 || !edge.isRoadbedBuilt()) { continue; } const auto curve=roads.getBezier(edge.id); if (!curve || curve->totalLength<.1) { continue; }
				const RectF bounds{Min({curve->p0.x,curve->p1.x,curve->p2.x,curve->p3.x})-edge.totalWidth(),Min({curve->p0.z,curve->p1.z,curve->p2.z,curve->p3.z})-edge.totalWidth(),
					Max({curve->p0.x,curve->p1.x,curve->p2.x,curve->p3.x})-Min({curve->p0.x,curve->p1.x,curve->p2.x,curve->p3.x})+edge.totalWidth()*2,
					Max({curve->p0.z,curve->p1.z,curve->p2.z,curve->p3.z})-Min({curve->p0.z,curve->p1.z,curve->p2.z,curve->p3.z})+edge.totalWidth()*2};
				if (!bounds.intersects(view)) { continue; }
				ParcelRoadGeometry::forEachRibbon(edge,*curve,[&](const ParcelGeometry::Quad& quad)
				{
					ribbons << quad; if (!edge.hasRoadLanes()) { railRibbons << quad; } JSON points; for (const Vec2 point : quad) { points.push_back(Array<double>{point.x,point.y}); } report[U"roadQuads"].push_back(points);
				});
			}
			if (trains)
			{
				const auto includeRail=[&](const ParcelGeometry::Quad& quad)
				{
					Array<Vec2> points; for (const Vec2 point : quad) { points << point; }
					const Polygon shape=Geometry2D::ConvexHull(points);
					if (shape && shape.boundingRect().intersects(view)) { railRibbons << quad; }
				};
				for (const auto& site : RailwaySite::footprints(*trains)) { includeRail(site); }
				for (const auto& edge : trains->edges())
				{
					const auto curve=trains->getBezier(edge.id); if (!curve) { continue; }
					Vec2 previousLeft{0,0},previousRight{0,0};
					const int count=Max(1,static_cast<int>(Ceil(curve->totalLength/8)));
					for (int sample=0;sample<=count;++sample)
					{
						const float arc=curve->totalLength*sample/count;
						const Vec3 point=curve->positionAt(arc),right=tangentToRight(curve->tangentAt(arc));
						const Vec2 left{point.x-right.x*5,point.z-right.z*5},other{point.x+right.x*5,point.z+right.z*5};
						if (sample>0) { includeRail({previousLeft,left,other,previousRight}); }
						previousLeft=left; previousRight=other;
					}
				}
			}
			for (const auto& quad : railRibbons)
			{
				JSON points; for (const Vec2 point : quad) { points.push_back(Array<double>{point.x,point.y}); } report[U"railQuads"].push_back(points);
			}
			struct Candidate { Vec2 position; float angle; int edgeId; float arc; bool storageBusy; };
			Optional<Candidate> witness,storageWitness; Array<std::pair<Vec2,int>> rejected;
			int tested=0,roadRejected=0,railRejected=0,bothRejected=0,otherTransportRejected=0,neighborRejected=0,greenRejected=0,storageRejected=0;
			const Vec2 faceDelta=face.center-town.center;
			const bool civic=town.plan.civic && town.plan.civic->contains(Vec2{faceDelta.dot(town.gridAxisX),faceDelta.dot(town.gridAxisZ)});
			const BuildingType candidateType=civic ? BuildingType::PublicFacility : BuildingType::Detached;
			report[U"civicReserved"]=civic; report[U"candidateType"]=static_cast<int>(candidateType);
			const double half=buildingFootprintXZ(candidateType)*.5;
			for (const int id : face.edges)
			{
				if (witness) { break; } const auto* edge=roads.getEdge(id); const auto curve=roads.getBezier(id); const Vec2 span=StreetBlocks::frontageSpan(roads,id);
				for (float arc=static_cast<float>(span.x);arc<span.y && !witness;arc+=.5f)
				{
					const Vec3 point=curve->positionAt(arc),right=tangentToRight(curve->tangentAt(arc)); const auto range=RoadGeometry::structuralRangeAt(*edge,arc/curve->totalLength);
					for (const int side : {-1,1}) for (double setback=GenerationSettings::get().development_minimumRoadSetback;setback<=6 && !witness;setback+=.25)
					{
						const Vec2 direction{right.x*side,right.z*side}; const double outer=side<0 ? -range.left : range.right;
						const Vec2 position=Vec2{point.x,point.z}+direction*(outer+half+setback); if (!face.contains(position)) { continue; }
						++tested; const float angle=static_cast<float>(Atan2(-direction.x,direction.y));
						const auto footprint=ParcelGeometry::footprint(position,half+GenerationSettings::get().development_footprintMargin,angle);
						if (index.overlaps(footprint)) { const bool road=roadOnly.overlaps(footprint),rail=railOnly.overlaps(footprint); roadRejected+=road; railRejected+=rail; bothRejected+=road && rail; otherTransportRejected+=!road && !rail; if (rejected.size()<1200) { rejected << std::pair{position,road ? 0 : 2}; } continue; }
						const auto padded=ParcelGeometry::footprint(position,half+.25,angle);
						if (sites.any([&](const Site& site) { return ParcelGeometry::overlaps(padded,site.padded); })) { ++neighborRejected; if (rejected.size()<1200) { rejected << std::pair{position,1}; } continue; }
						bool green=false;
						for (const Vec2 corner : footprint) { const Vec2 delta=corner-town.center; green |= UrbanMorphology::isReservedGreen(town.plan,{delta.dot(town.gridAxisX),delta.dot(town.gridAxisZ)}); }
						if (green) { ++greenRejected; continue; }
						Point coord; int col,row; ZoneGrid::worldToZoneCell(static_cast<float>(position.x),static_cast<float>(position.y),coord,col,row);
						const auto* chunk=world.getChunk(coord); const bool busy=!chunk || chunk->buildingGrid[{col,row}].type!=BuildingType::None;
						const Candidate candidate{position,angle,id,arc,busy};
						if (busy) { ++storageRejected; if (!storageWitness) { storageWitness=candidate; } continue; }
						witness=candidate;
					}
				}
			}
			report[U"witnessSearchTested"]=tested; report[U"witnessBothTransportRejected"]=bothRejected; report[U"witnessOtherTransportRejected"]=otherTransportRejected; report[U"witnessRailRejected"]=railRejected; report[U"witnessRoadRejected"]=roadRejected; report[U"witnessNeighborRejected"]=neighborRejected;
			report[U"witnessGreenRejected"]=greenRejected; report[U"witnessStorageRejected"]=storageRejected; report[U"foundStorageFreeWitness"]=witness.has_value();
			if (const auto selected=witness ? witness : storageWitness)
			{
				report[U"witness"][U"position"]=Array<double>{selected->position.x,selected->position.y}; report[U"witness"][U"angle"]=selected->angle;
				report[U"witness"][U"edgeId"]=selected->edgeId; report[U"witness"][U"arc"]=selected->arc; report[U"witness"][U"storageBusy"]=selected->storageBusy;
			}
			const RenderTexture target{Size{768,768},TextureFormat::R8G8B8A8_Unorm};
			{
				const ScopedRenderTarget2D output{target.clear(ColorF{.96,.96,.93})};
				polygon(face.outline).draw(ColorF{1,.82,.22,.45});
				for (const auto& ribbon : ribbons)
				{
					Array<Vec2> points; for (const Vec2 point : ribbon) { points << screen(point); }
					Geometry2D::ConvexHull(points).draw(ColorF{.43,.45,.47});
				}
				for (const auto& ribbon : railRibbons)
				{
					Array<Vec2> points; for (const Vec2 point : ribbon) { points << screen(point); }
					Geometry2D::ConvexHull(points).draw(ColorF{.60,.39,.17,.9});
				}
				if ((source.hasElement(U"publicGround") && source[U"publicGround"].isArray()))
				{
					for (const auto& ground : source[U"publicGround"].arrayView())
					{
						Array<Vec2> points; for (const auto& point : ground.arrayView()) { points << Vec2{point[0].get<double>(),point[1].get<double>()}; }
						polygon(points).draw(ColorF{.26,.48,.18});
					}
				}
				for (const auto& site : sites) { polygon(site.footprint).draw(ColorF{.23,.44,.67}); Circle{screen(site.center),2}.draw(ColorF{.06,.12,.2}); }
				for (const auto& sample : rejected) { Circle{screen(sample.first),1.3}.draw(sample.second==0 ? ColorF{.85,.23,.15,.65} : sample.second==2 ? ColorF{.9,.65,.12,.65} : ColorF{.65,.12,.65,.65}); }
				polygon(face.outline).drawFrame(1.3,ColorF{.85,.53,.04});
				if (const auto selected=witness ? witness : storageWitness) { polygon(ParcelGeometry::footprint(selected->position,half,selected->angle)).drawFrame(3,selected->storageBusy ? ColorF{.8,.5,0} : ColorF{0,.6,.25}); }
				RectF{0,0,768,75}.draw(ColorF{.98});
				font(U"seed{} face{}: gray road exclusions, blue occupied sites"_fmt(seed,faceIndex)).draw(12,10,ColorF{.1});
				font((source.hasElement(U"publicGround") && source[U"publicGround"].isArray()) ? U"green fill: explicit public ground, not housing" : U"red roads / gold rail exclusion / purple neighbor / green witness").draw(12,32,ColorF{.1});
				font(U"sampled={} road={} rail={} neighbor={} witness={}"_fmt(tested,roadRejected,railRejected,neighborRejected,witness.has_value())).draw(12,54,ColorF{.1});
				const double scaleMeters=20; Line{{35,731},{35+scaleMeters*scale,731}}.draw(2,ColorF{.1}); font(U"20m").draw(35,735,ColorF{.1});
			}
			Graphics2D::Flush(); Image image; target.readAsImage(image);
			const String name=(label.isEmpty() ? U"empty_face" : U"empty_face_"+label)+U"_seed{}_{}"_fmt(seed,faceIndex); image.save(directory+U"Screenshot/"+name+U".png"); report.save(directory+U"TestResults/"+name+U".json");
		}
	}
}

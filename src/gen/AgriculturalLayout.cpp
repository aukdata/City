#include "AgriculturalLayout.hpp"
#include "ParcelRoadIndex.hpp"
#include "SettlementPlan.hpp"
#include "StreetProfile.hpp"
#include "UrbanParcel.hpp"
#include "RoadDesignLimits.hpp"
#include "RoadTerrainFit.hpp"
#include "../road/RoadPlanDraft.hpp"
#include "../debug/DebugLog.hpp"
#include <queue>

namespace AgriculturalLayout
{
	namespace
	{
		constexpr double kCellSize=static_cast<double>(CHUNK_SIZE)/ZONE_CELLS;
		Vec2 horizontal(Vec3 point) { return {point.x,point.z}; }
		Vec3 groundPoint(const World& world,Vec2 point) { return {point.x,world.sampleHeight(static_cast<float>(point.x),static_cast<float>(point.y)),point.y}; }
		bool agricultural(const World& world,Vec2 point)
		{
			if (point.x<0 || point.y<0 || point.x>=WORLD_SIZE || point.y>=WORLD_SIZE) { return false; }
			const auto river=world.rivers().nearest(point);
			if (river.reach>=0 && river.distance<=river.halfWidth+(river.halfWidth>=42 ? 45 : 18)) { return false; }
			const Point cell{static_cast<int>(point.x/kCellSize),static_cast<int>(point.y/kCellSize)};
			const auto* chunk=world.getChunk({cell.x/ZONE_CELLS,cell.y/ZONE_CELLS});
			if (!chunk) { return false; }
			const Point local{cell.x%ZONE_CELLS,cell.y%ZONE_CELLS};
			const ZoneType zone=chunk->zoneMap[local];
			// 日本の平野では田畑が住宅のすぐ裏まで続く。未開発の住宅用地と白地も、宅地化されるまでは耕作地にする。
			// 公共施設用地（市街化調整区域）だけは耕作地にしない。開発時に農地と決まったセルも耕作できる。
			return zone==ZoneType::Agriculture
				|| (zone!=ZoneType::UrbanControl && (chunk->buildingGrid[local].type==BuildingType::None || chunk->buildingGrid[local].type==BuildingType::Farmland));
		}
		/// @brief 都市計画上の農業地域。一般道沿いの農家を市街地の宅地予定地へ置かないために使う。
		bool farmingZone(const World& world,Vec2 point)
		{
			if (point.x<0 || point.y<0 || point.x>=WORLD_SIZE || point.y>=WORLD_SIZE) { return false; }
			const Point cell{static_cast<int>(point.x/kCellSize),static_cast<int>(point.y/kCellSize)};
			const auto* chunk=world.getChunk({cell.x/ZONE_CELLS,cell.y/ZONE_CELLS});
			return chunk && chunk->zoneMap[{cell.x%ZONE_CELLS,cell.y%ZONE_CELLS}]==ZoneType::Agriculture;
		}
		/// @brief 造成済みのニュータウンの内側か。
		bool insidePlannedTown(Vec2 point,const Array<Frame>& frames)
		{
			for (const auto& frame:frames)
			{
				if (frame.plannedHalfExtent.x<=0) { continue; }
				const Vec2 delta=point-frame.center;
				if (Abs(delta.dot(frame.axisX))<=frame.plannedHalfExtent.x && Abs(delta.dot(frame.axisZ))<=frame.plannedHalfExtent.y) { return true; }
			}
			return false;
		}
		/// @brief 町割の内側か。市街地の空き地には道路沿いの小さな田だけを残す。
		bool insideTown(Vec2 point,const Array<Frame>& frames)
		{
			for (const auto& frame:frames)
			{
				if (frame.townHalfExtent.x<=0) { continue; }
				const Vec2 delta=point-frame.center;
				if (Abs(delta.dot(frame.axisX))<=frame.townHalfExtent.x && Abs(delta.dot(frame.axisZ))<=frame.townHalfExtent.y) { return true; }
			}
			return false;
		}
		/// @brief 建物が建て込んだ市街地か。駅前や中心街の空き地には田畑を作らない。
		bool builtUp(const World& world,Vec2 point)
		{
			constexpr int kRadius=4;
			constexpr int kBuiltLimit=30;
			const Point center{static_cast<int>(point.x/kCellSize),static_cast<int>(point.y/kCellSize)};
			int built=0;
			for (int dz=-kRadius;dz<=kRadius;++dz) { for (int dx=-kRadius;dx<=kRadius;++dx)
			{
				const Point cell=center+Point{dx,dz};
				if (cell.x<0 || cell.y<0) { continue; }
				const auto* chunk=world.getChunk({cell.x/ZONE_CELLS,cell.y/ZONE_CELLS});if (!chunk) { continue; }
				const auto type=chunk->buildingGrid[{cell.x%ZONE_CELLS,cell.y%ZONE_CELLS}].type;
				built+=type!=BuildingType::None && type!=BuildingType::Farmland && type!=BuildingType::RuralHouse && type!=BuildingType::VillageHouse;
			} }
			return built>kBuiltLimit;
		}
		uint32 hash(uint64 seed,Vec2 point)
		{
			return static_cast<uint32>(UrbanMorphology::mix(seed^(static_cast<uint64>(static_cast<uint32>(Floor(point.x)))<<32)^static_cast<uint32>(Floor(point.y))));
		}
		Array<Frame> localFrames(const RoadNetwork& roads,const Array<Frame>& frames)
		{
			if (!frames.isEmpty()) { return frames; }
			Vec2 center{};int count=0;
			for (const auto& node:roads.nodes()) { if (node.id>=0) { center+=horizontal(node.position);++count; } }
			Frame frame;frame.center=count ? center/count : Vec2{};frame.origin=frame.center;
			return {frame};
		}
		double settlementDistance(Vec2 point,const Array<Frame>& frames)
		{
			double distance=Math::Inf;
			for (const auto& frame:frames) { distance=Min(distance,point.distanceFrom(frame.center)); }
			return distance;
		}
		double density(Vec2 point,const Array<Frame>& frames)
		{
			const auto& config=GenerationSettings::get();const double distance=settlementDistance(point,frames);
			return distance>config.agriculture_maximumRadius ? 0 : Exp(-Square(distance/config.agriculture_densityRadius)*config.agriculture_densityFalloff);
		}
		ParcelGeometry::Quad ribbon(Vec2 a,Vec2 b,double halfWidth)
		{
			const Vec2 delta=b-a;const Vec2 normal=Vec2{-delta.y,delta.x}.normalized()*halfWidth;
			return {a-normal,b-normal,b+normal,a+normal};
		}
		/// @brief 実在する建物の占有面と農家位置。新しい農地が家を覆うことも防ぐ。
		struct Sites
		{
			RoadNetwork empty;
			ParcelRoadIndex occupied{empty};
			/// @brief 家の位置を格子で引く。戸建ても含むと数万軒になり、総当たりでは生成が遅くなる。
			static constexpr double kHomeBucket=256;
			HashTable<Point,Array<Vec2>> homeBuckets;
			static Point homeBucket(Vec2 point) { return {static_cast<int>(Floor(point.x/kHomeBucket)),static_cast<int>(Floor(point.y/kHomeBucket))}; }
			void addHome(Vec2 home) { homeBuckets[homeBucket(home)] << home; }
			explicit Sites(const World& world)
			{
				for (int z=0;z<WORLD_CHUNKS;++z) { for (int x=0;x<WORLD_CHUNKS;++x)
				{
					const auto* chunk=world.getChunk({x,z});if (!chunk) { continue; }
					for (const auto& patch:chunk->landPatches)
					{
						if (patch.type==LandPatchType::FarmField || patch.type==LandPatchType::PaddyField) { continue; }
						for (size_t i=1;i+1<patch.polygon.size();++i) { occupied.add({patch.polygon[0],patch.polygon[i],patch.polygon[i+1],patch.polygon[i+1]}); }
					}
					for (int row=0;row<ZONE_CELLS;++row) { for (int col=0;col<ZONE_CELLS;++col)
					{
						const auto& building=chunk->buildingGrid[{col,row}];
						if (building.type==BuildingType::None || building.type==BuildingType::Farmland) { continue; }
						const Vec2 center{(x*ZONE_CELLS+col+.5)*kCellSize+building.offsetX,(z*ZONE_CELLS+row+.5)*kCellSize+building.offsetZ};
						occupied.add(ParcelGeometry::footprint(center,buildingFootprintXZ(building.type)*.5+3,building.angle));
						// 平野の耕作者は農家だけでなく、集落縁の戸建てにも住む。
						if (building.type==BuildingType::RuralHouse || building.type==BuildingType::VillageHouse || building.type==BuildingType::Detached) { addHome(center); }
					} }
				} }
			}
			bool served(const Array<Vec2>& polygon,double maximum) const
			{
				const Point center=homeBucket(polygon.front());
				const int reach=static_cast<int>(Ceil(maximum/kHomeBucket));
				for (int dz=-reach;dz<=reach;++dz) { for (int dx=-reach;dx<=reach;++dx)
				{
					const auto found=homeBuckets.find(center+Point{dx,dz});
					if (found==homeBuckets.end()) { continue; }
					for (const Vec2 home:found->second)
					{
						if (home.distanceFromSq(polygon.front())>maximum*maximum) { continue; }
						bool within=true;for (const Vec2 point:polygon) { within &= point.distanceFromSq(home)<=maximum*maximum; }
						if (within) { return true; }
					}
				} }
				return false;
			}
		};
		int prepareHomes(World& world,const RoadNetwork& roads,uint64 seed,const TrainNetwork* railway,const Array<Frame>& frames)
		{
			const auto& config=GenerationSettings::get();ParcelRoadIndex occupied{roads,true};if (railway) { occupied.addRailway(*railway); }
			Sites sites{world};int placed=0;
			const double half=buildingFootprintXZ(BuildingType::RuralHouse)*.5;
			/// 道路の指定位置の左右どちらかへ、道に面した農家を1軒置く。
			const auto place=[&](const RoadEdge& edge,const CubicBezier& curve,float arc,bool farmRoad)
			{
				const Vec3 road=curve.positionAt(arc),right3=tangentToRight(curve.tangentAt(arc));
				for (const int side:{-1,1})
				{
					const Vec2 outward=horizontal(right3)*side,point=horizontal(road)+outward*(edge.totalWidth()*.5+half+config.agriculture_homeSetback);
					if (!agricultural(world,point) || (!farmRoad && !farmingZone(world,point))) { continue; }
					const double angle=Atan2(-outward.x,outward.y);const auto footprint=ParcelGeometry::footprint(point,half+config.agriculture_homeFootprintMargin,angle);
					if (occupied.overlaps(footprint) || sites.occupied.overlaps(footprint)) { continue; }
					double low=Math::Inf,high=-Math::Inf;bool valid=true;
					for (const auto corner:footprint)
					{
						const auto ground=groundPoint(world,corner);low=Min(low,ground.y);high=Max(high,ground.y);
						valid &= agricultural(world,corner) && ground.y>world.waterPlanningHeight(corner.x,corner.y)+config.agriculture_minimumFreeboard;
					}
					if (!valid || high-low>config.development_maximumBuildingRelief) { continue; }
					const Point cell{static_cast<int>(point.x/kCellSize),static_cast<int>(point.y/kCellSize)};
					auto* chunk=world.getChunk({cell.x/ZONE_CELLS,cell.y/ZONE_CELLS});auto& building=chunk->buildingGrid[{cell.x%ZONE_CELLS,cell.y%ZONE_CELLS}];
					if (building.type!=BuildingType::None && building.type!=BuildingType::Farmland) { continue; }
					building.type=BuildingType::RuralHouse;building.angle=static_cast<float>(angle);building.edgeId=edge.id;building.edgeT=curve.tFromArcLength(arc);
					building.offsetX=static_cast<float>(point.x-(cell.x+.5)*kCellSize);building.offsetZ=static_cast<float>(point.y-(cell.y+.5)*kCellSize);
					chunk->zoneMap[{cell.x%ZONE_CELLS,cell.y%ZONE_CELLS}]=ZoneType::LowResidential;chunk->markMeshDirty();
					sites.occupied.add(footprint);sites.addHome(point);++placed;
					return true;
				}
				return false;
			};
			// 日本の平野の農家は等間隔に散らばらず、数軒が道沿いに寄り合う小さな集落をつくる。
			constexpr int kMaximumNeighbors=4;
			constexpr double kNeighborGap=8;
			const double neighborStep=half*2+kNeighborGap;
			for (const auto& edge:roads.edges())
			{
				if (edge.id<0 || edge.useElevation || edge.tunnel) { continue; }
				// 農家は農道だけでなく、田園を通る集落道・旧道沿いにも点在する。一般道では農業地域に限る。
				const bool farmRoad=edge.farmAccess;
				if (!farmRoad && (edge.roadType!=RoadType::LocalRoad || !edge.hasRoadLanes() || !edge.isRoadbedBuilt())) { continue; }
				const auto curve=roads.getBezier(edge.id);if (!curve) { continue; }
				for (float arc=curve->totalLength*.5f;arc<curve->totalLength;arc+=static_cast<float>(config.agriculture_homeSpacing))
				{
					const Vec2 road=horizontal(curve->positionAt(arc));
					if (settlementDistance(road,frames)>config.agriculture_maximumRadius) { continue; }
					if (sites.served({road},config.agriculture_homeSpacing)) { continue; }
					if (!place(edge,*curve,arc,farmRoad)) { continue; }
					const int neighbors=static_cast<int>(hash(seed,road)%(kMaximumNeighbors+1));
					for (int index=1;index<=neighbors;++index)
					{
						const double offset=neighborStep*((index+1)/2)*(index%2 ? 1 : -1);
						if (arc+offset<half || arc+offset>curve->totalLength-half) { continue; }
						place(edge,*curve,static_cast<float>(arc+offset),farmRoad);
					}
				}
			}
			return placed;
		}
		/// @brief 既存道の接続点を節点にする。途中なら分割し、分割した道路に接道していた建物は近い方の子の道路へ付け替える。
		int attachToRoad(World& world,RoadNetwork& roads,const RoadPlanSnapIndex::Hit& hit,double snap)
		{
			const auto* edge=roads.getEdge(*hit.edgeId); if (!edge) { return -1; }
			for (const int nodeId : {edge->nodeA,edge->nodeB})
			{
				if (horizontal(roads.getNode(nodeId)->position).distanceFrom(horizontal(hit.position))<snap) { return nodeId; }
			}
			// 分割した道路に接道していた建物は、近い方の子の道路へ付け替える。
			const int parent=*hit.edgeId;
			const auto parentCurve=roads.getBezier(parent);
			const int split=roads.splitEdgeAtParameter(parent,hit.curveT);
			if (split<0 || !parentCurve) { return split; }
			Array<std::pair<int,CubicBezier>> children;
			for (const auto& attachment : roads.getNode(split)->attachments)
			{
				if (const auto curve=roads.getBezier(attachment.edgeId)) { children.emplace_back(attachment.edgeId,*curve); }
			}
			Vec2 low{Math::Inf,Math::Inf},high{-Math::Inf,-Math::Inf};
			for (const Vec3 p : {parentCurve->p0,parentCurve->p1,parentCurve->p2,parentCurve->p3}) { low={Min(low.x,p.x),Min(low.y,p.z)};high={Max(high.x,p.x),Max(high.y,p.z)}; }
			constexpr double kFrontageMargin=80;constexpr int kSamples=32;
			const int left=static_cast<int>(Floor((low.x-kFrontageMargin)/kCellSize)),right=static_cast<int>(Floor((high.x+kFrontageMargin)/kCellSize));
			const int top=static_cast<int>(Floor((low.y-kFrontageMargin)/kCellSize)),bottom=static_cast<int>(Floor((high.y+kFrontageMargin)/kCellSize));
			for (int z=Max(0,top);z<=bottom;++z) for (int x=Max(0,left);x<=right;++x)
			{
				auto* chunk=world.getChunk({x/ZONE_CELLS,z/ZONE_CELLS}); if (!chunk) { continue; }
				auto& building=chunk->buildingGrid[{x%ZONE_CELLS,z%ZONE_CELLS}];
				if (building.edgeId!=parent) { continue; }
				const Vec2 position=Vec2{(x+.5)*kCellSize+building.offsetX,(z+.5)*kCellSize+building.offsetZ};
				double best=Math::Inf;
				for (const auto& [childId,curve] : children)
				{
					for (int i=0;i<=kSamples;++i)
					{
						const float t=static_cast<float>(i)/kSamples;const double distance=horizontal(curve.evaluate(t)).distanceFrom(position);
						if (distance<best) { best=distance;building.edgeId=childId;building.edgeT=t; }
					}
				}
				chunk->markMeshDirty();
			}
			return split;
		}
		/// @brief 道路が横切る田畑・水路を、道路の両側に分けて残す。多数の道路は32m格子で区画と照合する。
		/// @param trimEnds 端で角が触れるだけの区画を切らないよう、判定する帯を両端で幅の半分だけ短くする。
		void clipFieldsAlong(World& world,const Array<std::pair<Vec2,Vec2>>& lines,double half,bool trimEnds=false)
		{
			const auto& config=GenerationSettings::get();
			struct Corridor { Vec2 from, normal; Polygon band; RectF bounds; };
			constexpr double kCell=32;
			const auto cellOf=[](Vec2 p) { return Point{static_cast<int>(Floor(p.x/kCell)),static_cast<int>(Floor(p.y/kCell))}; };
			Array<Corridor> corridors; HashTable<Point,Array<int>> grid; HashSet<Point> chunks;
			for (const auto& [from,to]:lines)
			{
				if (from.distanceFromSq(to)<1e-6) { continue; }
				const Vec2 direction=(to-from).normalized(),normal{-direction.y,direction.x};
				const Vec2 trim=trimEnds && from.distanceFrom(to)>half*4 ? direction*half : Vec2{0,0};
				Corridor corridor{from,normal,Polygon{Array<Vec2>{from+trim-normal*half,to-trim-normal*half,to-trim+normal*half,from+trim+normal*half}},
					RectF{Vec2{Min(from.x,to.x),Min(from.y,to.y)},Vec2{Abs(to.x-from.x),Abs(to.y-from.y)}}.stretched(half+1)};
				const int index=static_cast<int>(corridors.size());
				const Point low=cellOf(corridor.bounds.tl()), high=cellOf(corridor.bounds.br());
				for (int z=low.y;z<=high.y;++z) { for (int x=low.x;x<=high.x;++x) { grid[Point{x,z}] << index; } }
				for (int z=static_cast<int>(corridor.bounds.y/CHUNK_SIZE);z<=static_cast<int>((corridor.bounds.y+corridor.bounds.h)/CHUNK_SIZE);++z)
				{
					for (int x=static_cast<int>(corridor.bounds.x/CHUNK_SIZE);x<=static_cast<int>((corridor.bounds.x+corridor.bounds.w)/CHUNK_SIZE);++x) { chunks.insert(Point{x,z}); }
				}
				corridors << std::move(corridor);
			}
			const auto clip=[](const Array<Vec2>& polygon,Vec2 origin,Vec2 keep)
			{
				Array<Vec2> result;
				for (size_t i=0;i<polygon.size();++i)
				{
					const Vec2 p0=polygon[i],p1=polygon[(i+1)%polygon.size()];
					const double d0=(p0-origin).dot(keep),d1=(p1-origin).dot(keep);
					if (d0>=0) { result << p0; }
					if ((d0>=0)!=(d1>=0)) { result << p0.lerp(p1,d0/(d0-d1)); }
				}
				return result;
			};
			const auto area=[](const Array<Vec2>& polygon)
			{
				double sum=0; for (size_t i=0;i<polygon.size();++i) { const Vec2 a=polygon[i],b=polygon[(i+1)%polygon.size()]; sum+=a.x*b.y-b.x*a.y; } return Abs(sum)*.5;
			};
			for (const Point coord:chunks)
			{
				auto* chunk=world.getChunk(coord); if (!chunk) { continue; }
				Array<LandPatch> pieces; bool changed=false;
				for (auto& patch:chunk->landPatches)
				{
					if ((patch.type!=LandPatchType::PaddyField && patch.type!=LandPatchType::FarmField && patch.type!=LandPatchType::IrrigationDitch)
						|| patch.polygon.size()<3) { continue; }
					Vec2 low=patch.polygon.front(),high=low;
					for (const Vec2 p:patch.polygon) { low={Min(low.x,p.x),Min(low.y,p.y)}; high={Max(high.x,p.x),Max(high.y,p.y)}; }
					const RectF bounds{low,high-low};
					HashSet<int> nearby;
					const Point a=cellOf(low), b=cellOf(high);
					for (int z=a.y;z<=b.y;++z) { for (int x=a.x;x<=b.x;++x) { if (const auto found=grid.find(Point{x,z});found!=grid.end()) { for (const int i:found->second) { if (corridors[i].bounds.intersects(bounds)) { nearby.insert(i); } } } } }
					if (nearby.empty()) { continue; }
					Array<int> order(nearby.begin(),nearby.end()); order.sort();
					Array<Array<Vec2>> parts{patch.polygon}; bool split=false;
					for (const int i:order)
					{
						const auto& corridor=corridors[i];
						Array<Array<Vec2>> next;
						for (const auto& part:parts)
						{
							Array<Vec2> outline=part; UrbanParcel::normalize(outline);
							if (outline.size()<3 || !Polygon{outline}.intersects(corridor.band)) { next << part; continue; }
							// 線分の範囲外へはみ出す区画もあるので、両側を半平面で切る。
							split=true;
							for (const auto& side:{clip(part,corridor.from+corridor.normal*half,corridor.normal),clip(part,corridor.from-corridor.normal*half,-corridor.normal)})
							{
								// 畦道は区画の縁を通るので、角をかすめて生じる三角形の切れ端や細い帯は田として残さない。
								const bool sliver=trimEnds && patch.type!=LandPatchType::IrrigationDitch && (side.size()<4 || area(side)<area(part)*.2);
								if (!sliver && side.size()>=3 && area(side)>=(patch.type==LandPatchType::IrrigationDitch ? 1.0 : config.agriculture_minimumFieldArea*.25)) { next << side; }
							}
						}
						parts=std::move(next);
					}
					if (!split) { continue; }
					changed=true;
					for (auto& part:parts) { LandPatch piece=patch; piece.polygon=std::move(part); pieces << std::move(piece); }
					patch.polygon.clear();
				}
				if (changed)
				{
					chunk->landPatches.remove_if([](const LandPatch& patch) { return patch.polygon.size()<3; });
					for (auto& piece:pieces) { piece.id=static_cast<int>(chunk->landPatches.size()); chunk->landPatches << std::move(piece); }
					chunk->markMeshDirty();
				}
			}
		}
		/// @brief 畦道の生成結果。
		struct AzemichiResult
		{
			int fields=0, frontage=0, connected=0, unreachable=0, edges=0, loops=0, deadEnds=0, bridges=0, accesses=0, stranded=0;
			double length=0;
			std::array<double,7> phaseMs{}; ///< 区画収集・畦の検査・経路探索・経路選択・道路追加・地形と区画の調整が終わった時刻。
		};
		/// @brief 道路に面していない田畑を、畦をたどる畦道で道路網へつなぐ。
		/// @details 日本の水田では、農家は道路・農道から畦道を通って奥の田へ入る。区画の頂点を畦の交点、辺を畦とみなし、
		///   道路沿いの交点から曲がりの少ない最短経路木を伸ばして、どの田畑も1辺以上が道路か畦道に面するようにする。
		///   木の末端は近くの畦道・道路へ戻れれば輪にし、戻れなければ最後の田で終わる（末端の耕作道）。
		AzemichiResult connectFieldsByAzemichi(World& world,RoadNetwork& roads,const Sites& sites,const TrainNetwork* railway)
		{
			const auto& config=GenerationSettings::get();
			AzemichiResult result; const Stopwatch phase{StartImmediately::Yes};
			constexpr double kMerge=4.5;     ///< 隣の田の角をまとめる距離。0.97倍に縮めた区画の角どうしは最大約4m離れる。
			constexpr double kRoadReach=6.5; ///< 道路の縁からこの距離にある畦の交点は、その道路へ取り付ける。
			constexpr double kFrontage=5.0;  ///< 区画の辺の中点が道路の縁からこの距離なら、その田畑は道路に面している。
			constexpr double kVertexCell=32;
			Array<Array<Vec2>> fields;
			for (int z=0;z<WORLD_CHUNKS;++z) { for (int x=0;x<WORLD_CHUNKS;++x)
			{
				const auto* chunk=world.getChunk({x,z}); if (!chunk) { continue; }
				for (const auto& patch:chunk->landPatches)
				{
					if ((patch.type==LandPatchType::PaddyField || patch.type==LandPatchType::FarmField) && (patch.materialVariant & kManagedField) && patch.polygon.size()>=3)
					{
						fields << patch.polygon;
					}
				}
			} }
			result.fields=static_cast<int>(fields.size());
			if (fields.isEmpty()) { return result; }
			RoadPlanSnapIndex roadIndex; roadIndex.rebuild(roads);
			const auto accessRoad=[](const RoadEdge& edge)
			{
				return edge.hasRoadLanes() && edge.isRoadbedBuilt() && !edge.useElevation && !edge.tunnel && edge.roadType!=RoadType::Expressway;
			};
			const auto roadHit=[&](Vec2 point,double reach) { return roadIndex.find(roads,groundPoint(world,point),reach+16,Math::Inf,false,accessRoad); };
			/// 道路の縁からの水平距離。道路の上なら負になる。
			const auto roadGap=[&](Vec2 point,double reach)
			{
				const auto hit=roadHit(point,reach);
				if (!hit.edgeId) { return Math::Inf; }
				return horizontal(hit.position).distanceFrom(point)-roads.getEdge(*hit.edgeId)->totalWidth()*.5;
			};
			// 畦の交点: 隣り合う田の角を1点にまとめる。連鎖して遠くの角まで併合しないよう、最初の角を基準にする。
			Array<Vec2> seeds, sums; Array<int> counts; HashTable<Point,Array<int>> mergeGrid;
			const auto mergeCell=[](Vec2 p) { return Point{static_cast<int>(Floor(p.x/kMerge)),static_cast<int>(Floor(p.y/kMerge))}; };
			const auto cluster=[&](Vec2 p)
			{
				const Point cell=mergeCell(p); int best=-1; double bestDistance=kMerge*kMerge;
				for (int dz=-1;dz<=1;++dz) { for (int dx=-1;dx<=1;++dx)
				{
					const auto found=mergeGrid.find(cell+Point{dx,dz}); if (found==mergeGrid.end()) { continue; }
					for (const int id:found->second) { const double d=seeds[id].distanceFromSq(p); if (d<=bestDistance) { best=id; bestDistance=d; } }
				} }
				if (best<0) { best=static_cast<int>(seeds.size()); seeds << p; sums << Vec2{0,0}; counts << 0; mergeGrid[cell] << best; }
				sums[best]+=p; ++counts[best];
				return best;
			};
			Array<Array<int>> corners(fields.size());
			for (size_t i=0;i<fields.size();++i) { for (const Vec2 p:fields[i]) { corners[i] << cluster(p); } }
			Array<Vec2> position(seeds.size());
			for (size_t id=0;id<seeds.size();++id) { position[id]=sums[id]/counts[id]; }
			HashTable<Point,Array<int>> vertexGrid;
			const auto vertexCell=[](Vec2 p) { return Point{static_cast<int>(Floor(p.x/kVertexCell)),static_cast<int>(Floor(p.y/kVertexCell))}; };
			for (size_t id=0;id<position.size();++id) { vertexGrid[vertexCell(position[id])] << static_cast<int>(id); }
			// 畦: 区画の辺を交点で区切った線分。隣の区画の角が辺の途中に来る T 字も交点として分ける。
			struct Bund { int a=0, b=0; double length=0; Array<int> fields; bool usable=true; bool built=false; };
			Array<Bund> bunds; HashTable<uint64,int> bundIndex; Array<Array<int>> adjacency(position.size());
			const auto addBund=[&](int a,int b,int field)
			{
				if (a==b) { return -1; }
				const uint64 key=(static_cast<uint64>(Min(a,b))<<32)|static_cast<uint32>(Max(a,b));
				auto found=bundIndex.find(key);
				if (found==bundIndex.end())
				{
					found=bundIndex.emplace(key,static_cast<int>(bunds.size())).first;
					Bund bund; bund.a=a; bund.b=b; bund.length=position[a].distanceFrom(position[b]); bunds << bund;
					adjacency[a] << found->second; adjacency[b] << found->second;
				}
				auto& owners=bunds[found->second].fields;
				if (!owners.contains(field)) { owners << field; }
				return found->second;
			};
			for (size_t i=0;i<fields.size();++i)
			{
				const auto& ids=corners[i];
				for (size_t k=0;k<ids.size();++k)
				{
					const int a=ids[k], b=ids[(k+1)%ids.size()];
					if (a==b) { continue; }
					const Vec2 pa=position[a], delta=position[b]-pa; const double lengthSq=delta.lengthSq();
					if (lengthSq<1) { continue; }
					Array<std::pair<double,int>> splits;
					const Point low=vertexCell({Min(pa.x,position[b].x)-kMerge,Min(pa.y,position[b].y)-kMerge});
					const Point high=vertexCell({Max(pa.x,position[b].x)+kMerge,Max(pa.y,position[b].y)+kMerge});
					for (int cz=low.y;cz<=high.y;++cz) { for (int cx=low.x;cx<=high.x;++cx)
					{
						const auto found=vertexGrid.find(Point{cx,cz}); if (found==vertexGrid.end()) { continue; }
						for (const int v:found->second)
						{
							if (v==a || v==b) { continue; }
							const double t=(position[v]-pa).dot(delta)/lengthSq;
							if (t>.02 && t<.98 && (pa+delta*t).distanceFrom(position[v])<=kMerge*.8) { splits.emplace_back(t,v); }
						}
					} }
					std::sort(splits.begin(),splits.end());
					int previous=a;
					for (const auto& split:splits) { addBund(previous,split.second,static_cast<int>(i)); previous=split.second; }
					addBund(previous,b,static_cast<int>(i));
				}
			}
			// 道路に取り付けられる交点と、すでに道路に面している田畑。
			Array<double> gap(position.size());
			for (size_t id=0;id<position.size();++id) { gap[id]=roadGap(position[id],kRoadReach); }
			Array<double> frontageGap=gap; ///< 草地を横切って道路へ出す交点は、取り付け用の短い畦道を別に作る。
			const auto atRoad=[&](int v) { return gap[v]<=kRoadReach; };
			Array<bool> served(fields.size(),false);
			for (size_t i=0;i<fields.size();++i)
			{
				const auto& polygon=fields[i];
				for (size_t k=0;k<polygon.size() && !served[i];++k)
				{
					const Vec2 a=polygon[k], b=polygon[(k+1)%polygon.size()];
					served[i]=a.distanceFrom(b)>=8 && roadGap((a+b)*.5,kFrontage)<=kFrontage;
				}
				result.frontage+=served[i];
			}
			// 田畑の内部を横切る線は畦ではない。区画の境界から1m以上内側に入る標本があれば横切ると判定する。
			constexpr double kFieldCell=64;
			HashTable<Point,Array<int>> fieldGrid;
			const auto fieldCell=[](Vec2 p) { return Point{static_cast<int>(Floor(p.x/kFieldCell)),static_cast<int>(Floor(p.y/kFieldCell))}; };
			Array<RectF> fieldBounds(fields.size());
			for (size_t i=0;i<fields.size();++i)
			{
				Vec2 low=fields[i].front(),high=fields[i].front();
				for (const Vec2 p:fields[i]) { low={Min(low.x,p.x),Min(low.y,p.y)}; high={Max(high.x,p.x),Max(high.y,p.y)}; }
				fieldBounds[i]=RectF{low,high-low};
				const Point a=fieldCell(low), b=fieldCell(high);
				for (int cz=a.y;cz<=b.y;++cz) { for (int cx=a.x;cx<=b.x;++cx) { fieldGrid[Point{cx,cz}] << static_cast<int>(i); } }
			}
			const auto insideField=[&](Vec2 p)
			{
				const auto found=fieldGrid.find(fieldCell(p)); if (found==fieldGrid.end()) { return false; }
				for (const int i:found->second)
				{
					if (!fieldBounds[i].contains(p)) { continue; }
					const auto& polygon=fields[i]; bool inside=false; double edge=Math::Inf;
					for (size_t k=0,j=polygon.size()-1;k<polygon.size();j=k++)
					{
						const Vec2 a=polygon[k], b=polygon[j];
						if ((a.y>p.y)!=(b.y>p.y) && p.x<(b.x-a.x)*(p.y-a.y)/(b.y-a.y)+a.x) { inside=!inside; }
						const Vec2 d=b-a; const double t=Clamp((p-a).dot(d)/Max(d.lengthSq(),1e-9),0.0,1.0);
						edge=Min(edge,(a+d*t).distanceFrom(p));
					}
					if (inside && edge>1.0) { return true; }
				}
				return false;
			};
			// 道路に沿う畦・道路や田を横切る畦・建物や線路にかかる畦・水際は畦道にしない。
			RoadNetwork noRoads; ParcelRoadIndex rail{noRoads};
			if (railway) { rail.addRailway(*railway); }
			const double half=config.agriculture_azemichiWidth*.5;
			const auto usableSegment=[&](int a,int b)
			{
				const Vec2 pa=position[a], pb=position[b]; const double length=pa.distanceFrom(pb);
				if (atRoad(a) && atRoad(b) && roadGap((pa+pb)*.5,kRoadReach)<=kRoadReach) { return false; }
				const int samples=Max(2,static_cast<int>(Ceil(length/4)));
				for (int j=1;j<samples;++j)
				{
					const Vec2 p=pa.lerp(pb,static_cast<double>(j)/samples);
					if (insideField(p)) { return false; }
					if (j%2) { continue; }
					const bool nearRoadEnd=(atRoad(a) && p.distanceFrom(pa)<kRoadReach+6) || (atRoad(b) && p.distanceFrom(pb)<kRoadReach+6);
					if (!nearRoadEnd && roadGap(p,kRoadReach)<=1.5) { return false; }
					if (groundPoint(world,p).y<=world.waterPlanningHeight(p.x,p.y)+.3) { return false; }
				}
				const auto quad=ribbon(pa,pb,half+.5);
				return !sites.occupied.overlaps(quad) && !rail.overlaps(quad)
					&& Abs(groundPoint(world,pa).y-groundPoint(world,pb).y)<=Max(length,1.0)*.15;
			};
			result.phaseMs[0]=phase.msF();
			for (auto& bund:bunds) { bund.usable=usableSegment(bund.a,bund.b); }
			result.phaseMs[1]=phase.msF();
			// 道路沿いの交点から、曲がるたびに費用を足す最短経路木を伸ばす。状態は「どの畦をどちら向きに通ったか」。
			const auto stateFrom=[&](int state) { return state&1 ? bunds[state>>1].b : bunds[state>>1].a; };
			const auto stateTo=[&](int state) { return state&1 ? bunds[state>>1].a : bunds[state>>1].b; };
			const auto heading=[&](int state) { return (position[stateTo(state)]-position[stateFrom(state)]).normalized(); };
			const auto leaving=[&](int bund,int vertex) { return bunds[bund].a==vertex ? bund*2 : bund*2+1; };
			Array<double> distance; Array<int> parent;
			const auto search=[&]()
			{
				distance.assign(bunds.size()*2,Math::Inf); parent.assign(bunds.size()*2,-1);
				std::priority_queue<std::pair<double,int>,std::vector<std::pair<double,int>>,std::greater<>> queue;
				for (size_t v=0;v<position.size();++v)
				{
					if (!atRoad(static_cast<int>(v))) { continue; }
					for (const int bund:adjacency[v])
					{
						if (!bunds[bund].usable) { continue; }
						const int state=leaving(bund,static_cast<int>(v));
						if (bunds[bund].length<distance[state]) { distance[state]=bunds[bund].length; queue.emplace(distance[state],state); }
					}
				}
				while (!queue.empty())
				{
					const auto [cost,state]=queue.top(); queue.pop();
					if (cost>distance[state]) { continue; }
					const int vertex=stateTo(state);
					// 別の道路沿いに着いたら、そこから先はその道路を起点にした方が短い。
					if (atRoad(vertex)) { continue; }
					const Vec2 incoming=heading(state);
					for (const int bund:adjacency[vertex])
					{
						if (bund==(state>>1) || !bunds[bund].usable) { continue; }
						const int next=leaving(bund,vertex);
						const double turn=incoming.dot(heading(next));
						if (turn<-.5) { continue; }
						const double total=cost+bunds[bund].length+(turn<.94 ? config.agriculture_azemichiTurnPenalty : 0);
						if (total<distance[next] && total<=config.agriculture_azemichiMaximumLength) { distance[next]=total; parent[next]=state; queue.emplace(total,next); }
					}
				}
			};
			search();
			// 隣の田と角を共有しない離れた区画（家や道に挟まれた田、間に草地を挟む田など）は、近くの道路か
			// 届いている交点まで草地を横切る短い畦道を足してから探し直す。届いた区画が次の区画の足掛かりになるので数回繰り返す。
			for (int round=0;round<3;++round)
			{
				const double kBridge=config.agriculture_azemichiBridgeReach;
				Array<bool> reached(position.size(),false);
				for (size_t v=0;v<position.size();++v) { reached[v]=atRoad(static_cast<int>(v)); }
				for (size_t state=0;state<distance.size();++state) { if (distance[state]<Math::Inf) { reached[stateTo(static_cast<int>(state))]=true; } }
				int bridges=0;
				for (size_t field=0;field<fields.size();++field)
				{
					if (served[field] || corners[field].any([&](int v) { return reached[v]; })) { continue; }
					// 近い順に候補を並べ、通れる最初の1本だけを足す。道路へ直接出られるなら、その角を道路への取り付け点にする。
					Array<std::tuple<double,int,int>> candidates; ///< 距離・区画の角・接続先の交点（-1は道路）
					for (const int v:corners[field])
					{
						if (const auto hit=roadHit(position[v],kBridge); hit.edgeId)
						{
							const double reach=horizontal(hit.position).distanceFrom(position[v])-roads.getEdge(*hit.edgeId)->totalWidth()*.5;
							if (reach<kBridge) { candidates.emplace_back(reach,v,-1); }
						}
						const Point low=vertexCell(position[v]-Vec2{kBridge,kBridge}), high=vertexCell(position[v]+Vec2{kBridge,kBridge});
						for (int cz=low.y;cz<=high.y;++cz) { for (int cx=low.x;cx<=high.x;++cx)
						{
							const auto found=vertexGrid.find(Point{cx,cz}); if (found==vertexGrid.end()) { continue; }
							for (const int w:found->second)
							{
								if (!reached[w] || corners[field].contains(w)) { continue; }
								const double d=position[v].distanceFrom(position[w]);
								if (d>=1 && d<kBridge) { candidates.emplace_back(d,v,w); }
							}
						} }
					}
					std::sort(candidates.begin(),candidates.end());
					for (size_t index=0;index<Min<size_t>(candidates.size(),8);++index)
					{
						const auto [length,v,w]=candidates[index];
						if (w>=0)
						{
							if (!usableSegment(v,w)) { continue; }
							if (const int bund=addBund(v,w,static_cast<int>(field)); bund>=0) { bunds[bund].usable=true; }
						}
						else
						{
							const auto hit=roadHit(position[v],kBridge);
							const Vec2 target=horizontal(hit.position);
							bool clear=true;
							const int samples=Max(2,static_cast<int>(Ceil(target.distanceFrom(position[v])/4)));
							for (int j=1;j<samples && clear;++j)
							{
								const Vec2 p=position[v].lerp(target,static_cast<double>(j)/samples);
								clear=!insideField(p) && groundPoint(world,p).y>world.waterPlanningHeight(p.x,p.y)+.3;
							}
							if (!clear || sites.occupied.overlaps(ribbon(position[v],position[v].lerp(target,.5),half+.5)) || rail.overlaps(ribbon(position[v],target,half+.5))) { continue; }
							frontageGap[v]=Max(length,frontageGap[v]); gap[v]=0;
						}
						++bridges;
						break;
					}
				}
				result.bridges+=bridges;
				if (bridges==0) { break; }
				search();
			}
			// 道路から離れた田の集まり（ほ場整備の格子など）は、畦でつながる一団ごとに、最寄りの道路か
			// すでに届いている畦道まで草地を通る耕作道を1本だけ引いてから探し直す。届いた一団が次の足掛かりになる。
			for (int round=0;round<3;++round)
			{
				const double kLongReach=config.agriculture_azemichiAccessReach;
				Array<int> root(position.size());
				for (size_t v=0;v<root.size();++v) { root[v]=static_cast<int>(v); }
				const auto find=[&](int v) { while (root[v]!=v) { root[v]=root[root[v]]; v=root[v]; } return v; };
				for (const auto& bund:bunds) { if (bund.usable) { root[find(bund.a)]=find(bund.b); } }
				Array<bool> reached(position.size(),false);
				for (size_t v=0;v<position.size();++v) { reached[v]=atRoad(static_cast<int>(v)); }
				for (size_t state=0;state<distance.size();++state) { if (distance[state]<Math::Inf) { reached[stateTo(static_cast<int>(state))]=true; } }
				constexpr double kReachedCell=256;
				HashTable<Point,Array<int>> reachedGrid;
				for (size_t v=0;v<position.size();++v)
				{
					if (reached[v]) { reachedGrid[Point{static_cast<int>(Floor(position[v].x/kReachedCell)),static_cast<int>(Floor(position[v].y/kReachedCell))}] << static_cast<int>(v); }
				}
				HashTable<int,Array<int>> groups;
				for (size_t field=0;field<fields.size();++field)
				{
					if (served[field] || corners[field].any([&](int v) { return reached[v]; })) { continue; }
					groups[find(corners[field].front())] << static_cast<int>(field);
				}
				/// 草地を通る直線が、田・水域・急な起伏・道路の横断を避けているか。
				const auto clearPath=[&](Vec2 from,Vec2 to,double endSlack)
				{
					const double length=from.distanceFrom(to);
					const int samples=Max(2,static_cast<int>(Ceil(length/8)));
					double previous=groundPoint(world,from).y;
					for (int j=1;j<=samples;++j)
					{
						const Vec2 p=from.lerp(to,static_cast<double>(j)/samples);
						const double height=groundPoint(world,p).y;
						const bool nearEnd=p.distanceFrom(to)<endSlack;
						if ((p.distanceFrom(from)>=3 && !nearEnd && insideField(p))
							|| height<=world.waterPlanningHeight(p.x,p.y)+.3
							|| Abs(height-previous)>length/samples*config.agriculture_maximumTrackGrade*2
							|| (!nearEnd && roadGap(p,kRoadReach)<=1.5)) { return false; }
						previous=height;
					}
					return true;
				};
				int accesses=0;
				for (const auto& [group,members]:groups)
				{
					// 一団の角から近い道路と、一団の中心から近い到達済みの交点を候補にし、近い順に通れる1本を選ぶ。
					Array<std::tuple<double,int,int>> candidates; ///< 距離・一団の角・接続先の交点（-1は道路）
					Vec2 center{0,0}; int count=0;
					for (const int member:members)
					{
						for (const int v:corners[member])
						{
							center+=position[v]; ++count;
							const auto hit=roadHit(position[v],kLongReach);
							if (!hit.edgeId) { continue; }
							const double reach=horizontal(hit.position).distanceFrom(position[v])-roads.getEdge(*hit.edgeId)->totalWidth()*.5;
							if (reach<=kLongReach) { candidates.emplace_back(reach,v,-1); }
						}
					}
					center/=Max(count,1);
					const int cells=static_cast<int>(Ceil(kLongReach/kReachedCell));
					const Point middle{static_cast<int>(Floor(center.x/kReachedCell)),static_cast<int>(Floor(center.y/kReachedCell))};
					Array<std::pair<double,int>> nearby;
					for (int dz=-cells;dz<=cells;++dz) { for (int dx=-cells;dx<=cells;++dx)
					{
						const auto found=reachedGrid.find(middle+Point{dx,dz}); if (found==reachedGrid.end()) { continue; }
						for (const int w:found->second) { nearby.emplace_back(position[w].distanceFromSq(center),w); }
					} }
					std::partial_sort(nearby.begin(),nearby.begin()+Min<size_t>(nearby.size(),24),nearby.end());
					for (size_t index=0;index<Min<size_t>(nearby.size(),24);++index)
					{
						const int w=nearby[index].second; int best=-1; double bestDistance=kLongReach;
						for (const int member:members) { for (const int v:corners[member]) { const double d=position[v].distanceFrom(position[w]); if (d<bestDistance) { bestDistance=d; best=v; } } }
						if (best>=0) { candidates.emplace_back(bestDistance,best,w); }
					}
					std::sort(candidates.begin(),candidates.end());
					for (size_t index=0;index<Min<size_t>(candidates.size(),40);++index)
					{
						const auto [length,v,w]=candidates[index];
						if (w>=0)
						{
							if (!clearPath(position[v],position[w],3)) { continue; }
							const auto quad=ribbon(position[v],position[w],half+.5);
							if (sites.occupied.overlaps(quad) || rail.overlaps(quad)) { continue; }
							int owner=members.front();
							for (const int member:members) { if (corners[member].contains(v)) { owner=member; break; } }
							if (const int bund=addBund(v,w,owner); bund>=0) { bunds[bund].usable=true; }
						}
						else
						{
							const auto hit=roadHit(position[v],kLongReach);
							const Vec2 target=horizontal(hit.position);
							const double width=roads.getEdge(*hit.edgeId)->totalWidth();
							if (!clearPath(position[v],target,width*.5+6)) { continue; }
							const auto quad=ribbon(position[v],target-(target-position[v]).normalized()*Min(position[v].distanceFrom(target)*.5,width*.5+2),half+.5);
							if (sites.occupied.overlaps(quad) || rail.overlaps(quad)) { continue; }
							gap[v]=0; frontageGap[v]=Max(length,kRoadReach+.1);
						}
						++accesses;
						break;
					}
				}
				result.accesses+=accesses;
				if (accesses==0) { break; }
				search();
			}
			result.phaseMs[2]=phase.msF();
			// 近い田から順に、まだ道に面していない田畑へ経路を通す。後の田は先に通した畦道を共有する。
			Array<Array<int>> fieldBunds(fields.size());
			for (size_t bund=0;bund<bunds.size();++bund) { for (const int field:bunds[bund].fields) { fieldBunds[field] << static_cast<int>(bund); } }
			struct Target { double cost; int field; int state; };
			Array<int> arrival(position.size(),-1);
			for (size_t state=0;state<distance.size();++state)
			{
				if (distance[state]==Math::Inf) { continue; }
				int& best=arrival[stateTo(static_cast<int>(state))];
				if (best<0 || distance[state]<distance[best]) { best=static_cast<int>(state); }
			}
			Array<Target> targets;
			for (size_t field=0;field<fields.size();++field)
			{
				if (served[field]) { continue; }
				Target best{Math::Inf,static_cast<int>(field),-1};
				for (const int bund:fieldBunds[field])
				{
					for (const int state:{bund*2,bund*2+1}) { if (distance[state]<best.cost) { best.cost=distance[state]; best.state=state; } }
				}
				// 辺に沿って入れない田も、角まで畦道が来れば角の進入口から入れる（一筆進入路）。
				if (best.state<0)
				{
					bool roadCorner=false;
					for (const int v:corners[field])
					{
						roadCorner|=atRoad(v);
						if (arrival[v]>=0 && distance[arrival[v]]<best.cost) { best.cost=distance[arrival[v]]; best.state=arrival[v]; }
					}
					if (roadCorner) { ++result.connected; continue; }
				}
				if (best.state<0) { ++result.unreachable; continue; }
				targets << best;
			}
			targets.sort_by([](const Target& x,const Target& y) { return x.cost<y.cost; });
			for (const auto& target:targets)
			{
				if (fieldBunds[target.field].any([&](int bund) { return bunds[bund].built; })) { ++result.connected; continue; }
				for (int state=target.state;state>=0;state=parent[state]) { bunds[state>>1].built=true; }
				++result.connected;
			}
			// 末端は、近くの畦道か道路へ畦を1〜3本たどって戻れるなら輪にする。戻れない末端は最後の田で終わる。
			Array<int> degree(position.size(),0);
			for (const auto& bund:bunds) { if (bund.built) { ++degree[bund.a]; ++degree[bund.b]; } }
			const auto anchored=[&](int v) { return atRoad(v) || degree[v]>0; };
			for (size_t start=0;start<position.size();++start)
			{
				const int leaf=static_cast<int>(start);
				if (degree[leaf]!=1 || atRoad(leaf)) { continue; }
				// 畦道で4本以内にたどれる交点へ戻ると1枚の田を囲むだけの輪になるので、戻り先から外す。
				HashSet<int> nearby{leaf}; Array<int> frontier{leaf};
				for (int hop=0;hop<4;++hop)
				{
					Array<int> next;
					for (const int v:frontier) { for (const int bund:adjacency[v]) { if (!bunds[bund].built) { continue; } const int w=bunds[bund].a==v ? bunds[bund].b : bunds[bund].a; if (nearby.insert(w).second) { next << w; } } }
					frontier=std::move(next);
				}
				double bestLength=config.agriculture_azemichiLoopReach; Array<int> bestPath, path;
				const auto loopSearch=[&](auto&& self,int vertex,double length,int depth) -> void
				{
					for (const int bund:adjacency[vertex])
					{
						const auto& item=bunds[bund];
						if (item.built || !item.usable || path.contains(bund)) { continue; }
						const double total=length+item.length; if (total>=bestLength) { continue; }
						const int next=item.a==vertex ? item.b : item.a;
						if (next==leaf) { continue; }
						path << bund;
						if (anchored(next) && !nearby.contains(next)) { bestLength=total; bestPath=path; }
						else if (depth<3) { self(self,next,total,depth+1); }
						path.pop_back();
					}
				};
				loopSearch(loopSearch,leaf,0,1);
				if (bestPath.isEmpty()) { continue; }
				for (const int bund:bestPath) { bunds[bund].built=true; ++degree[bunds[bund].a]; ++degree[bunds[bund].b]; }
				++result.loops;
			}
			result.phaseMs[3]=phase.msF();
			// 畦道を道路網へ加える。直進が続く交点はまとめ、曲がる交点・分岐・道路への取り付けだけを節点にする。
			const auto straightThrough=[&](int v)
			{
				if (degree[v]!=2) { return false; }
				Array<Vec2> directions;
				for (const int bund:adjacency[v]) { if (bunds[bund].built) { directions << (position[bunds[bund].a==v ? bunds[bund].b : bunds[bund].a]-position[v]).normalized(); } }
				return directions.size()==2 && directions[0].dot(directions[1])<-.995;
			};
			const auto junction=[&](int v) { return atRoad(v) || !straightThrough(v); };
			Array<int> nodeOf(position.size(),-1);
			HashSet<int> added; HashSet<int> touched; Array<std::pair<Vec2,Vec2>> corridors;
			/// 2つの節点を直線の畦道で結ぶ。
			const auto connect=[&](int a,int b)
			{
				const Vec3 pa=roads.getNode(a)->position, pb=roads.getNode(b)->position;
				if (a==b || horizontal(pb-pa).length()<3) { return false; }
				const auto id=roads.addEdge(a,b,pa.lerp(pb,1.0/3),pa.lerp(pb,2.0/3),RoadType::LocalRoad,2);
				if (!id) { return false; }
				auto* edge=roads.getEdge(*id);
				GeneratedStreet::applyAzemichi(*edge); edge->edgeState=EdgeState::Existing;
				added.insert(*id); touched.insert(a); touched.insert(b);
				corridors.emplace_back(horizontal(pa),horizontal(pb));
				result.length+=horizontal(pb-pa).length();
				return true;
			};
			const auto nodeFor=[&](int v)
			{
				if (nodeOf[v]>=0 && roads.getNode(nodeOf[v])) { return nodeOf[v]; }
				int id=-1;
				if (atRoad(v))
				{
					const bool bridged=frontageGap[v]>kRoadReach;
					const auto hit=roadHit(position[v],bridged ? frontageGap[v]+kRoadReach : kRoadReach);
					if (hit.edgeId)
					{
						id=attachToRoad(world,roads,hit,8);
						if (id>=0) { for (const int child:roads.getNode(id)->edgeIds()) { roadIndex.appendEdge(roads,child); } }
						// 離れた田の角から道路までは、角に節点を置いて短い畦道で結ぶ。
						if (id>=0 && bridged)
						{
							const int corner=roads.addNode(groundPoint(world,position[v]),NodeType::Joint);
							id=connect(id,corner) ? corner : -1;
						}
					}
				}
				else { id=roads.addNode(groundPoint(world,position[v]),NodeType::Joint); }
				nodeOf[v]=id;
				return id;
			};
			Array<bool> walked(bunds.size(),false);
			for (size_t start=0;start<position.size();++start)
			{
				const int origin=static_cast<int>(start);
				if (degree[origin]==0 || !junction(origin)) { continue; }
				for (const int first:adjacency[origin])
				{
					if (!bunds[first].built || walked[first]) { continue; }
					int vertex=origin, bund=first;
					while (true)
					{
						walked[bund]=true;
						vertex=bunds[bund].a==vertex ? bunds[bund].b : bunds[bund].a;
						if (junction(vertex)) { break; }
						int next=-1;
						for (const int other:adjacency[vertex]) { if (other!=bund && bunds[other].built) { next=other; } }
						if (next<0 || walked[next]) { break; }
						bund=next;
					}
					const int a=nodeFor(origin), b=nodeFor(vertex);
					if (a<0 || b<0) { continue; }
					connect(a,b);
				}
			}
			// 途中の取り付けに失敗して既存の道路網へ戻れない畦道は、道路網に浮いた島になるので取り除く。
			{
				HashSet<int> connected; Array<int> pending;
				for (const auto& edge:roads.edges())
				{
					if (edge.id<0 || added.contains(edge.id)) { continue; }
					for (const int node:{edge.nodeA,edge.nodeB}) { if (connected.insert(node).second) { pending << node; } }
				}
				for (size_t next=0;next<pending.size();++next)
				{
					for (const int id:roads.getNode(pending[next])->edgeIds())
					{
						const auto* edge=roads.getEdge(id); if (!edge) { continue; }
						for (const int node:{edge->nodeA,edge->nodeB}) { if (connected.insert(node).second) { pending << node; } }
					}
				}
				Array<int> stranded;
				for (const int id:added) { if (const auto* edge=roads.getEdge(id); edge && !connected.contains(edge->nodeA)) { stranded << id; } }
				for (const int id:stranded)
				{
					const auto* edge=roads.getEdge(id);
					result.length-=horizontal(roads.getNode(edge->nodeB)->position-roads.getNode(edge->nodeA)->position).length();
					roads.removeEdge(id); added.erase(id); ++result.stranded;
				}
			}
			// 端点のない孤立した節点（取り付けに失敗した交点）は残さない。
			for (const int v:nodeOf) { if (v>=0 && roads.getNode(v) && roads.getNode(v)->attachments.isEmpty()) { roads.removeNode(v); touched.erase(v); } }
			for (const int node:touched) { if (roads.getNode(node)) { roads.updateNodeCutoffs(node); roads.rebuildLaneConnections(node); } }
			result.phaseMs[4]=phase.msF();
			if (!added.empty()) { RoadTerrainFit::apply(roads,world,added); }
			result.phaseMs[5]=phase.msF();
			clipFieldsAlong(world,corridors,half+.3,true);
			result.phaseMs[6]=phase.msF();
			result.edges=static_cast<int>(added.size());
			for (const int id:added)
			{
				const auto* edge=roads.getEdge(id); if (!edge) { continue; }
				for (const int node:{edge->nodeA,edge->nodeB}) { result.deadEnds+=roads.getNode(node)->attachments.size()==1; }
			}
			return result;
		}
	}

	Stats prepare(World& world,RoadNetwork& roads,uint64 seed,const Array<Frame>& inputFrames,const TrainNetwork* railway)
	{
		Stats stats;const auto& config=GenerationSettings::get();const auto frames=localFrames(roads,inputFrames);
		constexpr double kJunctionMargin = 32;
		constexpr double kFieldProbeHalfDepth = 20;
		constexpr double kMinimumSupportRatio = .65;
		/// @brief 描画用の短いエッジ分割を、ほ場計画の区切りと取り違えない連続沿道。
		struct Frontage
		{
			Array<CubicBezier> curves;
			float totalLength = 0;
			Vec3 positionAt(float arc) const
			{
				for (const auto& curve : curves) { if (arc <= curve.totalLength) { return curve.positionAt(arc); } arc -= curve.totalLength; }
				return curves.back().p3;
			}
			Vec3 tangentAt(float arc) const
			{
				for (const auto& curve : curves) { if (arc <= curve.totalLength) { return curve.tangentAt(arc); } arc -= curve.totalLength; }
				return curves.back().tangentAt(curves.back().totalLength);
			}
		};
		const auto eligible = [](const RoadEdge& edge)
		{
			return edge.id >= 0 && edge.hasRoadLanes() && edge.isRoadbedBuilt() && !edge.useElevation && !edge.tunnel
				&& !edge.farmAccess && edge.roadType != RoadType::Expressway;
		};
		const auto nextEdge = [&](int node,int previous) -> Optional<int>
		{
			const auto* incoming = roads.getEdge(previous);
			const auto curve = roads.getBezier(previous);
			const Vec3 reverse = curve->tangentAt(incoming->nodeA == node ? 0 : curve->totalLength) * (incoming->nodeA == node ? 1 : -1);
			Optional<int> next;
			for (const auto& attachment : roads.getNode(node)->attachments)
			{
				const auto* edge = roads.getEdge(attachment.edgeId);
				if (attachment.edgeId == previous || !edge || !eligible(*edge)) { continue; }
				if (next || edge->roadType != incoming->roadType) { return none; }
				const auto other = roads.getBezier(edge->id);
				const Vec3 direction = other->tangentAt(edge->nodeA == node ? 0 : other->totalLength) * (edge->nodeA == node ? 1 : -1);
				if (horizontal(reverse).normalized().dot(horizontal(direction).normalized()) > -.9) { return none; }
				next = edge->id;
			}
			return next;
		};
		Array<Frontage> frontages; HashSet<int> visited;
		for (const auto& edge : roads.edges())
		{
			if (!eligible(edge) || visited.contains(edge.id)) { continue; }
			int first = edge.id, start = edge.nodeA; HashSet<int> backwards{first};
			while (const auto previous = nextEdge(start,first))
			{
				if (visited.contains(*previous) || !backwards.insert(*previous).second) { break; }
				const auto* item = roads.getEdge(*previous); start = item->nodeA == start ? item->nodeB : item->nodeA; first = *previous;
			}
			Frontage frontage; int id = first, from = start;
			while (visited.insert(id).second)
			{
				const auto* item = roads.getEdge(id); const auto curve = *roads.getBezier(id);
				const bool forward = item->nodeA == from;
				frontage.curves << (forward ? curve : CubicBezier{curve.p3,curve.p2,curve.p1,curve.p0});
				frontage.totalLength += curve.totalLength;
				const int to = forward ? item->nodeB : item->nodeA; const auto next = nextEdge(to,id);
				if (!next || visited.contains(*next)) { break; }
				id = *next; from = to;
			}
			if (!frontage.curves.isEmpty()) { frontages << std::move(frontage); }
		}
		ParcelRoadIndex occupied{roads,true}; RoadNetwork empty; ParcelRoadIndex rail{empty};
		if (railway) { rail.addRailway(*railway); }
		Sites buildings{world}; RoadPlanSnapIndex connections; connections.rebuild(roads);
		const auto probe = [&](Vec3 position)
		{
			constexpr double kSnapTolerance = 2;
			return connections.find(roads,position,kSnapTolerance,kSnapTolerance,false,
				[](const RoadEdge& edge) { return edge.hasRoadLanes() && !edge.farmAccess; });
		};
		/// 接続点で既存道を分割し、取り付け先の節点を返す。
		const auto attachHit = [&](const RoadPlanSnapIndex::Hit& hit) -> Optional<int>
		{
			if (!hit.edgeId) { return none; }
			const auto* edge = roads.getEdge(*hit.edgeId);
			if (!edge) { return none; }
			if (hit.curveT == 0) { return edge->nodeA; }
			if (hit.curveT == 1) { return edge->nodeB; }
			const auto curve = roads.getBezier(edge->id);
			const float sample = hit.curveT * CubicBezier::SAMPLES;
			const int index = Min(CubicBezier::SAMPLES - 1,static_cast<int>(sample));
			const int node = roads.splitEdgeAt(edge->id,Math::Lerp(curve->arcTable[index],curve->arcTable[index + 1],sample - index));
			if (node < 0) { return none; }
			for (const int child : roads.getNode(node)->edgeIds()) { connections.appendEdge(roads,child); }
			return node;
		};
		/// @brief 両端の節点が決まった農道を追加する。途中で追加に失敗したら、この農道の辺と節点を戻す。
		const auto commitTrack = [&](int first,int last,const Array<CubicBezier>& curves)
		{
			Array<int> nodes{first}, addedEdges;
			for (size_t i = 0; i + 1 < curves.size(); ++i) { nodes << roads.addNode(curves[i].p3,NodeType::Joint); }
			nodes << last;
			bool valid = true;
			for (size_t i = 0; i < curves.size(); ++i)
			{
				const auto id = roads.addEdge(nodes[i],nodes[i + 1],curves[i].p1,curves[i].p2,RoadType::LocalRoad,2);
				if (!id) { valid = false; break; }
				auto* edge = roads.getEdge(*id); auto profile = GeneratedStreet::describe(GeneratedStreet::Role::FarmAccess);
				profile.laneWidth = static_cast<float>(config.agriculture_trackWidth / profile.lanes);
				GeneratedStreet::apply(*edge,profile); edge->edgeState = EdgeState::Existing; edge->farmAccess = true;
				edge->parts.remove_if([](const RoadPart& part) { return part.type == RoadPartType::UtilityPole; });
				addedEdges << *id;
			}
			if (!valid)
			{
				for (const int id : addedEdges) { roads.removeEdge(id); }
				for (size_t i = 1; i + 1 < nodes.size(); ++i) { roads.removeNode(nodes[i]); }
				return false;
			}
			for (const int id : addedEdges)
			{
				const auto curve = roads.getBezier(id);
				for (int i = 1; i <= 8; ++i) { occupied.add(ribbon(horizontal(curve->evaluate((i - 1) / 8.0f)),horizontal(curve->evaluate(i / 8.0f)),config.agriculture_trackWidth * .5 + 1)); }
				connections.appendEdge(roads,id);
			}
			for (const int node : nodes) { roads.updateNodeCutoffs(node); roads.rebuildLaneConnections(node); }
			stats.tracks += static_cast<int>(addedEdges.size()); stats.connections += 2;
			return true;
		};
		int blocks = 0, rejected = 0;
		Array<Vec2> blockFrontage;
		// 耕区の長辺に沿う農道を先に計画し、両端だけを既存道へ取り付ける。
		// 全区間と耕作可能な帯を確認するまで、道路や交差点を追加しない。
		for (const auto& frontage : frontages)
		{
			for (double begin = kJunctionMargin; begin + config.agriculture_accessSpacing < frontage.totalLength - kJunctionMargin;
				begin += config.agriculture_trackReach + config.agriculture_accessSpacing)
			{
				const double end = Min(begin + config.agriculture_trackReach, frontage.totalLength - kJunctionMargin);
				const Vec2 center = horizontal(frontage.positionAt(static_cast<float>((begin + end) * .5)));
				const double probability = density(center,frames);
				if (probability < config.agriculture_minimumDensity || (hash(seed,center) % 10000) / 10000.0 > probability) { continue; }
				for (const int side : {-1,1})
				{
					const double depth = config.agriculture_plotWidth * 2;
					const int sections = Max(3,static_cast<int>(Ceil((end - begin) / config.agriculture_trackStep)));
					Array<Vec3> points{frontage.positionAt(static_cast<float>(begin))};
					int supported = 0;
					for (int section = 0; section <= sections; ++section)
					{
						const float arc = static_cast<float>(Math::Lerp(begin,end,static_cast<double>(section) / sections));
						const Vec3 base = frontage.positionAt(arc);
						const Vec2 normal = horizontal(tangentToRight(frontage.tangentAt(arc))) * side;
						const Vec2 point = horizontal(base) + normal * depth;
						points << groundPoint(world,point);
						// 並行道の両側に、道路幅とは別に実際のほ場を置ける土地が必要。
						const Vec2 along = horizontal(frontage.tangentAt(arc)).normalized();
						bool usable = true; double low = Math::Inf, high = -Math::Inf;
						for (const int x : {-1,1}) { for (const int z : {-1,1})
						{
							const Vec2 fieldProbe = point + along * (config.agriculture_plotLength * .2 * x) + normal * (kFieldProbeHalfDepth * z);
							const Vec3 ground = groundPoint(world,fieldProbe);
							usable &= agricultural(world,fieldProbe) && ground.y > world.waterPlanningHeight(fieldProbe.x,fieldProbe.y) + config.agriculture_minimumFreeboard
								&& !rail.overlaps(ParcelGeometry::footprint(fieldProbe,kFieldProbeHalfDepth * .5,0))
								&& !buildings.occupied.overlaps(ParcelGeometry::footprint(fieldProbe,kFieldProbeHalfDepth * .5,0));
							low = Min(low,ground.y); high = Max(high,ground.y);
						} }
						supported += usable && high - low <= config.agriculture_maximumFieldRelief;
					}
					points << frontage.positionAt(static_cast<float>(end));
					if (supported < (sections + 1) * kMinimumSupportRatio) { ++rejected; continue; }
					Array<CubicBezier> curves; bool valid = true;
					for (size_t piece = 1; piece < points.size(); ++piece)
					{
						const Vec3 a = points[piece - 1], b = points[piece];
						const CubicBezier curve{a,a.lerp(b,1.0/3),a.lerp(b,2.0/3),b};
						const int samples = Max(1,static_cast<int>(Ceil(curve.totalLength / config.agriculture_sampleStep)));
						Vec3 last = a;
						for (int sample = 1; sample <= samples; ++sample)
						{
							const Vec3 p = curve.positionAt(curve.totalLength * sample / samples), ground = groundPoint(world,horizontal(p));
							const auto footprint = ParcelGeometry::footprint(horizontal(p),config.agriculture_trackWidth * .5 + 1,0);
							const bool atJunction = (piece == 1 && p.distanceFrom(points.front()) < kJunctionMargin)
								|| (piece + 1 == points.size() && p.distanceFrom(points.back()) < kJunctionMargin);
							valid &= (atJunction || agricultural(world,horizontal(p)))
								&& ground.y > world.waterPlanningHeight(p.x,p.z) + config.agriculture_minimumFreeboard
								&& Abs(ground.y - p.y) <= config.agriculture_maximumTrackCutFill
								&& Abs(ground.y - last.y) <= horizontal(ground - last).length() * config.agriculture_maximumTrackGrade
								&& !rail.overlaps(footprint) && !buildings.occupied.overlaps(footprint)
								&& (atJunction || !occupied.overlaps(footprint));
							last = ground;
						}
						curves << curve;
					}
					if (!valid) { ++rejected; continue; }
					// 片端だけ分割して取り付けに失敗すると不要な節点が残るため、両端の接続先を先に確かめる。
					if (!probe(points.front()).edgeId || !probe(points.back()).edgeId) { ++rejected; continue; }
					const auto first = attachHit(probe(points.front())), last = attachHit(probe(points.back()));
					if (!first || !last || !commitTrack(*first,*last,curves)) { ++rejected; continue; }
					++blocks;
					for (double arc = begin; arc <= end; arc += kJunctionMargin) { blockFrontage << horizontal(frontage.positionAt(static_cast<float>(arc))); }
				}
			}
		}
		DBG_LOG(U"[AgriculturalBlocks] seed={} blocks={} rejected={} tracks={} connections={}"_fmt(seed,blocks,rejected,stats.tracks,stats.connections));
		// 区画農道を通せない細長い耕地や山際の谷田には、既存道から地形に沿う枝農道を伸ばす。
		// 経路全体を計画し、別の道へ戻れる枝だけを追加するので、行き止まりや使われない分岐点を残さない。
		struct Root { Vec3 position; Optional<int> node; Vec2 direction; double reach; };
		Array<Root> roots; HashSet<Point> rootCells;
		const double rootSpacing = config.agriculture_accessSpacing * config.agriculture_rootSpacingRatio;
		const auto addRoots = [&](Vec3 position,Optional<int> node,Vec2 normal)
		{
			const Vec2 point = horizontal(position); const double probability = density(point,frames);
			if (probability < config.agriculture_minimumDensity || (hash(seed,point) % 10000) / 10000.0 > probability) { return; }
			if (Abs(groundPoint(world,point).y - position.y) > 1) { return; }
			// 区画農道を整備した沿道では、その間へ直交する枝道を足さず、耕区の並行配置を保つ。
			if (blockFrontage.any([&](Vec2 block) { return block.distanceFromSq(point) < Square(config.agriculture_accessSpacing * .5); })) { return; }
			if (!rootCells.insert(Point{static_cast<int>(point.x / rootSpacing),static_cast<int>(point.y / rootSpacing)}).second) { return; }
			const double reach = config.agriculture_trackReach * (config.agriculture_minimumReachRatio + (1 - config.agriculture_minimumReachRatio) * probability);
			for (const int side : {-1,1})
			{
				// 町の道路沿いにある住宅帯を越え、最初の区間の先が農地なら取り付けを許可する。
				if (agricultural(world,point + normal * side * config.agriculture_trackStep)) { roots << Root{position,node,normal * side,reach}; }
			}
		};
		for (const auto& node : roads.nodes())
		{
			if (node.id < 0 || node.attachments.isEmpty() || node.attachments.size() > 3) { continue; }
			const auto* edge = roads.getEdge(node.attachments.front().edgeId);
			if (!edge || !eligible(*edge)) { continue; }
			const auto curve = roads.getBezier(edge->id);
			addRoots(node.position,node.id,horizontal(tangentToRight(curve->tangentAt(edge->nodeA == node.id ? 0 : curve->totalLength))));
		}
		// 長い沿道では交差点の間からも枝を出す。分岐点は枝を採用したときだけ作る。
		for (const auto& frontage : frontages)
		{
			for (double arc = config.agriculture_accessSpacing; arc < frontage.totalLength - config.agriculture_accessSpacing * .5; arc += config.agriculture_accessSpacing)
			{
				const Vec3 position = frontage.positionAt(static_cast<float>(arc));
				if (!agricultural(world,horizontal(position))) { continue; }
				addRoots(position,none,horizontal(tangentToRight(frontage.tangentAt(static_cast<float>(arc)))));
			}
		}
		const auto connectable = [](const RoadEdge& edge)
		{
			return edge.hasRoadLanes() && edge.isRoadbedBuilt() && !edge.tunnel && !edge.useElevation && edge.roadType != RoadType::Expressway;
		};
		/// @brief 枝農道の1区間が、耕地・勾配・切盛り・既存施設の条件を満たすか調べる。
		const auto spurValid = [&](const CubicBezier& curve,bool leaving,double joiningWidth)
		{
			Vec3 last = curve.p0;
			const int samples = Max(3,static_cast<int>(Ceil(curve.totalLength / config.agriculture_sampleStep)));
			for (int i = 1; i <= samples; ++i)
			{
				const Vec3 p = curve.positionAt(curve.totalLength * i / samples), ground = groundPoint(world,horizontal(p));
				const auto footprint = ParcelGeometry::footprint(horizontal(p),config.agriculture_trackWidth * .5 + 1,0);
				const bool joining = p.distanceFrom(curve.p3) < joiningWidth;
				if (!(leaving || joining || agricultural(world,horizontal(p)))
					|| ground.y <= world.waterPlanningHeight(p.x,p.z) + config.agriculture_minimumFreeboard
					|| Abs(ground.y - last.y) > horizontal(p - last).length() * config.agriculture_maximumTrackGrade
					|| Abs(ground.y - p.y) >= config.agriculture_maximumTrackCutFill
					|| rail.overlaps(footprint) || buildings.occupied.overlaps(footprint)
					|| (p.distanceFrom(curve.p0) > 12 && !joining && occupied.overlaps(footprint))) { return false; }
				last = ground;
			}
			return true;
		};
		int spurs = 0, abandoned = 0;
		for (const auto& root : roots)
		{
			if (root.node && (!roads.getNode(*root.node) || roads.getNode(*root.node)->id < 0)) { continue; }
			const auto awayFromRoot = [&](const RoadEdge& edge)
			{
				return connectable(edge) && (!root.node || (edge.nodeA != *root.node && edge.nodeB != *root.node));
			};
			Array<CubicBezier> path; Optional<RoadPlanSnapIndex::Hit> finish;
			Vec3 start = root.position; Vec2 direction = root.direction;
			const double phase = (hash(seed,horizontal(root.position)) % 10000) / 10000.0 * Math::TwoPi;
			for (double run = 0; run < root.reach && !finish; run += config.agriculture_trackStep)
			{
				Optional<CubicBezier> best; Optional<RoadPlanSnapIndex::Hit> bestConnection; Vec2 bestDirection = direction; double bestCost = Math::Inf;
				// 乱数を区間ごとに振らず、連続した蛇行の目標方向を地形費用と比較する。
				const double bend = config.agriculture_meanderAngle * (Sin(phase + run * Math::TwoPi / config.agriculture_meanderWavelength) - Sin(phase));
				const Vec2 preferred{root.direction.x * Cos(bend) - root.direction.y * Sin(bend),root.direction.x * Sin(bend) + root.direction.y * Cos(bend)};
				for (const int turn : {0,-1,1,-2,2})
				{
					const double angle = turn * config.agriculture_trackHeadingStep;
					const Vec2 next{direction.x * Cos(angle) - direction.y * Sin(angle),direction.x * Sin(angle) + direction.y * Cos(angle)};
					if (next.dot(root.direction) < .25) { continue; }
					Vec3 end = groundPoint(world,horizontal(start) + (direction + next).normalized() * config.agriculture_trackStep);
					Optional<RoadPlanSnapIndex::Hit> connection;
					const auto hit = connections.find(roads,end,config.agriculture_connectionRadius,config.agriculture_maximumTrackCutFill,false,awayFromRoot);
					if (hit.edgeId)
					{
						const Vec2 span = horizontal(hit.position - start);
						if (span.length() > 12 && span.normalized().dot(direction) > .5 && hit.position.distanceFrom(root.position) > config.agriculture_trackStep) { end = hit.position; connection = hit; }
					}
					if (!connection && !agricultural(world,horizontal(end))) { continue; }
					const double handle = horizontal(end - start).length() / 3;
					const CubicBezier curve{start,{start.x + direction.x * handle,Math::Lerp(start.y,end.y,1.0/3),start.z + direction.y * handle},
						{end.x - next.x * handle,Math::Lerp(start.y,end.y,2.0/3),end.z - next.y * handle},end};
					if (curve.minimumHorizontalRadius() < RoadDesignLimits::forType(RoadType::LocalRoad).minimumRadius) { continue; }
					const double joiningWidth = connection ? roads.getEdge(*connection->edgeId)->totalWidth() * .5 + config.agriculture_trackWidth * .5 + 3 : 0;
					if (!spurValid(curve,run == 0,joiningWidth)) { continue; }
					double reliefCost = 0;
					for (int i = 1; i <= 4; ++i) { reliefCost += Square(groundPoint(world,horizontal(curve.evaluate(i / 4.0f))).y - groundPoint(world,horizontal(curve.evaluate((i - 1) / 4.0f))).y); }
					const double cost = reliefCost + Square(angle) * config.agriculture_trackTurnPenalty + (next - preferred).lengthSq() * config.agriculture_trackDirectionPenalty;
					if ((!bestConnection && connection) || (static_cast<bool>(bestConnection) == static_cast<bool>(connection) && cost < bestCost))
					{
						best = curve; bestDirection = next; bestCost = cost; bestConnection = connection;
					}
				}
				if (!best) { break; }
				path << *best; start = best->p3; direction = bestDirection; finish = bestConnection;
			}
			if (path.isEmpty()) { continue; }
			if (!finish)
			{
				// 届かなかった枝は、最寄りの道へ緩い曲線で戻せる場合だけ残す。
				const auto hit = connections.find(roads,start,config.agriculture_deadEndConnectionRadius,
					config.agriculture_deadEndConnectionRadius * config.agriculture_maximumTrackGrade,false,awayFromRoot);
				if (hit.edgeId && hit.position.distanceFrom(root.position) > config.agriculture_trackStep)
				{
					const Vec3 tangent = (path.back().p3 - path.back().p2).normalized(), span = hit.position - start;
					const double length = horizontal(span).length();
					if (length >= 24 && horizontal(span).normalized().dot(horizontal(tangent).normalized()) >= -.25)
					{
						Vec3 handle = tangent * (length / 3); handle.y = span.y / 3;
						const CubicBezier curve{start,start + handle,hit.position - span / 3,hit.position};
						const double joiningWidth = roads.getEdge(*hit.edgeId)->totalWidth() * .5 + config.agriculture_trackWidth * .5 + 3;
						if (RoadAlignment::respectsLimits(curve,RoadType::LocalRoad) && spurValid(curve,false,joiningWidth)) { path << curve; finish = hit; }
					}
				}
			}
			if (!finish || (!root.node && !probe(root.position).edgeId)) { ++abandoned; continue; }
			// 終点側を先に分割する。始点が同じ辺上でも、分割後の辺から位置で取り直せる。
			const auto last = attachHit(*finish);
			const auto first = root.node ? root.node : (last ? attachHit(probe(root.position)) : none);
			if (!first || !last || *first == *last || !commitTrack(*first,*last,path)) { ++abandoned; continue; }
			++spurs;
		}
		DBG_LOG(U"[AgriculturalSpurs] seed={} roots={} spurs={} abandoned={} tracks={}"_fmt(seed,roots.size(),spurs,abandoned,stats.tracks));
		// 伸長が終わった農道だけを再検査し、最寄りの接続可能な道まで枝を延長する。
		Array<int> deadEnds;
		for (const auto& node : roads.nodes())
		{
			if (node.id < 0 || node.attachments.size() != 1)
			{
				continue;
			}
			const auto* edge = roads.getEdge(node.attachments.front().edgeId);
			if (edge && edge->farmAccess)
			{
				deadEnds << node.id;
			}
		}
		for (int id : deadEnds)
		{
			const auto* node = roads.getNode(id);
			if (!node || node->attachments.size() != 1)
			{
				continue;
			}
			const auto* incoming = roads.getEdge(node->attachments.front().edgeId);
			const int parent = incoming->nodeA == id ? incoming->nodeB : incoming->nodeA;
			const Vec3 start = node->position;
			const auto incomingCurve = roads.getBezier(incoming->id);
			const Vec3 tangent = incomingCurve->tangentAt(incoming->nodeB == id ? incomingCurve->totalLength : 0) *
								 (incoming->nodeB == id ? 1 : -1);
			const auto hit = connections.find(roads, start, config.agriculture_deadEndConnectionRadius,
				config.agriculture_deadEndConnectionRadius * config.agriculture_maximumTrackGrade, false,
				[&](const RoadEdge& edge)
			{
				return edge.nodeA != id && edge.nodeB != id && edge.nodeA != parent && edge.nodeB != parent &&
					   edge.hasRoadLanes() && edge.isRoadbedBuilt() && !edge.tunnel && !edge.useElevation &&
					   edge.roadType != RoadType::Expressway;
			});
			if (!hit.edgeId)
			{
				continue;
			}
			const Vec3 end = hit.position, span = end - start;
			const double length = horizontal(span).length();
			if (length < 24 || horizontal(span).normalized().dot(horizontal(tangent).normalized()) < -.25)
			{
				continue;
			}
			Vec3 handle = tangent * (length / 3);
			handle.y = span.y / 3;
			const CubicBezier curve{start, start + handle, end - span / 3, end};
			if (!RoadAlignment::respectsLimits(curve, RoadType::LocalRoad))
			{
				continue;
			}
			const auto* target = roads.getEdge(*hit.edgeId);
			const double joinWidth = target->totalWidth() * .5 + config.agriculture_trackWidth * .5 + 3;
			bool valid = true;
			Vec3 last = start;
			const int samples = Max(1, static_cast<int>(Ceil(curve.totalLength / 8)));
			for (int sample = 1; sample <= samples; ++sample)
			{
				const double arc = curve.totalLength * sample / samples;
				const Vec3 point = curve.positionAt(static_cast<float>(arc)),
						   ground = groundPoint(world, horizontal(point));
				const auto footprint =
					ParcelGeometry::footprint(horizontal(point), config.agriculture_trackWidth * .5 + 1, 0);
				valid &= ground.y > world.waterPlanningHeight(point.x, point.z) + config.agriculture_minimumFreeboard &&
						 Abs(ground.y - point.y) < config.agriculture_maximumTrackCutFill &&
						 Abs(ground.y - last.y) <=
							 horizontal(ground - last).length() * config.agriculture_maximumTrackGrade &&
						 !rail.overlaps(footprint) && !buildings.occupied.overlaps(footprint);
				if (arc > 12 && point.distanceFrom(end) > joinWidth && occupied.overlaps(footprint))
				{
					valid = false;
				}
				if (!valid)
				{
					break;
				}
				last = ground;
			}
			if (!valid)
			{
				continue;
			}
			int endpoint = hit.curveT == 0 ? target->nodeA : target->nodeB;
			if (hit.curveT > 0 && hit.curveT < 1)
			{
				const auto targetCurve = roads.getBezier(target->id);
				const float sample = hit.curveT * CubicBezier::SAMPLES;
				const int index = Min(CubicBezier::SAMPLES - 1, static_cast<int>(sample));
				endpoint = roads.splitEdgeAt(target->id,
					Math::Lerp(targetCurve->arcTable[index], targetCurve->arcTable[index + 1], sample - index));
				if (endpoint < 0)
				{
					continue;
				}
				for (int child : roads.getNode(endpoint)->edgeIds())
				{
					connections.appendEdge(roads, child);
				}
			}
			if (const auto added = roads.addEdge(id, endpoint, curve.p1, curve.p2, RoadType::LocalRoad, 2))
			{
				auto* edge = roads.getEdge(*added);
				auto profile = GeneratedStreet::describe(GeneratedStreet::Role::FarmAccess);
				profile.laneWidth = static_cast<float>(config.agriculture_trackWidth / profile.lanes);
				GeneratedStreet::apply(*edge, profile);
				edge->edgeState = EdgeState::Existing;
				edge->farmAccess = true;
				edge->parts.remove_if([](const RoadPart& part) { return part.type == RoadPartType::UtilityPole; });
				roads.rebuildNodeConnectivity(id, endpoint);
				connections.appendEdge(roads, *added);
				for (int i = 1; i <= 16; ++i)
				{
					occupied.add(ribbon(horizontal(curve.evaluate((i - 1) / 16.0f)),
						horizontal(curve.evaluate(i / 16.0f)), edge->totalWidth() * .5 + 1));
				}
				++stats.tracks;
				++stats.connections;
			}
		}
		stats.homes=prepareHomes(world,roads,seed,railway,frames);stats.drains=stats.tracks*2;return stats;
	}

	Stats generate(World& world,RoadNetwork& roads,uint64 seed,const Array<Frame>& inputFrames,bool prepareAccess,const TrainNetwork* railway)
	{
		Stats stats=prepareAccess ? prepare(world,roads,seed,inputFrames,railway) : Stats{};
		const auto& config=GenerationSettings::get();const auto frames=localFrames(roads,inputFrames);
		ParcelRoadIndex occupied{roads,true};if (railway) { occupied.addRailway(*railway); }Sites sites{world};
		// 田畑は道路に平行な長方形の短冊。1列目から順に置き、重なる区画は奥行きを縮めるか見送る。
		int sparse=0,unsuitable=0,crowded=0,blocked=0,uneven=0;
		constexpr double kFullCultivationDensity=1.25;
		struct Candidate { Vec2 center; Vec2 left, right, normalLeft, normalRight; double depth; int tier; uint32 salt; };
		Array<Candidate> candidates;
		const double bucketSize=config.agriculture_plotLength*2;
		const auto bucket=[&](Vec2 p){return Point{static_cast<int>(Floor(p.x/bucketSize)),static_cast<int>(Floor(p.y/bucketSize))};};
		for (const auto& edge:roads.edges())
		{
			if (edge.id<0) { continue; }
			// 平野の田畑は農道だけでなく一般の生活道路・県道の両側にも連続して広がる。
			if (!edge.farmAccess && (edge.roadType==RoadType::Expressway || edge.useElevation)) { continue; }
			if (!prepareAccess && edge.farmAccess) { ++stats.tracks;stats.drains+=2; }
			const auto curve=roads.getBezier(edge.id);if (!curve) { continue; }
			const int count=Max(1,static_cast<int>(Round(curve->totalLength/(config.agriculture_plotLength*.5))));
			// 平野の田畑は道路沿いの1列目から奥へ、畦道でつながって途切れずに続く。
			constexpr int kFieldTiers=7;
			for (int tier=0;tier<kFieldTiers;++tier) for (int section=0;section<count;++section) { for (int side:{-1,1})
			{
				const float begin=curve->totalLength*section/count,end=curve->totalLength*(section+1)/count;
				const Vec3 a0=curve->positionAt(begin),b0=curve->positionAt(end),n1=tangentToRight(curve->tangentAt(begin))*side,n2=tangentToRight(curve->tangentAt(end))*side;
				const double tierOffset=tier*config.agriculture_plotWidth*2;
				const Vec3 a=a0+n1*tierOffset,b=b0+n2*tierOffset;
				const Vec2 center=horizontal((a+b)*.5+(n1+n2)*config.agriculture_plotWidth*.5);
				const uint32 salt=hash(seed,center)&~kManagedField;
				// Fields occur in contiguous farming districts rather than isolated random plots.
				const Vec2 districtCell{Floor(center.x/bucketSize)*bucketSize,Floor(center.y/bucketSize)*bucketSize};
				const double districtRoll=(hash(seed,districtCell)%10000)/10000.0;
				const double plotRoll=(salt%10000)/10000.0;
				++stats.candidates;
				// 集落に近い平野はほぼ全面が耕作地になり、遠い縁辺部だけ地区単位で耕作放棄地や原野が混じる。
				if (districtRoll*.72+plotRoll*.28>Min(1.0,density(center,frames)*kFullCultivationDensity)) { ++sparse;continue; }
				constexpr int kTownFieldTiers=2;
				if (!agricultural(world,center) || builtUp(world,center) || insidePlannedTown(center,frames) || (tier>=kTownFieldTiers && insideTown(center,frames)))
				{
					++unsuitable;
					continue;
				}
				const double near=(tier==0 ? edge.totalWidth()*.5+config.agriculture_drainWidth : 0.0)+config.agriculture_bundWidth;
				const double depth=config.agriculture_plotWidth*2*(1+config.agriculture_fieldShapeVariation*(static_cast<double>((salt>>8)%100)/50-1))-config.agriculture_bundWidth;
				candidates << Candidate{center,horizontal(a+n1*near),horizontal(b+n2*near),horizontal(n1),horizontal(n2),depth,tier,salt};
			} }
		}
		candidates.stable_sort_by([](const Candidate& x,const Candidate& y){ return x.tier<y.tier; });
		/// @brief 採用済みの田畑。多角形どうしの交差判定は重いので、外接矩形で先に除外する。
		// 田畑はほぼ凸の四〜五角形なので、三角形分割を伴う汎用の多角形演算を使わず分離軸で判定する。
		struct AcceptedField { Array<Vec2> outline; RectF bounds; };
		const auto boundsOf=[](const Array<Vec2>& outline)
		{
			Vec2 low=outline.front(),high=outline.front();
			for (const Vec2 point:outline) { low={Min(low.x,point.x),Min(low.y,point.y)}; high={Max(high.x,point.x),Max(high.y,point.y)}; }
			return RectF{low,high-low};
		};
		const auto separated=[](const Array<Vec2>& a,const Array<Vec2>& b)
		{
			for (const auto* shape:{&a,&b})
			{
				for (size_t i=0;i<shape->size();++i)
				{
					const Vec2 edge=(*shape)[(i+1)%shape->size()]-(*shape)[i],axis{-edge.y,edge.x};
					double minA=Math::Inf,maxA=-Math::Inf,minB=Math::Inf,maxB=-Math::Inf;
					for (const Vec2 point:a) { const double d=point.dot(axis); minA=Min(minA,d); maxA=Max(maxA,d); }
					for (const Vec2 point:b) { const double d=point.dot(axis); minB=Min(minB,d); maxB=Max(maxB,d); }
					if (maxA<minB || maxB<minA) { return true; }
				}
			}
			return false;
		};
		const auto inside=[](const Array<Vec2>& outline,Vec2 point)
		{
			bool result=false;
			for (size_t i=0,j=outline.size()-1;i<outline.size();j=i++)
			{
				const Vec2 a=outline[i],b=outline[j];
				if ((a.y>point.y)!=(b.y>point.y) && point.x<(b.x-a.x)*(point.y-a.y)/(b.y-a.y)+a.x) { result=!result; }
			}
			return result;
		};
		HashTable<Point,Array<AcceptedField>> accepted;
		const auto overlapsAccepted=[&](const Array<Vec2>& shape,Vec2 center)
		{
			const Point cell=bucket(center);const RectF bounds=boundsOf(shape);
			for (int dz=-1;dz<=1;++dz) { for (int dx=-1;dx<=1;++dx)
			{
				if (const auto found=accepted.find(cell+Point{dx,dz});found!=accepted.end())
				{
					for (const auto& other:found->second) { if (other.bounds.intersects(bounds) && !separated(other.outline,shape)) { return true; } }
				}
			} }
			return false;
		};
		enum class Placement { Placed, Crowded, Blocked, Uneven, Disconnected, Missing };
		/// @brief 区画の外形を地形・水域・道路・建物・既存の田畑と照合し、成立すれば田または畑として登録する。
		// 候補の田は互いに大きく重なり、同じ地点の地形・川・用途地域を何度も調べる。4m格子で結果を使い回す。
		struct TerrainSample { double height=0; bool cultivable=false; };
		constexpr double kSampleCache=4;
		HashTable<Point,TerrainSample> sampleCache;
		const auto terrainSample=[&](Vec2 raw)
		{
			const Point key{static_cast<int>(Floor(raw.x/kSampleCache)),static_cast<int>(Floor(raw.y/kSampleCache))};
			if (const auto found=sampleCache.find(key);found!=sampleCache.end()) { return found->second; }
			const Vec2 point{(key.x+.5)*kSampleCache,(key.y+.5)*kSampleCache};
			const double height=groundPoint(world,point).y;
			const TerrainSample sample{height,agricultural(world,point) && height>world.waterPlanningHeight(point.x,point.y)+config.agriculture_minimumFreeboard};
			sampleCache.emplace(key,sample);
			return sample;
		};
		const auto placeField=[&](const Array<Vec2>& outline,uint32 salt,double homeDistance)
		{
			Vec2 center{0,0};for (const Vec2 vertex:outline) { center+=vertex; }center/=static_cast<double>(outline.size());
			// 隣の田との間に畦の幅を残す。
			Array<Vec2> polygon;for (const Vec2 vertex:outline) { polygon << center.lerp(vertex,.97); }
			if (overlapsAccepted(polygon,center)) { return Placement::Crowded; }
			// 道路・建物との重なりは地形の標本より軽いので先に調べる。
			bool valid=true;double low=Math::Inf,high=-Math::Inf;
			for (size_t i=1;i+1<polygon.size() && valid;++i)
			{
				const ParcelGeometry::Quad triangle{polygon[0],polygon[i],polygon[i+1],polygon[i+1]};
				valid &= !occupied.overlaps(triangle) && !sites.occupied.overlaps(triangle);
			}
			for (const Vec2 vertex:polygon)
			{
				const int samples=Max(1,static_cast<int>(Ceil(vertex.distanceFrom(center)/config.agriculture_sampleStep)));
				for (int i=0;i<=samples && valid;++i)
				{
					const auto sample=terrainSample(center.lerp(vertex,static_cast<double>(i)/samples));low=Min(low,sample.height);high=Max(high,sample.height);
					valid &= sample.cultivable;
				}
			}
			if (!valid) { return Placement::Blocked; }
			if (high-low>config.agriculture_maximumFieldRelief) { return Placement::Uneven; }
			if (!sites.served(polygon,homeDistance)) { return Placement::Disconnected; }
			const Point coord{static_cast<int>(center.x/CHUNK_SIZE),static_cast<int>(center.y/CHUNK_SIZE)};auto* chunk=world.getChunk(coord);if (!chunk) { return Placement::Missing; }
			accepted[bucket(center)] << AcceptedField{polygon,boundsOf(polygon)};
			LandPatch patch;patch.id=static_cast<int>(chunk->landPatches.size());patch.polygon=std::move(polygon);patch.elevationOffset=config.agriculture_surfaceLift;
			const bool paddy=high-low<config.agriculture_paddyRelief && salt%config.agriculture_dryFieldDivisor!=0;
			patch.type=paddy ? LandPatchType::PaddyField : LandPatchType::FarmField;patch.materialVariant=salt|kManagedField;
			chunk->landPatches << std::move(patch);chunk->markMeshDirty();++stats.fields;stats.paddies+=paddy;
			return Placement::Placed;
		};
		const auto count=[&](Placement result)
		{
			crowded+=result==Placement::Crowded;blocked+=result==Placement::Blocked;uneven+=result==Placement::Uneven;stats.disconnected+=result==Placement::Disconnected;
		};
		for (const auto& candidate:candidates)
		{
			for (const double fraction:{1.0,.7,.45})
			{
				const double depth=candidate.depth*fraction;
				// 奥の畦は区画ごとにわずかに折れ、地形に沿った不整形さを残す。
				const Vec2 farLeft=candidate.left+candidate.normalLeft*depth,farRight=candidate.right+candidate.normalRight*depth;
				const double bend=depth*(static_cast<double>((candidate.salt>>12)%9)-4.0)*.012;
				Array<Vec2> outline{candidate.left,candidate.right,farRight,farRight.lerp(farLeft,.5)+(candidate.normalLeft+candidate.normalRight)*.5*bend,farLeft};
				UrbanParcel::normalize(outline);if (outline.size()<3) { break; }
				double area=0;for (size_t i=0;i<outline.size();++i) { const Vec2 p0=outline[i],p1=outline[(i+1)%outline.size()]; area+=p0.x*p1.y-p1.x*p0.y; }
				if (Abs(area)*.5<config.agriculture_minimumFieldArea) { break; }
				const auto result=placeField(outline,candidate.salt,config.agriculture_homeMaximumDistance);count(result);
				if (result==Placement::Placed || result==Placement::Disconnected || result==Placement::Missing) { break; }
			}
		}
		// 道路沿いの列で埋まらなかった平地を、最寄りの道路と同じ向きの格子に並ぶ短冊で埋める。
		// 区画の格子は道路の起点を基準にするため、隣り合う区画は畦をはさんで揃う。
		struct RoadLine { const Frame* frame=nullptr; double along=0, low=Math::Inf, high=-Math::Inf; int fields=0; };
		HashTable<int64,RoadLine> roadLines;
		HashSet<int> gridRoads;int gridRoadRejected=0;
		int infill=0,gridDitches=0;std::array<int,6> infillGrid{};const Stopwatch infillTimer{StartImmediately::Yes};
		{
			RoadPlanSnapIndex roadIndex;roadIndex.rebuild(roads);
			constexpr double kInfillStep=24;
			constexpr double kInfillSearch=400;
			constexpr double kRoadTile=72;
			HashTable<Point,RoadPlanSnapIndex::Hit> nearestRoad;
			const double plotLength=config.agriculture_plotLength*.8,plotWidth=config.agriculture_plotWidth*.6;
			// 全集落の範囲を1回だけ格子で走査する。集落ごとに重なる範囲を数え直さない。
			const double radius=config.agriculture_maximumRadius;
			double minX=Math::Inf,minZ=Math::Inf,maxX=-Math::Inf,maxZ=-Math::Inf;
			for (const auto& frame:frames)
			{
				minX=Min(minX,frame.center.x-radius);minZ=Min(minZ,frame.center.y-radius);maxX=Max(maxX,frame.center.x+radius);maxZ=Max(maxZ,frame.center.y+radius);
			}
			minX=Max(0.0,Floor(minX/kInfillStep)*kInfillStep);minZ=Max(0.0,Floor(minZ/kInfillStep)*kInfillStep);
			maxX=Min(static_cast<double>(WORLD_SIZE),maxX);maxZ=Min(static_cast<double>(WORLD_SIZE),maxZ);
			{
				for (double z=minZ;z<=maxZ;z+=kInfillStep) { for (double x=minX;x<=maxX;x+=kInfillStep)
				{
					const Vec2 point{x,z};
					const uint32 salt=hash(seed,point)&~kManagedField;
					const Vec2 districtCell{Floor(x/bucketSize)*bucketSize,Floor(z/bucketSize)*bucketSize};
					const double roll=(hash(seed,districtCell)%10000)/10000.0*.72+(salt%10000)/10000.0*.28;
					if (roll>Min(1.0,density(point,frames)*kFullCultivationDensity)) { continue; }
					bool covered=false;
					for (int dz=-1;dz<=1 && !covered;++dz) { for (int dx=-1;dx<=1 && !covered;++dx)
					{
						if (const auto found=accepted.find(bucket(point)+Point{dx,dz});found!=accepted.end())
						{
							for (const auto& field:found->second) { if (field.bounds.contains(point) && inside(field.outline,point)) { covered=true;break; } }
						}
					} }
					if (covered || !agricultural(world,point) || builtUp(world,point) || insidePlannedTown(point,frames)) { continue; }
					// 最寄り道路の探索は重いので、72m四方の区画ごとに1回だけ行い、区画内の田の向きをそろえる。
					const Point tile{static_cast<int>(Floor(x/kRoadTile)),static_cast<int>(Floor(z/kRoadTile))};
					auto cached=nearestRoad.find(tile);
					if (cached==nearestRoad.end())
					{
						const Vec2 tileCenter{(tile.x+.5)*kRoadTile,(tile.y+.5)*kRoadTile};
						cached=nearestRoad.emplace(tile,roadIndex.find(roads,groundPoint(world,tileCenter),kInfillSearch,Math::Inf,false,
							[](const RoadEdge& edge) { return edge.hasRoadLanes() && !edge.useElevation; })).first;
					}
					const auto& hit=cached->second;
					if (!hit.edgeId)
					{
						// 道路から遠い集落近くの平野も、ほ場整備のように格子に並べた田で埋める。格子の向きは最寄りの集落の町割に揃える。
						// 耕作道は土地パッチでは作らない（仕様: 農道は道路網の farmAccess 道路）。田の2列ごとに広めの畦を残す。
						// 整備範囲の縁は集落からの距離で一直線に切らず、256m四方の地区ごとにずらす。
						const double districtRoll=(hash(seed,districtCell)%10000)/10000.0;
						if (density(point,frames)<.35+.3*districtRoll) { continue; }
						const Frame* nearest=nullptr;double nearestDistance=Math::Inf;
						for (const auto& frame:frames) { const double d=frame.center.distanceFromSq(point); if (d<nearestDistance) { nearestDistance=d; nearest=&frame; } }
						if (!nearest) { continue; }
						const Vec2 along=nearest->axisX,across=nearest->axisZ,anchor=nearest->origin;
						// 2列ごとの広い畦の中央に用排水路を通す。
						constexpr double kWideBund=2.4,kDitch=.9;
						const double period=plotWidth*2+kWideBund;
						// 4列ごとに幹線農道の用地を空ける。農道は両端が既存の道路に届く列だけに後で通す。
						constexpr int kColumnsPerRoad=4;constexpr double kRoadGap=7;
						const double superPeriod=plotLength*kColumnsPerRoad+kRoadGap;
						const double a=(point-anchor).dot(along),c=(point-anchor).dot(across);
						const double superBlock=Floor(a/superPeriod),superOffset=a-superBlock*superPeriod;
						if (superOffset<kRoadGap) { continue; }
						const double column=Floor((superOffset-kRoadGap)/plotLength);
						const double block=Floor(c/period),offset=c-block*period;
						if (offset<kWideBund) { continue; }
						const int row=offset<kWideBund+plotWidth ? 0 : 1;
						const Vec2 origin=anchor+along*(superBlock*superPeriod+kRoadGap+column*plotLength)+across*(block*period+kWideBund+row*plotWidth);
						const Array<Vec2> outline{origin,origin+along*plotLength,origin+along*plotLength+across*plotWidth,origin+across*plotWidth};
						// 整備された大区画は通い耕作もあるため、家からの距離は道路沿いの田畑の2.4倍まで許す。
						constexpr double kGridHomeDistanceRatio=2.4;
						const auto result=placeField(outline,salt,config.agriculture_homeMaximumDistance*kGridHomeDistanceRatio);
						++infillGrid[static_cast<int>(result)];
						if (result!=Placement::Placed) { continue; }
						++infill;
						{
							const int64 key=(static_cast<int64>(nearest-frames.data())<<40)^(static_cast<int64>(superBlock)&0xFFFFFFFFFFLL);
							auto& line=roadLines[key];
							line.frame=nearest;line.along=superBlock*superPeriod+kRoadGap*.5;
							line.low=Min(line.low,block*period);line.high=Max(line.high,(block+1)*period);++line.fields;
						}
						if (row==0)
						{
							const Vec2 start=origin-across*((kWideBund+kDitch)*.5);
							const ParcelGeometry::Quad strip{start,start+along*plotLength,start+along*plotLength+across*kDitch,start+across*kDitch};
							if (!occupied.overlaps(strip) && !sites.occupied.overlaps(strip))
							{
								const Vec2 middle=(strip[0]+strip[2])*.5;
								if (auto* chunk=world.getChunk({static_cast<int>(middle.x/CHUNK_SIZE),static_cast<int>(middle.y/CHUNK_SIZE)}))
								{
									LandPatch ditch;ditch.id=static_cast<int>(chunk->landPatches.size());ditch.type=LandPatchType::IrrigationDitch;
									ditch.polygon={strip[0],strip[1],strip[2],strip[3]};ditch.elevationOffset=config.agriculture_surfaceLift*.5f;ditch.materialVariant=salt;
									chunk->landPatches << std::move(ditch);chunk->markMeshDirty();++gridDitches;
								}
							}
						}
						continue;
					}
					const auto* edge=roads.getEdge(*hit.edgeId);const auto curve=roads.getBezier(*hit.edgeId);if (!edge || !curve) { continue; }
					Vec2 along=horizontal(curve->tangentAt(curve->totalLength*hit.curveT));if (along.lengthSq()<1e-6) { continue; }
					along.normalize();const Vec2 across{-along.y,along.x};
					const Vec2 anchor=horizontal(roads.getNode(edge->nodeA)->position);
					const double u=Floor((point-anchor).dot(along)/plotLength)*plotLength,v=Floor((point-anchor).dot(across)/plotWidth)*plotWidth;
					// 家や道に挟まれた土地では、町なかの小さな田のように短辺・長辺とも縮めて収める。
					for (const Vec2 scale:{Vec2{1.0,1.0},Vec2{.6,1.0},Vec2{.35,1.0},Vec2{.35,.6}})
					{
						const double length=plotLength*scale.x,width=plotWidth*scale.y;
						const Vec2 cellCenter=anchor+along*(u+plotLength*.5)+across*(v+plotWidth*.5);
						const Vec2 toward=point-cellCenter;
						const Vec2 origin=cellCenter+along*Clamp(toward.dot(along),-(plotLength-length)*.5,(plotLength-length)*.5)
							+across*Clamp(toward.dot(across),-(plotWidth-width)*.5,(plotWidth-width)*.5)-along*(length*.5)-across*(width*.5);
						const Array<Vec2> outline{origin,origin+along*length,origin+along*length+across*width,origin+across*width};
						const auto result=placeField(outline,salt,config.agriculture_homeMaximumDistance);
						if (result==Placement::Placed) { ++infill;break; }
						// 起伏で収まらない区画も、小さくすれば斜面の段々畑として収まる。
						if (result!=Placement::Crowded && result!=Placement::Blocked && result!=Placement::Uneven) { break; }
					}
				} }
			}
			// 幹線農道：格子の農道用地を両方向へ延ばし、既存の道路に届く列だけを農道にする。行き止まりは作らない。
			{
				constexpr int kMinimumFields=12;constexpr double kStep=8,kReach=1500,kMaximumGrade=.06,kSnap=7;
				for (const auto& [key,line] : roadLines)
				{
					if (line.fields<kMinimumFields) { continue; }
					const Vec2 along=line.frame->axisX,across=line.frame->axisZ;
					const Vec2 base=line.frame->origin+along*line.along;
					// 田の範囲の中央から外へ進み、最初に当たる道路を端点にする。途中の地形・建物・線路で失敗したら諦める。
					const auto reach=[&](double startC,double direction) -> Optional<RoadPlanSnapIndex::Hit>
					{
						double previous=groundPoint(world,base+across*startC).y;
						for (double distance=kStep;distance<=kReach;distance+=kStep)
						{
							const Vec2 point=base+across*(startC+direction*distance);
							const auto hit=roadIndex.find(roads,groundPoint(world,point),kSnap,Math::Inf,false,
								[](const RoadEdge& edge) { return edge.hasRoadLanes() && !edge.useElevation && !edge.tunnel && edge.roadType!=RoadType::Expressway; });
							if (hit.edgeId) { return hit; }
							// 田の外の草地・林も通れる。水域と急勾配だけは避ける。
							const auto sample=terrainSample(point);
							if (sample.height<=world.waterPlanningHeight(point.x,point.y)+config.agriculture_minimumFreeboard
								|| Abs(sample.height-previous)/kStep>kMaximumGrade) { return none; }
							const Vec2 side=along*(config.agriculture_trackWidth*.5+1);
							const ParcelGeometry::Quad ribbon{point-side-across*kStep*.5,point+side-across*kStep*.5,point+side+across*kStep*.5,point-side+across*kStep*.5};
							if (sites.occupied.overlaps(ribbon)) { return none; }
							previous=sample.height;
						}
						return none;
					};
					const double middle=(line.low+line.high)*.5;
					const auto first=reach(middle,-1),second=reach(middle,1);
					if (!first || !second || *first->edgeId==*second->edgeId) { ++gridRoadRejected; continue; }
					const auto attach=[&](const RoadPlanSnapIndex::Hit& hit) { return attachToRoad(world,roads,hit,kSnap); };
					const int a=attach(*first),b=attach(*second);
					if (a<0 || b<0 || a==b) { ++gridRoadRejected; continue; }
					const Vec3 start=roads.getNode(a)->position,end=roads.getNode(b)->position;
					const auto id=roads.addEdge(a,b,start+(end-start)/3,end-(end-start)/3,RoadType::LocalRoad,2);
					if (!id) { ++gridRoadRejected; continue; }
					auto* edge=roads.getEdge(*id);auto profile=GeneratedStreet::describe(GeneratedStreet::Role::FarmAccess);
					profile.laneWidth=static_cast<float>(config.agriculture_trackWidth/profile.lanes);
					GeneratedStreet::apply(*edge,profile);edge->edgeState=EdgeState::Open;edge->farmAccess=true;
					edge->parts.remove_if([](const RoadPart& part) { return part.type==RoadPartType::UtilityPole; });
					for (const int node : {a,b}) { roads.updateNodeCutoffs(node);roads.rebuildLaneConnections(node); }
					gridRoads.insert(*id);++stats.tracks;stats.connections+=2;
					// 農道が横切る田畑は、農道の両側に分けて残す。
					clipFieldsAlong(world,{{horizontal(start),horizontal(end)}},config.agriculture_trackWidth*.5+1);
				}
			}
		}
		if (!gridRoads.empty()) { RoadTerrainFit::apply(roads,world,gridRoads); }
		// 道路に面していない田畑を、畦をたどる畦道で道路網へつなぐ。
		const Stopwatch azemichiTimer{StartImmediately::Yes};
		const auto azemichi=connectFieldsByAzemichi(world,roads,sites,railway);
		stats.azemichi=azemichi.edges;stats.unservedFields=azemichi.unreachable;
		DBG_LOG(U"[Azemichi] fields={} frontage={} connected={} unreachable={} edges={} lengthKm={:.1f} loops={} deadEnds={} bridges={} accesses={} stranded={} ms={:.0f} phasesMs={}"_fmt(
			azemichi.fields,azemichi.frontage,azemichi.connected,azemichi.unreachable,azemichi.edges,azemichi.length/1000,azemichi.loops,azemichi.deadEnds,azemichi.bridges,azemichi.accesses,azemichi.stranded,azemichiTimer.msF(),Array<double>(azemichi.phaseMs.begin(),azemichi.phaseMs.end())));

		DBG_LOG(U"[AgriculturalGrid] placed={} crowded={} blocked={} uneven={} ditches={} roads={} roadRejected={}"_fmt(infillGrid[0],infillGrid[1],infillGrid[2],infillGrid[3],gridDitches,gridRoads.size(),gridRoadRejected));
		DBG_LOG(U"[AgriculturalFields] fields={} infill={} infillMs={:.0f} sparse={} unsuitable={} crowded={} blocked={} uneven={} disconnected={}"_fmt(
			stats.fields,infill,infillTimer.msF(),sparse,unsuitable,crowded,blocked,uneven,stats.disconnected));
		return stats;
	}
}

#include "../gen/GenerationSettings.hpp"
#include "WorldRenderer.hpp"
#include "TerrainSurfaceGeometry.hpp"
#include "LandscapeMaterials.hpp"
#include "TreeGeometry.hpp"
#include "VegetationProfile.hpp"
#include "TransportLandscape.hpp"
#include "FrontageGeometry.hpp"
#include "ResidentialParcelAccess.hpp"
#include "../gen/ParcelGeometry.hpp"
#include "../road/RoadGeometry.hpp"
#include "../world/ZoneGrid.hpp"
#include "../gen/UrbanParcel.hpp"
#include "../asset/AssetRegistrar.hpp"
#include "../debug/DebugLog.hpp"

using namespace TerrainSurfaceGeometry;
using LandscapeMaterials::detailColorForKey;

void WorldRenderer::preloadBuildingModels()
{
	const Stopwatch timer{StartImmediately::Yes};
	TreeInstanceRenderer::preload();
	HashSet<uint32> loaded;
	for (int value = static_cast<int>(BuildingType::Detached); value < static_cast<int>(BuildingType::Count); ++value)
	{
		const auto type = static_cast<BuildingType>(value);
		if (!isObjBuildingType(type)) { continue; }
		for (int sample = 0; sample < 256; ++sample)
		{
			const uint8 variant = buildingModelVariant(type,sample,0);
			const uint32 key = (static_cast<uint32>(type)<<8)|variant;
			if (loaded.insert(key).second) { getBuildingModelAsset(type,variant); }
		}
	}
	DBG_LOG(U"[BuildingPreload] assets={} ms={:.2f}"_fmt(loaded.size(),timer.msF()));
}

namespace
{
	/// @brief 景観の面と植栽を独立して収集し、配置だけのジョブでは面を作らない。
	struct LandscapeGeometry
	{
		HashTable<int, MeshData> meshes;
		Array<TreeInstance> trees;
		bool surfaces = true, planting = true;
		size_t excludedTrees = 0;
		MeshData& operator[](int material) { return meshes[material]; }
	};
	float buildingBaseHeight(const World& world,const Building& building,float x,float z)
	{
		float height=world.sampleHeight(x,z);
		if (building.type==BuildingType::Parking) { return height; }
		const float half=buildingFootprintXZ(building.type)*.5f;
		const Vec2 along{Cos(building.angle)*half,Sin(building.angle)*half},across{-along.y,along.x};
		for (const int side : {-1,1})
		{
			for (const int end : {-1,1})
			{
				const Vec2 point=Vec2{x,z}+along*side+across*end;
				height=Max(height,world.sampleHeight(static_cast<float>(point.x),static_cast<float>(point.y)));
			}
		}
		return height+.04f;
	}

	/// @brief 非 OBJ 建物（Box 描画）の高さスケール。buildingHeight() はメートル基準。
	constexpr float kLegacyBoxHeightScale = 1.0f;
	/// @brief モデル TOML に scale が無い場合の既定値
	constexpr float kDefaultModelScale = 1.0f;

	String buildingAssetSubDir(BuildingType type)
	{
		return isResidentialBuildingType(type) ? U"residential" : U"commercial";
	}

	float parseModelScale(const TOMLReader& toml)
	{
		const double s = toml[U"scale"].getOr<double>(
			toml[U"render_scale"].getOr<double>(kDefaultModelScale));
		return static_cast<float>(Max(0.001, s));
	}

	/// @brief 建物 Box メッシュの頂点を中心 (cx, cz) まわりに角度 angle で Y 軸回転する
	/// @details rebuildBuildingMeshes と drawBuildingSilhouette で同じ変換を適用するための共通処理
	void rotateBoxVerticesY(MeshData& box, float cx, float cz, float angle)
	{
		if (angle == 0.0f) return;
		const float cosA = Math::Cos(angle);
		const float sinA = Math::Sin(angle);
		for (auto& v : box.vertices)
		{
			const float dx = v.pos.x - cx;
			const float dz = v.pos.z - cz;
			v.pos.x = cx + dx * cosA - dz * sinA;
			v.pos.z = cz + dx * sinA + dz * cosA;
			const float nx = v.normal.x;
			const float nz = v.normal.z;
			v.normal.x = nx * cosA - nz * sinA;
			v.normal.z = nx * sinA + nz * cosA;
		}
	}

	void appendMeshData(MeshData& dst, const MeshData& src)
	{
		const uint32 offset = static_cast<uint32>(dst.vertices.size());
		dst.vertices.append(src.vertices);
		for (const auto& tri : src.indices)
		{
			dst.indices << TriangleIndex32{ tri.i0 + offset, tri.i1 + offset, tri.i2 + offset };
		}
	}

	uint32 cellVisualHash(Point chunkCoord, int col, int row, uint32 salt)
	{
		uint32 value = static_cast<uint32>(chunkCoord.x * 73856093)
			^ static_cast<uint32>(chunkCoord.y * 19349663)
			^ static_cast<uint32>(col * 83492791)
			^ static_cast<uint32>(row * 2654435761u)
			^ salt;
		value ^= value >> 16;
		value *= 0x7FEB352Du;
		value ^= value >> 15;
		value *= 0x846CA68Bu;
		value ^= value >> 16;
		return value;
	}

	void appendRotatedBox(MeshData& dst, float cx, float cy, float cz, float sx, float sy, float sz, float angle)
	{
		MeshData box = MeshData::Box(Float3{ cx, cy, cz }, Float3{ sx, sy, sz });
		rotateBoxVerticesY(box, cx, cz, angle);
		appendMeshData(dst, box);
	}

	int landPatchMaterialKey(LandPatchType type, [[maybe_unused]] uint64 seed)
	{
		switch (type)
		{
		case LandPatchType::ParcelAsphalt: return 119;
		case LandPatchType::ParcelGravel:  return 120;
		case LandPatchType::FarmTrack: return 120;
		case LandPatchType::IrrigationDitch: return 135;
		case LandPatchType::GardenSoil:    return 122;
		case LandPatchType::Beach:        return 111;
		case LandPatchType::PaddyField:   return 131;
		case LandPatchType::FarmField:    return 130;
		case LandPatchType::Seawall:      return 117;
		default:                    return 100;
		}
	}

	struct LandRoadMask
	{
		RectF bounds;
		Array<Vec2> footprint;
		Polygon shape;
		float roadHeight=0;
		bool facility=false;
	};
	template<class HeightSource>
	void appendPublicGreen(LandscapeGeometry& groups, const HeightSource& world,
		const LandPatch& patch, const Array<LandRoadMask>& roadMasks);
	template<class HeightSource>
	void appendParcelLandscape(LandscapeGeometry& groups, const HeightSource& world,
		const Chunk& chunk, const LandPatch& patch, const Array<LandRoadMask>& roadMasks);
	template<class HeightSource>
	void appendLandPatchSurface(MeshData& dst, const HeightSource& world, const LandPatch& patch,
		const Array<LandRoadMask>& roadMasks)
	{
		if (patch.polygon.size() < 3)
		{
			return;
		}
		const RectF bounds = boundsOfPolygon(patch.polygon);
		Array<const LandRoadMask*> nearbyRoads;
		for (const auto& mask : roadMasks)
		{
			if (rectIntersects(bounds, mask.bounds))
			{
				nearbyRoads << &mask;
			}
		}
		const int materialKey = landPatchMaterialKey(patch.type, patch.materialVariant);
		// Align cultivation rows with the longest plot edge, not with world axes.
		Vec2 rowDirection{1,0};double longest=0;
		for (size_t i=0;i<patch.polygon.size();++i)
		{
			const Vec2 edge=patch.polygon[(i+1)%patch.polygon.size()]-patch.polygon[i];
			if (edge.lengthSq()>longest) { longest=edge.lengthSq();rowDirection=edge.normalized(); }
		}
		const Vec2 rowNormal{-rowDirection.y,rowDirection.x};
		Array<Vec2> outline = patch.polygon;
		UrbanParcel::normalize(outline);
		const Polygon shape{ outline };
		const int minCol = static_cast<int>(Floor(bounds.x / kTerrainCellSize));
		const int maxCol = static_cast<int>(Floor((bounds.x + bounds.w) / kTerrainCellSize));
		const int minRow = static_cast<int>(Floor(bounds.y / kTerrainCellSize));
		const int maxRow = static_cast<int>(Floor((bounds.y + bounds.h) / kTerrainCellSize));
		// Triangulate the parcel first so concave parcel outlines remain valid when clipped.
		for (const auto& triangle : shape.indices())
		{
			const auto& vertices = shape.vertices();
			Array<TerrainClipVertex> source;
			for (const auto index : { triangle.i0, triangle.i1, triangle.i2 })
			{
				const auto& vertex = vertices[index];
				source << TerrainClipVertex{ Vec3{ vertex.x, 0.0, vertex.y }, Float2{ 0, 0 } };
			}
			for (int row = minRow; row <= maxRow; ++row)
			{
				for (int col = minCol; col <= maxCol; ++col)
				{
					const Vec2 a{ col * kTerrainCellSize, row * kTerrainCellSize };
					const Vec2 b = a + Vec2{ kTerrainCellSize, 0 };
					const Vec2 c = a + Vec2{ 0, kTerrainCellSize };
					const Vec2 d = a + Vec2{ kTerrainCellSize, kTerrainCellSize };
					for (const Array<Vec2>& clip : { Array<Vec2>{ a, b, c }, Array<Vec2>{ b, d, c } })
					{
						auto piece = source;
						for (size_t side = 0; side < clip.size() && piece.size() >= 3; ++side)
						{
							piece = clipPolygonByHalfPlaneXZ(piece, clip[side], clip[(side + 1) % clip.size()], true);
						}
						for (auto& vertex : piece)
						{
							vertex.pos.y = world.sampleHeight(static_cast<float>(vertex.pos.x), static_cast<float>(vertex.pos.z)) + patch.elevationOffset;
							vertex.tex = terrainUvAt(vertex.pos, materialKey);
							if (materialKey==130 || materialKey==131)
							{
								const Vec2 local=Vec2{vertex.pos.x,vertex.pos.z}-patch.polygon.front();
								vertex.tex=Float2{static_cast<float>(local.dot(rowNormal)),static_cast<float>(local.dot(rowDirection))};
							}
						}
						Array<Array<TerrainClipVertex>> pieces{ piece };
						const RectF cellBounds{ a.x, a.y, kTerrainCellSize, kTerrainCellSize };
						for (const auto* road : nearbyRoads)
						{
							if (!rectIntersects(cellBounds, road->bounds))
							{
								continue;
							}
							Array<Array<TerrainClipVertex>> remainder;
							for (const auto& candidate : pieces)
							{
								for (auto& fragment : subtractConvexPolygonXZ(candidate, road->footprint))
								{
									remainder << std::move(fragment);
								}
							}
							pieces = std::move(remainder);
						}
						for (const auto& fragment : pieces)
						{
							appendPolygonAsTriangles(fragment, dst.vertices, dst.indices);
						}
					}
				}
			}
		}
	}

	template<class HeightSource>
	void appendLandPatchMesh(LandscapeGeometry& groups, const HeightSource& world, const Chunk& chunk, const LandPatch& patch,
		const Array<LandRoadMask>& roadMasks)
	{
		const bool drawSurface = (patch.type == LandPatchType::FarmField
			|| patch.type == LandPatchType::PaddyField
			|| patch.type == LandPatchType::FarmTrack || patch.type == LandPatchType::IrrigationDitch
			|| patch.type == LandPatchType::Seawall || patch.type == LandPatchType::ParcelAsphalt
			|| patch.type == LandPatchType::ParcelGravel || patch.type == LandPatchType::GardenSoil);
		if (groups.surfaces && drawSurface)
		{
			MeshData& surface = groups[landPatchMaterialKey(patch.type, patch.materialVariant)];
			appendLandPatchSurface(surface, world, patch, roadMasks);
		}
		const RectF bounds = boundsOfPolygon(patch.polygon);
		const float cx = static_cast<float>(bounds.x + bounds.w * 0.5);
		const float cz = static_cast<float>(bounds.y + bounds.h * 0.5);
		const float baseY = static_cast<float>(world.sampleHeight(cx, cz)) + patch.elevationOffset;

		if (groups.planting && patch.type==LandPatchType::GardenSoil && patch.sourceParcelKey<0 && patch.polygon.size()>=3)
		{
			appendPublicGreen(groups,world,patch,roadMasks);
		}
		if (patch.sourceParcelKey >= 0 && patch.polygon.size() >= 3
			&& (groups.surfaces || patch.type == LandPatchType::GardenSoil || patch.type == LandPatchType::ParcelGravel))
		{
			appendParcelLandscape(groups, world, chunk, patch, roadMasks);
		}

		if (!groups.surfaces) { return; }

		if (patch.type == LandPatchType::FarmField || patch.type == LandPatchType::PaddyField)
		{
			// Leave a four-metre entrance in the bank facing the cultivation track.
			Array<Line> banks;
			for (size_t side=0;side<patch.polygon.size();++side)
			{
				const Vec2 a=patch.polygon[side],b=patch.polygon[(side+1)%patch.polygon.size()];
				if ((patch.materialVariant & 0x80000000u)!=0 && Abs(a.x-b.x)<.01 && Abs(a.x-bounds.x)<.01)
				{
					const Vec2 direction=(b-a).normalized(),middle=(a+b)*.5;
					banks << Line{a,middle-direction*2.0} << Line{middle+direction*2.0,b};
				}
				else { banks << Line{a,b}; }
			}
			for (const auto& bankLine : banks)
			{
				const Vec2 a=bankLine.begin,b=bankLine.end;
				const double length = a.distanceFrom(b);
				const int pieces = Max(1,static_cast<int>(Ceil(length/8)));
				for (int piece = 0; piece < pieces; ++piece)
				{
					const Vec2 point = a.lerp(b,(piece+.5)/pieces);
					const bool touchesRoad = std::any_of(roadMasks.begin(),roadMasks.end(),[&](const LandRoadMask& road)
					{ return road.bounds.stretched(5).contains(point) && Circle{point,length/pieces*.5+.4}.intersects(road.shape); });
					if (touchesRoad) { continue; }
					const Vec2 start=a.lerp(b,static_cast<double>(piece)/pieces),end=a.lerp(b,static_cast<double>(piece+1)/pieces);
					const Vec2 normal=Vec2{-(b-a).y,(b-a).x}.normalized();
					MeshData bank;
					for (const Vec2 p : {start,end}) for (const auto cross : {Vec2{-.48,.01},Vec2{-.20,.20},Vec2{.20,.20},Vec2{.48,.01}})
					{
						const Vec2 at=p+normal*cross.x;
						bank.vertices << Vertex3D{Float3{static_cast<float>(at.x),world.sampleHeight(static_cast<float>(at.x),static_cast<float>(at.y))+static_cast<float>(cross.y),static_cast<float>(at.y)},Float3{0,1,0},Float2{at*.5}};
					}
					for (uint32 face=0;face<3;++face) { bank.indices << TriangleIndex32{face,face+1,face+4} << TriangleIndex32{face+1,face+5,face+4}; }
					bank.computeNormals();appendMeshData(groups[127],bank);
				}
			}
		}
		if (patch.type == LandPatchType::IrrigationDitch)
		{
			// Concrete lips and a dark water bed are distinct from the earth bund.
			for (const double x : {bounds.x,bounds.x+bounds.w-.12})
			{
				LandPatch lip=patch;
				lip.polygon={Vec2{x,bounds.y},Vec2{x+.12,bounds.y},Vec2{x+.12,bounds.y+bounds.h},Vec2{x,bounds.y+bounds.h}};
				lip.elevationOffset=.22f;
				appendLandPatchSurface(groups[117],world,lip,roadMasks);
			}
		}
		if (patch.type == LandPatchType::Seawall)
		{
			appendRotatedBox(groups[117], cx, baseY + 0.70f, cz,
				static_cast<float>(Max(2.4, bounds.w * 1.02)), 1.40f,
				static_cast<float>(Max(1.8, bounds.h * 0.52)), 0.0f);
		}
	}

	void appendGableRoof(MeshData& dst, float cx, float baseY, float cz,
	                     float sx, float depth, float roofH, float angle, bool ridgeAlongX)
	{
		const float hx = sx * 0.5f;
		const float hz = depth * 0.5f;
		const float ridgeX = ridgeAlongX ? hx : 0.0f;
		const float ridgeZ = ridgeAlongX ? 0.0f : hz;
		Array<Float3> local;
		if (ridgeAlongX)
		{
			local = {
				Float3{ -hx, baseY, -hz }, Float3{ hx, baseY, -hz }, Float3{ hx, baseY, hz }, Float3{ -hx, baseY, hz },
				Float3{ -ridgeX, baseY + roofH, 0.0f }, Float3{ ridgeX, baseY + roofH, 0.0f }
			};
		}
		else
		{
			local = {
				Float3{ -hx, baseY, -hz }, Float3{ hx, baseY, -hz }, Float3{ hx, baseY, hz }, Float3{ -hx, baseY, hz },
				Float3{ 0.0f, baseY + roofH, -ridgeZ }, Float3{ 0.0f, baseY + roofH, ridgeZ }
			};
		}

		MeshData md;
		const float cosA = Math::Cos(angle);
		const float sinA = Math::Sin(angle);
		for (const Float3& p : local)
		{
			const float x = ridgeAlongX ? p.x : p.x;
			const float z = p.z;
			Vertex3D v;
			v.pos = Float3{ cx + x * cosA - z * sinA, p.y, cz + x * sinA + z * cosA };
			v.normal = Float3{ 0.0f, 1.0f, 0.0f };
			v.tex = Float2{ 0.0f, 0.0f };
			md.vertices << v;
		}
		md.indices << TriangleIndex32{ 0, 1, 4 };
		md.indices << TriangleIndex32{ 1, 5, 4 };
		md.indices << TriangleIndex32{ 1, 2, 5 };
		md.indices << TriangleIndex32{ 2, 3, 5 };
		md.indices << TriangleIndex32{ 3, 4, 5 };
		md.indices << TriangleIndex32{ 3, 0, 4 };
		appendMeshData(dst, md);
	}

	/// @brief One compact urban lot carries two visibly separate narrow homes.
	void appendUrbanHousePair(LandscapeGeometry& groups, const World& world,
		const Building& building, float cx, float cz, uint32 hash)
	{
		const float footprint=buildingFootprintXZ(building.type);
		const float gap=FrontageGeometry::kPairedGap;
		const float houseWidth=(footprint-gap)*0.5f;
		const float baseY=buildingBaseHeight(world,building,cx,cz);
		const float cosA=Math::Cos(building.angle), sinA=Math::Sin(building.angle);
		auto worldOffset=[&](float lx, float lz)
		{
			return Vec2{ cx+lx*cosA-lz*sinA, cz+lx*sinA+lz*cosA };
		};
		// 狭小住宅の外壁はサイディングの色幅を持たせ、隣り合う2戸を同色にしない。
		constexpr int kSidingKeys[]={ 140, 141, 142, 143, 144, 145 };
		constexpr int kRoofKeys[]={ 146, 108, 103, 147, 148 };
		const uint32 firstSiding=hash%6u;
		// 道路側に駐車・玄関アプローチのコンクリート土間を残す。
		const float setback=FrontageGeometry::kPairedApronDepth;
		const Vec2 apron=worldOffset(0.0f,-footprint*0.5f+setback*0.5f);
		appendRotatedBox(groups[100],static_cast<float>(apron.x),baseY+FrontageGeometry::kPairedApronTop-FrontageGeometry::kPairedApronThickness*.5f,static_cast<float>(apron.y),
			footprint-0.2f,FrontageGeometry::kPairedApronThickness,setback,building.angle);
		for (int side=0;side<2;++side)
		{
			const uint32 local=hash>>(side*7+3);
			const float lx=(side==0 ? -1.0f : 1.0f)*(houseWidth+gap)*0.5f;
			const int floors=((local>>2)%3u)==0u ? 2 : 3;
			const float height=static_cast<float>(floors)*2.75f+0.35f;
			const float depth=footprint-setback-(side==0 ? 0.3f : 0.0f);
			const float lz=setback*0.5f-(side==0 ? 0.0f : 0.15f);
			const Vec2 house=worldOffset(lx,lz);
			const int wallKey=kSidingKeys[(firstSiding+static_cast<uint32>(side)*(1u+(local&3u)))%6u];
			const int roofKey=kRoofKeys[(local>>4)%std::size(kRoofKeys)];
			appendRotatedBox(groups[wallKey],static_cast<float>(house.x),baseY+height*0.5f,static_cast<float>(house.y),
				houseWidth,height,depth,building.angle);
			if (((local>>6)&3u)==0u)
			{
				// 3階建てに多い陸屋根とパラペット。
				appendRotatedBox(groups[117],static_cast<float>(house.x),baseY+height+0.25f,static_cast<float>(house.y),
					houseWidth+0.06f,0.50f,depth+0.06f,building.angle);
			}
			else
			{
				appendGableRoof(groups[roofKey],static_cast<float>(house.x),baseY+height,static_cast<float>(house.y),
					houseWidth+0.36f,depth+0.36f,0.95f,building.angle,false);
			}
			const float frontZ=lz-depth*0.5f-0.05f;
			const float sideX=lx+(side==0 ? -1.0f : 1.0f)*(houseWidth*0.5f+0.05f);
			const float doorX=lx+(side==0 ? 1.0f : -1.0f)*houseWidth*0.26f;
			const Vec2 door=worldOffset(doorX,frontZ);
			appendRotatedBox(groups[105],static_cast<float>(door.x),baseY+1.05f,static_cast<float>(door.y),0.90f,2.10f,0.08f,building.angle);
			const Vec2 canopy=worldOffset(doorX,frontZ-0.40f);
			appendRotatedBox(groups[115],static_cast<float>(canopy.x),baseY+2.40f,static_cast<float>(canopy.y),1.40f,0.10f,0.80f,building.angle);
			for (int floor=0;floor<floors;++floor)
			{
				const float y=baseY+1.55f+static_cast<float>(floor)*2.75f;
				const float windowWidth=floor==0 ? houseWidth*0.34f : houseWidth*0.62f;
				const float windowX=floor==0 ? lx-(side==0 ? 1.0f : -1.0f)*houseWidth*0.18f : lx;
				const Vec2 window=worldOffset(windowX,frontZ);
				appendRotatedBox(groups[116],static_cast<float>(window.x),y,static_cast<float>(window.y),
					windowWidth,floor==0 ? 1.10f : 1.30f,0.08f,building.angle);
				// 妻側の小窓で長い側壁を分節する。
				for (int k=0;k<2;++k)
				{
					const Vec2 sideWindow=worldOffset(sideX,lz+(k==0 ? -1.0f : 1.0f)*depth*0.22f);
					appendRotatedBox(groups[116],static_cast<float>(sideWindow.x),y+0.15f,static_cast<float>(sideWindow.y),
						0.08f,0.85f,0.60f,building.angle);
				}
				if (floor==1)
				{
					const Vec2 balcony=worldOffset(lx,frontZ-0.45f);
					appendRotatedBox(groups[117],static_cast<float>(balcony.x),y-0.85f,static_cast<float>(balcony.y),
						houseWidth*0.86f,0.14f,0.90f,building.angle);
					const Vec2 rail=worldOffset(lx,frontZ-0.88f);
					appendRotatedBox(groups[102],static_cast<float>(rail.x),y-0.35f,static_cast<float>(rail.y),
						houseWidth*0.86f,0.90f,0.06f,building.angle);
				}
			}
			const Vec2 unit=worldOffset(sideX+(side==0 ? -0.22f : 0.22f),lz+depth*0.30f);
			appendRotatedBox(groups[117],static_cast<float>(unit.x),baseY+0.35f,static_cast<float>(unit.y),
				0.34f,0.60f,0.80f,building.angle);
		}
	}

	/// @brief 戸建ての側方にカーポートか物置を置き、隣家との間の空地を生活の設備で埋める。
	void appendHouseSideStructure(LandscapeGeometry& groups, const World& world, const Building& building,
		Point chunkCoord, int col, int row, float cx, float cz, uint32 hash, const Array<LandRoadMask>& roadMasks)
	{
		const uint32 kind = hash % 10u;
		if (kind >= 7u) { return; }
		const float half = buildingFootprintXZ(building.type) * 0.5f;
		const float cosA = Math::Cos(building.angle), sinA = Math::Sin(building.angle);
		auto worldOffset = [&](float lx, float lz) { return Vec2{ cx + lx * cosA - lz * sinA, cz + lx * sinA + lz * cosA }; };
		const float side = ((hash >> 5) & 1u) ? 1.0f : -1.0f;
		const bool carport = kind < 5u;
		const float width = carport ? 2.8f : 1.8f;
		const float depth = carport ? 5.0f : 1.4f;
		const float lx = side * (half + 0.25f + width * 0.5f);
		const float lz = carport ? -half + depth * 0.5f : half - depth * 0.5f;
		const Vec2 center = worldOffset(lx, lz);
		const double reach = Max(width, depth) * 0.5;
		for (const auto& road : roadMasks)
		{
			if (!TreeGeometry::clearOfCorridor(center, reach + 0.5, road.shape)) { return; }
		}
		constexpr float cellSize = static_cast<float>(CHUNK_SIZE) / ZONE_CELLS;
		const int globalCol = chunkCoord.x * ZONE_CELLS + col, globalRow = chunkCoord.y * ZONE_CELLS + row;
		for (int dz = -2; dz <= 2; ++dz)
		{
			for (int dx = -2; dx <= 2; ++dx)
			{
				const int x = globalCol + dx, z = globalRow + dz;
				if ((dx == 0 && dz == 0) || x < 0 || z < 0) { continue; }
				const Chunk* other = world.getChunk(Point{ x / ZONE_CELLS, z / ZONE_CELLS });
				if (!other) { continue; }
				const Building& neighbor = other->buildingGrid[{ x % ZONE_CELLS, z % ZONE_CELLS }];
				if (neighbor.type == BuildingType::None || neighbor.type == BuildingType::Farmland) { continue; }
				const Vec2 neighborCenter{ (x + 0.5) * cellSize + neighbor.offsetX, (z + 0.5) * cellSize + neighbor.offsetZ };
				if (center.distanceFrom(neighborCenter) < buildingFootprintXZ(neighbor.type) * 0.72 + reach + 0.3) { return; }
			}
		}
		const float baseY = world.sampleHeight(static_cast<float>(center.x), static_cast<float>(center.y));
		if (carport)
		{
			appendRotatedBox(groups[100], static_cast<float>(center.x), baseY + 0.03f, static_cast<float>(center.y), width, 0.04f, depth, building.angle);
			appendRotatedBox(groups[(hash >> 7) & 1u ? 108 : 117], static_cast<float>(center.x), baseY + 2.35f, static_cast<float>(center.y),
				width + 0.2f, 0.08f, depth + 0.2f, building.angle);
			for (const float px : { -1.0f, 1.0f })
			{
				for (const float pz : { -1.0f, 1.0f })
				{
					const Vec2 post = worldOffset(lx + px * (width * 0.5f - 0.1f), lz + pz * (depth * 0.5f - 0.1f));
					appendRotatedBox(groups[115], static_cast<float>(post.x), baseY + 1.17f, static_cast<float>(post.y), 0.08f, 2.34f, 0.08f, building.angle);
				}
			}
			if (((hash >> 9) % 3u) != 0u)
			{
				const Vec2 car = worldOffset(lx, lz - 0.2f);
				appendRotatedBox(groups[((hash >> 11) & 1u) ? 107 : 108], static_cast<float>(car.x), baseY + 0.72f, static_cast<float>(car.y),
					1.65f, 1.30f, 4.10f, building.angle);
			}
		}
		else
		{
			appendRotatedBox(groups[(hash >> 7) & 1u ? 102 : 141], static_cast<float>(center.x), baseY + 1.0f, static_cast<float>(center.y),
				width, 2.0f, depth, building.angle);
			appendRotatedBox(groups[108], static_cast<float>(center.x), baseY + 2.05f, static_cast<float>(center.y),
				width + 0.1f, 0.1f, depth + 0.1f, building.angle);
		}
	}

	/// @brief 工業団地の区画を埋める低層の大型倉庫。
	void appendIndustrialWarehouse(LandscapeGeometry& groups, const World& world,
		const Building& building, float cx, float cz)
	{
		const float baseY=buildingBaseHeight(world,building,cx,cz);
		const float angle=building.angle;
		const float cosA=Math::Cos(angle), sinA=Math::Sin(angle);
		const auto worldOffset=[&](float x,float z)
		{
			return Vec2{cx+x*cosA-z*sinA,cz+x*sinA+z*cosA};
		};
		appendRotatedBox(groups[static_cast<int>(BuildingType::IndustrialWarehouse)],
			cx,baseY+4.25f,cz,27.4f,8.5f,27.4f,angle);
		appendRotatedBox(groups[107],
			cx,baseY+8.65f,cz,27.8f,0.3f,27.8f,angle);
		for (int bay=-1;bay<=1;++bay)
		{
			const Vec2 door=worldOffset(bay*8.0f,-13.75f);
			appendRotatedBox(groups[116],static_cast<float>(door.x),baseY+2.35f,
				static_cast<float>(door.y),5.2f,4.7f,0.12f,angle);
		}
		for (int vent=-1;vent<=1;++vent)
		{
			const Vec2 position=worldOffset(vent*7.5f,2.0f);
			appendRotatedBox(groups[117],static_cast<float>(position.x),baseY+9.05f,
				static_cast<float>(position.y),3.0f,0.6f,2.4f,angle);
		}
	}

	float targetBuildingModelFootprint(BuildingType type)
	{
		return buildingFootprintXZ(type);
	}

	float targetBuildingModelHeight(BuildingType type)
	{
		// 2階建て住宅の階高と屋根を保つ。シミュレーション上の収容人数は変更しない。
		constexpr float kDetachedModelHeightLimit = 8.5f;
		if (type == BuildingType::Detached)
		{
			return kDetachedModelHeightLimit;
		}
		if (type == BuildingType::PublicFacility)
		{
			constexpr float kCivicModelHeightLimit = 15.0f;
			return kCivicModelHeightLimit;
		}
		if (type == BuildingType::Parking)
		{
			constexpr float kParkingModelHeightLimit = 4.5f;
			return kParkingModelHeightLimit;
		}
		if (type == BuildingType::Office) { return 64.0f; }
		if (type == BuildingType::Shop) { return 36.0f; }
		return Max(1.0f, buildingHeight(type));
	}

	float normalizedObjScale(BuildingType type, const Box& localBounds, float assetScale)
	{
		float scale = assetScale;
		const float localFootprint = static_cast<float>(Max(localBounds.size.x, localBounds.size.z));
		if (localFootprint > 0.001f)
		{
			const float currentFootprint = localFootprint * scale;
			const float targetFootprint = targetBuildingModelFootprint(type);
			// 住宅も敷地幅まで拡大し、隣家との間が広すぎる疎な街並みにしない。
			if (currentFootprint > targetFootprint || type == BuildingType::Office || type == BuildingType::Shop
				|| type == BuildingType::MidApartment || type == BuildingType::HighApartment
				|| type == BuildingType::Detached || type == BuildingType::LowApartment)
			{
				scale *= targetFootprint / currentFootprint;
			}
		}

		const float localHeight = static_cast<float>(localBounds.size.y);
		if (localHeight > 0.001f)
		{
			const float currentHeight = localHeight * scale;
			const float targetHeight = targetBuildingModelHeight(type);
			if (currentHeight > targetHeight)
			{
				scale *= targetHeight / currentHeight;
			}
		}
		return scale;
	}
	float boxBuildingFootprintScale(BuildingType type, int gx, int gz)
	{
		const uint32 hash = static_cast<uint32>(gx) * 73856093u ^ static_cast<uint32>(gz) * 19349663u;
		const float variation = 0.86f + static_cast<float>(hash % 29u) * (0.24f / 28.0f);
		switch (type)
		{
		case BuildingType::Factory:        return 1.28f * variation;
		case BuildingType::PublicFacility: return 1.16f * variation;
		case BuildingType::Parking:        return 1.05f * variation;
		case BuildingType::ParkBuilding:   return 0.92f * variation;
		default:                           return variation;
		}
	}

	/// @brief 原型を参照する配置情報だけを保存する。距離で再生成しない。
	void appendLandscapeTree(LandscapeGeometry& groups, Vec2 position, float ground, uint32 variation,
		double width, double height, bool cedar, bool woodland = false)
	{
		const float angle = static_cast<float>((variation % 97) * .065);
		groups.trees << TreeInstance{{Float4{static_cast<float>(position.x), ground, static_cast<float>(position.y), static_cast<float>(width)},
			Float4{static_cast<float>(height), Cos(angle), Sin(angle), 0}},
			static_cast<uint8>((variation % 4) + (cedar ? 4 : 0) + (woodland ? 8 : 0)), static_cast<uint8>(variation & 1u),
			static_cast<uint8>(TreeGeometry::materialKey(0, position) / 1000 - 1)};
	}
	void appendParkTree(LandscapeGeometry& groups, Vec2 position, float groundY, uint32 variation, double heightScale = 1.0)
	{
		const double scale=GenerationSettings::get().vegetation_parkScaleMinimum+(variation%11)*GenerationSettings::get().vegetation_parkScaleVariationStep;
		appendLandscapeTree(groups,position,groundY,variation,4.8*scale,4.8*heightScale*scale,false);
	}
	/// @brief 住区公園・緑道に植樹する。歩道との離隔を確保し、既存の樹木 LOD を共有する。
	template<class HeightSource>
	void appendPublicGreen(LandscapeGeometry& groups, const HeightSource& world,
		const LandPatch& patch, const Array<LandRoadMask>& roadMasks)
	{
		const Polygon shape{patch.polygon};
		const RectF bounds=shape.boundingRect();
		const double step=GenerationSettings::get().urbanFabric_newTownParkTreeSpacing;
		const double clearance=GenerationSettings::get().urbanFabric_newTownParkTreeClearance;
		for (double z=Ceil(bounds.y/step)*step;z<bounds.y+bounds.h;z+=step)
		{
			for (double x=Ceil(bounds.x/step)*step;x<bounds.x+bounds.w;x+=step)
			{
				const Vec2 point{x,z};
				if (!shape.contains(point)) { continue; }
				bool clear=true;
				for (size_t i=0;i<patch.polygon.size() && clear;++i)
				{
					const Vec2 a=patch.polygon[i],span=patch.polygon[(i+1)%patch.polygon.size()]-a;
					const double t=Clamp((point-a).dot(span)/Max(1.0,span.lengthSq()),0.0,1.0);
					clear=point.distanceFrom(a+span*t)>clearance;
				}
				for (const auto& road:roadMasks)
				{
					if (clear && Circle{point,clearance}.intersects(road.shape)) { clear=false; }
				}
				if (!clear) { continue; }
				const uint32 hash=static_cast<uint32>(x/step)*73856093u ^ static_cast<uint32>(z/step)*19349663u;
				appendParkTree(groups,point,world.sampleHeight(static_cast<float>(x),static_cast<float>(z)),hash,1);
			}
		}
	}
	/// @brief Deterministic woodland batches follow undeveloped slopes, excluding roads and plots.
	template<class HeightSource>
	void appendWoodland(LandscapeGeometry& groups, const Chunk& chunk,const HeightSource& world,
		const Array<LandRoadMask>& roadMasks,const RiverNetwork& rivers)
	{
		if (chunk.zoneMap.isEmpty() || chunk.heightMap.isEmpty()) { return; }
		Grid<bool> blocked(ZONE_CELLS,ZONE_CELLS,false);
		for (int row = 0; row < ZONE_CELLS; ++row) for (int col = 0; col < ZONE_CELLS; ++col)
		{
			if (chunk.zoneMap[{col,row}] == ZoneType::Unzoned && chunk.buildingGrid[{col,row}].type == BuildingType::None) { continue; }
			for (int dz = -1; dz <= 1; ++dz) for (int dx = -1; dx <= 1; ++dx)
			{
				if (InRange(col+dx,0,ZONE_CELLS-1) && InRange(row+dz,0,ZONE_CELLS-1)) { blocked[{col+dx,row+dz}] = true; }
			}
		}
		Grid<Array<size_t>> nearbyRoads(ZONE_CELLS,ZONE_CELLS);
		for (size_t roadIndex=0;roadIndex<roadMasks.size();++roadIndex)
		{
			const auto& road=roadMasks[roadIndex];
			const RectF bounds=road.bounds.stretched(32);
			const int left=Clamp(static_cast<int>(Floor((bounds.x-chunk.coord.x*CHUNK_SIZE)/16)),0,ZONE_CELLS-1);
			const int right=Clamp(static_cast<int>(Floor((bounds.x+bounds.w-chunk.coord.x*CHUNK_SIZE)/16)),0,ZONE_CELLS-1);
			const int top=Clamp(static_cast<int>(Floor((bounds.y-chunk.coord.y*CHUNK_SIZE)/16)),0,ZONE_CELLS-1);
			const int bottom=Clamp(static_cast<int>(Floor((bounds.y+bounds.h-chunk.coord.y*CHUNK_SIZE)/16)),0,ZONE_CELLS-1);
			for (int row=top;row<=bottom;++row) for (int col=left;col<=right;++col) { nearbyRoads[{col,row}]<<roadIndex; }
		}
		const VegetationProfile::BoundaryField boundary{{chunk.coord.x*CHUNK_SIZE,chunk.coord.y*CHUNK_SIZE,CHUNK_SIZE,CHUNK_SIZE},[&](Vec2 point)
		{
			const auto* source=world.getChunk({static_cast<int>(Floor(point.x/CHUNK_SIZE)),static_cast<int>(Floor(point.y/CHUNK_SIZE))});
			if(!source) { return false; }
			const int col=Clamp(static_cast<int>((point.x-source->coord.x*CHUNK_SIZE)/16),0,ZONE_CELLS-1),row=Clamp(static_cast<int>((point.y-source->coord.y*CHUNK_SIZE)/16),0,ZONE_CELLS-1);
			if(!source->zoneMap.isEmpty() && source->zoneMap[{col,row}]!=ZoneType::Unzoned) { return false; }
			const float x=static_cast<float>(point.x),z=static_cast<float>(point.y);
			const double slope=Max(Abs(world.sampleHeight(x+8,z)-world.sampleHeight(x-8,z)),Abs(world.sampleHeight(x,z+8)-world.sampleHeight(x,z-8)))/16;
			return VegetationProfile::habitat(world.sampleHeight(x,z),slope);
		}};
		int excludedByFacilities=0;
		for (int row=0;row<ZONE_CELLS;++row) for (int col=0;col<ZONE_CELLS;++col)
		{
			if (blocked[{col,row}]) { continue; }
			const uint32 candidateCount=nearbyRoads[{col,row}].isEmpty() ? GenerationSettings::get().vegetation_interiorCandidates : GenerationSettings::get().vegetation_roadsideCandidates;
			for (uint32 candidate=0;candidate<candidateCount;++candidate)
			{
				const uint32 hash=cellVisualHash(chunk.coord,col,row,179u+candidate*31u);
				const float x=chunk.coord.x*CHUNK_SIZE+(col+.5f)*16+static_cast<float>((hash>>8)%141u)*.1f-7.0f;
				const float z=chunk.coord.y*CHUNK_SIZE+(row+.5f)*16+static_cast<float>((hash>>16)%141u)*.1f-7.0f;
				const Vec2 position{x,z};const float ground=world.sampleHeight(x,z);
				if (!rivers.vegetationAllowed(position,ground,7)) { continue; }
				const double slope=Max(Abs(world.sampleHeight(x+8,z)-world.sampleHeight(x-8,z)),Abs(world.sampleHeight(x,z+8)-world.sampleHeight(x,z-8)))/16;
				const auto altitude=VegetationProfile::at(ground);
				const double probability=altitude.trees*boundary.density(position)*GenerationSettings::get().vegetation_forestDensityPercent/100.0;
				const double draw=(hash%10000u)/10000.0;
				const bool alpine=draw>=probability && draw<probability+altitude.alpine
					&& slope<=GenerationSettings::get().vegetation_maximumForestSlope;
				if(draw>=probability && !alpine) { continue; }
				const float scale=(GenerationSettings::get().vegetation_treeScaleMinimum+static_cast<float>((hash>>3)%29u)*.01f)*static_cast<float>(alpine ? 1 : altitude.treeScale);
				const bool cedar=Sin(x*GenerationSettings::get().vegetation_speciesFrequencyX)+Cos(z*GenerationSettings::get().vegetation_speciesFrequencyZ)+Sin((x-z)*GenerationSettings::get().vegetation_speciesFrequencyDiagonal) > GenerationSettings::get().vegetation_speciesThreshold;
				const double treeWidth=(alpine ? GenerationSettings::get().vegetation_alpineWidth : cedar ? GenerationSettings::get().vegetation_cedarWidth : GenerationSettings::get().vegetation_broadleafWidth)*scale;
				bool clear=true;
				for (const size_t roadIndex : nearbyRoads[{col,row}])
				{
					const auto& road=roadMasks[roadIndex];
					if (!TreeGeometry::clearOfCorridor(position,treeWidth,road.shape)) { excludedByFacilities+=road.facility;clear=false;break; }
				}
				if (!clear) { continue; }
				appendLandscapeTree(groups,position,ground,hash,treeWidth,(alpine ? GenerationSettings::get().vegetation_alpineHeight : cedar ? GenerationSettings::get().vegetation_cedarHeight : GenerationSettings::get().vegetation_broadleafHeight)*scale,cedar && !alpine,true);
				if (candidate==0 && !alpine)
				{
					// 足元の低木を高木と別の高さで重ねる。路肩へはみ出す株は上の離隔で除外する。
					const float angle = static_cast<float>(hash % 31);
					groups.trees << TreeInstance{{Float4{x, ground - .2f, z, 3.5f * scale},
						Float4{1.5f * scale, Cos(angle), Sin(angle), 0}}, TreeInstanceRenderer::kShrubModel, 1,
						static_cast<uint8>(TreeGeometry::materialKey(0, position) / 1000 - 1)};
				}
			}
		}
		groups.excludedTrees += excludedByFacilities;
	}
	template<class HeightSource>
	void appendParcelLandscape(LandscapeGeometry& groups, const HeightSource& world,
		const Chunk& chunk, const LandPatch& patch, const Array<LandRoadMask>& roadMasks)
	{
		const int col = static_cast<int>(patch.sourceParcelKey & 255);
		const int row = static_cast<int>((patch.sourceParcelKey >> 8) & 255);
		if (col >= ZONE_CELLS || row >= ZONE_CELLS) { return; }
		const Building& building = chunk.buildingGrid[{ col, row }];
		const Vec2 center{ chunk.coord.x * CHUNK_SIZE + (col + 0.5) * 16 + building.offsetX,
			chunk.coord.y * CHUNK_SIZE + (row + 0.5) * 16 + building.offsetZ };
		const Vec2 inward{ -Sin(building.angle), Cos(building.angle) };
		const Vec2 along{ inward.y, -inward.x };
		const double half = buildingFootprintXZ(building.type) * 0.5;
		Array<Vec2> outline = patch.polygon;
		UrbanParcel::normalize(outline);
		const Polygon shape{ outline };
		Array<const LandRoadMask*> adjacentRoads;
		const RectF parcelBounds = boundsOfPolygon(outline);
		for (const auto& road : roadMasks)
		{
			if (rectIntersects(parcelBounds.stretched(3), road.bounds)) { adjacentRoads << &road; }
		}
		auto clearOfRoad = [&](Vec2 position, double radius)
		{
			for (const auto* road : adjacentRoads)
			{
				if (Circle{ position, radius }.intersects(road->shape)) { return false; }
			}
			return true;
		};
		auto fits = [&](Vec2 position, double radius)
		{
			if (!shape.contains(position) || !clearOfRoad(position, radius)) { return false; }
			for (size_t side = 0; side < patch.polygon.size(); ++side)
			{
				const Vec2 a = patch.polygon[side], b = patch.polygon[(side + 1) % patch.polygon.size()];
				const Vec2 delta = b - a;
				const double fraction = Clamp((position - a).dot(delta) / Max(0.001, delta.lengthSq()), 0.0, 1.0);
				if (position.distanceFrom(a + delta * fraction) < radius) { return false; }
			}
			return true;
		};
		if (patch.type == LandPatchType::GardenSoil)
		{
			// Side and rear boundaries leave the original model's street entrance unobstructed.
			for (size_t side = 0; groups.surfaces && side < patch.polygon.size(); ++side)
			{
				const Vec2 a = patch.polygon[side], b = patch.polygon[(side + 1) % patch.polygon.size()];
				const Vec2 midpoint = (a + b) * 0.5;
				if ((midpoint - center).dot(inward) < -half + 0.4) { continue; }
				const double length = a.distanceFrom(b);
				const int pieces = Max(1, static_cast<int>(Ceil(length / 3.0)));
				for (int segment = 0; segment < pieces; ++segment)
				{
					const Vec2 position = a.lerp(b, (segment + 0.5) / pieces);
					if (!clearOfRoad(position, length / pieces * 0.5 + 0.18)) { continue; }
					appendRotatedBox(groups[123], static_cast<float>(position.x),
						world.sampleHeight(static_cast<float>(position.x), static_cast<float>(position.y)) + 0.30f,
						static_cast<float>(position.y), static_cast<float>(length / pieces), 0.60f, 0.12f,
						static_cast<float>(Atan2(b.y - a.y, b.x - a.x)));
				}
			}
			for (int index = 0; groups.planting && index < 4; ++index)
			{
				const Vec2 position = center + inward * (half + 4.0 + (index / 2) * 5.0)
					+ along * ((index % 2 == 0 ? -1 : 1) * (2.8 + (patch.materialVariant % 7) * 0.12));
				if (fits(position, TreeGeometry::horizontalClearance(4.8*1.06)) && ((patch.materialVariant >> index) & 3u) != 0)
				{
					appendParkTree(groups, position, world.sampleHeight(static_cast<float>(position.x), static_cast<float>(position.y)), patch.materialVariant + index, chunk.isUrbanizationArea ? 1.0 : 2.0);
				}
			}
		}
		else if (patch.type == LandPatchType::ParcelGravel && isResidentialBuildingType(building.type))
		{
			// 砂利敷きの住宅地にも隣地境界沿いと裏庭に庭木を残す。
			for (int index = 0; groups.planting && index < 4; ++index)
			{
				const double side = (index % 2 == 0 ? -1.0 : 1.0) * (half + 1.6);
				const Vec2 position = center + along * side + inward * (index < 2 ? half * 0.4 : half + 2.4);
				if (((patch.materialVariant >> (index * 3 + 2)) & 1u) == 0u
					&& fits(position, TreeGeometry::horizontalClearance(3.2)))
				{
					appendParkTree(groups, position, world.sampleHeight(static_cast<float>(position.x), static_cast<float>(position.y)), patch.materialVariant + index, 0.7);
				}
			}
		}
		else if (groups.surfaces && (building.type == BuildingType::Parking || building.type == BuildingType::Factory
			|| (building.type == BuildingType::Shop && (patch.materialVariant % 5) == 0)))
		{
			// Real-size rear parking stalls occupy the commercial service yard.
			for (int index = -2; index <= 2; ++index)
			{
				const Vec2 position = center + inward * (half + 4.2) + along * (index * 2.5);
				if (!fits(position, 2.7)) { continue; }
				appendRotatedBox(groups[102], static_cast<float>(position.x),
					world.sampleHeight(static_cast<float>(position.x), static_cast<float>(position.y)) + 0.035f,
					static_cast<float>(position.y), 4.8f, 0.012f, 0.08f,
					static_cast<float>(Atan2(inward.y, inward.x)));
			}
		}
	}
	/// @brief Build a bounded per-chunk index once; each entry path queries only intersecting32m cells.
	struct ResidentialAccessIndex
	{
		struct Item { int64 owner=-1; RectF bounds; Array<Vec2> outline; };
		Array<Item> items;
		HashTable<Point,Array<size_t>> cells;
		HashTable<int64,Array<const LandPatch*>> parcels;
		ResidentialAccessIndex(const World& world,const Chunk& chunk,const Array<LandRoadMask>& masks)
		{
			const RectF region{chunk.coord.x*CHUNK_SIZE-32,chunk.coord.y*CHUNK_SIZE-32,CHUNK_SIZE+64,CHUNK_SIZE+64};
			const auto add=[&](int64 owner,Array<Vec2> outline)
			{
				if (outline.size()<3) { return; } const RectF bounds=boundsOfPolygon(outline);
				if (!bounds.intersects(region)) { return; }
				const size_t id=items.size(); items << Item{owner,bounds,std::move(outline)};
				const int x0=static_cast<int>(Floor(Max(bounds.x,region.x)/32)),x1=static_cast<int>(Floor(Min(bounds.x+bounds.w,region.x+region.w)/32));
				const int z0=static_cast<int>(Floor(Max(bounds.y,region.y)/32)),z1=static_cast<int>(Floor(Min(bounds.y+bounds.h,region.y+region.h)/32));
				for (int z=z0;z<=z1;++z) for (int x=x0;x<=x1;++x) { cells[{x,z}] << id; }
			};
			for (const auto& mask : masks) { add(-1,mask.footprint); }
			for (int z=Max(0,chunk.coord.y-1);z<=Min(WORLD_CHUNKS-1,chunk.coord.y+1);++z)
				for (int x=Max(0,chunk.coord.x-1);x<=Min(WORLD_CHUNKS-1,chunk.coord.x+1);++x)
				{
					const auto* source=world.getChunk({x,z}); if (!source) { continue; }
					for (const auto& patch : source->landPatches)
					{
						if (source==&chunk && patch.sourceParcelKey>=0 && patch.polygon.size()>=3) { parcels[patch.sourceParcelKey] << &patch; }
						add(patch.sourceParcelKey,patch.polygon);
					}
					for (int row=0;row<ZONE_CELLS;++row) for (int col=0;col<ZONE_CELLS;++col)
					{
						const auto& building=source->buildingGrid[{col,row}];
						if (building.type==BuildingType::None || building.type==BuildingType::Farmland) { continue; }
						const Vec2 center{x*CHUNK_SIZE+(col+.5)*16+building.offsetX,z*CHUNK_SIZE+(row+.5)*16+building.offsetZ};
						const auto footprint=ParcelGeometry::footprint(center,buildingFootprintXZ(building.type)*.5+.1,building.angle);
						add(ZoneGrid::zoneCellKey({x,z},col,row),Array<Vec2>{footprint.begin(),footprint.end()});
					}
				}
		}
		Array<Array<Vec2>> query(RectF bounds,int64 owner) const
		{
			Array<Array<Vec2>> result; HashSet<size_t> seen;
			for (int z=static_cast<int>(Floor(bounds.y/32));z<=static_cast<int>(Floor((bounds.y+bounds.h)/32));++z)
				for (int x=static_cast<int>(Floor(bounds.x/32));x<=static_cast<int>(Floor((bounds.x+bounds.w)/32));++x)
				{
					const auto found=cells.find({x,z}); if (found==cells.end()) { continue; }
					for (const auto id : found->second)
					{
						if (!seen.insert(id).second) { continue; } const auto& item=items[id];
						if (item.owner!=owner && item.bounds.intersects(bounds)) { result << item.outline; }
					}
				}
			return result;
		}
	};

	/// @brief Join a verified residential doorway to its assigned street without changing ownership.
	int appendResidentialAccess(LandscapeGeometry& groups,const World& world,const RoadNetwork& roads,
		const Chunk& chunk,int col,int row,const Building& building,Vec2 center,
		const Array<ResidentialParcelAccess::Entry>& entrances,const ResidentialAccessIndex& index,Array<WorldRenderer::ResidentialAccessRecord>& acceptedEntries)
	{
		const auto* edge=roads.getEdge(building.edgeId); const auto curve=roads.getBezier(building.edgeId);
		if (!edge || !curve || edge->tunnel || edge->useElevation || !edge->hasRoadLanes() || curve->totalLength<.1f) { return 0; }
		const int64 key=ZoneGrid::zoneCellKey(chunk.coord,col,row);
		const auto owned=index.parcels.find(key); if (owned==index.parcels.end()) { return 0; }
		const auto& own=owned->second;
		int built=0;
		const Vec2 outward{Sin(building.angle),-Cos(building.angle)};
		for (const auto& entry : entrances)
		{
			const Vec2 entrance=entry.point;
			// Search only the assigned edge; a nearby unrelated road is never an access substitute.
			const auto distance=[&](float arc) { const Vec3 p=curve->positionAt(arc); return entrance.distanceFromSq(Vec2{p.x,p.z}); };
			int best=0; double minimum=distance(0);
			for (int sample=1;sample<=32;++sample) { const double value=distance(curve->totalLength*sample/32); if (value<minimum) { minimum=value;best=sample; } }
			float low=curve->totalLength*Max(0,best-1)/32,high=curve->totalLength*Min(32,best+1)/32;
			for (int iteration=0;iteration<18;++iteration) { const float a=low+(high-low)/3,b=high-(high-low)/3; if (distance(a)<distance(b)) { high=b; } else { low=a; } }
			const float arc=(low+high)*.5f; const Vec3 point=curve->positionAt(arc);
			const Vec2 roadPoint{point.x,point.z}; const Vec3 tangent=curve->tangentAt(arc);
			const Vec2 right=Vec2{tangent.z,-tangent.x}.normalized();
			const auto range=RoadGeometry::structuralRangeAt(*edge,arc/curve->totalLength); if (!range.valid) { continue; }
			const bool onRight=(center-roadPoint).dot(right)>=0;
			bool covered=false;
			for (const auto& part : edge->parts)
			{
				if (!RoadGeometry::isStructuralStrip(part) || part.type!=RoadPartType::RoadsideGutter || part.defId!=U"roadside_gutter_covered_concrete") { continue; }
				const float outer=RoadGeometry::partOffsetAt(part,arc/curve->totalLength,!onRight);
				covered |= Abs(outer-(onRight ? range.right : range.left))<.001;
			}
			if (!covered) { continue; } // Proper raised sidewalks and unknown asset profiles keep their existing treatment.
			const double roadTop=RoadGeometry::surfaceY(*edge,point,world.sampleHeight(static_cast<float>(point.x),static_cast<float>(point.z)))+.010;
			const Vec2 roadEnd=roadPoint+right*(onRight ? range.right+.01 : range.left-.01);
			const Vec2 approach=roadEnd-entrance;
			if (approach.lengthSq()<.04 || approach.normalized().dot(outward)<Cos(35_deg)) { continue; }
			if (Abs(world.sampleHeight(static_cast<float>(roadEnd.x),static_cast<float>(roadEnd.y))-point.y)>.15) { continue; }
			const RectF bounds{Min(entrance.x,roadEnd.x)-1,Min(entrance.y,roadEnd.y)-1,Abs(entrance.x-roadEnd.x)+2,Abs(entrance.y-roadEnd.y)+2};
			if (!ResidentialParcelAccess::insideMaskDomain(bounds,RectF{chunk.coord.x*CHUNK_SIZE,chunk.coord.y*CHUNK_SIZE,CHUNK_SIZE,CHUNK_SIZE})) { continue; }
			const auto obstacles=index.query(bounds,key);
			// A continuous dry walking surface follows the same sampled terrain as the parcel.
			bool terrainSafe=true; std::array<float,3> previous{};
			const Vec2 lateral=Vec2{-approach.y,approach.x}.normalized();
			const int samples=Max(1,static_cast<int>(Ceil(approach.length()/.5)));
			for (int sample=0;sample<=samples && terrainSafe;++sample)
			{
				for (int side=0;side<3;++side)
				{
					const Vec2 p=entrance.lerp(roadEnd,static_cast<double>(sample)/samples)+lateral*((side-1)*ResidentialParcelAccess::kWidth*.5);
					const float height=world.sampleHeight(static_cast<float>(p.x),static_cast<float>(p.y));
					terrainSafe &= height>=world.waterSurfaceHeight(p.x,p.y)+GenerationSettings::get().development_buildingFreeboard;
					if (sample>0 && Abs(height-previous[side])>approach.length()/samples*.12) { terrainSafe=false; }
					previous[side]=height;
				}
			}
			if (!terrainSafe) { continue; }
			for (const auto* parcel : own)
			{
				const auto outline=ResidentialParcelAccess::create(parcel->polygon,roadEnd,entrance,obstacles);
				if (!outline) { continue; }
				const auto mesh=ResidentialParcelAccess::ramp(roadEnd,entry,roadTop,[&](Vec2 point)
				{
					return static_cast<double>(world.sampleHeight(static_cast<float>(point.x),static_cast<float>(point.y)));
				},parcel->elevationOffset,ResidentialParcelAccess::kWidth,parcel->polygon);
				if (!mesh) { continue; }
				appendMeshData(groups[117],*mesh);
				const size_t finalTop=mesh->vertices.size()-12;
				const Vec3 emittedRoad=(Vec3{mesh->vertices[0].pos}+Vec3{mesh->vertices[3].pos})*.5;
				const Vec3 emittedEntry=(Vec3{mesh->vertices[finalTop+1].pos}+Vec3{mesh->vertices[finalTop+2].pos})*.5;
				const Vec2 boundary=roadPoint+right*(onRight ? range.right : range.left);
				acceptedEntries << WorldRenderer::ResidentialAccessRecord{edge->id,key,building.type,
					{boundary.x,roadTop-.010,boundary.y},emittedRoad,emittedEntry,{entry.point.x,entry.height,entry.point.y}};
				++built;
				break;
			}
		}
		return built;
	}

	void appendUrbanLotDetails(LandscapeGeometry& groups, const Chunk& chunk, const World& world,
	                           int col, int row, const Building& building, float cx, float cz,
	                           float cellSize,const Array<LandRoadMask>& roadMasks)
	{
		// 駐車枠・精算機・駐車車両は専用OBJに含まれる。
		if (building.type == BuildingType::Parking)
		{
			return;
		}
		const uint32 hash = cellVisualHash(chunk.coord, col, row, static_cast<uint32>(building.type));
		const float angle = building.angle;
		const float baseY = world.sampleHeight(cx, cz);
		const float lotSize = cellSize * (0.74f + static_cast<float>((hash >> 3) % 3u) * 0.010f);
		const bool commercialLike = (building.type == BuildingType::Shop || building.type == BuildingType::Office
			|| building.type == BuildingType::Factory || building.type == BuildingType::PublicFacility);
		const int surfaceKey = (building.type == BuildingType::Parking) ? 119 : (commercialLike ? 120 : (((hash >> 9) % 100u < 8u) ? 121 : 120));
		const float surfaceSize = (building.type == BuildingType::Parking) ? lotSize : (commercialLike ? lotSize * 0.36f : lotSize * 0.22f);
		if (building.type == BuildingType::Parking || commercialLike)
		{
			appendRotatedBox(groups[surfaceKey], cx, baseY + 0.020f, cz, surfaceSize, 0.035f, surfaceSize * (commercialLike ? 0.42f : 0.30f), angle);
		}

		const float cosA = Math::Cos(angle);
		const float sinA = Math::Sin(angle);
		auto worldOffset = [&](float lx, float lz)
		{
			return Vec2{ cx + lx * cosA - lz * sinA, cz + lx * sinA + lz * cosA };
		};

		if (building.type == BuildingType::Parking)
		{
			appendRotatedBox(groups[109], cx, baseY + 0.09f, cz, lotSize * 0.92f, 0.08f, lotSize * 0.74f, angle);
			for (int i = -1; i <= 1; ++i)
			{
				const Vec2 p = worldOffset(static_cast<float>(i) * lotSize * 0.22f,
					((hash >> (10 + i + 1)) & 1u) ? lotSize * 0.08f : -lotSize * 0.13f);
				appendRotatedBox(groups[(i == 0) ? 108 : 107], static_cast<float>(p.x), baseY + 0.30f,
					static_cast<float>(p.y), 1.65f, 0.42f, 3.35f, angle);
			}
			return;
		}

		if (building.type == BuildingType::ParkBuilding)
		{
			appendRotatedBox(groups[101], cx, baseY + 0.08f, cz, lotSize * 0.78f, 0.10f, lotSize * 0.78f, angle);
			for (int i = 0; i < 3; ++i)
			{
				const Vec2 p = worldOffset((static_cast<float>((hash >> (i * 4)) % 9u) - 4.0f) * 0.9f,
					(static_cast<float>((hash >> (i * 5 + 7)) % 9u) - 4.0f) * 0.9f);
				bool clear=true;
				for (const auto& road : roadMasks) { if (!TreeGeometry::clearOfCorridor(p,4.8*1.06,road.shape)) { clear=false;break; } }
				if (clear) { appendParkTree(groups, p, world.sampleHeight(static_cast<float>(p.x), static_cast<float>(p.y)), hash + i); }
			}
			return;
		}

		const float frontageZ = -lotSize * 0.43f;
		if (commercialLike)
		{
			appendRotatedBox(groups[119], cx, baseY + 0.040f, cz, lotSize * 0.52f, 0.025f, lotSize * 0.28f, angle);
			const Vec2 sign = worldOffset(lotSize * 0.34f, frontageZ);
			appendRotatedBox(groups[115], static_cast<float>(sign.x), baseY + 1.15f, static_cast<float>(sign.y), 0.16f, 2.30f, 0.16f, angle);
			appendRotatedBox(groups[112], static_cast<float>(sign.x), baseY + 2.45f, static_cast<float>(sign.y), 1.20f, 0.60f, 0.12f, angle);
		}
		else
		{
			if (((hash >> 9) % 100u) < 52u) appendRotatedBox(groups[101], cx, baseY + 0.030f, cz, lotSize * 0.34f, 0.018f, lotSize * 0.22f, angle);
			const Vec2 car = worldOffset(((hash >> 11) & 1u) ? lotSize * 0.25f : -lotSize * 0.25f, lotSize * 0.30f);
			if (((hash >> 6) % 100u) < 52u)
			{
				appendRotatedBox(groups[100], static_cast<float>(car.x), baseY + 0.08f, static_cast<float>(car.y), 2.15f, 0.05f, 4.05f, angle);
				appendRotatedBox(groups[((hash >> 14) & 1u) ? 107 : 108], static_cast<float>(car.x), baseY + 0.32f,
					static_cast<float>(car.y), 1.55f, 0.44f, 3.10f, angle);
			}
			const Vec2 approach = worldOffset(0.0f, frontageZ * 0.54f);
			appendRotatedBox(groups[120], static_cast<float>(approach.x), baseY + 0.036f, static_cast<float>(approach.y),
				lotSize * 0.16f, 0.020f, lotSize * 0.70f, angle);
			if (((hash >> 27) % 100u) < 34u)
			{
				const Vec2 bin = worldOffset(-lotSize * 0.35f, frontageZ + 0.12f);
				appendRotatedBox(groups[117], static_cast<float>(bin.x), baseY + 0.28f, static_cast<float>(bin.y), 0.70f, 0.56f, 0.42f, angle);
			}
		}

		if ((hash % 100u) < 92u)
		{
			const float side = ((hash >> 8) & 1u) ? 1.0f : -1.0f;
			const Vec2 fence = worldOffset(side * lotSize * 0.38f,
				(static_cast<float>((hash >> 12) % 7u) - 3.0f) * 0.65f);
			appendRotatedBox(groups[102], static_cast<float>(fence.x), baseY + 0.34f, static_cast<float>(fence.y),
				0.18f, 0.68f, lotSize * 0.46f, angle);
		}

		if (((hash >> 5) % 100u) < 58u)
		{
			const float side = ((hash >> 17) & 1u) ? 1.0f : -1.0f;
			const Vec2 tree = worldOffset(side * lotSize * 0.36f,
				lotSize * (0.20f + static_cast<float>((hash >> 21) % 18u) * 0.012f));
			for (const auto& road : roadMasks) { if (!TreeGeometry::clearOfCorridor(tree,3,road.shape)) { return; } }
			appendParkTree(groups, tree, world.sampleHeight(static_cast<float>(tree.x), static_cast<float>(tree.y)), hash, 0.6);
		}


	}
	void appendFarmlandDetails(LandscapeGeometry& groups, const Chunk& chunk, const World& world,
	                           int col, int row, float cx, float cz, float cellSize)
	{
		const uint32 hash = cellVisualHash(chunk.coord, col, row, 0xA6B4C893u);
		const float angle = ((hash & 1u) ? 0.0f : static_cast<float>(90.0_deg))
			+ static_cast<float>((static_cast<int>((hash >> 8) % 7u) - 3) * 0.015f);
		const float baseY = world.sampleHeight(cx, cz);
		const float fieldW = cellSize * (0.78f + static_cast<float>(hash % 18u) * 0.010f);
		const float fieldD = cellSize * (0.56f + static_cast<float>((hash >> 16) % 24u) * 0.010f);
		const int fieldKey = ((hash >> 5) % 100u < 42u) ? 113 : (((hash >> 10) & 1u) ? 104 : 114);
		appendRotatedBox(groups[fieldKey], cx, baseY + 0.025f, cz, fieldW, 0.05f, fieldD, angle);

		const float cosA = Math::Cos(angle);
		const float sinA = Math::Sin(angle);
		for (int i = -3; i <= 3; ++i)
		{
			const float lx = static_cast<float>(i) * fieldW * 0.135f;
			const float px = cx + lx * cosA;
			const float pz = cz + lx * sinA;
			appendRotatedBox(groups[(i == 0 && ((hash >> 20) & 1u)) ? 110 : 101], px, baseY + 0.065f, pz,
				0.18f, 0.045f, fieldD * 0.98f, angle);
		}
		for (int i = -1; i <= 1; ++i)
		{
			const float lz = static_cast<float>(i) * fieldD * 0.29f;
			const float px = cx - lz * sinA;
			const float pz = cz + lz * cosA;
			appendRotatedBox(groups[114], px, baseY + 0.055f, pz, fieldW * 0.94f, 0.035f, 0.16f, angle);
		}

		if (((hash >> 24) % 100u) < 34u)
		{
			const float px = cx + fieldW * 0.48f * cosA;
			const float pz = cz + fieldW * 0.48f * sinA;
			appendRotatedBox(groups[110], px, baseY + 0.05f, pz, 0.42f, 0.04f, fieldD, angle);
		}
	}
	void appendBoxBuildingDetails(LandscapeGeometry& groups, const Chunk& chunk, const World& world,
	                              int col, int row, BuildingType type, float cx, float cz,
	                              float footprint, float height, float angle)
	{
		const uint32 hash = cellVisualHash(chunk.coord, col, row, static_cast<uint32>(type) ^ 0x712A4C3Du);
		const float baseY = world.sampleHeight(cx, cz);
		const float cosA = Math::Cos(angle);
		const float sinA = Math::Sin(angle);
		auto worldOffset = [&](float lx, float lz)
		{
			return Vec2{ cx + lx * cosA - lz * sinA, cz + lx * sinA + lz * cosA };
		};

		if (!isObjBuildingType(type))
		{
			appendRotatedBox(groups[103], cx, baseY + height + 0.08f, cz,
			                 footprint * 0.82f, 0.16f, footprint * 0.82f, angle);
		}

		if (type == BuildingType::Parking || type == BuildingType::ParkBuilding)
		{
			return;
		}

		const bool residential = isResidentialBuildingType(type);
		const bool shop = (type == BuildingType::Shop);
		const bool officeLike = (type == BuildingType::Office || type == BuildingType::PublicFacility);
		const int floorCount = Max(1, static_cast<int>(Floor(height / (residential ? 2.8f : 3.2f))));
		const int visibleFloors = Min(floorCount, residential ? 4 : 7);
		const float frontZ = -footprint * 0.515f;
		const float backZ = footprint * 0.515f;
		const float sideX = footprint * 0.515f;

		for (int floor = 0; floor < visibleFloors; ++floor)
		{
			const float y = baseY + 1.15f + static_cast<float>(floor) * (residential ? 2.65f : 3.0f);
			if (y > baseY + height - 0.55f) break;
			const int windowKey = ((hash >> (floor + 3)) & 1u) ? 116 : 102;
			const float rowWidth = footprint * (residential ? 0.20f : 0.16f);
			for (int w = -1; w <= 1; ++w)
			{
				const float lx = static_cast<float>(w) * footprint * 0.23f;
				const Vec2 front = worldOffset(lx, frontZ);
				appendRotatedBox(groups[windowKey], static_cast<float>(front.x), y, static_cast<float>(front.y),
					rowWidth, residential ? 0.56f : 0.76f, 0.10f, angle);
			}
			if (residential && floor > 0)
			{
				const Vec2 balcony = worldOffset(0.0f, frontZ - 0.18f);
				appendRotatedBox(groups[117], static_cast<float>(balcony.x), y - 0.20f, static_cast<float>(balcony.y),
					footprint * 0.68f, 0.12f, 0.28f, angle);
			}
			if (!residential)
			{
				const Vec2 left = worldOffset(-sideX, 0.0f);
				appendRotatedBox(groups[windowKey], static_cast<float>(left.x), y, static_cast<float>(left.y),
					0.10f, 0.66f, footprint * 0.44f, angle);
				const Vec2 right = worldOffset(sideX, 0.0f);
				appendRotatedBox(groups[windowKey], static_cast<float>(right.x), y, static_cast<float>(right.y),
					0.10f, 0.66f, footprint * 0.44f, angle);
			}
		}

		if (residential)
		{
			const bool ridgeAlongX = ((hash >> 23) & 1u) != 0u;
			const float roofBase = baseY + Max(2.7f, height - (type == BuildingType::Detached ? 1.05f : 0.55f));
			appendGableRoof(groups[103], cx, roofBase, cz, footprint * 0.96f, footprint * 0.86f,
				(type == BuildingType::Detached ? 1.15f : 0.55f), angle, ridgeAlongX);
			const Vec2 eave = worldOffset(0.0f, frontZ - 0.12f);
			appendRotatedBox(groups[103], static_cast<float>(eave.x), baseY + Min(height, 4.8f), static_cast<float>(eave.y),
				footprint * 0.84f, 0.12f, 0.42f, angle);
			const Vec2 unit = worldOffset(sideX + 0.16f, footprint * 0.22f);
			appendRotatedBox(groups[117], static_cast<float>(unit.x), baseY + 1.20f, static_cast<float>(unit.y),
				0.42f, 0.42f, 0.22f, angle);

			if (type == BuildingType::Detached)
			{
				const float wingSide = ((hash >> 4) & 1u) ? 1.0f : -1.0f;
				const Vec2 wing = worldOffset(wingSide * footprint * 0.42f, footprint * 0.10f);
				appendRotatedBox(groups[static_cast<int>(type)], static_cast<float>(wing.x), baseY + 1.15f, static_cast<float>(wing.y),
					footprint * 0.34f, 2.30f, footprint * 0.54f, angle);
				appendGableRoof(groups[103], static_cast<float>(wing.x), baseY + 2.30f, static_cast<float>(wing.y),
					footprint * 0.40f, footprint * 0.62f, 0.62f, angle, !ridgeAlongX);
				const Vec2 shed = worldOffset(-wingSide * footprint * 0.46f, footprint * 0.38f);
				appendRotatedBox(groups[117], static_cast<float>(shed.x), baseY + 0.58f, static_cast<float>(shed.y),
					1.25f, 1.16f, 1.70f, angle);
				appendRotatedBox(groups[103], static_cast<float>(shed.x), baseY + 1.22f, static_cast<float>(shed.y),
					1.45f, 0.16f, 1.95f, angle);
			}
		}

		if (shop)
		{
			const Vec2 sign = worldOffset(0.0f, frontZ - 0.10f);
			appendRotatedBox(groups[112], static_cast<float>(sign.x), baseY + Min(height * 0.74f, 3.15f), static_cast<float>(sign.y),
				footprint * 0.72f, 0.55f, 0.12f, angle);
			const Vec2 awning = worldOffset(0.0f, frontZ - 0.32f);
			appendRotatedBox(groups[118], static_cast<float>(awning.x), baseY + 2.25f, static_cast<float>(awning.y),
				footprint * 0.78f, 0.16f, 0.62f, angle);
			for (int d = -1; d <= 1; ++d)
			{
				const Vec2 door = worldOffset(static_cast<float>(d) * footprint * 0.22f, frontZ - 0.07f);
				appendRotatedBox(groups[116], static_cast<float>(door.x), baseY + 1.10f, static_cast<float>(door.y),
					footprint * 0.16f, 1.35f, 0.10f, angle);
			}
		}

		if (officeLike || type == BuildingType::Factory)
		{
			const Vec2 rear = worldOffset(0.0f, backZ + 0.10f);
			appendRotatedBox(groups[117], static_cast<float>(rear.x), baseY + Max(1.5f, height * 0.42f), static_cast<float>(rear.y),
				footprint * 0.36f, 0.38f, 0.24f, angle);
		}
	}
	/// @brief 住宅 OBJ を全 part 単色で描画する（シルエット用）
	void drawModelSilhouette(Model& model, const Mat4x4& worldMat, const ColorF& color)
	{
		const Transformer3D transform{ worldMat };
		for (const auto& obj : model.objects())
		{
			for (const auto& part : obj.parts)
				part.mesh.draw(color);
		}
	}
}

WorldRenderer::BuildingModelAsset& WorldRenderer::getBuildingModelAsset(BuildingType type, uint8 variant)
{
	// 建物種別ごとの OBJ/TOML を遅延ロードし、以後はモデルとスケールを共有キャッシュで再利用する。
	const uint32 key = (static_cast<uint32>(type) << 8) | static_cast<uint32>(variant);
	auto it = m_buildingModels.find(key);
	if (it != m_buildingModels.end())
	{
		return it->second;
	}

	String stem;
	if (!tryGetBuildingModelStemForVariant(type, variant, stem))
	{
		auto [inserted, _] = m_buildingModels.emplace(key, BuildingModelAsset{});
		return inserted->second;
	}
	const String subDir = buildingAssetSubDir(type);
	const String path = U"assets/buildings/{}/{}.obj"_fmt(subDir, stem);
	const String tomlPath = U"assets/buildings/{}/{}.toml"_fmt(subDir, stem);

	BuildingModelAsset asset;
	const TOMLReader toml{ tomlPath };
	if (toml)
	{
		asset.scale = parseModelScale(toml);
		asset.frontWall = toml[U"front_wall_z_m"].getOpt<float>();
	}
	else
	{
		Console << U"[WorldRenderer] building TOML load failed: " << tomlPath
		        << U" (scale fallback=" << kDefaultModelScale << U")";
	}

	const String distantPath = modelLodPath(path, 1);
	if (FileSystem::IsFile(distantPath))
	{
		asset.distantModel = Model{ distantPath };
		Model::RegisterDiffuseTextures(asset.distantModel, TextureDesc::MippedSRGB);
	}
	const String farPath = modelLodPath(path, 2);
	if (FileSystem::IsFile(farPath))
	{
		const Model farModel{farPath};
		Model::RegisterDiffuseTextures(farModel, TextureDesc::MippedSRGB);
		asset.farGeometry = loadModelMeshSource(farPath, farModel);
	}
	asset.model = Model{ path };
	if (!asset.model.isEmpty())
	{
		Model::RegisterDiffuseTextures(asset.model, TextureDesc::MippedSRGB);
		if (type != BuildingType::Parking && type != BuildingType::Factory && !isCompleteSiteBuilding(type))
		{
			const auto& bounds = asset.model.boundingBox();
			const float modelScale = normalizedObjScale(type,bounds,asset.scale);
			const double width = bounds.size.x * modelScale;
			const double front = asset.frontWall.value_or(static_cast<float>(bounds.center.z-bounds.size.z*.5)) * modelScale;
			for (uint32 variation=0;variation<asset.frontages.size();++variation)
			{
				HashTable<int,MeshData> materials;
				for (const auto& part : FrontageGeometry::build(width,front,type==BuildingType::Shop || type==BuildingType::Office,variation))
				{
					appendMeshData(materials[part.material],part.mesh);
				}
				for (const auto& [material,mesh] : materials)
				{
					asset.frontages[variation] << BuildingBatch{material,detailColorForKey(material),Mesh{mesh}};
				}
			}
		}
	}
	else
	{
		Console << U"[WorldRenderer] building model load failed: " << path;
	}

	auto [inserted, _] = m_buildingModels.emplace(key, std::move(asset));
	return inserted->second;
}

Optional<OrientedBox> WorldRenderer::buildingHitBox(const Chunk& chunk, const World& world,
                                                     int col, int row)
{
	if (col < 0 || col >= ZONE_CELLS || row < 0 || row >= ZONE_CELLS) return none;
	const Building& b = chunk.buildingGrid[{ col, row }];
	if (b.type == BuildingType::None || b.type == BuildingType::Farmland) return none;

	constexpr float cellSize  = static_cast<float>(CHUNK_SIZE) / ZONE_CELLS;
	const float footprint = buildingFootprintXZ(b.type);
	const Vec3 origin = chunk.worldOrigin();
	const float cx = static_cast<float>(origin.x + (col + 0.5) * cellSize) + b.offsetX;
	const float cz = static_cast<float>(origin.z + (row + 0.5) * cellSize) + b.offsetZ;

	if (isObjBuildingType(b.type))
	{
		const float gy = buildingBaseHeight(world,b,cx,cz);
		const int gx = chunk.coord.x * ZONE_CELLS + col;
		const int gz = chunk.coord.y * ZONE_CELLS + row;
		const uint8 variant = buildingModelVariant(b.type, gx, gz);
		BuildingModelAsset& asset = getBuildingModelAsset(b.type, variant);
		if (asset.model.isEmpty()) return none;
		const float yaw = -b.angle;

		const Box& lb = asset.model.boundingBox();
		const float modelScale = normalizedObjScale(b.type, lb, asset.scale);
		return RenderDistance::transformBox(lb, modelScale, yaw, Vec3{cx,gy,cz});
	}

	if (b.type == BuildingType::Parking || b.type == BuildingType::ParkBuilding)
	{
		return none;
	}

	const float height = buildingHeight(b.type) * kLegacyBoxHeightScale;
	if (height <= 0.0f) return none;
	const int gx = chunk.coord.x * ZONE_CELLS + col;
	const int gz = chunk.coord.y * ZONE_CELLS + row;
	const float visualFootprint = (b.type==BuildingType::UrbanHousePair || b.type==BuildingType::IndustrialWarehouse)
		? footprint : footprint * boxBuildingFootprintScale(b.type, gx, gz);
	const float cy = world.sampleHeight(cx, cz) + height * 0.5f;
	return OrientedBox{ Vec3{ cx, cy, cz }, Vec3{ visualFootprint, height, visualFootprint },
	                   Quaternion::RotateY(-b.angle) };
}

void WorldRenderer::drawBuildingSilhouette(const Chunk& chunk, const World& world,
                                            int col, int row, const ColorF& color)
{
	if (col < 0 || col >= ZONE_CELLS || row < 0 || row >= ZONE_CELLS) return;
	if (m_renderDistance != RenderDistance::kDefault)
	{
		const auto bounds = buildingHitBox(chunk,world,col,row);
		if (!bounds || !buildingWithinRenderDistance(chunk,col,row,*bounds,m_buildingEye)) { return; }
	}
	const Building& b = chunk.buildingGrid[{ col, row }];
	if (b.type == BuildingType::None || b.type == BuildingType::Farmland) return;

	constexpr float cellSize  = static_cast<float>(CHUNK_SIZE) / ZONE_CELLS;
	const float footprint = buildingFootprintXZ(b.type);
	const Vec3 origin = chunk.worldOrigin();
	const float cx = static_cast<float>(origin.x + (col + 0.5) * cellSize) + b.offsetX;
	const float cz = static_cast<float>(origin.z + (row + 0.5) * cellSize) + b.offsetZ;

	if (isObjBuildingType(b.type))
	{
		const float gy = buildingBaseHeight(world,b,cx,cz);
		const int gx = chunk.coord.x * ZONE_CELLS + col;
		const int gz = chunk.coord.y * ZONE_CELLS + row;
		const uint8 variant = buildingModelVariant(b.type, gx, gz);
		BuildingModelAsset& asset = getBuildingModelAsset(b.type, variant);
		if (asset.model.isEmpty()) return;
		const float yaw = -b.angle;

		drawModelSilhouette(asset.model,
		                    (Mat4x4::Scale(normalizedObjScale(b.type, asset.model.boundingBox(), asset.scale))
		                   * Mat4x4::RotateY(yaw)).translated(cx, gy, cz),
		                    color);
		return;
	}

	if (b.type == BuildingType::Parking || b.type == BuildingType::ParkBuilding)
	{
		return;
	}

	const float height = buildingHeight(b.type) * kLegacyBoxHeightScale;
	if (height <= 0.0f) return;
	const int gx = chunk.coord.x * ZONE_CELLS + col;
	const int gz = chunk.coord.y * ZONE_CELLS + row;
	const float visualFootprint = (b.type==BuildingType::UrbanHousePair || b.type==BuildingType::IndustrialWarehouse)
		? footprint : footprint * boxBuildingFootprintScale(b.type, gx, gz);
	const float cy = world.sampleHeight(cx, cz) + height * 0.5f;

	// rebuildBuildingMeshes と同じパイプライン（MeshData::Box + 頂点手動回転）で描画する
	MeshData box = MeshData::Box(
		Float3{ cx, cy, cz },
		Float3{ visualFootprint, height, visualFootprint });
	rotateBoxVerticesY(box, cx, cz, b.angle);
	Mesh{ box }.draw(color);
}

Array<WorldRenderer::TerrainMeshData> WorldRenderer::buildLandscapeMeshData(const Chunk& chunk,
	const Array<TerrainSubtractionQuad>& quads, const Array<Chunk>& heightSnapshots,
	const RiverNetwork& rivers, const Array<Polygon>& sites, LandscapeBuildStats& stats,
	Array<TreeInstance>& trees, bool treesOnly, bool woodland)
{
	const Stopwatch timer{StartImmediately::Yes};
	struct HeightSnapshot
	{
		const Array<Chunk>& chunks;
		const Chunk* getChunk(Point coord) const
		{
			for(const auto& chunk:chunks) { if(chunk.coord==coord) { return &chunk; } }return nullptr;
		}
		float sampleHeight(float x, float z) const
		{
			const Point coord{static_cast<int>(Floor(x/CHUNK_SIZE)),static_cast<int>(Floor(z/CHUNK_SIZE))};
			for (const auto& snapshot : chunks)
			{
				if (snapshot.coord == coord) { return snapshot.getHeight(x,z); }
			}
			return 0.0f;
		}
	} heights{heightSnapshots};
	Array<LandRoadMask> masks;
	for (const auto& quad : quads)
	{
		const Vec2 center=quad.bounds.center();
		const Vec3 ground{center.x,heights.sampleHeight(static_cast<float>(center.x),static_cast<float>(center.y)),center.y};
		bool above=false;
		for (const auto& plane : quad.cutPlanes) { above|=plane.normal.y>.01 && plane.normal.dot(ground)>plane.offset+.01; }
		if (above) { continue; }
		Array<Vec2> outline = quad.footprint;
		UrbanParcel::normalize(outline);
		masks << LandRoadMask{quad.bounds,quad.footprint,Polygon{outline},quad.bedBottomY};
	}
	for (const auto& site : sites) { masks << LandRoadMask{site.boundingRect(),site.outer(),site,0,true}; }
	for (const auto& reach : rivers.reaches)
	{
		const Vec2 a{reach.start.x,reach.start.z},b{reach.end.x,reach.end.z},direction=(b-a).normalized(),right{direction.y,-direction.x};
		const double width=Max(reach.halfWidth,reach.endHalfWidth)+12;
		Array<Vec2> outline{a-direction*12-right*width,b+direction*12-right*width,b+direction*12+right*width,a-direction*12+right*width};
		UrbanParcel::normalize(outline); masks<<LandRoadMask{reach.bounds,outline,Polygon{outline}};
	}
	stats.masksMilliseconds = timer.msF();
	LandscapeGeometry groups;
	groups.surfaces = !treesOnly; groups.planting = treesOnly;
	for (const auto& patch : chunk.landPatches) { appendLandPatchMesh(groups,heights,chunk,patch,masks); }
	stats.patchesMilliseconds = timer.msF() - stats.masksMilliseconds;
	if (woodland && treesOnly)
	{
		appendWoodland(groups, chunk, heights, masks, rivers);
	}
	stats.woodlandMilliseconds = timer.msF() - stats.masksMilliseconds - stats.patchesMilliseconds;
	trees = std::move(groups.trees);
	stats.excludedTrees = groups.excludedTrees;
	Array<TerrainMeshData> result;
	for (auto& [key,mesh] : groups.meshes)
	{
		stats.vertices += mesh.vertices.size();
		stats.triangles += mesh.indices.size();
		result << TerrainMeshData{key,std::move(mesh)};
	}
	return result;
}

MeshData WorldRenderer::landPatchSurface(const World& world,const RoadNetwork& network,Point coord,const LandPatch& patch)
{
	Array<LandRoadMask> masks;
	for (const auto& quad : getChunkSubtractionQuads(network,coord))
	{
		Array<Vec2> outline=quad.footprint;UrbanParcel::normalize(outline);
		masks << LandRoadMask{quad.bounds,quad.footprint,Polygon{outline},quad.bedBottomY};
	}
	MeshData surface;
	appendLandPatchSurface(surface,world,patch,masks);
	return surface;
}

void WorldRenderer::rebuildBuildingMeshes(Key key, const Chunk& chunk, const World& world,const RoadNetwork& roads)
{
	constexpr float cellSize  = static_cast<float>(CHUNK_SIZE) / ZONE_CELLS;
	const float footprint = buildingFootprintXZ();

	const Vec3 origin = chunk.worldOrigin();

	m_buildingDetailBounds.erase(key);
	// 建物種別ごとに MeshData を積み上げる（Box 描画用）
	LandscapeGeometry groups;
	// 住宅 OBJ インスタンス
	Array<BuildingModelInstance> modelInstances;
	int accessPaths=0; m_residentialAccessEntries[key].clear();

	Array<LandRoadMask> roadMasks;
	if (const auto masks = m_chunkSubtractorCache.find(key); masks != m_chunkSubtractorCache.end())
	{
		for (const auto& mask : masks->second)
		{
			Array<Vec2> outline = mask.footprint;
			UrbanParcel::normalize(outline);
			roadMasks << LandRoadMask{ mask.bounds, mask.footprint, Polygon{ outline }, mask.bedBottomY };
		}
	}
	if (const auto sites=m_transportSites.find(key); sites!=m_transportSites.end())
	{
		for (const auto& site : sites->second) { roadMasks << LandRoadMask{site.boundingRect(),site.outer(),site,0,true}; }
	}
	Optional<ResidentialAccessIndex> accessIndex;
	for (const auto& building : chunk.buildingGrid)
	{
		if (building.type==BuildingType::None) { continue; }
		const auto* assigned=roads.getEdge(building.edgeId); if (!assigned) { continue; }
		if (assigned->parts.any([](const RoadPart& part) { return part.defId==U"roadside_gutter_covered_concrete"; }))
		{
			accessIndex.emplace(world,chunk,roadMasks); break;
		}
	}
	if (!m_asyncTerrain)
	{
		for (const LandPatch& patch : chunk.landPatches) { appendLandPatchMesh(groups, world, chunk, patch, roadMasks); }
		if (m_woodlandEnabled)
		{
			appendWoodland(groups, chunk, world, roadMasks, world.rivers());
		}
	}

	for (int row = 0; row < ZONE_CELLS; ++row)
	{
		for (int col = 0; col < ZONE_CELLS; ++col)
		{
			const Building& b = chunk.buildingGrid[{ col, row }];
			const float cellCenterX = static_cast<float>(origin.x + (col + 0.5) * cellSize);
			const float cellCenterZ = static_cast<float>(origin.z + (row + 0.5) * cellSize);
			if (b.type == BuildingType::None)
			{
				continue;
			}

			const float cx = cellCenterX + b.offsetX;
			const float cz = cellCenterZ + b.offsetZ;

			if (b.type == BuildingType::Farmland)
			{
				continue;
			}

			if (b.type==BuildingType::UrbanHousePair)
			{
				appendUrbanHousePair(groups,world,b,cx,cz,cellVisualHash(chunk.coord,col,row,8841));
				Array<ResidentialParcelAccess::Entry> entries; const Vec2 along{Cos(b.angle),Sin(b.angle)},inward{-along.y,along.x};
				for (const auto local : FrontageGeometry::pairedGroundEntries(buildingFootprintXZ(b.type))) { entries << ResidentialParcelAccess::Entry{Vec2{cx,cz}+along*local.x+inward*local.y,buildingBaseHeight(world,b,cx,cz)+FrontageGeometry::kPairedApronTop}; }
				if (accessIndex) { accessPaths+=appendResidentialAccess(groups,world,roads,chunk,col,row,b,{cx,cz},entries,*accessIndex,m_residentialAccessEntries[key]); }
				continue;
			}
			if (b.type==BuildingType::IndustrialWarehouse)
			{
				appendIndustrialWarehouse(groups,world,b,cx,cz);
				continue;
			}

			// 住宅系は OBJ で描画する（地表位置に Y 軸回転のみ適用）
			if (isObjBuildingType(b.type))
			{
				const float gy = buildingBaseHeight(world,b,cx,cz);
				const float ground=world.sampleHeight(cx,cz);
				if (gy-ground>.07f && b.type!=BuildingType::Parking)
				{
					const float width=buildingFootprintXZ(b.type)+.12f,height=(gy-ground)*2+.12f;
					appendRotatedBox(groups[117],cx,gy-height*.5f,cz,width,height,width,b.angle);
				}
				if (b.type==BuildingType::Detached || b.type==BuildingType::LowApartment)
				{
					appendHouseSideStructure(groups,world,b,chunk.coord,col,row,cx,cz,cellVisualHash(chunk.coord,col,row,6113),roadMasks);
				}
				const int gx = chunk.coord.x * ZONE_CELLS + col;
				const int gz = chunk.coord.y * ZONE_CELLS + row;
				const uint8 variant = buildingModelVariant(b.type, gx, gz);
				BuildingModelAsset& asset = getBuildingModelAsset(b.type, variant);
				const float modelScale = normalizedObjScale(b.type, asset.model.boundingBox(), asset.scale);
				if ((b.type==BuildingType::Detached || b.type==BuildingType::LowApartment || b.type==BuildingType::VillageHouse) && asset.frontWall && !asset.model.isEmpty() && accessIndex)
				{
					const Vec2 local=FrontageGeometry::groundEntry(*asset.frontWall*modelScale);
					const Vec2 along{Cos(b.angle),Sin(b.angle)},inward{-along.y,along.x};
					const Vec2 entrance=Vec2{cx,cz}+along*local.x+inward*local.y;
					accessPaths+=appendResidentialAccess(groups,world,roads,chunk,col,row,b,{cx,cz},{{entrance,gy+FrontageGeometry::kStepHeight}},*accessIndex,m_residentialAccessEntries[key]);
				}
				modelInstances.push_back({
					b.type,
					variant,
					Float3{ cx, gy, cz },
					b.angle,
					modelScale,
					static_cast<uint8>(cellVisualHash(chunk.coord,col,row,7231)%6)
				});
				continue;
			}

			appendUrbanLotDetails(groups, chunk, world, col, row, b, cx, cz, cellSize,roadMasks);
			if (b.type == BuildingType::Parking || b.type == BuildingType::ParkBuilding)
			{
				continue;
			}

			const float height = buildingHeight(b.type) * kLegacyBoxHeightScale;
			if (height <= 0.0f) continue;
			const int gx = chunk.coord.x * ZONE_CELLS + col;
			const int gz = chunk.coord.y * ZONE_CELLS + row;
			const float visualFootprint = footprint * boxBuildingFootprintScale(b.type, gx, gz);
			const float cy = world.sampleHeight(cx, cz) + height * 0.5f;

			MeshData box = MeshData::Box(
				Float3{ cx, cy, cz },
				Float3{ visualFootprint, height, visualFootprint });

			// 最近傍道路の向きに合わせてY軸回転
			rotateBoxVerticesY(box, cx, cz, b.angle);

			auto& dst = groups[static_cast<int>(b.type)];
			const uint32 offset = static_cast<uint32>(dst.vertices.size());
			dst.vertices.append(box.vertices);
			for (const auto& tri : box.indices)
			{
				dst.indices << TriangleIndex32{
					tri.i0 + offset, tri.i1 + offset, tri.i2 + offset };
			}
			appendBoxBuildingDetails(groups, chunk, world, col, row, b.type, cx, cz,
			                         visualFootprint, height, b.angle);
		}
	}

	if (groups.excludedTrees) { DBG_LOG(U"[RailLandscape] key={} excludedTrees={}"_fmt(key, groups.excludedTrees)); }
	m_lotTreeCache[key] = std::move(groups.trees);
	auto& batches = m_buildingMeshCache[key];
	batches.clear();
	for (auto& [typeInt, meshData] : groups.meshes)
	{
		if (meshData.vertices.isEmpty()) continue;
		const ColorF color = (typeInt >= 100)
			? detailColorForKey(typeInt)
			: buildingColor(static_cast<BuildingType>(typeInt));
		batches.push_back({
			typeInt,
			color,
			Mesh{ meshData }
		});
		if (RenderDistance::limitedMaterial(typeInt))
		{
			const Box bounds = batches.back().mesh.boundingBox();
			const auto previous = m_buildingDetailBounds.find(key);
			m_buildingDetailBounds[key] = previous == m_buildingDetailBounds.end()
				? bounds : RenderDistance::merge(previous->second, bounds);
		}
	}

	DBG_LOG(U"[ResidentialAccess] chunk=({}, {}) paths={}"_fmt(chunk.coord.x,chunk.coord.y,accessPaths));
	// The far tier is baked once per edited chunk, grouped by asset material.
	auto& far = m_farBuildings[key]; far = FarBuildings{};
	HashTable<uint64, MeshData> merged;
	HashTable<uint64, Material> materials;
	float highest = chunk.heightMax;
	for (auto& instance : modelInstances)
	{
		auto& asset = getBuildingModelAsset(instance.type, instance.modelVariant);
		instance.distanceBounds = RenderDistance::buildingSphere(RenderDistance::transformBox(
			asset.model.boundingBox(), instance.scale, -instance.angle, Vec3{instance.pos}));
		highest = Max(highest, instance.pos.y + static_cast<float>(asset.model.boundingBox().size.y * instance.scale));
		const uint32 assetId = (static_cast<uint32>(instance.type) << 8) | instance.modelVariant;
		for (size_t part = 0; part < asset.farGeometry.size(); ++part)
		{
			const uint64 materialId = (static_cast<uint64>(assetId) << 32) | part;
			const auto& source = asset.farGeometry[part];
			auto geometry = source.geometry;
			geometry.scale(instance.scale).rotate(Quaternion::RotateY(-instance.angle)).translate(instance.pos);
			appendMeshData(merged[materialId], geometry);
			materials[materialId] = source.material;
		}
		if (!asset.farGeometry.isEmpty()) { ++far.count; }
	}
	far.bounds = Box{Vec3{origin.x + CHUNK_SIZE * .5, (chunk.heightMin + highest) * .5, origin.z + CHUNK_SIZE * .5},
		Vec3{CHUNK_SIZE + 100, Max(1.0f, highest - chunk.heightMin), CHUNK_SIZE + 100}};
	Optional<Box> farBounds;
	for (const auto& [materialId, geometry] : merged)
	{
		far.batches << StaticModelBatch{Mesh{geometry}, materials[materialId], static_cast<uint32>(geometry.indices.size())};
		const Box bounds = far.batches.back().mesh.boundingBox();
		farBounds = farBounds ? RenderDistance::merge(*farBounds,bounds) : bounds;
	}
	far.distanceBounds = farBounds.value_or(far.bounds);
	m_buildingModelCache[key] = std::move(modelInstances);
}

bool WorldRenderer::drawLandscapeBatch(int materialKey,Key key) const
{
	const Point coord{static_cast<int>(key>>32),static_cast<int>(static_cast<uint32>(key))};
	const auto range=m_treeHeightRanges.find(key);
	const Vec2 heights=range==m_treeHeightRanges.end() ? Vec2{} : range->second;
	const bool closeChunk=TreeGeometry::nearChunk(coord,m_buildingEye,heights);
	if (materialKey==128 || materialKey==129) { return !closeChunk; }
	if (materialKey<1000) { return materialKey!=124; }
	if (!closeChunk) { return false; }
	const bool near=TreeGeometry::nearTile(materialKey,coord,m_buildingEye,heights);
	const int material=materialKey%1000;
	return (material==128 || material==129) ? !near : near;
}

bool WorldRenderer::useFarBuildings(Key key, Vec3 eye) const
{
	const auto cached = m_farBuildings.find(key);
	const auto instances = m_buildingModelCache.find(key);
	if (cached == m_farBuildings.end() || instances == m_buildingModelCache.end()) { return false; }
	const auto& far = cached->second;
	constexpr double kFarDistance = 850;
	return !RenderDistance::contains(eye, far.bounds, kFarDistance) && !far.batches.isEmpty()
		&& far.count == instances->second.size();
}

bool WorldRenderer::detailWithinRenderDistance(Key key, Vec3 eye) const
{
	const auto bounds = m_buildingDetailBounds.find(key);
	return bounds == m_buildingDetailBounds.end() || RenderDistance::contains(eye, bounds->second, m_renderDistance);
}

bool WorldRenderer::buildingWithinRenderDistance(const Chunk& chunk, int col, int row, const OrientedBox& bounds, Vec3 eye) const
{
	if (m_renderDistance == RenderDistance::kDefault) { return true; }
	if (col < 0 || row < 0 || col >= ZONE_CELLS || row >= ZONE_CELLS) { return false; }
	const Key key = chunkCoordToKey(chunk.coord);
	if (!isObjBuildingType(chunk.buildingGrid[{col,row}].type)) { return detailWithinRenderDistance(key,eye); }
	if (useFarBuildings(key,eye)) { return RenderDistance::contains(eye,m_farBuildings.at(key).distanceBounds,m_renderDistance); }
	return RenderDistance::contains(eye,RenderDistance::buildingSphere(bounds),m_renderDistance);
}

void WorldRenderer::drawCachedBuildings(Key key)
{
	submitCachedTrees(key);
	bool farDrawn = false;
	if (const auto cached = m_farBuildings.find(key); cached != m_farBuildings.end())
	{
		const auto& far = cached->second;
		if (useFarBuildings(key,m_buildingEye))
		{
			m_buildingsConsidered += far.count;
			farDrawn = true;
			if (RenderDistance::contains(m_buildingEye,far.distanceBounds,m_renderDistance))
			{
				const ScopedCustomShader3D shader{m_buildingShader};
				for (const auto& batch : far.batches)
				{
					if (m_buildingFrustum && !m_buildingFrustum->intersects(batch.mesh.boundingSphere())) { continue; }
					batch.draw(); ++m_buildingDrawCalls; m_buildingTriangles += batch.triangles;
				}
				m_buildingsSubmitted += far.count;
			}
		}
	}
	if (const auto it = m_buildingModelCache.find(key); !farDrawn && it != m_buildingModelCache.end())
	{
		const ScopedCustomShader3D buildingShader{m_buildingShader};
		for (const auto& inst : it->second)
		{
			++m_buildingsConsidered;
			if (!RenderDistance::contains(m_buildingEye,inst.distanceBounds,m_renderDistance)) { continue; }
			BuildingModelAsset& asset = getBuildingModelAsset(inst.type, inst.modelVariant);
			const double modelHeight = asset.model.boundingBox().size.y * inst.scale;
			const Vec3 center = Vec3{ inst.pos } + Vec3{ 0, modelHeight * 0.5, 0 };
			const double radius = Max(12.0, modelHeight * 0.55);
			const Sphere frustumBounds = m_renderDistance == RenderDistance::kDefault ? Sphere{center,radius} : inst.distanceBounds;
			if (m_buildingFrustum && !m_buildingFrustum->intersects(frustumBounds))
			{
				continue;
			}
			++m_buildingsSubmitted;
			if (asset.model.isEmpty()) continue;

			const Mat4x4 worldMat = (Mat4x4::Scale(inst.scale)
			                       * Mat4x4::RotateY(-inst.angle))
				.translated(inst.pos.x, inst.pos.y, inst.pos.z);
			const Model& model = (m_buildingEye.distanceFromSq(Vec3{ inst.pos }) > 250 * 250 && !asset.distantModel.isEmpty())
				? asset.distantModel : asset.model;
			const auto& materials = model.materials();
			for (const auto& obj : model.objects())
			{
				const Transformer3D transform{ worldMat };
				obj.draw(materials);
				m_buildingDrawCalls += obj.parts.size();
			}
			drawFrontage(asset,inst,false);
		}
	}

	for (const auto* cache : { &m_buildingMeshCache, &m_landscapeMeshCache })
	{
		const auto it = cache->find(key);
		if (it == cache->end()) { continue; }
		for (const auto& batch : it->second)
		{
			if (!drawLandscapeBatch(batch.materialKey,key)) { continue; }
			if (RenderDistance::limitedMaterial(batch.materialKey)
				&& (cache == &m_buildingMeshCache ? !detailWithinRenderDistance(key,m_buildingEye)
					: !RenderDistance::contains(m_buildingEye,batch.mesh.boundingBox(),m_renderDistance))) { continue; }
			const int material=batch.materialKey%1000;
			const bool distant=m_distantDetailChunks.contains(key);
			if (distant && material==123) { continue; }
			if ((material==130 && m_fieldShader) || (material==131 && m_paddyShader))
			{
				const ScopedCustomShader3D shader{material==131 ? m_paddyShader : m_fieldShader};
				batch.mesh.draw(TextureAsset(Asset::Sand),ColorF{1});
				continue;
			}
			if ((material==125 || material==126 || material==128 || material==129) && m_foliageShader)
			{
				const ScopedCustomShader3D shader{m_foliageShader};
				batch.mesh.draw(batch.color);
				continue;
			}
			if (material == 100)
			{
				batch.mesh.draw(TextureAsset(Asset::Concrete), batch.color);
			}
			else if (material == 119)
			{
				batch.mesh.draw(TextureAsset(Asset::Asphalt), batch.color);
			}
			else if (material == 120)
			{
				batch.mesh.draw(TextureAsset(Asset::Gravel), batch.color);
			}
			else if (material == 101 || material == 113)
			{
				batch.mesh.draw(TextureAsset(Asset::SparseGrass), batch.color);
			}
			else if (material == 127 || material == 106 || material == 110 || material == 122 || material == 124 || material == 125 || material == 126)
			{
				batch.mesh.draw(TextureAsset(Asset::Grass), batch.color);
			}
			else if (material == 111)
			{
				batch.mesh.draw(TextureAsset(Asset::CoastSand), batch.color);
			}
			else if (material == 114)
			{
				batch.mesh.draw(TextureAsset(Asset::Sand), batch.color);
			}
			else if (material == 117)
			{
				batch.mesh.draw(TextureAsset(Asset::Concrete), batch.color);
			}
			else
			{
				batch.mesh.draw(batch.color);
			}
		}
	}
}

void WorldRenderer::renderShadowCasters(Vec3 focus, double radius) const
{
	m_treeRenderer.clear();
	const double radiusSq = radius * radius;
	for (const auto& [key, instances] : m_buildingModelCache)
	{
		const bool farTier = useFarBuildings(key,m_buildingEye);
		const bool modelsVisible = !farTier || RenderDistance::contains(m_buildingEye,m_farBuildings.at(key).distanceBounds,m_renderDistance);
		for (const auto& instance : instances)
		{
			if (!modelsVisible || (!farTier && !RenderDistance::contains(m_buildingEye,instance.distanceBounds,m_renderDistance))) { continue; }
			const double dx = instance.pos.x - focus.x;
			const double dz = instance.pos.z - focus.z;
			if (dx * dx + dz * dz > radiusSq)
			{
				continue;
			}
			const uint32 assetKey = (static_cast<uint32>(instance.type) << 8) | instance.modelVariant;
			const auto asset = m_buildingModels.find(assetKey);
			if (asset == m_buildingModels.end())
			{
				continue;
			}
			const Mat4x4 transform = (Mat4x4::Scale(instance.scale) * Mat4x4::RotateY(-instance.angle))
				.translated(instance.pos.x, instance.pos.y, instance.pos.z);
			{
				const Transformer3D worldTransform{ transform };
				asset->second.model.draw();
			}
			drawFrontage(asset->second,instance,true);
		}
		if (const auto landscape = m_landscapeMeshCache.find(key); landscape != m_landscapeMeshCache.end())
		{
			const Point coord{static_cast<int>(key>>32),static_cast<int>(static_cast<uint32>(key))};
			if (Vec2{(coord.x+.5)*CHUNK_SIZE,(coord.y+.5)*CHUNK_SIZE}.distanceFrom(Vec2{focus.x,focus.z}) < radius+CHUNK_SIZE)
			{
				for (const auto& batch : landscape->second)
				{
					if (RenderDistance::limitedMaterial(batch.materialKey)
						&& !RenderDistance::contains(m_buildingEye,batch.mesh.boundingBox(),m_renderDistance)) { continue; }
					if (drawLandscapeBatch(batch.materialKey,key)) { batch.mesh.draw(ColorF{1.0}); }
				}
			}
		}
		const auto batches = m_buildingMeshCache.find(key);
		if (batches == m_buildingMeshCache.end())
		{
			continue;
		}
		const Point coord{ static_cast<int>(key >> 32), static_cast<int>(static_cast<uint32>(key)) };
		const Vec2 chunkCenter{ (coord.x + 0.5) * CHUNK_SIZE, (coord.y + 0.5) * CHUNK_SIZE };
		if (chunkCenter.distanceFrom(Vec2{ focus.x, focus.z }) > radius + CHUNK_SIZE)
		{
			continue;
		}
		submitCachedTrees(key);
		for (const auto& batch : batches->second)
		{
			if (RenderDistance::limitedMaterial(batch.materialKey) && !detailWithinRenderDistance(key,m_buildingEye)) { continue; }
			if (drawLandscapeBatch(batch.materialKey,key)) { batch.mesh.draw(ColorF{ 1.0 }); }
		}
	}
	m_treeRenderer.draw({}, true);
}

void WorldRenderer::drawFrontage(const BuildingModelAsset& asset, const BuildingModelInstance& instance, bool shadowPass) const
{
	// Reuse uploaded model-local meshes. Creating a complete town's props inside
	// rebuildBuildingMeshes caused measured 100+ ms streaming stalls.
	constexpr double kFrontageDistance = 110.0;
	if (m_buildingEye.distanceFromSq(Vec3{instance.pos}) > Square(kFrontageDistance)) { return; }
	const Mat4x4 transform=Mat4x4::RotateY(-instance.angle).translated(instance.pos.x,instance.pos.y,instance.pos.z);
	const Transformer3D worldTransform{transform};
	for (const auto& part : asset.frontages[instance.frontageVariant])
	{
		if (shadowPass) { part.mesh.draw(ColorF{1}); }
		else if (part.materialKey==134)
		{
			static const Texture signs{U"assets/buildings/commercial/japan_street_atlas.png",TextureDesc::MippedSRGB};
			part.mesh.draw(signs,ColorF{1});
		}
		else if (part.materialKey==125 && m_foliageShader)
		{
			const ScopedCustomShader3D foliage{m_foliageShader};part.mesh.draw(part.color);
		}
		else { part.mesh.draw(part.color); }
	}
}

void WorldRenderer::setTransportSites(const TrainNetwork& network,const RoadNetwork& roads,const World& world)
{
	if (!m_transportSitesDirty && m_railwayNodeCount==network.nodes().size() && m_railwayEdgeCount==network.edges().size()
		&& m_railwayDepotCount==network.depots().size()) { return; }
	m_railwayNodeCount=network.nodes().size(); m_railwayEdgeCount=network.edges().size(); m_railwayDepotCount=network.depots().size();
	std::unordered_set<Key> changed;
	for (const auto& entry : m_transportSites) { changed.insert(entry.first); }
	m_transportSites.clear();
	for (const auto& footprint : TransportLandscape::footprints(network,roads,world))
	{
		Array<Vec2> outline{footprint.begin(),footprint.end()}; UrbanParcel::normalize(outline);
		const Polygon site{outline}; const RectF bounds=site.boundingRect().stretched(32);
		const int left=static_cast<int>(Floor(bounds.x/CHUNK_SIZE)),right=static_cast<int>(Floor((bounds.x+bounds.w)/CHUNK_SIZE));
		const int top=static_cast<int>(Floor(bounds.y/CHUNK_SIZE)),bottom=static_cast<int>(Floor((bounds.y+bounds.h)/CHUNK_SIZE));
		for (int z=top;z<=bottom;++z) for (int x=left;x<=right;++x)
		{
			const Key key=chunkCoordToKey({x,z}); m_transportSites[key]<<site; changed.insert(key);
		}
	}
	for (const Key key : changed) { m_landscapeMeshCache.erase(key); m_buildingMeshCache.erase(key); m_residentialAccessEntries.erase(key); }
	invalidateTerrainChunkKeys(changed);
	m_transportSitesDirty=false;
}

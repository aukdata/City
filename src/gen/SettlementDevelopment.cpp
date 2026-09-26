#include "GenerationSettings.hpp"
#include "SettlementDevelopment.hpp"
#include "AgriculturalLayout.hpp"
#include "UrbanFacilities.hpp"
#include "StreetBlocks.hpp"
#include "ParcelGeometry.hpp"
#include "UrbanParcel.hpp"
#include "ParcelRoadIndex.hpp"
#include "../road/RoadGeometry.hpp"
#include "../world/ZoneGrid.hpp"
#include "../debug/DebugLog.hpp"

using ZoneGrid::zoneCellKey;
using ZoneGrid::worldToZoneCell;
using ZoneGrid::cellCenterXZ;

namespace
{
	uint32 settlementCellHash(uint64 seed, int settlementIndex, int gx, int gz)
	{
		uint64 value = seed ^ (static_cast<uint64>(settlementIndex) * 0x9E3779B97F4A7C15ULL);
		value ^= static_cast<uint64>(gx) * 0xBF58476D1CE4E5B9ULL;
		value ^= static_cast<uint64>(gz) * 0x94D049BB133111EBULL;
		value ^= value >> 30;
		value *= 0xBF58476D1CE4E5B9ULL;
		value ^= value >> 27;
		value *= 0x94D049BB133111EBULL;
		value ^= value >> 31;
		return static_cast<uint32>(value);
	}

	float hash01(uint64 seed, int settlementIndex, int gx, int gz)
	{
		return static_cast<float>(settlementCellHash(seed, settlementIndex, gx, gz) & 0xFFFFu) / 65535.0f;
	}

	Vec2 landUseAxisX(const MapGenerator::Settlement& settlement, uint64 seed, int settlementIndex)
	{
		if (settlement.gridAxisX.lengthSq() > 1e-6f)
		{
			Vec2 axis = settlement.gridAxisX;
			axis.normalize();
			return axis;
		}
		const float angle = static_cast<float>((seed + settlementIndex * 97) % 6283) * 0.001f;
		return Vec2{ Math::Cos(angle), Math::Sin(angle) };
	}

	float localSlope(const Chunk& chunk, Point chunkCoord, float wx, float wz)
	{
		const float sample = GenerationSettings::get().development_slopeSampleRadius;
		const float hx0 = sampleHeightMap(chunk.heightMap, chunkCoord, wx - sample, wz);
		const float hx1 = sampleHeightMap(chunk.heightMap, chunkCoord, wx + sample, wz);
		const float hz0 = sampleHeightMap(chunk.heightMap, chunkCoord, wx, wz - sample);
		const float hz1 = sampleHeightMap(chunk.heightMap, chunkCoord, wx, wz + sample);
		const float dx = (hx1 - hx0) / (sample * 2.0f);
		const float dz = (hz1 - hz0) / (sample * 2.0f);
		return Math::Sqrt(dx * dx + dz * dz);
	}

	Vec2 planLocal(const MapGenerator::Settlement& settlement, const Vec2& position)
	{
		const Vec2 delta=position-settlement.center;
		return {delta.dot(settlement.gridAxisX),delta.dot(settlement.gridAxisZ)};
	}

	ZoneType pickInitialZone([[maybe_unused]] uint64 seed, [[maybe_unused]] int settlementIndex,
		const MapGenerator::Settlement& settlement, const Chunk& chunk, Point chunkCoord,
		[[maybe_unused]] int globalGX, [[maybe_unused]] int globalGZ, float wx, float wz)
	{
		const float height=sampleHeightMap(chunk.heightMap,chunkCoord,wx,wz);
		const float slope=localSlope(chunk,chunkCoord,wx,wz);
		if (height<GenerationSettings::get().development_minimumZonedHeight || slope>GenerationSettings::get().development_maximumZonedSlope) { return ZoneType::Unzoned; }
		const Vec2 local=planLocal(settlement,{wx,wz});
		const auto use=UrbanMorphology::sample(settlement.plan,local);
		switch (use.district)
		{
		case UrbanMorphology::District::Civic: return ZoneType::UrbanControl;
		case UrbanMorphology::District::Industry: return ZoneType::Industrial;
		case UrbanMorphology::District::OldTown:
		case UrbanMorphology::District::Station: return ZoneType::Commercial;
		case UrbanMorphology::District::PlannedHousing: return ZoneType::Residential;
		case UrbanMorphology::District::Housing:
			return settlement.plan.scale==2 || !UrbanMorphology::inCore(settlement.plan,local,22)
				? ZoneType::LowResidential : ZoneType::Residential;
		default:
			return slope<GenerationSettings::get().development_maximumFarmSlope && UrbanMorphology::contains(settlement.plan,local,GenerationSettings::get().development_agriculturalBeltWidth)
				? ZoneType::Agriculture : ZoneType::Unzoned;
		}
	}

	int zonePriority(ZoneType zone)
	{
		switch (zone)
		{
		case ZoneType::Commercial: return 5;
		case ZoneType::Residential: return 4;
		case ZoneType::LowResidential: return 3;
		case ZoneType::Industrial: return 2;
		case ZoneType::Agriculture: return 1;
		case ZoneType::UrbanControl: return 0;
		default: return -1;
		}
	}
	int nearestSettlementIndex(const Array<MapGenerator::Settlement>& settlements, float wx, float wz)
	{
		int bestIndex = -1;
		float bestScore = 1e30f;
		for (int i = 0; i < static_cast<int>(settlements.size()); ++i)
		{
			const auto& s = settlements[i];
			const float dx = wx - static_cast<float>(s.center.x);
			const float dz = wz - static_cast<float>(s.center.y);
			const float radius = static_cast<float>(Max(1.0,UrbanMorphology::coverageRadius(s.plan)));
			const float score = (dx * dx + dz * dz) / (radius * radius);
			if (score < bestScore)
			{
				bestScore = score;
				bestIndex = i;
			}
		}
		return bestIndex;
	}

	Array<Vec2> makeCellPatchPolygon(Point chunk, int minX, int minY, int width, int height, uint64 salt)
	{
		constexpr float cellSize = static_cast<float>(CHUNK_SIZE) / ZONE_CELLS;
		const float x0 = static_cast<float>(chunk.x * ZONE_CELLS + minX) * cellSize;
		const float z0 = static_cast<float>(chunk.y * ZONE_CELLS + minY) * cellSize;
		const float x1 = static_cast<float>(chunk.x * ZONE_CELLS + minX + width) * cellSize;
		const float z1 = static_cast<float>(chunk.y * ZONE_CELLS + minY + height) * cellSize;
		const float w = x1 - x0;
		const float h = z1 - z0;
		const float cornerInset = Min(w, h) * 0.11f;
		const float edgeInset = Min(w, h) * 0.05f;
		auto noise = [&](int shift, float scale)
		{
			return (static_cast<float>((salt >> shift) & 255u) / 255.0f - 0.5f) * scale;
		};
		return Array<Vec2>{
			Vec2{ x0 + cornerInset + noise(0, edgeInset), z0 + noise(8, edgeInset) },
			Vec2{ x0 + w * (0.48f + noise(16, 0.05f)), z0 + edgeInset + noise(24, edgeInset) },
			Vec2{ x1 - cornerInset + noise(32, edgeInset), z0 + noise(40, edgeInset) },
			Vec2{ x1 - edgeInset + noise(6, edgeInset), z0 + h * (0.48f + noise(14, 0.05f)) },
			Vec2{ x1 - cornerInset + noise(22, edgeInset), z1 + noise(30, edgeInset) },
			Vec2{ x0 + w * (0.52f + noise(38, 0.05f)), z1 - edgeInset + noise(46, edgeInset) },
			Vec2{ x0 + cornerInset + noise(4, edgeInset), z1 + noise(12, edgeInset) },
			Vec2{ x0 + edgeInset + noise(20, edgeInset), z0 + h * (0.52f + noise(28, 0.05f)) }
		};
	}
	Array<Vec2> makeOrientedPatchPolygon(Vec2 center, float width, float depth, float angle, uint64 salt)
	{
		const float hx = width * 0.5f;
		const float hz = depth * 0.5f;
		const float cosA = Math::Cos(angle);
		const float sinA = Math::Sin(angle);
		auto noise = [&](int shift, float scale)
		{
			return (static_cast<float>((salt >> shift) & 255u) / 255.0f - 0.5f) * scale;
		};
		auto transform = [&](float x, float z)
		{
			return Vec2{ center.x + x * cosA - z * sinA, center.y + x * sinA + z * cosA };
		};
		return Array<Vec2>{
			transform(-hx * 0.86f + noise(0, hx * 0.10f), -hz * 0.98f + noise(8, hz * 0.08f)),
			transform(noise(16, hx * 0.12f), -hz * 0.90f + noise(24, hz * 0.06f)),
			transform(hx * 0.92f + noise(32, hx * 0.08f), -hz * 0.72f + noise(40, hz * 0.10f)),
			transform(hx * 0.98f + noise(6, hx * 0.06f), noise(14, hz * 0.12f)),
			transform(hx * 0.76f + noise(22, hx * 0.12f), hz * 0.94f + noise(30, hz * 0.08f)),
			transform(noise(38, hx * 0.14f), hz * 0.86f + noise(46, hz * 0.08f)),
			transform(-hx * 0.94f + noise(4, hx * 0.08f), hz * 0.68f + noise(12, hz * 0.12f)),
			transform(-hx * 0.98f + noise(20, hx * 0.06f), noise(28, hz * 0.10f))
		};
	}
	Array<Vec2> makeCoastalBandPolygon(Point chunk, int minX, int shoreY, int width, int depth, int landwardSign, uint64 salt)
	{
		const int clampedDepth = Max(1, depth);
		const int minY = (landwardSign >= 0) ? shoreY : Max(0, shoreY - clampedDepth + 1);
		const int maxY = (landwardSign >= 0) ? Min(ZONE_CELLS, shoreY + clampedDepth) : Min(ZONE_CELLS, shoreY + 1);
		const Array<Vec2> base = makeCellPatchPolygon(chunk, minX, minY, width, Max(1, maxY - minY), salt);
		if (base.size() < 8) return base;
		const float cellSize = static_cast<float>(CHUNK_SIZE) / ZONE_CELLS;
		const float wave = cellSize * (0.20f + static_cast<float>((salt >> 11) & 7u) * 0.018f);
		auto jitter = [&](int shift)
		{
			return (static_cast<float>((salt >> shift) & 255u) / 255.0f - 0.5f) * wave;
		};
		Array<Vec2> result = base;
		const float shoreZ = static_cast<float>(chunk.y * ZONE_CELLS + shoreY + ((landwardSign >= 0) ? 0 : 1)) * cellSize;
		result[0].y = shoreZ + jitter(3);
		result[1].y = shoreZ + jitter(17);
		result[2].y = shoreZ + jitter(29);
		return result;
	}
	bool hasWaterNeighbor(const Chunk& chunk, int x, int y)
	{
		for (int dy = -1; dy <= 1; ++dy)
		{
			for (int dx = -1; dx <= 1; ++dx)
			{
				if (dx == 0 && dy == 0) continue;
				const int nx = x + dx;
				const int ny = y + dy;
				if (nx < 0 || ny < 0 || nx >= ZONE_CELLS || ny >= ZONE_CELLS) continue;
				if (chunk.heightMap[ny][nx] < -0.35f) return true;
			}
		}
		return false;
	}
	float infillDensity(MapGenerator::SettlementKind kind, ZoneType zone)
	{
		if (kind == MapGenerator::SettlementKind::RegionalCity)
		{
			if (zone == ZoneType::Commercial) return GenerationSettings::get().development_infill_city_Commercial;
			if (zone == ZoneType::Residential) return GenerationSettings::get().development_infill_city_Residential;
			if (zone == ZoneType::LowResidential) return GenerationSettings::get().development_infill_city_LowResidential;
			if (zone == ZoneType::Industrial) return GenerationSettings::get().development_infill_city_Industrial;
		}
		if (kind == MapGenerator::SettlementKind::LocalTown)
		{
			if (zone == ZoneType::Commercial) return GenerationSettings::get().development_infill_town_Commercial;
			if (zone == ZoneType::Residential) return GenerationSettings::get().development_infill_town_Residential;
			if (zone == ZoneType::LowResidential) return GenerationSettings::get().development_infill_town_LowResidential;
			if (zone == ZoneType::Industrial) return GenerationSettings::get().development_infill_town_Industrial;
		}
		if (zone == ZoneType::Residential) return GenerationSettings::get().development_infill_village_Residential;
		if (zone == ZoneType::LowResidential) return GenerationSettings::get().development_infill_village_LowResidential;
		return 0.0f;
	}
}
// =============================================================================
// ゾーン一括割り当て
// =============================================================================

void SettlementDevelopment::applyZonesGlobal()
{
	const Stopwatch sw{ StartImmediately::Yes };
	constexpr float cellSize = static_cast<float>(CHUNK_SIZE) / ZONE_CELLS;

	for (int si = 0; si < static_cast<int>(m_districts.size()); ++si)
	{
		const auto& settlement = m_districts[si];
		const float outerDist=static_cast<float>(UrbanMorphology::coverageRadius(settlement.plan)+GenerationSettings::get().development_zoneCoverageMargin);

		const float scx = static_cast<float>(settlement.center.x);
		const float scz = static_cast<float>(settlement.center.y);
		const int chunkRadius = static_cast<int>(Ceil(outerDist / CHUNK_SIZE)) + 1;
		const int ccx = static_cast<int>(Math::Floor(scx / CHUNK_SIZE));
		const int ccz = static_cast<int>(Math::Floor(scz / CHUNK_SIZE));

		for (int dcy = -chunkRadius; dcy <= chunkRadius; ++dcy)
		{
			for (int dcx = -chunkRadius; dcx <= chunkRadius; ++dcx)
			{
				const Point cc{ ccx + dcx, ccz + dcy };
				Chunk* chunk = m_world.getChunk(cc);
				if (!chunk) continue;

				const float chunkOriginX = static_cast<float>(cc.x * CHUNK_SIZE);
				const float chunkOriginZ = static_cast<float>(cc.y * CHUNK_SIZE);
				const int gxMin = Max(0, static_cast<int>((scx - outerDist - chunkOriginX) / cellSize));
				const int gxMax = Min(ZONE_CELLS - 1, static_cast<int>((scx + outerDist - chunkOriginX) / cellSize));
				const int gzMin = Max(0, static_cast<int>((scz - outerDist - chunkOriginZ) / cellSize));
				const int gzMax = Min(ZONE_CELLS - 1, static_cast<int>((scz + outerDist - chunkOriginZ) / cellSize));
				if (gxMin > gxMax || gzMin > gzMax) continue;

				for (int gz = gzMin; gz <= gzMax; ++gz)
				{
					for (int gx = gxMin; gx <= gxMax; ++gx)
					{
						const float wx = chunkOriginX + (gx + 0.5f) * cellSize;
						const float wz = chunkOriginZ + (gz + 0.5f) * cellSize;
						const int globalGX = cc.x * ZONE_CELLS + gx;
						const int globalGZ = cc.y * ZONE_CELLS + gz;
						const ZoneType candidate = pickInitialZone(m_seed, si, settlement,
							*chunk, cc, globalGX, globalGZ, wx, wz);
						if (candidate == ZoneType::Unzoned ||
							(candidate == ZoneType::Agriculture &&
								!m_options.enabled(GenerationOptions::Element::Farms)))
						{
							continue;
						}

						ZoneType& current = chunk->zoneMap[{ gx, gz }];
						if (zonePriority(candidate) >= zonePriority(current))
						{
							current = candidate;
						}
					}
				}
			}
		}
	}

	Logger << U"[applyZonesGlobal] realistic land-use {:.0f}ms"_fmt(sw.msF());
}
// =============================================================================
// 初期建物配置
// =============================================================================

namespace
{
	namespace InitialBuilding
	{
		float density(MapGenerator::SettlementKind kind, ZoneType zone)
		{
			if (kind == MapGenerator::SettlementKind::RegionalCity)
			{
				if (zone == ZoneType::Commercial) return GenerationSettings::get().development_initial_city_Commercial;
				if (zone == ZoneType::Residential) return GenerationSettings::get().development_initial_city_Residential;
				if (zone == ZoneType::LowResidential) return GenerationSettings::get().development_initial_city_LowResidential;
				if (zone == ZoneType::Industrial) return GenerationSettings::get().development_initial_city_Industrial;
				if (zone == ZoneType::Agriculture) return GenerationSettings::get().development_initial_city_Agriculture;
			}
			else if (kind == MapGenerator::SettlementKind::LocalTown)
			{
				if (zone == ZoneType::Commercial) return GenerationSettings::get().development_initial_town_Commercial;
				if (zone == ZoneType::Residential) return GenerationSettings::get().development_initial_town_Residential;
				if (zone == ZoneType::LowResidential) return GenerationSettings::get().development_initial_town_LowResidential;
				if (zone == ZoneType::Industrial) return GenerationSettings::get().development_initial_town_Industrial;
				if (zone == ZoneType::Agriculture) return GenerationSettings::get().development_initial_town_Agriculture;
			}
			else
			{
				if (zone == ZoneType::Residential) return GenerationSettings::get().development_initial_village_Residential;
				if (zone == ZoneType::LowResidential) return GenerationSettings::get().development_initial_village_LowResidential;
				if (zone == ZoneType::Agriculture) return GenerationSettings::get().development_initial_village_Agriculture;
			}
			return 0.0f;
		}

		Building spawn(ZoneType zone, MapGenerator::SettlementKind kind, GameTime gameNow, uint32 hash)
		{
			Building building;
			building.builtAt = gameNow;
			const uint32 roll = hash % 100u;
			switch (zone)
			{
			case ZoneType::LowResidential:
				if (roll < GenerationSettings::get().development_choice_LowResidential_5) building.type = BuildingType::ParkBuilding;
				else if (roll < GenerationSettings::get().development_choice_LowResidential_9) building.type = BuildingType::Parking;
				else building.type = (roll < (kind == MapGenerator::SettlementKind::RuralSettlement ? GenerationSettings::get().development_choice_LowResidential_94 : GenerationSettings::get().development_choice_LowResidential_82))
					? BuildingType::Detached : BuildingType::LowApartment;
				break;
			case ZoneType::Residential:
				if (roll < GenerationSettings::get().development_choice_Residential_5) building.type = BuildingType::ParkBuilding;
				else if (roll < GenerationSettings::get().development_choice_Residential_10) building.type = BuildingType::PublicFacility;
				else if (roll < GenerationSettings::get().development_choice_Residential_11) building.type = BuildingType::MidApartment;
				else building.type = (roll < GenerationSettings::get().development_choice_Residential_84) ? BuildingType::Detached : BuildingType::LowApartment;
				break;
			case ZoneType::Commercial:
				if (roll < GenerationSettings::get().development_choice_Commercial_8) building.type = BuildingType::Parking;
				else if (roll < GenerationSettings::get().development_choice_Commercial_13) building.type = BuildingType::PublicFacility;
				else building.type = (roll < GenerationSettings::get().development_choice_Commercial_86) ? BuildingType::Shop : BuildingType::Office;
				break;
			case ZoneType::Industrial:
				if (roll < GenerationSettings::get().development_choice_Industrial_12) building.type = BuildingType::Parking;
				else if (roll < GenerationSettings::get().development_choice_Industrial_18) building.type = BuildingType::PublicFacility;
				else building.type = BuildingType::Factory;
				break;
			case ZoneType::Agriculture:
				building.type = BuildingType::Farmland;
				break;
			default:
				building.type = BuildingType::None;
				break;
			}
			return building;
		}
		void applySettlementContext(Building& building, const MapGenerator::Settlement& settlement,
			const Vec2& position, uint32 hash)
		{
			const auto use=UrbanMorphology::sample(settlement.plan,planLocal(settlement,position));
			const uint32 roll=(hash/17u)%100u;
			const Vec2 local=planLocal(settlement,position);
			const auto nearStation=[&](Vec2 station)
			{
				return Abs(local.x-station.x)<GenerationSettings::get().development_stationKeepoutX
					&& Abs(local.y-station.y)<GenerationSettings::get().development_stationKeepoutZ;
			};
			const bool reservedStation=settlement.plan.structure==UrbanStructure::Type::None
				? (settlement.plan.station && nearStation(*settlement.plan.station))
				: settlement.plan.centers.any([&](const UrbanStructure::Center& center) { return center.rail && nearStation(center.position); });
			if (reservedStation)
			{
				building.type=BuildingType::None; return;
			}
			switch (use.district)
			{
			case UrbanMorphology::District::Civic: building.type=BuildingType::None; break;
			case UrbanMorphology::District::Industry:
				building.type=roll<GenerationSettings::get().development_context_Industry_92 ? BuildingType::Factory : BuildingType::Parking; break;
			case UrbanMorphology::District::Station:
				building.type=roll<GenerationSettings::get().development_context_Station_38 ? BuildingType::Office : (roll<GenerationSettings::get().development_context_Station_66 ? BuildingType::MidApartment
					: (roll<GenerationSettings::get().development_context_Station_78 && settlement.plan.scale==0 ? BuildingType::HighApartment : BuildingType::Shop)); break;
			case UrbanMorphology::District::OldTown:
				building.type=roll<GenerationSettings::get().development_context_OldTown_78 ? BuildingType::Shop : (roll<GenerationSettings::get().development_context_OldTown_91 ? BuildingType::MidApartment : BuildingType::LowApartment); break;
			case UrbanMorphology::District::PlannedHousing:
				building.type=roll<GenerationSettings::get().development_context_PlannedHousing_62 ? BuildingType::MidApartment : (roll<GenerationSettings::get().development_context_PlannedHousing_82 ? BuildingType::LowApartment
					: (roll<GenerationSettings::get().development_context_PlannedHousing_96 ? BuildingType::Detached : BuildingType::PublicFacility)); break;
			case UrbanMorphology::District::Housing:
				if (!UrbanMorphology::inCore(settlement.plan,local,22))
				{
					building.type=roll<GenerationSettings::get().development_context_Housing_92 ? BuildingType::Detached : BuildingType::LowApartment;
				}
				else
				{
					building.type=roll<(settlement.plan.scale==2 ? GenerationSettings::get().development_context_Housing_94 : (settlement.plan.scale==1 ? GenerationSettings::get().development_context_Housing_82 : GenerationSettings::get().development_context_Housing_58)) ? BuildingType::Detached
						: (roll<GenerationSettings::get().development_context_Housing_84 ? BuildingType::LowApartment : BuildingType::MidApartment);
				}
				break;
			default: building.type=BuildingType::None; break;
			}
			if (use.district==UrbanMorphology::District::Housing)
			{
				const double dense=UrbanMorphology::downtownIntensity(settlement.plan,local);
				if (dense>0)
				{
					const double detached= Math::Lerp(GenerationSettings::get().development_context_Housing_58,
						GenerationSettings::get().urbanFabric_downtownDetachedPercent,dense);
					const double lowApartment=Math::Lerp(GenerationSettings::get().development_context_Housing_84,
						GenerationSettings::get().urbanFabric_downtownLowApartmentPercent,dense);
					building.type=roll<detached ? BuildingType::Detached : (roll<lowApartment ? BuildingType::LowApartment : BuildingType::MidApartment);
				}
			}
			if (settlement.plan.origin==UrbanMorphology::Origin::Planned)
			{
				// 生活中心には公共施設も混ぜ、住宅と商店だけの団地にしない。
				if (use.district==UrbanMorphology::District::OldTown && roll<GenerationSettings::get().development_choice_Commercial_13) { building.type=BuildingType::PublicFacility; }
				// 住棟の高さを住区内で揃え、外周の戸建てと中心側の集合住宅を混ぜ散らさない。
				if (use.district==UrbanMorphology::District::PlannedHousing) { building.type=roll<GenerationSettings::get().urbanFabric_newTownMidrisePercent ? BuildingType::MidApartment : BuildingType::LowApartment; }
				if (use.district==UrbanMorphology::District::Housing) { building.type=roll<GenerationSettings::get().urbanFabric_newTownDetachedPercent ? BuildingType::Detached : BuildingType::LowApartment; }
			}
			UrbanStructure::adaptBuilding(building,settlement.plan,local,roll);
			// Small local towns have only occasional mid-rise buildings near the station.
			if (settlement.plan.scale==1)
			{
				const bool stationLandmark=use.district==UrbanMorphology::District::Station && (hash&7u)==0;
				if (building.type==BuildingType::MidApartment && !stationLandmark) { building.type=BuildingType::LowApartment; }
				if (building.type==BuildingType::LowApartment && !stationLandmark && (hash/29u)%5u!=0u)
				{
					building.type=use.district==UrbanMorphology::District::OldTown ? BuildingType::Shop : BuildingType::Detached;
				}
				if (building.type==BuildingType::HighApartment || building.type==BuildingType::OfficeTower) { building.type=BuildingType::LowApartment; }
				if (building.type==BuildingType::Office && !stationLandmark) { building.type=BuildingType::Shop; }
			}
			if (building.type == BuildingType::Detached
				&& (settlement.plan.scale == 2 || !UrbanMorphology::inCore(settlement.plan, local,GenerationSettings::get().development_ruralHouseCoreMargin))
				&& (hash / 101u) % 100u < GenerationSettings::get().development_context_Housing_70)
			{
				building.type = BuildingType::RuralHouse;
			}
			// A dense city lot holds two narrow dwellings within one simulation cell.
			if (building.type==BuildingType::Detached && settlement.plan.scale==0
				&& settlement.plan.origin!=UrbanMorphology::Origin::Planned
				&& use.district==UrbanMorphology::District::Housing)
			{
				const double dense=UrbanMorphology::downtownIntensity(settlement.plan,local);
				if ((hash/67u)%100u < static_cast<uint32>(dense*90.0)) { building.type=BuildingType::UrbanHousePair; }
			}
			if (settlement.plan.scale<=1 && UrbanMorphology::inCore(settlement.plan,local)
				&& (use.district==UrbanMorphology::District::Housing || use.district==UrbanMorphology::District::PlannedHousing))
			{
				// Everyday shops, small parking lots and pocket parks punctuate residential streets.
				const uint32 mix=(hash/151u)%1000u;
				const uint32 shop=settlement.plan.scale==0 ? 25u : 18u;
				const uint32 baseParking=settlement.plan.scale==0 ? 55u : 42u;
				const uint32 basePark=settlement.plan.scale==0 ? 75u : 55u;
				const double remainingOpenLots=1.0-UrbanMorphology::downtownIntensity(settlement.plan,local);
				const uint32 parking=shop+static_cast<uint32>((baseParking-shop)*remainingOpenLots);
				const uint32 park=parking+static_cast<uint32>((basePark-baseParking)*remainingOpenLots);
				if (mix<shop) { building.type=BuildingType::Shop; }
				else if (mix<parking) { building.type=BuildingType::Parking; }
				else if (mix<park) { building.type=BuildingType::ParkBuilding; }
			}
		}

		BuildingType ruralFringeBuildingType(MapGenerator::SettlementKind kind, float distFromCenter, float settlementRadius, uint32 hash)
		{
			const float fringeStart = settlementRadius * (kind == MapGenerator::SettlementKind::RuralSettlement ? GenerationSettings::get().development_villageFringeStart : GenerationSettings::get().development_townFringeStart);
			const float fringeEnd = settlementRadius * (kind == MapGenerator::SettlementKind::RuralSettlement ? GenerationSettings::get().development_villageFringeEnd : GenerationSettings::get().development_townFringeEnd);
			if (distFromCenter < fringeStart || distFromCenter > fringeEnd)
			{
				return BuildingType::Farmland;
			}

			const uint32 roll = (hash / 37u) % 100u;
			if (roll < GenerationSettings::get().development_ruralFringe_detached) return BuildingType::Detached;
			if (roll < GenerationSettings::get().development_ruralFringe_factory) return BuildingType::Factory;
			if (roll < GenerationSettings::get().development_ruralFringe_parking) return BuildingType::Parking;
			if (roll < GenerationSettings::get().development_ruralFringe_park) return BuildingType::ParkBuilding;
			return BuildingType::Farmland;
		}
	}
	

	String buildingTomlPathFromStem(const String& stem)
	{
		if (stem.starts_with(U"residential_") || stem.starts_with(U"rural_house_"))
			return U"assets/buildings/residential/{}.toml"_fmt(stem);
		return U"assets/buildings/commercial/{}.toml"_fmt(stem);
	}

	float loadSetbackFromToml(const String& stem)
	{
		const String tomlPath = buildingTomlPathFromStem(stem);
		const TOMLReader toml{ tomlPath };
		if (!toml)
		{
			Console << U"[BuildingSetback] TOML not found/invalid: " << tomlPath
			        << U" (fallback=" << GenerationSettings::get().development_defaultBuildingSetbackM << U"m)";
			return GenerationSettings::get().development_defaultBuildingSetbackM;
		}

		const double setback = toml[U"setback_from_road_m"].getOr<double>(
			toml[U"setback_m"].getOr<double>(GenerationSettings::get().development_defaultBuildingSetbackM));
		return static_cast<float>(Max(0.0, setback));
	}

	float setbackFromRoadByModel(BuildingType type, int gx, int gz)
	{
		String stem;
		if (!tryGetBuildingModelStem(type, gx, gz, stem))
			return GenerationSettings::get().development_defaultBuildingSetbackM;

		static HashTable<String, float> s_cache;
		if (const auto it = s_cache.find(stem); it != s_cache.end())
			return it->second;

		const float value = loadSetbackFromToml(stem);
		s_cache[stem] = value;
		return value;
	}


	Vec2 interiorBuildingOffset(uint32 hash, float maxOffset)
	{
		const float ox = (static_cast<float>(hash & 0xFFu) / 255.0f - 0.5f) * maxOffset * 2.0f;
		const float oz = (static_cast<float>((hash >> 8) & 0xFFu) / 255.0f - 0.5f) * maxOffset * 2.0f;
		return Vec2{ ox, oz };
	}
	struct EdgeProjection
	{
		float edgeT = 0.0f;
		Vec2  position{ 0.0f, 0.0f };
		Vec2  tangent{ 1.0f, 0.0f };
		Vec2  right{ 0.0f, -1.0f };
		float angle = 0.0f;
		float distance = 0.0f;
	};

	bool projectPointToEdgeXZ(const RoadNetwork& network, int edgeId, const Vec2& point, EdgeProjection& out)
	{
		const auto bez = network.getBezier(edgeId);
		if (!bez || bez->totalLength <= 1e-3f) return false;

		const int sampleCount = Max(32, static_cast<int>(Ceil(bez->totalLength / 8.0f)));
		float bestArc = 0.0f;
		double bestDistSq = Math::Inf;
		int bestIndex = 0;

		for (int i = 0; i <= sampleCount; ++i)
		{
			const float arc = bez->totalLength * (static_cast<float>(i) / sampleCount);
			const Vec3 pos = bez->positionAt(arc);
			const double dx = pos.x - point.x;
			const double dz = pos.z - point.y;
			const double distSq = dx * dx + dz * dz;
			if (distSq < bestDistSq)
			{
				bestDistSq = distSq;
				bestArc = arc;
				bestIndex = i;
			}
		}

		float lo = bez->totalLength * (Max(bestIndex - 1, 0) / static_cast<float>(sampleCount));
		float hi = bez->totalLength * (Min(bestIndex + 1, sampleCount) / static_cast<float>(sampleCount));
		for (int iter = 0; iter < 24; ++iter)
		{
			const float m1 = lo + (hi - lo) / 3.0f;
			const float m2 = hi - (hi - lo) / 3.0f;
			const Vec3 p1 = bez->positionAt(m1);
			const Vec3 p2 = bez->positionAt(m2);
			const double d1 = (p1.x - point.x) * (p1.x - point.x) + (p1.z - point.y) * (p1.z - point.y);
			const double d2 = (p2.x - point.x) * (p2.x - point.x) + (p2.z - point.y) * (p2.z - point.y);
			if (d1 < d2) hi = m2;
			else lo = m1;
		}

		bestArc = (lo + hi) * 0.5f;
		const Vec3 pos = bez->positionAt(bestArc);
		const Vec3 tan = bez->tangentAt(bestArc);
		Vec2 tangent{ static_cast<float>(tan.x), static_cast<float>(tan.z) };
		if (tangent.lengthSq() <= 1e-8f) return false;
		tangent.normalize();

		out.edgeT = Clamp(bestArc / bez->totalLength, 0.0f, 1.0f);
		out.position = Vec2{ static_cast<float>(pos.x), static_cast<float>(pos.z) };
		out.tangent = tangent;
		out.right = Vec2{ tangent.y, -tangent.x };
		out.angle = static_cast<float>(std::atan2(tangent.y, tangent.x));
		out.distance = static_cast<float>(Math::Sqrt(
			(out.position.x - point.x) * (out.position.x - point.x)
			+ (out.position.y - point.y) * (out.position.y - point.y)));
		return true;
	}

	struct EdgeFacingSlot
	{
		Point chunkCoord;
		int   col = 0;
		int   row = 0;
		int   edgeId = -1;
		float edgeT = 0.0f;
		float angle = 0.0f;
		float halfWidth = 0.0f;
		float roadDist = 0.0f;
		float frontageScore = 0.0f; ///< 道路端までの距離。近接する幹線を優先。
		Vec2  roadToCellDir{ 1.0f, 0.0f };
		float centerDistSq = 0.0f;
		Vec2 roadPosition{ 0, 0 };
		double urbanIntensity=0; ///< 敷地の後退距離に適用する中心街の密度。
	};
	Vec2 roadsideBuildingOffset(const EdgeFacingSlot& slot, BuildingType type, int gx, int gz)
	{
		constexpr float kCellSize = static_cast<float>(CHUNK_SIZE) / ZONE_CELLS;
		const Vec2 cellCenter{
			slot.chunkCoord.x * CHUNK_SIZE + (slot.col + 0.5f) * kCellSize,
			slot.chunkCoord.y * CHUNK_SIZE + (slot.row + 0.5f) * kCellSize };
		const float targetDistance = slot.halfWidth + buildingFootprintXZ(type) * 0.5f
			+ static_cast<float>(Math::Lerp(static_cast<double>(Max(1.2f, setbackFromRoadByModel(type, gx, gz))),
				GenerationSettings::get().urbanFabric_downtownSetback,isCompleteSiteBuilding(type) ? 0.0 : slot.urbanIntensity));
		return slot.roadPosition + slot.roadToCellDir * targetDistance - cellCenter;
	}


	bool hasRoadClearance(
		const RoadNetwork& network,
		int edgeId,
		const Vec2& position,
		float halfBuilding,
		float minimumSetback)
	{
		EdgeProjection projection;
		if (!projectPointToEdgeXZ(network, edgeId, position, projection))
		{
			return false;
		}
		const RoadEdge* edge = network.getEdge(edgeId);
		if (!edge || !edge->isRoadbedBuilt())
		{
			return false;
		}
		const RoadGeometry::LateralRange range = RoadGeometry::structuralRangeAt(*edge, projection.edgeT);
		if (!range.valid)
		{
			return false;
		}
		const Vec2 toPosition = position - projection.position;
		const float signedLateral = static_cast<float>(toPosition.dot(projection.right));
		const float roadOuter = (signedLateral >= 0.0f) ? Max(0.0f, range.right) : Max(0.0f, -range.left);
		return Math::Abs(signedLateral) >= roadOuter + halfBuilding + minimumSetback;
	}
	bool overlapsExistingBuilding(
		const World& world,
		Point cc,
		int gx,
		int gz,
		float wx,
		float wz,
		float halfBuilding,
		float angle)
	{
		constexpr float cellSize = static_cast<float>(CHUNK_SIZE) / ZONE_CELLS;
		const int checkRange =
			static_cast<int>(Ceil((halfBuilding + maximumBuildingFootprint() * .5f + 16.0f) / cellSize)) + 2;
		for (int oy = -checkRange; oy <= checkRange; ++oy)
		{
			for (int ox = -checkRange; ox <= checkRange; ++ox)
			{
				int nx = gx + ox;
				int nz = gz + oy;
				Point ncc = cc;
				if (nx < 0) { nx += ZONE_CELLS; --ncc.x; }
				else if (nx >= ZONE_CELLS) { nx -= ZONE_CELLS; ++ncc.x; }
				if (nz < 0) { nz += ZONE_CELLS; --ncc.y; }
				else if (nz >= ZONE_CELLS) { nz -= ZONE_CELLS; ++ncc.y; }

				const Chunk* nchunk = world.getChunk(ncc);
				if (!nchunk) continue;
				const Building& nb = nchunk->buildingGrid[{ nx, nz }];
				if (nb.type == BuildingType::None) continue;

				const Vec2 ncenter = cellCenterXZ(ncc, nx, nz) + Vec2{ nb.offsetX, nb.offsetZ };
				const float nHalf = buildingFootprintXZ(nb.type) * 0.5f + 0.25f;
				if (ParcelGeometry::overlaps(ParcelGeometry::footprint(Vec2{ wx, wz }, halfBuilding + 0.25f, angle),
					ParcelGeometry::footprint(ncenter, nHalf, nb.angle)))
				{
					return true;
				}
			}
		}
		return false;
	}

	Array<EdgeFacingSlot> collectEdgeFacingSlots(
		const MapGenerator::Settlement& settlement,
		const World& world,
		const RoadNetwork& network)
	{
		const float halfBuilding = buildingFootprintXZ() * 0.5f;
		const float offsetFromEdge = halfBuilding + GenerationSettings::get().development_defaultBuildingSetbackM;
		const Vec2 center{ settlement.center.x, settlement.center.y };

		HashTable<int64, EdgeFacingSlot> bestByCell;

		for (const auto& edge : network.edges())
		{
			if (edge.id < 0 || !edge.hasRoadLanes() || !edge.isRoadbedBuilt()) continue;
			if (edge.roadType != RoadType::LocalRoad && edge.roadType != RoadType::Arterial) continue;

			const auto bez = network.getBezier(edge.id);
			if (!bez || bez->totalLength <= 1.0f) continue;
			const float edgeHalfWidth = edge.totalWidth() * 0.5f;
			const Vec3 middle=bez->positionAt(bez->totalLength*0.5f);
			const Vec2 localMiddle=planLocal(settlement,{middle.x,middle.z});
			const auto use=UrbanMorphology::sample(settlement.plan,localMiddle);
			const double downtown=UrbanMorphology::downtownIntensity(settlement.plan,localMiddle);
			const auto& settings=GenerationSettings::get();
			const float baseFrontage=Min(settlement.plan.scale<2 ? settings.development_urbanFrontagePitch : settings.development_ruralFrontagePitch,static_cast<float>(use.frontage));
			const float frontagePitch=settlement.plan.origin==UrbanMorphology::Origin::Planned ? static_cast<float>(use.frontage)
				: static_cast<float>(Math::Lerp(baseFrontage,settings.urbanFabric_downtownFrontagePitch,downtown));
			// Compact corners are still checked against every road polygon before placement.
			const float cornerSetback=static_cast<float>(Math::Lerp(static_cast<double>(settings.development_cornerSetback),settings.urbanFabric_downtownCornerSetback,downtown));
			const float startArc = edge.cutoffA + cornerSetback;
			const float endArc = bez->totalLength - edge.cutoffB - cornerSetback;
			if (endArc < startArc) { continue; }
			const int sampleCount = Max(1, static_cast<int>(Floor((endArc - startArc) / frontagePitch)) + 1);

			for (int i = 0; i < sampleCount; ++i)
			{
				const float t = sampleCount == 1 ? 0.5f : static_cast<float>(i) / (sampleCount - 1);
				const float arc = Math::Lerp(startArc, endArc, t);
				const Vec3 pos = bez->positionAt(arc);
				const Vec3 tan = bez->tangentAt(arc);
				Vec2 tangent{ static_cast<float>(tan.x), static_cast<float>(tan.z) };
				if (tangent.lengthSq() <= 1e-8f) continue;
				tangent.normalize();
				const Vec2 right{ tangent.y, -tangent.x };


				if (!UrbanMorphology::contains(settlement.plan,planLocal(settlement,{pos.x,pos.z}),GenerationSettings::get().development_frontageCoverageMargin)) { continue; }

				for (const float side : { -1.0f, 1.0f })
				{
					const Vec2 slotPos = Vec2{ static_cast<float>(pos.x), static_cast<float>(pos.z) }
						+ right * side * (edgeHalfWidth + offsetFromEdge);

					Point cc;
					int gx = 0, gz = 0;
					worldToZoneCell(static_cast<float>(slotPos.x), static_cast<float>(slotPos.y), cc, gx, gz);

					const Chunk* chunk = world.getChunk(cc);
					if (!chunk) continue;
					const ZoneType zone = chunk->zoneMap[{ gx, gz }];
					if (zone == ZoneType::Unzoned) continue;

					const Vec2 cellCenter = cellCenterXZ(cc, gx, gz);
					EdgeProjection projection;
					if (!projectPointToEdgeXZ(network, edge.id, slotPos, projection)) continue;

					const Vec2 toCell = slotPos - projection.position;
					if (toCell.lengthSq() <= 1e-6f) continue;
					if ((toCell.dot(projection.right) * side) <= 1e-4f) continue;
					const Vec2 roadToCellDir = toCell.normalized();

					const float centerDx = static_cast<float>(cellCenter.x) - static_cast<float>(center.x);
					const float centerDz = static_cast<float>(cellCenter.y) - static_cast<float>(center.y);

					EdgeFacingSlot slot;
					slot.chunkCoord = cc;
					slot.col = gx;
					slot.row = gz;
					slot.edgeId = edge.id;
					slot.edgeT = projection.edgeT;
					slot.angle = static_cast<float>(std::atan2(-roadToCellDir.x, roadToCellDir.y));
					slot.halfWidth = edgeHalfWidth;
					slot.roadDist = static_cast<float>(cellCenter.distanceFrom(projection.position));
					slot.frontageScore = Max(0.0f,slot.roadDist-edgeHalfWidth)
						- (edge.roadType==RoadType::Arterial || edge.totalWidth()>=18.0f ? 4.0f : 0.0f);
					slot.roadToCellDir = roadToCellDir;
					slot.centerDistSq = centerDx * centerDx + centerDz * centerDz;
					slot.roadPosition = projection.position;
					slot.urbanIntensity=UrbanMorphology::downtownIntensity(settlement.plan,planLocal(settlement,slotPos));

					const int64 key = zoneCellKey(cc, gx, gz);
					const auto it = bestByCell.find(key);
					if (it == bestByCell.end()
					 || slot.frontageScore < it->second.frontageScore
					 || (Math::Abs(slot.frontageScore - it->second.frontageScore) < 1e-4f && slot.centerDistSq < it->second.centerDistSq))
					{
						bestByCell[key] = slot;
					}
				}
			}
		}

		Array<EdgeFacingSlot> slots;
		slots.reserve(bestByCell.size());
		for (const auto& [_, slot] : bestByCell) slots << slot;
		slots.sort_by([](const EdgeFacingSlot& a, const EdgeFacingSlot& b)
		{
			if (a.centerDistSq != b.centerDistSq) return a.centerDistSq < b.centerDistSq;
			if (a.chunkCoord.x != b.chunkCoord.x) return a.chunkCoord.x < b.chunkCoord.x;
			if (a.chunkCoord.y != b.chunkCoord.y) return a.chunkCoord.y < b.chunkCoord.y;
			if (a.row != b.row) return a.row < b.row;
			return a.col < b.col;
		});
		return slots;
	}
}

SettlementDevelopment::Validation SettlementDevelopment::placeInitialBuildings(bool preserveLandPatches)
{
	if (!m_options.enabled(GenerationOptions::Element::Buildings))
	{
		return {true, U"建物の初期生成なし"};
	}
	if (!preserveLandPatches && m_options.enabled(GenerationOptions::Element::Farms))
	{
		const auto farms=AgriculturalLayout::prepare(m_world,m_network,m_seed,agriculturalFrames(),&m_trainNetwork);
		DebugLog::print(U"[AgriculturalAccess] roads={} homes={}"_fmt(farms.tracks,farms.homes));
	}
	const Stopwatch sw{ StartImmediately::Yes };

	int placed = UrbanFacilities::generate(m_world, m_network, m_trainNetwork, m_districts, m_seed).placed;
	int rejectedRoad = 0;
	int rejectedGreen = 0;
	int rejectedSlope = 0;
	int rejectedDensity = 0, rejectedClearance = 0, rejectedNeighbor = 0, candidateSlots = 0;
	ParcelRoadIndex roadIndex{ m_network,true };
	roadIndex.addRailway(m_trainNetwork);
	auto isBuildableFootprint = [&](const Building& building, Vec2 center, float maximumRelief=GenerationSettings::get().development_maximumBuildingRelief)
	{
		const auto footprint = ParcelGeometry::footprint(center, buildingFootprintXZ(building.type) * 0.5 + GenerationSettings::get().development_footprintMargin, building.angle);
		if (roadIndex.overlaps(footprint))
		{
			++rejectedRoad;
			return false;
		}
		// セル中心から接道位置へ移した後の建物全体で確認し、緑道沿いの角地へのはみ出しを防ぐ。
		for (const auto& district:m_districts)
		{
			if (district.plan.origin!=UrbanMorphology::Origin::Planned && district.plan.neighborhoodParks.isEmpty()) { continue; }
			for (const Vec2 corner:footprint)
			{
				if (UrbanMorphology::isReservedGreen(district.plan,planLocal(district,corner))) { ++rejectedGreen; return false; }
			}
		}
		float minHeight = Math::Inf, maxHeight = -Math::Inf;
		for (const Vec2& corner : footprint)
		{
			const float height = m_world.sampleHeight(static_cast<float>(corner.x), static_cast<float>(corner.y));
			if (height<m_world.waterSurfaceHeight(corner.x,corner.y)+GenerationSettings::get().development_buildingFreeboard) { ++rejectedSlope; return false; }
			minHeight = Min(minHeight, height);
			maxHeight = Max(maxHeight, height);
		}
		
		if (minHeight < GenerationSettings::get().development_coastalBuildHeight || maxHeight - minHeight > maximumRelief)
		{
			++rejectedSlope;
			return false;
		}
		return true;
	};
	HashTable<int64, EdgeFacingSlot> edgeFacingSlotsByCell;

	// 道路沿いスロットを地区ごとに収集し、ゾーン・道路距離・重なり判定を満たす場所へ初期建物を置く。
	for (int si = 0; si < static_cast<int>(m_districts.size()); ++si)
	{
		const auto& s = m_districts[si];


		const float radiusSq=static_cast<float>(Square(UrbanMorphology::coverageRadius(s.plan)+GenerationSettings::get().development_serviceCoverageMargin));
		const Array<EdgeFacingSlot> slots = collectEdgeFacingSlots(s, m_world, m_network);
		candidateSlots += static_cast<int>(slots.size());
		// Large service plots are reserved before homes fill the roadside slots.
		Array<Vec2> convenienceSites, fuelSites;
		for (const auto& slot : slots)
		{
			const RoadEdge* road = m_network.getEdge(slot.edgeId);
			Chunk* chunk = m_world.getChunk(slot.chunkCoord);
			if (!road || !chunk || road->roadType == RoadType::Expressway || road->useElevation) { continue; }
			if (chunk->buildingGrid[{slot.col, slot.row}].type != BuildingType::None) { continue; }
			const Vec2 center = cellCenterXZ(slot.chunkCoord, slot.col, slot.row);
			const Vec2 local = planLocal(s, center);
			const auto use = UrbanMorphology::sample(s.plan, local);
			if (use.district == UrbanMorphology::District::Civic || use.district == UrbanMorphology::District::Station
				|| use.district == UrbanMorphology::District::Countryside) { continue; }
			const int gx = slot.chunkCoord.x * ZONE_CELLS + slot.col, gz = slot.chunkCoord.y * ZONE_CELLS + slot.row;
			const uint32 hash = settlementCellHash(m_seed, si, gx, gz);
			if (hash % 19u != 0u) { continue; }
			const bool rural = s.plan.scale == 2 || !UrbanMorphology::inCore(s.plan, local, -100);
			const bool fuel = (hash / 19u) % 3u == 0u;
			if (fuel && road->roadType == RoadType::LocalRoad) { continue; }
			if (rural && road->totalWidth() < GenerationSettings::get().development_minimumServiceRoadWidth) { continue; }
			auto& sites = fuel ? fuelSites : convenienceSites;
			if (sites.size() >= (fuel ? 3u : 6u)) { continue; }
			if (sites.any([&](Vec2 site) { return site.distanceFrom(center) < (fuel ? GenerationSettings::get().development_fuelStationSpacing : GenerationSettings::get().development_convenienceStoreSpacing); })) { continue; }
			Building building;
			building.type = fuel ? (rural ? BuildingType::RoadsideFuelStation : BuildingType::UrbanFuelStation)
				: (rural ? BuildingType::RoadsideConvenience : BuildingType::UrbanConvenience);
			building.angle = slot.angle;
			building.edgeId = slot.edgeId;
			building.edgeT = slot.edgeT;
			const Vec2 offset = roadsideBuildingOffset(slot, building.type, gx, gz);
			building.offsetX = static_cast<float>(offset.x);
			building.offsetZ = static_cast<float>(offset.y);
			const Vec2 position = center + offset;
			if (!isBuildableFootprint(building, position, GenerationSettings::get().development_maximumServiceRelief)
				|| !hasRoadClearance(m_network, slot.edgeId, position, buildingFootprintXZ(building.type) * .5f, GenerationSettings::get().development_minimumRoadSetback)
				|| overlapsExistingBuilding(m_world, slot.chunkCoord, slot.col, slot.row,
					static_cast<float>(position.x), static_cast<float>(position.y), buildingFootprintXZ(building.type) * .5f, building.angle)) { continue; }
			chunk->buildingGrid[{slot.col, slot.row}] = building;
			chunk->zoneMap[{slot.col, slot.row}] = ZoneType::Commercial;
			chunk->meshDirty = true;
			sites << position;
			++placed;
		}
		DBG_LOG(U"[RoadsideServices] district={} convenience={} fuel={}"_fmt(si, convenienceSites.size(), fuelSites.size()));
		for (const auto& slot : slots)
		{
			Chunk* chunk = m_world.getChunk(slot.chunkCoord);
			if (!chunk) continue;
			if (chunk->buildingGrid[{ slot.col, slot.row }].type != BuildingType::None) continue;

			const ZoneType zone = chunk->zoneMap[{ slot.col, slot.row }];
			if (zone == ZoneType::Unzoned) continue;

			const int64 slotKey = zoneCellKey(slot.chunkCoord, slot.col, slot.row);
			const auto slotIt = edgeFacingSlotsByCell.find(slotKey);
			if (slotIt == edgeFacingSlotsByCell.end() || slot.frontageScore < slotIt->second.frontageScore)
			{
				edgeFacingSlotsByCell[slotKey] = slot;
			}

			const Vec2 centerPos = cellCenterXZ(slot.chunkCoord, slot.col, slot.row);
			const float dx = static_cast<float>(centerPos.x - s.center.x);
			const float dz = static_cast<float>(centerPos.y - s.center.y);
			const float distFromCenterSq = dx * dx + dz * dz;
			if (distFromCenterSq > radiusSq) continue;

			const float h = sampleHeightMap(
				chunk->heightMap, slot.chunkCoord,
				static_cast<float>(centerPos.x), static_cast<float>(centerPos.y));
			if (h < GenerationSettings::get().development_minimumBuildingHeight) continue;

			const int globalGX = slot.chunkCoord.x * ZONE_CELLS + slot.col;
			const int globalGZ = slot.chunkCoord.y * ZONE_CELLS + slot.row;
			const uint32 cellHash = settlementCellHash(m_seed, si, globalGX, globalGZ);
			Building b = InitialBuilding::spawn(zone, s.kind, 0.0, cellHash);
			InitialBuilding::applySettlementContext(b,s,centerPos,cellHash);

			if (b.type == BuildingType::None) continue;

			float roadScore;
			if      (slot.roadDist < GenerationSettings::get().development_nearDist) roadScore = 1.0f;
			else if (slot.roadDist < GenerationSettings::get().development_farDist)  roadScore = 1.0f - (slot.roadDist - GenerationSettings::get().development_nearDist) / (GenerationSettings::get().development_farDist - GenerationSettings::get().development_nearDist);
			else                                roadScore = 0.0f;

			const auto landUse=UrbanMorphology::sample(s.plan,planLocal(s,centerPos));
			const float densityFactor=static_cast<float>(landUse.occupancy);
			const bool continuousFrontage=landUse.district==UrbanMorphology::District::Housing
				|| landUse.district==UrbanMorphology::District::OldTown
				|| landUse.district==UrbanMorphology::District::Station;
			const double frontageDensity=continuousFrontage
				? Math::Sqrt(slot.urbanIntensity) : 0.0;
			const float score=static_cast<float>(Math::Lerp(static_cast<double>(roadScore*densityFactor),1.0,frontageDensity));
			if (score < GenerationSettings::get().development_minimumBuildingScore) continue;

			const float roll = (cellHash % 1000) / 1000.0f;
			if (roll > score) { ++rejectedDensity; continue; }

			const Vec2 roadOffset = roadsideBuildingOffset(slot, b.type, globalGX, globalGZ);
			b.offsetX = static_cast<float>(roadOffset.x);
			b.offsetZ = static_cast<float>(roadOffset.y);
			b.angle = slot.angle;
			b.edgeId = slot.edgeId;
			b.edgeT = slot.edgeT;

			const Vec2 finalPos = centerPos + roadOffset;
			if (b.type != BuildingType::Farmland)
			{
				if (!isBuildableFootprint(b, finalPos))
				{
					continue;
				}
				const float minimumSetback = Max(GenerationSettings::get().development_minimumRoadSetback, setbackFromRoadByModel(b.type, globalGX, globalGZ) * GenerationSettings::get().development_modelSetbackFraction);
				if (!hasRoadClearance(m_network, slot.edgeId, finalPos, buildingFootprintXZ(b.type) * 0.5f, minimumSetback)) { ++rejectedClearance; continue; }
				if (overlapsExistingBuilding(
					m_world, slot.chunkCoord, slot.col, slot.row,
					static_cast<float>(finalPos.x), static_cast<float>(finalPos.y), buildingFootprintXZ(b.type) * 0.5f, b.angle)) { ++rejectedNeighbor; continue; }
			}

			chunk->buildingGrid[{ slot.col, slot.row }] = b;
			chunk->meshDirty = true;
			++placed;
		}
	}

	int midrise = 0, towers = 0, offices = 0, detached = 0;
	for (int z = 0; z < WORLD_CHUNKS; ++z)
	{
		for (int x = 0; x < WORLD_CHUNKS; ++x)
		{
			if (const Chunk* chunk = m_world.getChunk(Point{x,z}))
			{
				for (const auto& building : chunk->buildingGrid)
				{
					midrise += building.type == BuildingType::MidApartment;
					towers += building.type == BuildingType::HighApartment;
					offices += building.type == BuildingType::Office;
					detached += building.type == BuildingType::Detached;
				}
			}
		}
	}
	DBG_LOG(U"[NewTownGreenClearance] rejected={}"_fmt(rejectedGreen));
	DebugLog::print(U"[UrbanMix] midrise={} towers={} offices={} detached={}"_fmt(midrise,towers,offices,detached));
	DebugLog::print(U"[PlacementReview] slots={} placed={} density={} clearance={} neighbor={}"_fmt(candidateSlots, placed, rejectedDensity, rejectedClearance, rejectedNeighbor));
	int infillPlaced = 0;
	for (int chunkY = 0; chunkY < WORLD_CHUNKS; ++chunkY)
	{
		for (int chunkX = 0; chunkX < WORLD_CHUNKS; ++chunkX)
		{
			Chunk* chunk = m_world.getChunk(Point{ chunkX, chunkY });
			if (!chunk) continue;
			bool changed = false;
			for (int row = 0; row < ZONE_CELLS; ++row)
			{
				for (int col = 0; col < ZONE_CELLS; ++col)
				{
					Building& building = chunk->buildingGrid[{ col, row }];
					if (building.type != BuildingType::None) continue;

					const ZoneType zone = chunk->zoneMap[{ col, row }];
					if (zone != ZoneType::Commercial && zone != ZoneType::Residential
						&& zone != ZoneType::LowResidential && zone != ZoneType::Industrial) continue;

					const Vec2 centerPos = cellCenterXZ(Point{ chunkX, chunkY }, col, row);
					const int si = nearestSettlementIndex(m_districts,
						static_cast<float>(centerPos.x), static_cast<float>(centerPos.y));
					if (si < 0) continue;

					const auto& settlement = m_districts[si];
					const int globalGX = chunkX * ZONE_CELLS + col;
					const int globalGZ = chunkY * ZONE_CELLS + row;
					const uint32 cellHash = settlementCellHash(m_seed ^ 0xE35A11B5ULL, si, globalGX, globalGZ);
					const float density = infillDensity(settlement.kind, zone)
						* static_cast<float>(UrbanMorphology::sample(settlement.plan,planLocal(settlement,centerPos)).occupancy);
					if ((cellHash % 1000u) >= static_cast<uint32>(density * 1000.0f)) continue;

					const float centerHeight = sampleHeightMap(chunk->heightMap, Point{ chunkX, chunkY }, static_cast<float>(centerPos.x), static_cast<float>(centerPos.y));
					if (centerHeight < GenerationSettings::get().development_minimumBuildingHeight) continue;
					Building b = InitialBuilding::spawn(zone, settlement.kind, 0.0, cellHash);
					InitialBuilding::applySettlementContext(b,settlement,centerPos,cellHash);
					if (b.type == BuildingType::None || b.type == BuildingType::Farmland) continue;

					const int64 roadSlotKey = zoneCellKey(Point{ chunkX, chunkY }, col, row);
					if (const auto roadSlot = edgeFacingSlotsByCell.find(roadSlotKey); roadSlot != edgeFacingSlotsByCell.end())
					{
						const EdgeFacingSlot& slot = roadSlot->second;
						const float setbackM = setbackFromRoadByModel(b.type, globalGX, globalGZ);
						const Vec2 roadOffset = roadsideBuildingOffset(slot, b.type, globalGX, globalGZ);
						b.offsetX = static_cast<float>(roadOffset.x);
						b.offsetZ = static_cast<float>(roadOffset.y);
						b.angle = slot.angle;
						b.edgeId = slot.edgeId;
						b.edgeT = slot.edgeT;

						const Vec2 finalPos = centerPos + roadOffset;
						if (!isBuildableFootprint(b, finalPos))
						{
							continue;
						}
						const float minimumSetback = Max(GenerationSettings::get().development_minimumRoadSetback, setbackM * GenerationSettings::get().development_modelSetbackFraction);
						if (!hasRoadClearance(m_network, slot.edgeId, finalPos, buildingFootprintXZ(b.type) * 0.5f, minimumSetback)) { ++rejectedClearance; continue; }
						if (overlapsExistingBuilding(
							m_world, Point{ chunkX, chunkY }, col, row,
							static_cast<float>(finalPos.x), static_cast<float>(finalPos.y), buildingFootprintXZ(b.type) * 0.5f, b.angle)) { ++rejectedNeighbor; continue; }
					}
					else
					{
						continue;
					}
					building = b;
					++infillPlaced;
					changed = true;
				}
			}
			if (changed) chunk->meshDirty = true;
		}
	}

	int blockCount=0,emptyBefore=0,blockInfill=0,emptyAfter=0,greenTrafficIslands=0;
	const auto blocks=StreetBlocks::collect(m_network);
	for (const auto& block : blocks)
	{
		const MapGenerator::Settlement* town=nullptr;
		for (const auto& candidate : m_districts)
		{
			if (!candidate.plan.ready || candidate.plan.frontageRoads) { continue; }
			bool within=true;
			for (const Vec2 point : block.outline)
			{
				const Vec2 local=planLocal(candidate,point);
				if (Abs(local.x)>candidate.plan.halfExtent.x+1 || Abs(local.y)>candidate.plan.halfExtent.y+1) { within=false; break; }
			}
			if (within) { town=&candidate; break; }
		}
		if (!town || UrbanMorphology::isReservedGreen(town->plan,planLocal(*town,block.center))) { continue; }
		++blockCount; bool occupied=false;
		Point lowChunk,highChunk; int lowX,lowZ,highX,highZ;
		worldToZoneCell(static_cast<float>(block.bounds.x),static_cast<float>(block.bounds.y),lowChunk,lowX,lowZ);
		worldToZoneCell(static_cast<float>(block.bounds.x+block.bounds.w),static_cast<float>(block.bounds.y+block.bounds.h),highChunk,highX,highZ);
		for (int globalZ=lowChunk.y*ZONE_CELLS+lowZ;globalZ<=highChunk.y*ZONE_CELLS+highZ && !occupied;++globalZ)
		{
			for (int globalX=lowChunk.x*ZONE_CELLS+lowX;globalX<=highChunk.x*ZONE_CELLS+highX;++globalX)
			{
				const Point coord{globalX/ZONE_CELLS,globalZ/ZONE_CELLS}; const int col=globalX%ZONE_CELLS,row=globalZ%ZONE_CELLS;
				const Chunk* chunk=m_world.getChunk(coord); if (!chunk) { continue; }
				const auto& building=chunk->buildingGrid[{col,row}];
				if (building.type!=BuildingType::None && building.type!=BuildingType::Farmland && building.type!=BuildingType::Parking
					&& block.contains(cellCenterXZ(coord,col,row)+Vec2{building.offsetX,building.offsetZ})) { occupied=true; break; }
			}
		}
		if (occupied) { continue; }
		++emptyBefore; int tried=0,terrainFailures=0; const int roadBefore=rejectedRoad,slopeBefore=rejectedSlope;
		for (const int edgeId : block.edges)
		{
			if (occupied) { break; }
			const auto* edge=m_network.getEdge(edgeId); const auto curve=m_network.getBezier(edgeId);
			for (float arc=edge->cutoffA+9;arc<curve->totalLength-edge->cutoffB-9 && !occupied;arc+=6)
			{
				const float fraction=arc/curve->totalLength;
				const auto range=RoadGeometry::structuralRangeAt(*edge,fraction);
				const Vec3 point=curve->positionAt(arc),right=tangentToRight(curve->tangentAt(arc));
				for (const int side : {-1,1})
				{
					const Vec2 direction{right.x*side,right.z*side};
					const double outer=side<0 ? -range.left : range.right;
					const bool civic=town->plan.civic && town->plan.civic->contains(planLocal(*town,block.center));
					Building building=InitialBuilding::spawn(ZoneType::LowResidential,town->kind,0,static_cast<uint32>(edgeId));
					building.type=civic ? BuildingType::PublicFacility : BuildingType::Detached;
					const float half=buildingFootprintXZ(building.type)*.5f;
					const Vec2 position=Vec2{point.x,point.z}+direction*(outer+half+GenerationSettings::get().development_suburbanSetback);
					if (!block.contains(position)) { continue; }
					Point coord; int col,row; worldToZoneCell(static_cast<float>(position.x),static_cast<float>(position.y),coord,col,row);
					Chunk* chunk=m_world.getChunk(coord); if (!chunk || chunk->buildingGrid[{col,row}].type!=BuildingType::None) { continue; }
					const Vec2 cell=cellCenterXZ(coord,col,row);
					building.angle=static_cast<float>(std::atan2(-direction.x,direction.y)); building.edgeId=edgeId; building.edgeT=curve->tFromArcLength(arc);
					building.offsetX=static_cast<float>(position.x-cell.x); building.offsetZ=static_cast<float>(position.y-cell.y); ++tried;
					if (!isBuildableFootprint(building,position,GenerationSettings::get().development_maximumSuburbanRelief)) { ++terrainFailures; continue; }
					if (overlapsExistingBuilding(m_world,coord,col,row,static_cast<float>(position.x),static_cast<float>(position.y),half,building.angle)) { continue; }
					chunk->buildingGrid[{col,row}]=building; chunk->zoneMap[{col,row}]=civic ? ZoneType::Residential : ZoneType::LowResidential; chunk->meshDirty=true;
					++blockInfill; occupied=true; break;
				}
			}
		}
		if (!occupied)
		{
			// A small street face with no legal building footprint is an open green island.
			if (block.area<=GenerationSettings::get().parcels_narrowBlockArea*2 && tried>0
				&& rejectedRoad-roadBefore==tried && rejectedSlope-slopeBefore==0)
			{
				int released=0;
				for (int globalZ=lowChunk.y*ZONE_CELLS+lowZ;globalZ<=highChunk.y*ZONE_CELLS+highZ;++globalZ)
				{
					for (int globalX=lowChunk.x*ZONE_CELLS+lowX;globalX<=highChunk.x*ZONE_CELLS+highX;++globalX)
					{
						const Point coord{globalX/ZONE_CELLS,globalZ/ZONE_CELLS};
						const int col=globalX%ZONE_CELLS,row=globalZ%ZONE_CELLS;
						Chunk* chunk=m_world.getChunk(coord);
						if (!chunk || !block.contains(cellCenterXZ(coord,col,row))
							|| chunk->buildingGrid[{col,row}].type!=BuildingType::None) { continue; }
						chunk->zoneMap[{col,row}]=ZoneType::Unzoned;
						chunk->meshDirty=true;
						++released;
					}
				}
				if (released>0)
				{
					++greenTrafficIslands;
					DBG_LOG(U"[GreenTrafficIsland] center=({}, {}) area={} cells={}"_fmt(block.center.x,block.center.y,block.area,released));
					continue;
				}
			}
			++emptyAfter;
			DBG_LOG(U"[EmptyBlock] center=({}, {}) area={} tried={} footprintRejected={} road={} slope={}"_fmt(block.center.x,block.center.y,block.area,tried,terrainFailures,rejectedRoad-roadBefore,rejectedSlope-slopeBefore));
		}
	}
	DBG_LOG(U"[BlockCoverage] blocks={} emptyBefore={} infill={} greenIslands={} emptyAfter={}"_fmt(blockCount,emptyBefore,blockInfill,greenTrafficIslands,emptyAfter));
	int fieldCells = 0;
	for (int chunkY = 0; chunkY < WORLD_CHUNKS; ++chunkY)
	{
		for (int chunkX = 0; chunkX < WORLD_CHUNKS; ++chunkX)
		{
			Chunk* chunk = m_world.getChunk(Point{ chunkX, chunkY });
			if (!chunk) continue;
			bool changed = false;
			for (int row = 0; row < ZONE_CELLS; ++row)
			{
				for (int col = 0; col < ZONE_CELLS; ++col)
				{
					if (chunk->zoneMap[{ col, row }] != ZoneType::Agriculture) continue;
					Building& building = chunk->buildingGrid[{ col, row }];
					if (building.type != BuildingType::None) continue;
					const int globalGX = chunkX * ZONE_CELLS + col;
					const int globalGZ = chunkY * ZONE_CELLS + row;
					const uint32 hash = settlementCellHash(m_seed, 0, globalGX / 2, globalGZ / 2);
					if ((hash % 100u) >= GenerationSettings::get().development_farmOccupancyPercent) continue;
					++fieldCells;
				}
			}
			if (changed) chunk->meshDirty = true;
		}
	}
	generateLandPatches(preserveLandPatches);
	Logger << U"[placeInitialBuildings] {} 棟配置, インフィル{}棟, 農地{}セル ({:.0f}ms)"_fmt(placed, infillPlaced, fieldCells, sw.msF());
	DebugLog::print(U"[placeInitialBuildings] placed={} infill={} fields={} rejectedRoad={} rejectedSlope={} elapsedMs={:.1f}"_fmt(placed, infillPlaced, fieldCells, rejectedRoad, rejectedSlope, sw.msF()));
	refreshBuildingAnglesFromEdges();
	Validation result = validateGeneratedCityConstraints();
	result.passed = result.passed && emptyAfter == 0;
	result.summary+=U" greenTrafficIslands={} emptyDevelopableBlocks={} generationPassed={}"_fmt(greenTrafficIslands,emptyAfter,result.passed);
	return result;
}

void SettlementDevelopment::generateLandPatches(bool preserveExisting)
{
	auto isUrbanLandZone = [](ZoneType zone)
	{
		return zone == ZoneType::LowResidential || zone == ZoneType::Residential
			|| zone == ZoneType::Commercial || zone == ZoneType::Industrial;
	};

	auto parcelTypeFor = [](ZoneType zone, BuildingType buildingType, uint32 salt)
	{
		if (zone == ZoneType::Agriculture)
		{
			return ((salt >> 3) & 1u) ? LandPatchType::PaddyField : LandPatchType::FarmField;
		}
		if (buildingType >= BuildingType::OfficeTower)
		{
			return LandPatchType::ParcelAsphalt;
		}
		if (buildingType == BuildingType::Office || buildingType == BuildingType::MidApartment || buildingType == BuildingType::HighApartment) { return LandPatchType::ParcelAsphalt; }
		if (buildingType == BuildingType::Parking) { return LandPatchType::ParcelAsphalt; }
		if (buildingType == BuildingType::Shop || zone == ZoneType::Commercial)
		{
			return ((salt >> 5) % 5u)==0u ? LandPatchType::ParcelGravel : LandPatchType::ParcelAsphalt;
		}
		if (zone == ZoneType::Industrial)
		{
			return ((salt >> 5) & 1u) ? LandPatchType::ParcelAsphalt : LandPatchType::ParcelGravel;
		}
		return LandPatchType::GardenSoil;
	};

	// 市役所・学校の中心セル以外も施設用地。空き公共緑地を上に敷き詰めない。
	HashSet<int64> facilityCells;
	for (int z = 0; z < WORLD_CHUNKS; ++z)
	{
		for (int x = 0; x < WORLD_CHUNKS; ++x)
		{
			const auto* chunk = m_world.getChunk({x, z});
			if (!chunk)
			{
				continue;
			}
			for (int row = 0; row < ZONE_CELLS; ++row)
			{
				for (int col = 0; col < ZONE_CELLS; ++col)
				{
					const auto& building = chunk->buildingGrid[{col, row}];
					if (building.type < BuildingType::OfficeTower)
					{
						continue;
					}
					const Vec2 center = cellCenterXZ({x, z}, col, row) + Vec2{building.offsetX, building.offsetZ};
					const auto footprint =
						ParcelGeometry::footprint(center, buildingFootprintXZ(building.type) * .5 + 2, building.angle);
					const double reach = buildingFootprintXZ(building.type) * .72 + 16;
					for (double wz = center.y - reach; wz <= center.y + reach; wz += 16)
					{
						for (double wx = center.x - reach; wx <= center.x + reach; wx += 16)
						{
							Point coord;
							int cx = 0, cz = 0;
							worldToZoneCell(static_cast<float>(wx), static_cast<float>(wz), coord, cx, cz);
							if (ParcelGeometry::overlaps(
									footprint, ParcelGeometry::footprint(cellCenterXZ(coord, cx, cz), 8, 0)))
							{
								facilityCells.insert(zoneCellKey(coord, cx, cz));
							}
						}
					}
				}
			}
		}
	}
	for (int chunkY = 0; chunkY < WORLD_CHUNKS; ++chunkY)
	{
		for (int chunkX = 0; chunkX < WORLD_CHUNKS; ++chunkX)
		{
			Chunk* chunkPtr = m_world.getChunk(Point{ chunkX, chunkY });
			if (!chunkPtr) continue;
			Chunk& chunk = *chunkPtr;
			if (preserveExisting && !chunk.landPatches.isEmpty())
			{
				for (LandPatch& patch : chunk.landPatches) { UrbanParcel::normalize(patch.polygon); }
				continue;
			}
			chunk.landPatches.clear();
			const Point coord{ chunkX, chunkY };
			uint32 patchIndex = 0;
			Grid<bool> agriculturePatchCovered(ZONE_CELLS, ZONE_CELLS, false);
			for (int row = 0; row < ZONE_CELLS; ++row)
			{
				for (int col = 0; col < ZONE_CELLS; ++col)
				{
					const ZoneType zone = chunk.zoneMap[{ col, row }];
					const bool civicZone=(zone==ZoneType::UrbanControl);
					const bool agricultureZone = civicZone && chunk.buildingGrid[{col, row}].type == BuildingType::None;
					if (!isUrbanLandZone(zone) && !civicZone)
					{
						continue;
					}
					if (agricultureZone && facilityCells.contains(zoneCellKey(coord, col, row)))
					{
						continue;
					}
					if (agricultureZone && agriculturePatchCovered[{ col, row }]) continue;

					if (agricultureZone)
					{
						const int kFieldColumns = GenerationSettings::get().development_fieldColumns, kFieldRows = GenerationSettings::get().development_fieldRows;
						int width = 0;
						while (width < kFieldColumns && col + width < ZONE_CELLS &&
							   chunk.zoneMap[{col + width, row}] == zone &&
							   !facilityCells.contains(zoneCellKey(coord, col + width, row)) &&
							   chunk.buildingGrid[{col + width, row}].type == BuildingType::None &&
							   !agriculturePatchCovered[{col + width, row}])
						{
							++width;
						}
						int height = 1;
						for (; height < kFieldRows && row + height < ZONE_CELLS; ++height)
						{
							bool available = true;
							for (int x = 0; x < width; ++x)
							{
								available &= chunk.zoneMap[{col + x, row + height}] == zone &&
											 !facilityCells.contains(zoneCellKey(coord, col + x, row + height)) &&
											 chunk.buildingGrid[{col + x, row + height}].type == BuildingType::None &&
											 !agriculturePatchCovered[{col + x, row + height}];
							}
							if (!available) { break; }
						}
						constexpr float kCellSize = static_cast<float>(CHUNK_SIZE) / ZONE_CELLS;
						const Vec2 corner{ (chunkX * ZONE_CELLS + col) * kCellSize + GenerationSettings::get().development_fieldCellInset,
							(chunkY * ZONE_CELLS + row) * kCellSize + GenerationSettings::get().development_fieldCellInset };
						const double sizeX = width * kCellSize - GenerationSettings::get().development_fieldCellCombinedInset, sizeZ = height * kCellSize - GenerationSettings::get().development_fieldCellCombinedInset;
						LandPatch field;
						field.id = static_cast<int>(patchIndex++);
						field.sourceParcelKey = -1;
						field.type = civicZone ? LandPatchType::GardenSoil : LandPatchType::FarmField;
						field.elevationOffset = GenerationSettings::get().development_fieldSurfaceLift;
						field.materialVariant = settlementCellHash(m_seed, 211, chunkX * ZONE_CELLS + col, chunkY * ZONE_CELLS + row);
						const float cornerHeight = m_world.sampleHeight(static_cast<float>(corner.x),static_cast<float>(corner.y));
						const float diagonalHeight = m_world.sampleHeight(static_cast<float>(corner.x+sizeX),static_cast<float>(corner.y+sizeZ));
						if (!civicZone && Abs(cornerHeight-diagonalHeight) < GenerationSettings::get().development_paddyMaximumRelief && field.materialVariant%4u != 0u) { field.type = LandPatchType::PaddyField; }

						bool dry=true;
						for (double dz=0;dz<=sizeZ;dz+=8) for (double dx=0;dx<=sizeX;dx+=8)
						{
							const Vec2 point=corner+Vec2{dx,dz};
							dry &= m_world.sampleHeight(static_cast<float>(point.x),static_cast<float>(point.y))>m_world.waterSurfaceHeight(point.x,point.y)+1;
						}
						if (!dry) { continue; }
						field.polygon = { corner, corner + Vec2{ sizeX, 0 }, corner + Vec2{ sizeX, sizeZ }, corner + Vec2{ 0, sizeZ } };
						chunk.landPatches << field;
						for (int y = 0; y < height; ++y)
						{
							for (int x = 0; x < width; ++x) { agriculturePatchCovered[{ col + x, row + y }] = true; }
						}
						continue;
					}
					const Building& building = chunk.buildingGrid[{ col, row }];
					if (building.type == BuildingType::None || building.type == BuildingType::Farmland) { continue; }
					const int64 cellKey = zoneCellKey(coord, col, row);
					const uint32 salt = settlementCellHash(m_seed ^ 0xA24BAED5u, 211,
						chunkX * ZONE_CELLS + col, chunkY * ZONE_CELLS + row);
					const Vec2 sampleCenter = cellCenterXZ(coord, col, row) + Vec2{ building.offsetX, building.offsetZ };
					EdgeProjection projection;
					if (!projectPointToEdgeXZ(m_network, building.edgeId, sampleCenter, projection)) { continue; }
					const RoadEdge* edge = m_network.getEdge(building.edgeId);
					if (!edge || !edge->isRoadbedBuilt()) { continue; }
					const RoadGeometry::LateralRange range = RoadGeometry::structuralRangeAt(*edge, projection.edgeT);
					if (!range.valid) { continue; }
					const bool rightSide = (sampleCenter - projection.position).dot(projection.right) >= 0.0;
					const Vec2 frontageDir = rightSide ? projection.right : -projection.right;
					const float structuralOuter = rightSide ? Max(0.0f, range.right) : Max(0.0f, -range.left);
					constexpr float kCellSize = static_cast<float>(CHUNK_SIZE) / ZONE_CELLS;
					const float buildingHalf = buildingFootprintXZ(building.type) * 0.5f;
					const int districtIndex=nearestSettlementIndex(m_districts,static_cast<float>(sampleCenter.x),static_cast<float>(sampleCenter.y));
					const auto use=districtIndex>=0 ? UrbanMorphology::sample(m_districts[districtIndex].plan,planLocal(m_districts[districtIndex],sampleCenter)) : UrbanMorphology::LandUse{};
					const float plotDepth=use.district==UrbanMorphology::District::Industry ? GenerationSettings::get().development_industrialPlotDepth
						: (use.district==UrbanMorphology::District::OldTown ? GenerationSettings::get().development_merchantPlotDepth : GenerationSettings::get().development_residentialPlotDepth);
					const double density=districtIndex>=0 && !isCompleteSiteBuilding(building.type)
						? UrbanMorphology::downtownIntensity(m_districts[districtIndex].plan,planLocal(m_districts[districtIndex],sampleCenter)) : 0;
					const auto& settings=GenerationSettings::get();
					// Dense street lots follow the building outline; the road gap belongs to the street.
					const double roadFront=structuralOuter+settings.development_parcelRoadGap;
					const double buildingFront=projection.distance-buildingHalf-settings.development_footprintMargin;
					const double frontOffset=Math::Lerp(roadFront,Max(roadFront,buildingFront),density);
					const double rawDepth=Math::Lerp(static_cast<double>(plotDepth),settings.urbanFabric_downtownPlotDepth,density);
					const double depth=districtIndex>=0 && m_districts[districtIndex].plan.scale==1 && zone==ZoneType::Commercial
						? Min(rawDepth,30.0) : rawDepth;
					const double backMargin=Math::Lerp(static_cast<double>(settings.development_parcelBackMargin),settings.urbanFabric_downtownBackMargin,density);
					const bool completeSite=isCompleteSiteBuilding(building.type);
					const double backOffset=completeSite
						? Max(frontOffset+2.0,projection.distance+buildingHalf+1.0)
						: Max(structuralOuter+depth,projection.distance+buildingHalf+backMargin);
					const double normalHalf=Max(buildingHalf+settings.development_parcelSideMargin,Max(kCellSize*settings.development_parcelCellHalfRatio,static_cast<float>(use.frontage)*settings.development_parcelFrontageHalfRatio));
					const double halfAlong=completeSite ? buildingHalf+1.0 : Math::Lerp(normalHalf,buildingHalf+settings.urbanFabric_downtownSideMargin,density);
					const Vec2 along = projection.tangent;
					LandPatch patch;
					patch.id = static_cast<int>(patchIndex++);
					patch.sourceParcelKey = cellKey;
					patch.type = completeSite && building.type!=BuildingType::RuralHouse
						? LandPatchType::ParcelAsphalt : parcelTypeFor(zone, building.type, salt);
					patch.elevationOffset = GenerationSettings::get().development_parcelSurfaceLift;
					patch.materialVariant = salt;
					patch.polygon = {
						projection.position - along * halfAlong + frontageDir * frontOffset,
						projection.position + along * halfAlong + frontageDir * frontOffset,
						projection.position + along * halfAlong + frontageDir * backOffset,
						projection.position - along * halfAlong + frontageDir * backOffset
					};
					patch.polygon = UrbanParcel::partition(m_world, coord, col, row, sampleCenter, std::move(patch.polygon), density >= 0.5);
					if (patch.polygon.size() >= 3) { chunk.landPatches << patch; }
				}
			}

			for (int y = 0; y < ZONE_CELLS; ++y)
			{
				int x = 0;
				while (x < ZONE_CELLS)
				{
					float averageHeight = 0.0f;
					int runLength = 0;
					while (x + runLength < ZONE_CELLS && hasWaterNeighbor(chunk, x + runLength, y))
					{
						averageHeight += chunk.heightMap[y][x + runLength];
						++runLength;
					}
					if (runLength <= 0)
					{
						++x;
						continue;
					}
					averageHeight /= static_cast<float>(runLength);
					const int stripWidth = Min(18, Max(6, runLength));
					int waterAbove = 0;
					int waterBelow = 0;
					for (int sx = x; sx < Min(ZONE_CELLS, x + stripWidth); ++sx)
					{
						if (y > 0 && chunk.heightMap[y - 1][sx] < GenerationSettings::get().development_coastalWaterThreshold) ++waterAbove;
						if (y + 1 < ZONE_CELLS && chunk.heightMap[y + 1][sx] < GenerationSettings::get().development_coastalWaterThreshold) ++waterBelow;
					}
					const int landwardSign = (waterAbove >= waterBelow) ? 1 : -1;
					const uint32 salt = settlementCellHash(m_seed, 97, coord.x * ZONE_CELLS + x, coord.y * ZONE_CELLS + y);
					if (averageHeight >= GenerationSettings::get().development_beachMinimumHeight && averageHeight < GenerationSettings::get().development_beachMaximumHeight)
					{
						LandPatch patch;
						patch.id = static_cast<int>(patchIndex++);
						patch.type = LandPatchType::Beach;
						const int beachDepth = 3 + static_cast<int>((salt >> 6) & 3u);
						patch.polygon = makeCoastalBandPolygon(coord, x, y, stripWidth, beachDepth, landwardSign, salt);
						patch.elevationOffset = Max(0.010f, 0.035f - averageHeight * 0.24f);
						patch.materialVariant = salt;
						chunk.landPatches << patch;
					}
					else if (averageHeight >= GenerationSettings::get().development_beachMaximumHeight && averageHeight < GenerationSettings::get().development_coastalGrassMaximumHeight)
					{
						LandPatch patch;
						patch.id = static_cast<int>(patchIndex++);
						patch.type = LandPatchType::Seawall;
						const int seawallDepth = 2 + static_cast<int>((salt >> 9) & 1u);
						patch.polygon = makeCoastalBandPolygon(coord, x, y, stripWidth, seawallDepth, landwardSign, salt ^ 0x9E3779B9u);
						patch.elevationOffset = 0.035f;
						patch.materialVariant = salt;
						chunk.landPatches << patch;
					}
					x += stripWidth;
				}
			}

		}
	}
	if (!preserveExisting && m_options.enabled(GenerationOptions::Element::Farms))
	{
		const auto farmFrames=agriculturalFrames();
		const auto fields=AgriculturalLayout::generate(m_world,m_network,m_seed,farmFrames,false,&m_trainNetwork);
		DebugLog::print(U"[AgriculturalLayout] candidates={} fields={} paddies={} tracks={} drains={} disconnected={}"_fmt(
			fields.candidates,fields.fields,fields.paddies,fields.tracks,fields.drains,fields.disconnected));
		JSON report; report[U"seed"]=m_seed; report[U"fields"]=fields.fields; report[U"tracks"]=fields.tracks;
		Array<Vec2> farmHomes;Array<const LandPatch*> farmPlots;
		for (int z=0;z<WORLD_CHUNKS;++z) { for (int x=0;x<WORLD_CHUNKS;++x)
		{
			const auto* chunk=m_world.getChunk({x,z});if (!chunk) { continue; }
			for (int row=0;row<ZONE_CELLS;++row) { for (int col=0;col<ZONE_CELLS;++col)
			{
				const auto& home=chunk->buildingGrid[{col,row}];if (home.type==BuildingType::RuralHouse) { farmHomes << cellCenterXZ({x,z},col,row)+Vec2{home.offsetX,home.offsetZ}; }
			} }
			for (const auto& patch:chunk->landPatches) { if (patch.type==LandPatchType::FarmField || patch.type==LandPatchType::PaddyField) { farmPlots << &patch; } }
		} }
		double maximumHomeDistance=0;int isolatedFields=0,irregularFields=0;
		for (size_t index=0;index<farmPlots.size();++index)
		{
			const auto& patch=*farmPlots[index];double nearest=Math::Inf;
			for (const Vec2 home:farmHomes) { double farthest=0;for (const Vec2 point:patch.polygon) { farthest=Max(farthest,home.distanceFrom(point)); }nearest=Min(nearest,farthest); }
			maximumHomeDistance=Max(maximumHomeDistance,nearest);isolatedFields+=nearest>500;irregularFields+=patch.polygon.size()>4;
			if (index<64) { const Vec2 center=Polygon{patch.polygon}.centroid();report[U"fieldExamples"][index]=Array<double>{center.x,center.y,nearest}; }
		}
		report[U"ruralHomes"]=farmHomes.size();report[U"maximumHomeDistance"]=maximumHomeDistance;report[U"isolatedFields"]=isolatedFields;report[U"irregularFields"]=irregularFields;
		for (size_t index=0;index<farmFrames.size();++index)
		{
			const auto& frame=farmFrames[index];
			report[U"farmFrames"][index][U"center"]=Array<double>{frame.center.x,frame.center.y};
			report[U"farmFrames"][index][U"axis"]=Array<double>{frame.axisX.x,frame.axisX.y};
			report[U"farmFrames"][index][U"plotSize"]=Array<double>{frame.plotSize.x,frame.plotSize.y};
		}
		int town=0;
		for (const auto& settlement : m_districts)
		{
			if (settlement.kind!=MapGenerator::SettlementKind::RegionalCity) { continue; }
			const auto& plan=settlement.plan;
			const double radius=UrbanMorphology::coverageRadius(plan)+64;
			Array<int> buildings(4,0),lowRise(4,0);
			const int minX=Max(0,static_cast<int>((settlement.center.x-radius)/CHUNK_SIZE));
			const int minY=Max(0,static_cast<int>((settlement.center.y-radius)/CHUNK_SIZE));
			const int maxX=Min(WORLD_CHUNKS-1,static_cast<int>((settlement.center.x+radius)/CHUNK_SIZE));
			const int maxY=Min(WORLD_CHUNKS-1,static_cast<int>((settlement.center.y+radius)/CHUNK_SIZE));
			for (int y=minY;y<=maxY;++y) { for (int x=minX;x<=maxX;++x)
			{
				const auto* chunk=m_world.getChunk(Point{x,y}); if (!chunk) { continue; }
				for (int row=0;row<ZONE_CELLS;++row) { for (int col=0;col<ZONE_CELLS;++col)
				{
					const auto& building=chunk->buildingGrid[{col,row}];
					if (building.type==BuildingType::None || building.type==BuildingType::Farmland) { continue; }
					const Vec2 point=cellCenterXZ({x,y},col,row)+Vec2{building.offsetX,building.offsetZ};
					const Vec2 local=planLocal(settlement,point);
					if (!UrbanMorphology::contains(plan,local,22)) { continue; }
					const double depth=Max(Abs(local.x)-plan.halfExtent.x,Abs(local.y)-plan.halfExtent.y);
					const int belt=depth<=22 ? 0 : (depth<=250 ? 1 : (depth<=500 ? 2 : 3));
					++buildings[belt];
					lowRise[belt]+=building.type==BuildingType::Detached || building.type==BuildingType::RuralHouse || building.type==BuildingType::LowApartment;
				} }
			} }
			double maximumDepth=0;
			for (const auto& line : plan.fringeStreets) { for (const Vec2 point : {line.begin,line.end}) { maximumDepth=Max(maximumDepth,Max(Abs(point.x)-plan.halfExtent.x,Abs(point.y)-plan.halfExtent.y)); } }
			JSON item; item[U"center"]=Array<double>{settlement.center.x,settlement.center.y};
			item[U"frontageSegments"]=static_cast<int>(plan.fringeStreets.size()); item[U"maximumDepth"]=maximumDepth;
			item[U"buildingsByDepth"]=buildings; item[U"lowRiseByDepth"]=lowRise;
			report[U"towns"][town++]=item;
			DBG_LOG(U"[SuburbanDevelopment] center=({}, {}) streets={} depth={:.1f} buildings={}"_fmt(settlement.center.x,settlement.center.y,plan.fringeStreets.size(),maximumDepth,buildings));
		}
		report.save(U"landscape_audit.json");
	}
}
void SettlementDevelopment::migrateLegacyBuildingFrontageReferences()
{
	HashTable<int64, EdgeFacingSlot> edgeFacingSlotsByCell;
	for (const auto& settlement : m_districts)
	{
		const Array<EdgeFacingSlot> slots = collectEdgeFacingSlots(settlement, m_world, m_network);
		for (const EdgeFacingSlot& slot : slots)
		{
			const int64 key = zoneCellKey(slot.chunkCoord, slot.col, slot.row);
			const auto it = edgeFacingSlotsByCell.find(key);
			if (it == edgeFacingSlotsByCell.end() || slot.frontageScore < it->second.frontageScore)
			{
				edgeFacingSlotsByCell[key] = slot;
			}
		}
	}

	int assigned = 0;
	int unresolved = 0;
	for (int chunkY = 0; chunkY < WORLD_CHUNKS; ++chunkY)
	{
		for (int chunkX = 0; chunkX < WORLD_CHUNKS; ++chunkX)
		{
			Chunk* chunk = m_world.getChunk(Point{ chunkX, chunkY });
			if (!chunk) continue;
			bool changed = false;
			for (int row = 0; row < ZONE_CELLS; ++row)
			{
				for (int col = 0; col < ZONE_CELLS; ++col)
				{
					Building& building = chunk->buildingGrid[{ col, row }];
					if (building.type == BuildingType::None || building.type == BuildingType::Farmland) continue;
					if (building.edgeId >= 0 && m_network.getEdge(building.edgeId)) continue;

					const int64 key = zoneCellKey(Point{ chunkX, chunkY }, col, row);
					const auto slotIt = edgeFacingSlotsByCell.find(key);
					if (slotIt == edgeFacingSlotsByCell.end())
					{
						++unresolved;
						continue;
					}

					const EdgeFacingSlot& slot = slotIt->second;
					const int globalGX = chunkX * ZONE_CELLS + col;
					const int globalGZ = chunkY * ZONE_CELLS + row;
					const Vec2 roadOffset = roadsideBuildingOffset(slot, building.type, globalGX, globalGZ);
					building.offsetX = static_cast<float>(roadOffset.x);
					building.offsetZ = static_cast<float>(roadOffset.y);
					building.angle = slot.angle;
					building.edgeId = slot.edgeId;
					building.edgeT = slot.edgeT;
					changed = true;
					++assigned;
				}
			}
			if (changed) chunk->meshDirty = true;
		}
	}

	if (assigned > 0 || unresolved > 0)
	{
		Logger << U"[migrateLegacyBuildingFrontageReferences] assigned={} unresolved={}"_fmt(assigned, unresolved);
	}
}
SettlementDevelopment::Validation SettlementDevelopment::validateGeneratedCityConstraints() const
{
	ParcelRoadIndex roadIndex{ m_network,true };
	roadIndex.addRailway(m_trainNetwork);
	HashTable<int64, EdgeFacingSlot> edgeFacingSlotsByCell;
	for (const auto& settlement : m_districts)
	{
		const Array<EdgeFacingSlot> slots = collectEdgeFacingSlots(settlement, m_world, m_network);
		for (const EdgeFacingSlot& slot : slots)
		{
			const int64 key = zoneCellKey(slot.chunkCoord, slot.col, slot.row);
			const auto it = edgeFacingSlotsByCell.find(key);
			if (it == edgeFacingSlotsByCell.end() || slot.frontageScore < it->second.frontageScore)
			{
				edgeFacingSlotsByCell[key] = slot;
			}
		}
	}

	HashSet<int64> generatedParcelKeys;
	HashTable<int, int> parcelFailures;
	for (int chunkY = 0; chunkY < WORLD_CHUNKS; ++chunkY)
	{
		for (int chunkX = 0; chunkX < WORLD_CHUNKS; ++chunkX)
		{
			const Chunk* chunk = m_world.getChunk(Point{ chunkX, chunkY });
			if (!chunk) continue;
			for (const LandPatch& patch : chunk->landPatches)
			{
				if ((patch.type == LandPatchType::ParcelAsphalt || patch.type == LandPatchType::ParcelGravel || patch.type == LandPatchType::GardenSoil)
					&& patch.sourceParcelKey >= 0)
				{
					generatedParcelKeys.insert(patch.sourceParcelKey);
					++parcelFailures[static_cast<int>(Polygon::Validate(patch.polygon))];
				}
			}
		}
	}

	for (const auto& [failure, count] : parcelFailures)
	{
		DebugLog::print(U"[ParcelValidation] failure={} count={}"_fmt(failure, count));
	}
	int buildingCount = 0;
	int noFrontageCount = 0;
	int missingEdgeCount = 0;
	int roadOverlapCount = 0;
	int frontageDistanceCount = 0;
	int coastalBuildingCount = 0;
	int missingParcelCount = 0;
	for (int chunkY = 0; chunkY < WORLD_CHUNKS; ++chunkY)
	{
		for (int chunkX = 0; chunkX < WORLD_CHUNKS; ++chunkX)
		{
			const Point chunkCoord{ chunkX, chunkY };
			const Chunk* chunk = m_world.getChunk(chunkCoord);
			if (!chunk) continue;
			for (int row = 0; row < ZONE_CELLS; ++row)
			{
				for (int col = 0; col < ZONE_CELLS; ++col)
				{
					const Building& building = chunk->buildingGrid[{ col, row }];
					if (building.type == BuildingType::None || building.type == BuildingType::Farmland) continue;
					++buildingCount;

					const int64 cellKey = zoneCellKey(chunkCoord, col, row);
					if (!generatedParcelKeys.contains(cellKey))
					{
						++missingParcelCount;
					}
					if (building.edgeId < 0)
					{
						++noFrontageCount;
					}

					const Vec2 center = cellCenterXZ(chunkCoord, col, row) + Vec2{ building.offsetX, building.offsetZ };
					if (roadIndex.overlaps(ParcelGeometry::footprint(center, buildingFootprintXZ(building.type) * 0.5, building.angle)))
					{
						++roadOverlapCount; DBG_LOG(U"[BuildingOverlap] coord=({}, {}) cell=({}, {}) type={} edge={} center=({}, {}) line={}"_fmt(chunkX,chunkY,col,row,static_cast<int>(building.type),building.edgeId,center.x,center.y,__LINE__));
					}
					const float h = sampleHeightMap(chunk->heightMap, chunkCoord, static_cast<float>(center.x), static_cast<float>(center.y));
					if (h < GenerationSettings::get().development_validationMinimumHeight)
					{
						++coastalBuildingCount;
					}

					const RoadEdge* edge = m_network.getEdge(building.edgeId);
					if (!edge || !edge->isRoadbedBuilt())
					{
						++missingEdgeCount;
						continue;
					}

					EdgeProjection projection;
					if (!projectPointToEdgeXZ(m_network, building.edgeId, center, projection))
					{
						++missingEdgeCount;
						continue;
					}

					const int globalGX = chunkX * ZONE_CELLS + col;
					const int globalGZ = chunkY * ZONE_CELLS + row;
					const RoadGeometry::LateralRange structuralRange = RoadGeometry::structuralRangeAt(*edge, projection.edgeT);
					const float signedLateral = static_cast<float>((center - projection.position).dot(projection.right));
					const float roadOuter = structuralRange.valid
						? ((signedLateral >= 0.0f) ? Max(0.0f, structuralRange.right) : Max(0.0f, -structuralRange.left))
						: edge->totalWidth() * 0.5f;
					const float buildingHalf = buildingFootprintXZ(building.type) * 0.5f;
					const float minimumClearance = roadOuter + buildingHalf * GenerationSettings::get().development_validationClearanceFraction;
					const float maximumFrontageDistance = roadOuter + buildingHalf + setbackFromRoadByModel(building.type, globalGX, globalGZ) + GenerationSettings::get().development_validationFrontageMargin;
					if (projection.distance < minimumClearance)
					{
						++roadOverlapCount; DBG_LOG(U"[BuildingOverlap] coord=({}, {}) cell=({}, {}) type={} edge={} center=({}, {}) line={}"_fmt(chunkX,chunkY,col,row,static_cast<int>(building.type),building.edgeId,center.x,center.y,__LINE__));
					}
					const float cosA = Math::Cos(building.angle);
					const float sinA = Math::Sin(building.angle);
					int footprintInsideRoadCorners = 0;
					for (const Vec2 localCorner : { Vec2{ -buildingHalf, -buildingHalf }, Vec2{ buildingHalf, -buildingHalf }, Vec2{ buildingHalf, buildingHalf }, Vec2{ -buildingHalf, buildingHalf } })
					{
						const Vec2 corner{ center.x + localCorner.x * cosA - localCorner.y * sinA, center.y + localCorner.x * sinA + localCorner.y * cosA };
						EdgeProjection cornerProjection;
						if (projectPointToEdgeXZ(m_network, building.edgeId, corner, cornerProjection)
							&& cornerProjection.distance < roadOuter + GenerationSettings::get().development_validationCornerMargin)
						{
							++footprintInsideRoadCorners;
						}
					}
					if (footprintInsideRoadCorners >= 2)
					{
						++roadOverlapCount; DBG_LOG(U"[BuildingOverlap] coord=({}, {}) cell=({}, {}) type={} edge={} center=({}, {}) line={}"_fmt(chunkX,chunkY,col,row,static_cast<int>(building.type),building.edgeId,center.x,center.y,__LINE__));
					}
					if (projection.distance > maximumFrontageDistance)
					{
						++frontageDistanceCount;
					}
				}
			}
		}
	}
	const bool parcelsValid = parcelFailures.size() == 1 && parcelFailures.contains(static_cast<int>(PolygonFailureType::OK));
	const bool passed = (parcelsValid && noFrontageCount == 0 && missingEdgeCount == 0 && missingParcelCount == 0 && roadOverlapCount == 0
		&& frontageDistanceCount == 0 && coastalBuildingCount == 0);
	const String summary = U"buildings={} noFrontage={} missingEdge={} missingParcel={} roadOverlap={} frontageDistance={} coastal={} passed={}"_fmt(
		buildingCount, noFrontageCount, missingEdgeCount, missingParcelCount, roadOverlapCount, frontageDistanceCount, coastalBuildingCount, passed);
	Logger << U"[CityConstraintValidation] " + summary;
	DebugLog::print(U"[CityConstraintValidation] " + summary);
	return Validation{ passed, summary };
}
void SettlementDevelopment::refreshBuildingAnglesFromEdges()
{
	int updated = 0;
	for (Chunk* chunk : m_world.getActiveChunks())
	{
		if (!chunk) continue;
		bool chunkChanged = false;
		for (int row = 0; row < ZONE_CELLS; ++row)
		{
			for (int col = 0; col < ZONE_CELLS; ++col)
			{
				Building& b = chunk->buildingGrid[{ col, row }];
				if (b.type == BuildingType::None) continue;
				if (b.edgeId < 0) continue;

				const Vec2 center = cellCenterXZ(chunk->coord, col, row) + Vec2{ b.offsetX, b.offsetZ };
				EdgeProjection projection;
				if (!projectPointToEdgeXZ(m_network, b.edgeId, center, projection))
				{
					continue;
				}
				const Vec2 inward = ((center - projection.position).dot(projection.right) >= 0.0)
					? projection.right : -projection.right;
				const float newAngle = static_cast<float>(Atan2(-inward.x, inward.y));
				if (Math::Abs(newAngle - b.angle) <= 1e-4f) continue;
				b.angle = newAngle;
				chunkChanged = true;
				++updated;
			}
		}
		if (chunkChanged) chunk->meshDirty = true;
	}

	if (updated > 0)
	{
		Logger << U"[refreshBuildingAnglesFromEdges] {} buildings"_fmt(updated);
	}
}

// =============================================================================
// 提出用スクリーンショット
// =============================================================================


Array<AgriculturalLayout::Frame> SettlementDevelopment::agriculturalFrames() const
{
	Array<AgriculturalLayout::Frame> farmFrames;
		for (const auto& settlement : m_districts)
		{
			const auto& plan=settlement.plan;
			AgriculturalLayout::Frame frame;
			frame.center=settlement.center; frame.axisX=settlement.gridAxisX; frame.axisZ=settlement.gridAxisZ;
			frame.origin=settlement.center-frame.axisX*plan.halfExtent.x-frame.axisZ*plan.halfExtent.y;
			const auto columns=UrbanMorphology::streetCoordinates(plan,false),rows=UrbanMorphology::streetCoordinates(plan,true);
			frame.plotSize={columns[1]-columns[0],(rows[1]-rows[0])/4.0};
			farmFrames << frame;
		}
	return farmFrames;
}

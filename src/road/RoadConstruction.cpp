#include "RoadConstruction.hpp"
#include "RoadGeometry.hpp"

namespace RoadConstruction
{
	String Progress::name() const
	{
		switch (stage)
		{
		case Stage::Clearance: return U"既設物撤去工";
		case Stage::Earthwork: return elevated ? U"下部工（基礎・橋脚）" : U"土工・路床工";
		case Stage::BaseCourse: return elevated ? U"上部工（桁・床版）" : U"路盤工";
		case Stage::Paving: return elevated ? U"橋面舗装工" : U"舗装工（基層・表層）";
		case Stage::Marking: return U"区画線工";
		default: return U"供用開始";
		}
	}
	Progress progress(double elapsed, double duration, bool elevated)
	{
		Progress result;
		result.elevated = elevated;
		result.total = Clamp(elapsed / Max(1.0, duration), 0.0, 1.0);
		// Gameplay proportions, not estimates of real-world construction schedules.
		const std::array<double, 6> ends = elevated
			? std::array<double, 6>{0.10, 0.40, 0.70, 0.90, 1.0, 1.0}
			: std::array<double, 6>{0.12, 0.36, 0.56, 0.88, 1.0, 1.0};
		double start = 0;
		for (int i = 0; i < 5; ++i)
		{
			if (result.total < ends[i])
			{
				result.stage = static_cast<Stage>(i);
				result.fraction = (result.total - start) / (ends[i] - start);
				return result;
			}
			start = ends[i];
		}
		result.stage = Stage::Complete;
		result.fraction = 1;
		return result;
	}
	Progress progress(const RoadNetwork& network, const RoadEdge& edge, GameTime now)
	{
		double start = edge.constructionStartTime, duration = 60;
		if (const auto* plan = network.getPlan(edge.planId); plan && plan->constructionStart)
		{
			start = *plan->constructionStart;
			duration = plan->completionDate ? *plan->completionDate - start : plan->constructionDuration;
		}
		return progress(now - start, duration, edge.useElevation);
	}

	namespace
	{
		struct Volume { ParcelGeometry::Quad footprint; double bottom, top; };
		uint64 cellKey(const Cell& cell)
		{
			return (static_cast<uint64>(cell.chunk.y * WORLD_CHUNKS + cell.chunk.x) << 12)
				| static_cast<uint64>(cell.cell.y * ZONE_CELLS + cell.cell.x);
		}
		void eraseCell(World& world, const Cell& cell)
		{
			if (auto* chunk = world.getChunk(cell.chunk))
			{
				chunk->buildingGrid[cell.cell] = Building{};
				chunk->zoneMap[cell.cell] = ZoneType::Unzoned;
				const int64 parcelKey = (chunkCoordToKey(cell.chunk) << 16) ^ (static_cast<int64>(cell.cell.y) << 8) ^ cell.cell.x;
				chunk->landPatches.remove_if([&](const LandPatch& patch) { return patch.sourceParcelKey == parcelKey; });
				chunk->meshDirty = true;
			}
		}
	}
	Array<Cell> affectedCells(const RoadNetwork& network, const Array<int>& edges,
		const World& world, const BoundsResolver& resolver)
	{
		Array<Cell> result;
		HashSet<uint64> found;
		for (const int edgeId : edges)
		{
			const auto* edge = network.getEdge(edgeId);
			const auto curve = network.getBezier(edgeId);
			if (!edge || !curve) continue;
			Array<Volume> volumes;
			const int segments = Max(1, static_cast<int>(Ceil(curve->totalLength / 2.0)));
			for (int i = 0; i < segments; ++i)
			{
				const float a = curve->totalLength * i / segments, b = curve->totalLength * (i + 1) / segments;
				const Vec3 p = curve->positionAt(a), q = curve->positionAt(b);
				const auto ra = RoadGeometry::structuralRangeAt(*edge, static_cast<float>(i) / segments);
				const auto rb = RoadGeometry::structuralRangeAt(*edge, static_cast<float>(i + 1) / segments);
				const Vec3 rightA = tangentToRight(curve->tangentAt(a)), rightB = tangentToRight(curve->tangentAt(b));
				auto xz = [](Vec3 v) { return Vec2{v.x, v.z}; };
				Volume volume;
				volume.footprint = { xz(p + rightA * (ra.left - 0.5)), xz(q + rightB * (rb.left - 0.5)),
					xz(q + rightB * (rb.right + 0.5)), xz(p + rightA * (ra.right + 0.5)) };
				volume.bottom = edge->useElevation ? Min(p.y, q.y) - 1.5 : -10000;
				volume.top = edge->useElevation ? Max(p.y, q.y) + 5.0 : 10000;
				volumes << volume;
			}
			if (edge->useElevation)
			{
				for (const auto& object : network.objects())
				{
					if (object.id < 0 || object.parentEdgeId != edgeId || object.type != RoadObjectType::Pier) continue;
					const Vec3 p = curve->positionAt(object.arcPos);
					volumes << Volume{ParcelGeometry::footprint({p.x,p.z}, 3.0, 0), -10000, p.y};
				}
			}
			// Bucket the short swept quads by chunk; no full-world scan when a road starts.
			HashTable<Point, Array<size_t>> buckets;
			for (size_t i = 0; i < volumes.size(); ++i)
			{
				double left = Math::Inf, right = -Math::Inf, top = Math::Inf, bottom = -Math::Inf;
				for (Vec2 p : volumes[i].footprint) { left=Min(left,p.x); right=Max(right,p.x); top=Min(top,p.y); bottom=Max(bottom,p.y); }
				for (int y = Max(0,static_cast<int>(Floor((top-32)/CHUNK_SIZE))); y <= Min(WORLD_CHUNKS-1,static_cast<int>(Floor((bottom+32)/CHUNK_SIZE))); ++y)
					for (int x = Max(0,static_cast<int>(Floor((left-32)/CHUNK_SIZE))); x <= Min(WORLD_CHUNKS-1,static_cast<int>(Floor((right+32)/CHUNK_SIZE))); ++x)
						buckets[Point{x,y}] << i;
			}
			for (const auto& [coord, indices] : buckets)
			{
				const Chunk* chunk = world.getChunk(coord);
				if (!chunk) continue;
				for (int row = 0; row < ZONE_CELLS; ++row)
					for (int col = 0; col < ZONE_CELLS; ++col)
					{
						const Building& building = chunk->buildingGrid[{col,row}];
						if (building.type == BuildingType::None && chunk->zoneMap[{col,row}] == ZoneType::Unzoned) continue;
						const Cell cell{coord,{col,row},building.type != BuildingType::None};
						if (found.contains(cellKey(cell))) continue;
						const double x = coord.x * CHUNK_SIZE + (col+.5)*16 + building.offsetX;
						const double z = coord.y * CHUNK_SIZE + (row+.5)*16 + building.offsetZ;
						const double height = cell.building ? Max(1.0f,buildingHeight(building.type)) : 50.0;
						Bounds bounds{{x,world.sampleHeight(static_cast<float>(x),static_cast<float>(z))+height*.5,z},
							{buildingFootprintXZ(building.type),height,buildingFootprintXZ(building.type)},building.angle};
						if (resolver && cell.building) { if (auto exact = resolver(*chunk,col,row)) bounds = *exact; }
						const Vec2 along{Cos(bounds.angle)*bounds.size.x*.5,Sin(bounds.angle)*bounds.size.x*.5};
						const Vec2 across{-Sin(bounds.angle)*bounds.size.z*.5,Cos(bounds.angle)*bounds.size.z*.5};
						const Vec2 center{bounds.center.x,bounds.center.z};
						const ParcelGeometry::Quad footprint{center-along-across,center+along-across,center+along+across,center-along+across};
						for (size_t index : indices)
						{
							const auto& v = volumes[index];
							if (bounds.center.y+bounds.size.y*.5 < v.bottom || bounds.center.y-bounds.size.y*.5 > v.top) continue;
							if (ParcelGeometry::overlaps(footprint,v.footprint)) { found.insert(cellKey(cell)); result << cell; break; }
						}
					}
			}
		}
		return result;
	}
	int ClearanceLedger::clear(World& world, const Array<Cell>& cells)
	{
		int removed = 0;
		for (const Cell& cell : cells)
		{
			if (auto* chunk = world.getChunk(cell.chunk); chunk && chunk->buildingGrid[cell.cell].type != BuildingType::None) ++removed;
			if (m_keys.insert(cellKey(cell)).second) m_cells << cell;
			eraseCell(world,cell);
		}
		return removed;
	}
	void ClearanceLedger::apply(World& world) const { for (const auto& cell : m_cells) eraseCell(world,cell); }
	bool ClearanceLedger::save(FilePathView path) const
	{
		JSON json;
		json[U"count"] = m_cells.size();
		for (size_t i = 0; i < m_cells.size(); ++i)
		{
			auto entry = json[U"cell_{}"_fmt(i)];
			entry[U"x"] = m_cells[i].chunk.x; entry[U"y"] = m_cells[i].chunk.y;
			entry[U"col"] = m_cells[i].cell.x; entry[U"row"] = m_cells[i].cell.y;
		}
		return json.save(path);
	}
	bool ClearanceLedger::load(FilePathView path)
	{
		const JSON json = JSON::Load(path);
		if (!json || !json.hasElement(U"count")) return false;
		const int count = json[U"count"].getOr<int>(-1);
		if (count < 0 || count > WORLD_CHUNKS*WORLD_CHUNKS*ZONE_CELLS*ZONE_CELLS) return false;
		ClearanceLedger loaded;
		for (int i=0; i<count; ++i)
		{
			const String key=U"cell_{}"_fmt(i);
			if(!json.hasElement(key)) return false;
			const auto entry = json[key];
			for(StringView field:{U"x",U"y",U"col",U"row"}) if(!entry.hasElement(field)) return false;
			Cell cell{{entry[U"x"].getOr<int>(-1),entry[U"y"].getOr<int>(-1)},
				{entry[U"col"].getOr<int>(-1),entry[U"row"].getOr<int>(-1)}};
			if (cell.chunk.x<0 || cell.chunk.y<0 || cell.chunk.x>=WORLD_CHUNKS || cell.chunk.y>=WORLD_CHUNKS
				|| cell.cell.x<0 || cell.cell.y<0 || cell.cell.x>=ZONE_CELLS || cell.cell.y>=ZONE_CELLS) return false;
			if (loaded.m_keys.insert(cellKey(cell)).second) loaded.m_cells << cell;
		}
		*this = std::move(loaded);
		return true;
	}
}

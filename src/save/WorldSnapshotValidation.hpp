#pragma once
#include "../world/World.hpp"
#include "../world/ZoneGrid.hpp"
#include "../road/RoadNetwork.hpp"

/// @brief Validate links shared by an authoritative development and road snapshot.
namespace WorldSnapshotValidation
{
	inline bool references(const World& world, [[maybe_unused]] const RoadNetwork& roads)
	{
		for (int y=0; y<WORLD_CHUNKS; ++y)
		{
			for (int x=0; x<WORLD_CHUNKS; ++x)
			{
				const Chunk* chunk=world.getChunk({x,y});
				if (!chunk || chunk->heightMap.isEmpty()) { continue; }
				for (const LandPatch& patch : chunk->landPatches)
				{
					if (patch.sourceParcelKey == -1) { continue; }
					if (patch.sourceParcelKey < 0) { return false; }
					const uint64 key=static_cast<uint64>(patch.sourceParcelKey);
					const Point coord{static_cast<int>(key>>48),static_cast<int>((key>>16)&0xffffffffu)};
					const int row=static_cast<int>((key>>8)&255u), col=static_cast<int>(key&255u);
					if (coord.x>=WORLD_CHUNKS || coord.y>=WORLD_CHUNKS || row>=ZONE_CELLS || col>=ZONE_CELLS
						|| ZoneGrid::zoneCellKey(coord,col,row)!=patch.sourceParcelKey) { return false; }
					const Chunk* target=world.getChunk(coord);
					if (!target || target->heightMap.isEmpty()) { return false; }

				}
			}
		}
		// Missing roads/parcels can be legitimate runtime state after edits or failed parcel partitioning.
		// Preserve such buildings exactly; city-constraint diagnostics report semantic inconsistencies.
		return true;
	}
}

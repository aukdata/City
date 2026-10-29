#pragma once
#include "World.hpp"

/// @brief A worker-owned copy of heights in a conservative rectangle; no mutable World references.
class HeightSnapshot
{
public:
	HeightSnapshot(const World& world,RectF bounds)
	{
		const int x0=Clamp(static_cast<int>(Floor(bounds.x/CHUNK_SIZE)),0,WORLD_CHUNKS-1);
		const int z0=Clamp(static_cast<int>(Floor(bounds.y/CHUNK_SIZE)),0,WORLD_CHUNKS-1);
		const int x1=Clamp(static_cast<int>(Floor((bounds.x+bounds.w)/CHUNK_SIZE)),0,WORLD_CHUNKS-1);
		const int z1=Clamp(static_cast<int>(Floor((bounds.y+bounds.h)/CHUNK_SIZE)),0,WORLD_CHUNKS-1);
		for(int z=z0;z<=z1;++z) for(int x=x0;x<=x1;++x)
		{
			if(const auto* chunk=world.getChunk({x,z})) { m_heights.emplace(chunkCoordToKey({x,z}),chunk->heightMap); }
		}
	}
	[[nodiscard]] float sampleHeight(float x,float z) const
	{
		const Point coord{static_cast<int>(Floor(x/CHUNK_SIZE)),static_cast<int>(Floor(z/CHUNK_SIZE))};
		if(coord.x<0 || coord.y<0 || coord.x>=WORLD_CHUNKS || coord.y>=WORLD_CHUNKS) { return 0; }
		const auto found=m_heights.find(chunkCoordToKey(coord));
		return found==m_heights.end() ? 0 : sampleHeightMap(found->second,coord,x,z);
	}
private:
	HashTable<int64,Grid<float>> m_heights;
};

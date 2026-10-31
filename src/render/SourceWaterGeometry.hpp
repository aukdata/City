#pragma once
#include "../world/World.hpp"

/// @brief Shared production water mesh builder, also exercised by native rendering tests.
namespace SourceWaterGeometry
{
	/// @brief Per-chunk river lookups on the half-step lattice. They depend only on position and the
	/// fixed routed topology, so a chunk rebuilt for new water depths reuses them unchanged.
	struct LatticeCache
	{
		const void* owner = nullptr;
		int side = 0;
		Array<int32> reach; ///< Nearest river reach, -2 until computed.
		Array<uint32> first;
		Array<uint32> count;
		std::vector<SourceWater::ChannelCandidate> candidates;
		/// @brief What the last cached build read that can change in play: the stage of every channel the
		/// lattice consulted and the ground heights. Equal inputs rebuild a bit-identical mesh.
		bool built = false;
		Array<uint32> channels;
		Array<double> stages;
		Array<float> ground;
	};
	/// @brief Record the channel stages and ground read by the build that just used this cache.
	void remember(const World& world,Point chunk,LatticeCache& cache);
	/// @brief True when rebuilding this chunk would read exactly what the last cached build read.
	[[nodiscard]] bool unchanged(const World& world,Point chunk,const LatticeCache& cache);
	/// @param mode 1: fixed sea; 2: owned land, clipped against the real terrain.
	/// @param cache Optional; reused only for the same water, origin, cells and step.
	MeshData build(const World& world,double originX,double originZ,int cells,double step,int mode,LatticeCache* cache=nullptr);
}

#pragma once
#include "../world/World.hpp"

/// @brief Shared production water mesh builder, also exercised by native rendering tests.
namespace SourceWaterGeometry
{
	/// @param mode 1: fixed sea; 2: owned land, clipped against the real terrain.
	MeshData build(const World& world,double originX,double originZ,int cells,double step,int mode);
}

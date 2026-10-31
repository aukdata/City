#pragma once
#include "RoadNetwork.hpp"

/// @brief Shared junction outline for asphalt, roadside strips and terrain clipping.
namespace JunctionGeometry
{
	struct Band
	{
		RoadPart part;
		double nearStart = 0.0, farStart = 0.0;
		double nearEnd = 0.0, farEnd = 0.0;
	};
	struct Section
	{
		Vec3 position;
		Vec3 outward;
		double fraction = 0.0;
		/// @brief Roadside reach before it meets the neighbouring approach's roadside.
		double limit = Math::Inf;
	};
	struct Corner
	{
		Array<Section> sections;
		Array<Band> bands;
	};
	struct Layout
	{
		MeshData asphalt;
		Array<Corner> corners;
		bool elevated = false;
		bool groundConnected = false;
		bool repaired = false;
	};
	[[nodiscard]] Layout build(const RoadNetwork& network, int nodeId, bool onlyOpenEdges = true, const World* world = nullptr);
	/// @brief Near and far roadside offsets at a section, clipped where an acute wedge closes.
	[[nodiscard]] std::pair<double, double> bandSpan(const Section& section, const Band& band);
	[[nodiscard]] Vec3 bandPosition(const Section& section, const Band& band, double across);
	/// @brief Exact asphalt and roadside footprint; Y is the road's design elevation.
	[[nodiscard]] MeshData terrainFootprint(const Layout& layout);
}

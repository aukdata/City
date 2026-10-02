#pragma once
#include "../road/RoadNetwork.hpp"

/// @brief Conservative bounds shared by visible and planned junction caps.
namespace RoadNodeBounds
{
	/// @brief Include trimmed road mouths, even when an acute bend lies far from its node.
	[[nodiscard]] inline double radius(const RoadNetwork& network, const RoadNode& node)
	{
		constexpr double kMinimumRadius = 30.0;
		double result = kMinimumRadius;
		for (const auto& attachment : node.attachments)
		{
			const auto* edge = network.getEdge(attachment.edgeId);
			if (!edge) { continue; }
			const double cutoff = edge->nodeA == node.id ? edge->cutoffA : edge->cutoffB;
			result = Max(result, cutoff + static_cast<double>(edge->totalWidth()));
		}
		return result;
	}
}

#pragma once
#include <Siv3D.hpp>

/// @brief Arbitration between a nearby road node and an actual edge candidate.
namespace RoadSelection
{
	enum class Target : uint8 { None, Node, Edge };

	/// @brief Give endpoint handles priority without hiding the body of a short road.
	[[nodiscard]] inline Target choose(Optional<Vec2> nodeScreenPosition,
		Vec2 cursor, bool edgeCandidate)
	{
		constexpr double kNodeHandleRadius = 12.0;
		if (nodeScreenPosition && (!edgeCandidate
			|| nodeScreenPosition->distanceFromSq(cursor) <= kNodeHandleRadius * kNodeHandleRadius))
		{
			return Target::Node;
		}
		return edgeCandidate ? Target::Edge : Target::None;
	}
}

#pragma once
#include "../road/RoadTypes.hpp"

/// @brief Apply road inspector edits through the scene's network notification callback.
namespace RoadInspectorEdit
{
	/// @brief Preserve the numeric widget's value identity and commit only actual speed changes.
	template <class Edit, class Notify>
	[[nodiscard]] inline bool editSpeedLimit(RoadEdge& edge, Edit&& edit,
		Notify&& notify)
	{
		const float previousSpeedLimit = edge.speedLimit;
		edit(edge.speedLimit);
		if (edge.speedLimit == previousSpeedLimit)
		{
			return false;
		}
		notify(edge.nodeA, edge.nodeB);
		return true;
	}
}

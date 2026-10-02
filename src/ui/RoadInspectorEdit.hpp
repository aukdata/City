#pragma once
#include "../road/RoadNetwork.hpp"

/// @brief Apply road inspector edits through the scene's network notification callback.
namespace RoadInspectorEdit
{
	/// @brief Preserve section widget identities and refresh connectivity before notifying simulation.
	template <class Edit, class Notify>
	[[nodiscard]] inline bool editSections(RoadNetwork& network,
		RoadEdge& edge, Edit&& edit, Notify&& notify)
	{
		if (!edit(edge))
		{
			return false;
		}
		network.rebuildNodeConnectivity(edge.nodeA, edge.nodeB);
		notify(edge.nodeA, edge.nodeB);
		return true;
	}

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

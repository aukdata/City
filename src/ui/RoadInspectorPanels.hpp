#pragma once
#include "PanelManager.hpp"
#include "KeyboardActions.hpp"

/// @brief Share one visible inspector between an edge and its independently selected route.
namespace RoadInspectorPanels
{
	/// @brief Keep the existing dimensions while making the two road inspectors mutually exclusive.
	inline void registerPanels(PanelManager& panels, int sceneHeight)
	{
		constexpr int kVerticalMargins = 20;
		constexpr double kEdgeWidth = 374, kRouteWidth = 312;
		constexpr StringView kGroup = U"road_inspector";
		const double height = static_cast<double>(sceneHeight - kVerticalMargins);
		panels.registerPanel(U"edge_info", {kEdgeWidth, height}, true, true, kGroup);
		panels.registerPanel(U"route_info", {kRouteWidth, height}, true, true, kGroup);
	}

	/// @brief A dismissed route name must not keep shortcuts blocked after its field stops drawing.
	inline void releaseHiddenRouteNameFocus(const PanelManager& panels, TextEditState& routeName)
	{
		if (!panels.isVisible(U"route_info") && GameInput::textInput == &routeName)
		{
			GameInput::releaseTextFocus();
		}
	}
}

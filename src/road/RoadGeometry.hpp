#pragma once
#include "RoadTypes.hpp"

namespace RoadGeometry
{
	struct LateralRange
	{
		float left = 0.0f;
		float right = 0.0f;
		bool valid = false;

		[[nodiscard]] float width() const { return valid ? (right - left) : 0.0f; }
	};

	[[nodiscard]] bool isStructuralStrip(const RoadPart& part);
	[[nodiscard]] bool isRenderableStrip(const RoadPart& part);
	[[nodiscard]] bool isRoadOwnedObject(const RoadPart& part);
	[[nodiscard]] float partOffsetAt(const RoadPart& part, float t, bool leftSide);
	[[nodiscard]] LateralRange structuralRangeAt(const RoadEdge& edge, float t);
	[[nodiscard]] LateralRange roadbedRangeAt(const RoadEdge& edge, float t);
	[[nodiscard]] float structuralWidth(const RoadEdge& edge);
	[[nodiscard]] double surfaceY(const RoadEdge& edge, const Vec3& roadPosition, double terrainHeight);
	[[nodiscard]] double markingY(const RoadEdge& edge, const Vec3& roadPosition, double terrainHeight);
	[[nodiscard]] double furnitureBaseY(const RoadEdge& edge, const Vec3& roadPosition, double terrainHeight);
}
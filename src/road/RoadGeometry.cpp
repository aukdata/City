#include "RoadGeometry.hpp"

namespace RoadGeometry
{
	bool isStructuralStrip(const RoadPart& part)
	{
		return part.build == BuildState::Built
			&& part.placement == RoadPartPlacement::Strip
			&& part.envelopeRole == RoadPartEnvelopeRole::Structural;
	}

	bool isRenderableStrip(const RoadPart& part)
	{
		return part.build == BuildState::Built
			&& part.placement == RoadPartPlacement::Strip
			&& part.type != RoadPartType::RoadsideObject
			&& part.type != RoadPartType::UtilityPole;
	}

	bool isRoadOwnedObject(const RoadPart& part)
	{
		return part.build == BuildState::Built
			&& part.envelopeRole == RoadPartEnvelopeRole::RoadOwnedObject;
	}

	float partOffsetAt(const RoadPart& part, float t, bool leftSide)
	{
		return leftSide
			? Math::Lerp(part.offsetA_L, part.offsetB_L, t)
			: Math::Lerp(part.offsetA_R, part.offsetB_R, t);
	}

	LateralRange structuralRangeAt(const RoadEdge& edge, float t)
	{
		LateralRange range;
		for (const auto& part : edge.parts)
		{
			if (!isStructuralStrip(part))
			{
				continue;
			}

			const float left = partOffsetAt(part, t, true);
			const float right = partOffsetAt(part, t, false);
			if (!range.valid)
			{
				range.left = Min(left, right);
				range.right = Max(left, right);
				range.valid = true;
			}
			else
			{
				range.left = Min(range.left, Min(left, right));
				range.right = Max(range.right, Max(left, right));
			}
		}
		return range;
	}

	LateralRange roadbedRangeAt(const RoadEdge& edge, float t)
	{
		LateralRange range;
		for (const auto& part : edge.parts)
		{
			if (!isRenderableStrip(part) || part.type != RoadPartType::Roadbed)
			{
				continue;
			}

			const float left = partOffsetAt(part, t, true);
			const float right = partOffsetAt(part, t, false);
			if (!range.valid)
			{
				range.left = Min(left, right);
				range.right = Max(left, right);
				range.valid = true;
			}
			else
			{
				range.left = Min(range.left, Min(left, right));
				range.right = Max(range.right, Max(left, right));
			}
		}
		return range;
	}

	float structuralWidth(const RoadEdge& edge)
	{
		const LateralRange range = structuralRangeAt(edge, 0.5f);
		return range.width();
	}

	double surfaceY(const RoadEdge& edge, const Vec3& roadPosition, double terrainHeight)
	{
		return (edge.useElevation ? roadPosition.y : terrainHeight) + kRoadSurfaceLift;
	}

	double markingY(const RoadEdge& edge, const Vec3& roadPosition, double terrainHeight)
	{
		return (edge.useElevation ? roadPosition.y : terrainHeight) + kRoadLineLift;
	}

	double furnitureBaseY(const RoadEdge& edge, const Vec3& roadPosition, double terrainHeight)
	{
		return surfaceY(edge, roadPosition, terrainHeight);
	}
}
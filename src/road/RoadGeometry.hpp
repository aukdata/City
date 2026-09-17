#pragma once
#include "RoadTypes.hpp"

class World;
struct CubicBezier;

namespace RoadGeometry
{
	/// @brief 信号柱の道路上の配置。高さを地形へ合わせる処理は描画側が受け持つ。
	struct SignalAnchor
	{
		Vec3 roadPosition;
		Vec3 position;
		float yaw = 0.0f;
	};

	/// @brief ノードに進入する車から見た路肩位置と向き。接続していない端点は none。
	[[nodiscard]] Optional<SignalAnchor> signalAnchor(const RoadEdge& edge, const CubicBezier& bezier, int nodeId);

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
	/// @brief Roadbed triangles used to drape markings on the actual rendered surface.
	[[nodiscard]] MeshData roadbedSurface(const RoadEdge& edge, const CubicBezier& bezier, const World& world);
	/// @brief Clip paint to pavement triangles and interpolate their height, including cross-slope.
	[[nodiscard]] MeshData projectMarking(const MeshData& paint, const MeshData& surface);
	/// @brief Remove roadside faces inside a repaired, overlapping junction pavement.
	[[nodiscard]] MeshData excludeSurface(const MeshData& strips, const MeshData& pavement);
}

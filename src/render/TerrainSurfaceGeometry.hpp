#pragma once
#include "../world/Chunk.hpp"

/// @brief 地形と敷地に共通のクリッピング・三角形化。GPU や描画キャッシュに依存しない。
namespace TerrainSurfaceGeometry
{
	inline constexpr float kTerrainCellSize = static_cast<float>(CHUNK_SIZE) / HEIGHT_CELLS;
	inline constexpr float kTerrainClipEpsilon = 1e-4f;
	struct TerrainClipVertex
	{
		Vec3 pos;
		Float2 tex;
	};

	Float2 terrainUvAt(const Vec3& pos, int materialKey = 0);
	TerrainClipVertex withMaterialUv(const TerrainClipVertex& src, int materialKey);
	float signedAreaXZ(const Array<Vec2>& polygon);
	RectF boundsOfPolygon(const Array<Vec2>& polygon);
	bool rectIntersects(const RectF& a, const RectF& b);
	Array<TerrainClipVertex> clipPolygonByHeight(const Array<TerrainClipVertex>& polygon, float planeY, bool keepBelow);
	Array<TerrainClipVertex> clipPolygonByHalfPlaneXZ(const Array<TerrainClipVertex>& polygon,
		const Vec2& edgeA, const Vec2& edgeB, bool keepInside);
	Array<Array<TerrainClipVertex>> subtractConvexPolygonXZ(const Array<TerrainClipVertex>& subject,
		const Array<Vec2>& clipPolygon, Optional<float> roadbedY = none);
	void appendPolygonAsTriangles(const Array<TerrainClipVertex>& polygon,
		Array<Vertex3D>& vertices, Array<TriangleIndex32>& indices);
}

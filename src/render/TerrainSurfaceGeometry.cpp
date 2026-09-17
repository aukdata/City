#include "TerrainSurfaceGeometry.hpp"

namespace TerrainSurfaceGeometry
{
	constexpr float kTerrainTileSize      = 8.0f;
	constexpr float kTerrainUvCosA        = 0.97237f;
	constexpr float kTerrainUvSinA        = 0.23345f;

	float terrainTextureTileSize(int materialKey)
	{
		switch (materialKey)
		{
		case 100: return 18.0f;
		case 101: return 30.0f;
		case 110: return 22.0f;
		case 111: return 28.0f;
		case 113: return 34.0f;
		case 114: return 24.0f;
		case 117: return 10.0f;
		case 118: return 12.0f;
		case 119: return 10.0f;
		case 120: return 11.0f;
		case 4: return 10.0f;
		case 5: return 10.0f;
		default: return kTerrainTileSize;
		}
	}

	Float2 terrainUvAt(const Vec3& pos, int materialKey)
	{
		const float tileSize = Max(1.0f, terrainTextureTileSize(materialKey));
		const float u = static_cast<float>(pos.x) / tileSize;
		const float v = static_cast<float>(pos.z) / tileSize;
		return Float2{ kTerrainUvCosA * u - kTerrainUvSinA * v, kTerrainUvSinA * u + kTerrainUvCosA * v };
	}

	TerrainClipVertex withMaterialUv(const TerrainClipVertex& src, int materialKey)
	{
		TerrainClipVertex out = src;
		out.tex = terrainUvAt(src.pos, materialKey);
		return out;
	}

	TerrainClipVertex lerpClipVertex(const TerrainClipVertex& a, const TerrainClipVertex& b, double t)
	{
		TerrainClipVertex out;
		out.pos = a.pos + (b.pos - a.pos) * t;
		out.tex = a.tex + (b.tex - a.tex) * static_cast<float>(t);
		return out;
	}

	float signedAreaXZ(const Array<Vec2>& polygon)
	{
		float area = 0.0f;
		for (size_t i = 0; i < polygon.size(); ++i)
		{
			const Vec2& a = polygon[i];
			const Vec2& b = polygon[(i + 1) % polygon.size()];
			area += static_cast<float>(a.x * b.y - b.x * a.y);
		}
		return area * 0.5f;
	}

	RectF boundsOfPolygon(const Array<Vec2>& polygon)
	{
		if (polygon.isEmpty()) return RectF{};
		float minX = static_cast<float>(polygon[0].x);
		float maxX = static_cast<float>(polygon[0].x);
		float minY = static_cast<float>(polygon[0].y);
		float maxY = static_cast<float>(polygon[0].y);
		for (const auto& p : polygon)
		{
			minX = Min(minX, static_cast<float>(p.x));
			maxX = Max(maxX, static_cast<float>(p.x));
			minY = Min(minY, static_cast<float>(p.y));
			maxY = Max(maxY, static_cast<float>(p.y));
		}
		return RectF{ minX, minY, Max(0.0f, maxX - minX), Max(0.0f, maxY - minY) };
	}

	bool rectIntersects(const RectF& a, const RectF& b)
	{
		return (a.x < (b.x + b.w))
		    && (b.x < (a.x + a.w))
		    && (a.y < (b.y + b.h))
		    && (b.y < (a.y + a.h));
	}

	/// @brief Drop zero-area fragments at shared clipping edges before they can multiply.
	void removeDegenerateClip(Array<TerrainClipVertex>& polygon)
	{
		if (polygon.size() < 3) { polygon.clear(); return; }
		Array<TerrainClipVertex> unique;
		for (const auto& vertex : polygon)
		{
			if (unique.isEmpty() || unique.back().pos.distanceFromSq(vertex.pos) > 1e-12) { unique << vertex; }
		}
		if (unique.size() > 1 && unique.front().pos.distanceFromSq(unique.back().pos) <= 1e-12) { unique.pop_back(); }
		double area = 0.0;
		if (unique.size() >= 3)
		{
			const Vec3 origin = unique.front().pos;
			for (size_t i = 1; i + 1 < unique.size(); ++i)
			{
				const Vec3 a = unique[i].pos - origin, b = unique[i+1].pos - origin;
				area += a.x*b.z - a.z*b.x;
			}
		}
		if (Abs(area) < 1e-8) { polygon.clear(); }
		else { polygon = std::move(unique); }
	}

	Array<TerrainClipVertex> clipPolygonByHeight(const Array<TerrainClipVertex>& polygon,
	                                             float planeY, bool keepBelow)
	{
		Array<TerrainClipVertex> out;
		if (polygon.size() < 3) return out;

		auto isInside = [&](const TerrainClipVertex& v)
		{
			return keepBelow
				? (v.pos.y <= planeY + kTerrainClipEpsilon)
				: (v.pos.y >= planeY - kTerrainClipEpsilon);
		};

		for (size_t i = 0; i < polygon.size(); ++i)
		{
			const TerrainClipVertex& curr = polygon[i];
			const TerrainClipVertex& next = polygon[(i + 1) % polygon.size()];
			const bool currInside = isInside(curr);
			const bool nextInside = isInside(next);

			if (currInside && nextInside)
			{
				out << next;
				continue;
			}

			const float dy = static_cast<float>(next.pos.y - curr.pos.y);
			if (Abs(dy) <= kTerrainClipEpsilon)
			{
				if (currInside && !nextInside)
					out << curr;
				continue;
			}

			const float t = Clamp(static_cast<float>((planeY - curr.pos.y) / dy), 0.0f, 1.0f);
			const TerrainClipVertex hit = lerpClipVertex(curr, next, t);

			if (currInside && !nextInside)
			{
				out << curr;
				out << hit;
			}
			else if (!currInside && nextInside)
			{
				out << hit;
				out << next;
			}
		}

		removeDegenerateClip(out);
		return out;
	}

	Array<TerrainClipVertex> clipPolygonByHalfPlaneXZ(const Array<TerrainClipVertex>& polygon,
	                                                  const Vec2& edgeA, const Vec2& edgeB,
	                                                  bool keepInside)
	{
		Array<TerrainClipVertex> out;
		if (polygon.size() < 3) return out;

		const Vec2 edge = edgeB - edgeA;
		auto signedDistance = [&](const TerrainClipVertex& v)
		{
			const Vec2 p{ v.pos.x, v.pos.z };
			return edge.x * (p.y - edgeA.y) - edge.y * (p.x - edgeA.x);
		};
		auto isInside = [&](double d)
		{
			return keepInside ? (d >= -kTerrainClipEpsilon) : (d <= kTerrainClipEpsilon);
		};

		for (size_t i = 0; i < polygon.size(); ++i)
		{
			const TerrainClipVertex& curr = polygon[i];
			const TerrainClipVertex& next = polygon[(i + 1) % polygon.size()];
			const double d0 = signedDistance(curr);
			const double d1 = signedDistance(next);
			const bool currInside = isInside(d0);
			const bool nextInside = isInside(d1);

			if (currInside && nextInside)
			{
				out << next;
				continue;
			}

			const double denom = d0 - d1;
			if (Abs(denom) <= kTerrainClipEpsilon)
			{
				if (currInside && !nextInside)
					out << curr;
				continue;
			}

			const double t = Clamp(d0 / denom, 0.0, 1.0);
			const TerrainClipVertex hit = lerpClipVertex(curr, next, t);

			if (currInside && !nextInside)
			{
				out << curr;
				out << hit;
			}
			else if (!currInside && nextInside)
			{
				out << hit;
				out << next;
			}
		}

		removeDegenerateClip(out);
		return out;
	}

	Array<Array<TerrainClipVertex>> subtractConvexPolygonXZ(const Array<TerrainClipVertex>& subject,
	                                                        const Array<Vec2>& clipPolygon, Optional<float> roadbedY)
	{
		Array<Array<TerrainClipVertex>> pending, kept;
		if (subject.size() < 3 || clipPolygon.size() < 3)
		{
			if (subject.size() >= 3) kept << subject;
			return kept;
		}

		pending << subject;
		for (size_t i = 0; i < clipPolygon.size(); ++i)
		{
			const Vec2 a = clipPolygon[i];
			const Vec2 b = clipPolygon[(i + 1) % clipPolygon.size()];
			Array<Array<TerrainClipVertex>> nextPending;

			for (const auto& poly : pending)
			{
				Array<TerrainClipVertex> inside = clipPolygonByHalfPlaneXZ(poly, a, b, true);
				Array<TerrainClipVertex> outside = clipPolygonByHalfPlaneXZ(poly, a, b, false);
				if (outside.size() >= 3) kept << std::move(outside);
				if (inside.size() >= 3) nextPending << std::move(inside);
			}

			pending = std::move(nextPending);
			if (pending.isEmpty()) break;
		}

		if (roadbedY)
		{
			// Keep earth beneath the pavement so road shoulders never expose water or the void.
			for (auto& polygon : pending)
			{
				for (auto& vertex : polygon)
				{
					vertex.pos.y = *roadbedY - 0.001f;
				}
				kept << std::move(polygon);
			}
		}
		return kept;
	}

	Float3 polygonNormal(const Array<TerrainClipVertex>& polygon)
	{
		if (polygon.size() < 3) return Float3{ 0.0f, 1.0f, 0.0f };
		const Vec3 a = polygon[1].pos - polygon[0].pos;
		const Vec3 b = polygon[2].pos - polygon[0].pos;
		const Vec3 n = a.cross(b);
		if (n.lengthSq() <= 1e-10) return Float3{ 0.0f, 1.0f, 0.0f };
		const Vec3 normalized = (n.y < 0.0 ? -n : n).normalized();
		return Float3{ static_cast<float>(normalized.x), static_cast<float>(normalized.y), static_cast<float>(normalized.z) };
	}

	void appendPolygonAsTriangles(const Array<TerrainClipVertex>& polygon,
	                              Array<Vertex3D>& vertices,
	                              Array<TriangleIndex32>& indices)
	{
		if (polygon.size() < 3) return;
		const Float3 normal = polygonNormal(polygon);

		for (size_t i = 1; i + 1 < polygon.size(); ++i)
		{
			const uint32 base = static_cast<uint32>(vertices.size());
			const TerrainClipVertex tri[3] = { polygon[0], polygon[i], polygon[i + 1] };
			for (const auto& v : tri)
			{
				Vertex3D vert;
				vert.pos = Float3{ static_cast<float>(v.pos.x), static_cast<float>(v.pos.y), static_cast<float>(v.pos.z) };
				vert.normal = normal;
				vert.tex = v.tex;
				vertices << vert;
			}
			const bool upward=(tri[1].pos-tri[0].pos).cross(tri[2].pos-tri[0].pos).y>=0;
			indices << (upward ? TriangleIndex32{base,base+1,base+2} : TriangleIndex32{base,base+2,base+1});
		}
	}

}

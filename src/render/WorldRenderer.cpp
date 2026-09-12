#include "WorldRenderer.hpp"
#include "TreeGeometry.hpp"
#include "../gen/UrbanParcel.hpp"
#include "../asset/AssetRegistrar.hpp"
#include "../debug/DebugLog.hpp"
#include "../road/RoadGeometry.hpp"
#include "../road/JunctionGeometry.hpp"
#include <Siv3D/Profiler.hpp>
#include <Siv3D/ViewFrustum.hpp>
#include <algorithm>

namespace
{
	float buildingBaseHeight(const World& world,const Building& building,float x,float z)
	{
		float height=world.sampleHeight(x,z);
		if (building.type==BuildingType::Parking) { return height; }
		const float half=buildingFootprintXZ(building.type)*.5f;
		const Vec2 along{Cos(building.angle)*half,Sin(building.angle)*half},across{-along.y,along.x};
		for (const int side : {-1,1})
		{
			for (const int end : {-1,1})
			{
				const Vec2 point=Vec2{x,z}+along*side+across*end;
				height=Max(height,world.sampleHeight(static_cast<float>(point.x),static_cast<float>(point.y)));
			}
		}
		return height+.04f;
	}
	constexpr float kTerrainCellSize      = static_cast<float>(CHUNK_SIZE) / HEIGHT_CELLS;
	constexpr float kRoadTerrainQuadStep  = 24.0f;
	constexpr float kRoadTerrainRelief    = 0.012f;
	constexpr float kTerrainClipEpsilon   = 1e-4f;
	constexpr float kChunkSizeF           = static_cast<float>(CHUNK_SIZE);
	constexpr float kTerrainTileSize      = 8.0f;
	constexpr float kTerrainUvCosA        = 0.97237f;
	constexpr float kTerrainUvSinA        = 0.23345f;

	struct TerrainClipVertex
	{
		Vec3   pos;
		Float2 tex;
	};

	float roadEdgeOffsetAt(const RoadPart& part, float ft, bool leftSide)
	{
		return leftSide
			? Math::Lerp(part.offsetA_L, part.offsetB_L, ft)
			: Math::Lerp(part.offsetA_R, part.offsetB_R, ft);
	}

	bool calcRoadCrossSectionRange(const RoadEdge& edge, float ft, float& outLeft, float& outRight)
	{
		bool found = false;
		for (const auto& part : edge.parts)
		{
			if (!RoadGeometry::isStructuralStrip(part))
			{
				continue;
			}

			const float left  = roadEdgeOffsetAt(part, ft, true);
			const float right = roadEdgeOffsetAt(part, ft, false);
			if (!found)
			{
				outLeft = Min(left, right);
				outRight = Max(left, right);
				found = true;
			}
			else
			{
				outLeft = Min(outLeft, Min(left, right));
				outRight = Max(outRight, Max(left, right));
			}
		}
		return found;
	}



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

	Float2 terrainUvAt(const Vec3& pos, int materialKey = 0)
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

	int terrainMaterialKeyForCell(const Chunk& chunk, int col, int row)
	{
		const int x = Clamp(col, 0, ZONE_CELLS - 1);
		const int z = Clamp(row, 0, ZONE_CELLS - 1);
		const float h = (chunk.heightMap[{ x, z }] + chunk.heightMap[{ x + 1, z }]
			+ chunk.heightMap[{ x, z + 1 }] + chunk.heightMap[{ x + 1, z + 1 }]) * 0.25f;
		if (h > 28.0f) return 1;
		return 0;
	}

	ColorF terrainMaterialColor(int materialKey)
	{
		switch (materialKey)
		{
		case 1: return ColorF{ 0.50, 0.62, 0.38 };
		case 2: return ColorF{ 0.38, 0.54, 0.30 };
		case 3: return ColorF{ 0.50, 0.45, 0.30 };
		case 4: return ColorF{ 0.76, 0.70, 0.52 };
		case 5: return ColorF{ 0.48, 0.58, 0.56 };
		default: return ColorF{ 0.42, 0.57, 0.32 };
		}
	}

	StringView terrainMaterialTexture(int materialKey)
	{
		switch (materialKey)
		{
		case 1:
		case 2:
			return Asset::SparseGrass;
		case 4:
			return Asset::Sand;
		case 5:
			return Asset::CoastSand;
		case 100:
		case 117:
		case 119:
		case 120:
			return Asset::Concrete;
		case 110:
		case 111:
		case 114:
			return Asset::Sand;
		case 101:
		case 113:
			return Asset::SparseGrass;
		default:
			return Asset::SparseGrass;
		}
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
	                                                        const Array<Vec2>& clipPolygon, Optional<float> roadbedY = none)
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
			indices << TriangleIndex32{ base, base + 1, base + 2 };
		}
	}

	std::unordered_set<int64> chunkKeysAroundPoint(const Vec3& pos, float radius)
	{
		std::unordered_set<int64> keys;
		const float minX = static_cast<float>(pos.x) - radius;
		const float maxX = static_cast<float>(pos.x) + radius;
		const float minZ = static_cast<float>(pos.z) - radius;
		const float maxZ = static_cast<float>(pos.z) + radius;
		const int chunkX0 = Clamp(static_cast<int>(Math::Floor(minX / kChunkSizeF)), 0, WORLD_CHUNKS - 1);
		const int chunkX1 = Clamp(static_cast<int>(Math::Floor(maxX / kChunkSizeF)), 0, WORLD_CHUNKS - 1);
		const int chunkZ0 = Clamp(static_cast<int>(Math::Floor(minZ / kChunkSizeF)), 0, WORLD_CHUNKS - 1);
		const int chunkZ1 = Clamp(static_cast<int>(Math::Floor(maxZ / kChunkSizeF)), 0, WORLD_CHUNKS - 1);
		for (int cz = chunkZ0; cz <= chunkZ1; ++cz)
			for (int cx = chunkX0; cx <= chunkX1; ++cx)
				keys.insert(chunkCoordToKey(Point{ cx, cz }));
		return keys;
	}

	String formatChunkKeySet(const std::unordered_set<int64>& keys)
	{
		Array<String> parts;
		parts.reserve(keys.size());
		for (const int64 key : keys)
		{
			const Point chunk{
				static_cast<int>(key >> 32),
				static_cast<int>(static_cast<uint32>(key))
			};
			parts << U"({}, {})"_fmt(chunk.x, chunk.y);
		}
		parts.sort();
		return U"[{}]"_fmt(parts.join(U", "));
	}

	String formatChunkKeyArray(const Array<int64>& keys)
	{
		Array<String> parts;
		parts.reserve(keys.size());
		for (const int64 key : keys)
		{
			const Point chunk{
				static_cast<int>(key >> 32),
				static_cast<int>(static_cast<uint32>(key))
			};
			parts << U"({}, {})"_fmt(chunk.x, chunk.y);
		}
		parts.sort();
		return U"[{}]"_fmt(parts.join(U", "));
	}

	String formatIntArray(const Array<int>& values)
	{
		Array<String> parts;
		parts.reserve(values.size());
		for (const int value : values)
			parts << ToString(value);
		parts.sort();
		return U"[{}]"_fmt(parts.join(U", "));
	}

}

namespace { ColorF detailColorForKey(int key); }

void WorldRenderer::preloadBuildingModels()
{
	const Stopwatch timer{StartImmediately::Yes};
	HashSet<uint32> loaded;
	for (int value = static_cast<int>(BuildingType::Detached); value <= static_cast<int>(BuildingType::Parking); ++value)
	{
		const auto type = static_cast<BuildingType>(value);
		if (!isObjBuildingType(type)) { continue; }
		for (int sample = 0; sample < 256; ++sample)
		{
			const uint8 variant = buildingModelVariant(type,sample,0);
			const uint32 key = (static_cast<uint32>(type)<<8)|variant;
			if (loaded.insert(key).second) { getBuildingModelAsset(type,variant); }
		}
	}
	DBG_LOG(U"[BuildingPreload] assets={} ms={:.2f}"_fmt(loaded.size(),timer.msF()));
}

void WorldRenderer::render(World& world, const RoadNetwork& network, const BasicCamera3D& camera)
{
	m_buildingEye=camera.getEyePosition();
	Array<Key> distantTrees;
	for (const Key key : m_detailedTreeChunks)
	{
		const Point coord{static_cast<int>(key>>32),static_cast<int>(static_cast<uint32>(key))};
		if (TreeGeometry::nearChunk(coord,m_buildingEye)) { continue; }
		if (auto cache=m_landscapeMeshCache.find(key);cache!=m_landscapeMeshCache.end())
		{
			cache->second.remove_if([](const BuildingBatch& batch){ return batch.materialKey>=1000; });
		}
		distantTrees << key;
	}
	for (const Key key : distantTrees) { m_detailedTreeChunks.erase(key);++m_geometryRevision; }
	// Upload a bounded portion of a completed worker snapshot per frame. Keep the
	// previous complete chunk visible until both terrain and landscape are ready.
	for (size_t index=0;index<m_terrainJobs.size();++index)
	{
		auto& job=m_terrainJobs[index];
		if (!job.result)
		{
			if (job.future.wait_for(std::chrono::seconds{0})!=std::future_status::ready) { continue; }
			job.result=job.future.get();
		}
		if (job.epoch!=m_terrainEpoch || job.revision!=m_terrainRevisions[job.key])
		{
			m_terrainJobs.erase(m_terrainJobs.begin()+index);
			break;
		}
		auto& result=*job.result;
		const Stopwatch upload{StartImmediately::Yes};
		constexpr double kUploadBudgetMilliseconds=2.0;
		while (job.terrainIndex<result.batches.size() && upload.msF()<kUploadBudgetMilliseconds)
		{
			auto& data=result.batches[job.terrainIndex++];
			if (!data.meshData.indices.isEmpty()) { job.uploadedTerrain << TerrainMeshBatch{data.materialKey,DynamicMesh{data.meshData}}; }
			data.meshData=MeshData{};
		}
		while (job.terrainIndex==result.batches.size() && job.landscapeIndex<result.landscape.size() && upload.msF()<kUploadBudgetMilliseconds)
		{
			auto& data=result.landscape[job.landscapeIndex++];
			if (!data.meshData.indices.isEmpty()) { job.uploadedLandscape << BuildingBatch{data.materialKey,detailColorForKey(data.materialKey),Mesh{data.meshData}}; }
			data.meshData=MeshData{};
		}
		const double elapsed=upload.msF();
		job.uploadTotalMilliseconds+=elapsed;job.maxUploadMilliseconds=Max(job.maxUploadMilliseconds,elapsed);++job.uploadFrames;
		if (job.terrainIndex==result.batches.size() && job.landscapeIndex==result.landscape.size())
		{
			m_meshCache[job.key]=std::move(job.uploadedTerrain);
			m_landscapeMeshCache[job.key]=std::move(job.uploadedLandscape);
			if (job.detailedTrees) { m_detailedTreeChunks.insert(job.key); }
			else { m_detailedTreeChunks.erase(job.key); }
			m_pendingTerrainRebuildKeys.erase(job.key);
			++m_geometryRevision;
			DBG_LOG(U"[Streaming] terrain workerMs={:.2f} uploadTotalMs={:.2f} maxFrameUploadMs={:.2f} uploadFrames={} detailedTrees={}"_fmt(
				result.milliseconds,job.uploadTotalMilliseconds,job.maxUploadMilliseconds,job.uploadFrames,job.detailedTrees));
			m_terrainJobs.erase(m_terrainJobs.begin()+index);
		}
		break;
	}
	// アクティブチャンクを近傍優先で回し、地形本体と水面を分けて描画する。
	// 地形メッシュを動的更新するため W100 警告を抑制する
	Profiler::EnableAssetCreationWarning(false);
	m_terrainRebuildBudget = Clamp(static_cast<int>(m_pendingTerrainRebuildKeys.size()), 1, 16);

	m_buildingFrustum = ViewFrustum{ camera, 9000.0 };
	m_buildingsConsidered = m_buildingsSubmitted = 0;
	const Vec3 eye = camera.getEyePosition();
	m_buildingEye = eye;
	const int camCx = static_cast<int>(Math::Floor(eye.x / CHUNK_SIZE));
	const int camCz = static_cast<int>(Math::Floor(eye.z / CHUNK_SIZE));
	const Point camChunk{ camCx, camCz };

	const auto& activeChunks = world.getActiveChunks();
	const bool activeChunkOrderChanged = (m_sortedChunks.size() != activeChunks.size())
		|| !std::equal(activeChunks.begin(), activeChunks.end(), m_sortedChunks.begin(), m_sortedChunks.end());

	// ソート済み順序を維持しつつ、カメラ位置やアクティブ集合が変わった時だけ並べ直す
	if (camChunk != m_lastSortChunk || activeChunks.size() != m_lastActiveCount || activeChunkOrderChanged)
	{
		m_sortedChunks = activeChunks;
		m_sortedChunks.sort_by([&](const Chunk* a, const Chunk* b)
		{
			return a->worldOrigin().distanceFromSq(eye) < b->worldOrigin().distanceFromSq(eye);
		});
		m_lastSortChunk  = camChunk;
		m_lastActiveCount = activeChunks.size();
	}

	const auto sceneSize = Scene::Size();
	constexpr float kMargin = 512.0f;

	for (Chunk* chunk : m_sortedChunks)
	{
		if (!chunk) continue;

		// カメラチャンクと周囲8チャンク（計9チャンク）は常に描画
		const int dx = Math::Abs(chunk->coord.x - camCx);
		const int dz = Math::Abs(chunk->coord.y - camCz);
		const bool isNearCamera = (dx <= 1 && dz <= 1);

		// BoundingBox の 8 頂点がすべてスクリーン外なら描画スキップ。
		// 頂点単位で判定するため、連続地形の境界付近が過剰カリングされにくい。
		const Vec3 o = chunk->worldOrigin();
		const double yLo = static_cast<double>(chunk->heightMin);
		const double yHi = static_cast<double>(chunk->heightMax);
		const double cs  = static_cast<double>(CHUNK_SIZE);
		const Float3 corners[8] = {
			Float3{ static_cast<float>(o.x),      static_cast<float>(yLo), static_cast<float>(o.z)      },
			Float3{ static_cast<float>(o.x + cs), static_cast<float>(yLo), static_cast<float>(o.z)      },
			Float3{ static_cast<float>(o.x),      static_cast<float>(yLo), static_cast<float>(o.z + cs) },
			Float3{ static_cast<float>(o.x + cs), static_cast<float>(yLo), static_cast<float>(o.z + cs) },
			Float3{ static_cast<float>(o.x),      static_cast<float>(yHi), static_cast<float>(o.z)      },
			Float3{ static_cast<float>(o.x + cs), static_cast<float>(yHi), static_cast<float>(o.z)      },
			Float3{ static_cast<float>(o.x),      static_cast<float>(yHi), static_cast<float>(o.z + cs) },
			Float3{ static_cast<float>(o.x + cs), static_cast<float>(yHi), static_cast<float>(o.z + cs) },
		};
		bool anyVisible = false;
		for (const auto& c : corners)
		{
			const Float3 sp = camera.worldToScreenPoint(c);
			if (sp.z > 0.0f &&
			    sp.x >= -kMargin && sp.x <= sceneSize.x + kMargin &&
			    sp.y >= -kMargin && sp.y <= sceneSize.y + kMargin)
			{
				anyVisible = true;
				break;
			}
		}
		if (!anyVisible && !isNearCamera) continue;

		drawChunk(*chunk, world, network);
	}

	// ---- 水面（y=0 の半透明平面）----
	{
		const ScopedRenderStates3D blend{ BlendState::Default2D };
		const ColorF waterColor = ColorF{ 0.34, 0.56, 0.68, 0.72 }.removeSRGBCurve();
		constexpr double cs = static_cast<double>(CHUNK_SIZE);
		bool hasWater = false;
		int minChunkX = 0;
		int maxChunkX = 0;
		int minChunkZ = 0;
		int maxChunkZ = 0;

		for (const Chunk* chunk : m_sortedChunks)
		{
			if (!chunk || chunk->heightMin > 0.0f) continue;
			if (!hasWater)
			{
				minChunkX = maxChunkX = chunk->coord.x;
				minChunkZ = maxChunkZ = chunk->coord.y;
				hasWater = true;
			}
			else
			{
				minChunkX = Min(minChunkX, chunk->coord.x);
				maxChunkX = Max(maxChunkX, chunk->coord.x);
				minChunkZ = Min(minChunkZ, chunk->coord.y);
				maxChunkZ = Max(maxChunkZ, chunk->coord.y);
			}
		}

		if (hasWater)
		{
			const double minX = static_cast<double>(minChunkX) * cs;
			const double maxX = static_cast<double>(maxChunkX + 1) * cs;
			const double minZ = static_cast<double>(minChunkZ) * cs;
			const double maxZ = static_cast<double>(maxChunkZ + 1) * cs;
			Box{ (minX + maxX) * 0.5, -0.08, (minZ + maxZ) * 0.5,
				maxX - minX, 0.04, maxZ - minZ }.draw(waterColor);
		}
	}
}

void WorldRenderer::drawChunk(Chunk& chunk, const World& world, const RoadNetwork& network)
{
	// チャンク単位で地形と建物の GPU キャッシュを更新し、そのまま描画まで完結させる。
	const Key key = chunkCoordToKey(chunk.coord);
	const double nearestDistance = m_buildingEye.distanceFrom(Vec3{
		Clamp(m_buildingEye.x, chunk.coord.x * static_cast<double>(CHUNK_SIZE), (chunk.coord.x + 1) * static_cast<double>(CHUNK_SIZE)),
		Clamp(m_buildingEye.y, static_cast<double>(chunk.heightMin), static_cast<double>(chunk.heightMax) + 8),
		Clamp(m_buildingEye.z, chunk.coord.y * static_cast<double>(CHUNK_SIZE), (chunk.coord.y + 1) * static_cast<double>(CHUNK_SIZE)) });
	if (nearestDistance > 600) { m_distantDetailChunks.insert(key); }
	else { m_distantDetailChunks.erase(key); }
	const bool needsTreeDetails=m_asyncTerrain && TreeGeometry::nearChunk(chunk.coord,m_buildingEye) && !m_detailedTreeChunks.contains(key);
	const bool terrainDirty = chunk.meshDirty || m_pendingTerrainRebuildKeys.contains(key) || needsTreeDetails;

	auto rebuildTerrainBatches = [&]
	{
		Array<TerrainMeshBatch> batches;
		for (auto& data : buildTerrainMeshData(chunk, getChunkSubtractionQuads(network, chunk.coord)))
		{
			if (data.meshData.vertices.isEmpty()) continue;
			TerrainMeshBatch batch;
			batch.materialKey = data.materialKey;
			batch.mesh = DynamicMesh{ data.meshData };
			batches << std::move(batch);
		}
		m_meshCache[key] = std::move(batches);
		++m_geometryRevision;
	};

	if (m_asyncTerrain)
	{
		const bool absent = !m_meshCache.contains(key);
		if (absent || terrainDirty)
		{
			// A cheap uncut terrain remains visible until the exact road cut is ready.
			if (absent)
			{
				Array<TerrainMeshBatch> batches;
				for (auto& data : buildTerrainMeshData(chunk, {}))
				{
					batches << TerrainMeshBatch{data.materialKey, DynamicMesh{data.meshData}};
				}
				m_meshCache[key] = std::move(batches);
				m_pendingTerrainRebuildKeys.insert(key);
			}
			if (chunk.meshDirty || !m_buildingMeshCache.contains(key))
			{
				const Stopwatch timer{ StartImmediately::Yes };
				rebuildBuildingMeshes(key, chunk, world);
				DBG_LOG(U"[Streaming] buildings chunk=({}, {}) ms={:.2f}"_fmt(chunk.coord.x,chunk.coord.y,timer.msF()));
				++m_terrainRevisions[key];
				chunk.meshDirty = false;
			}
			const bool running = std::any_of(m_terrainJobs.begin(), m_terrainJobs.end(), [key](const TerrainJob& job) { return job.key == key; });
			if (!running && m_terrainJobs.size() < 2)
			{
				Chunk snapshot;
				snapshot.coord = chunk.coord;
				snapshot.heightMap = chunk.heightMap;
				snapshot.buildingGrid = chunk.buildingGrid;
				snapshot.zoneMap = chunk.zoneMap;
				snapshot.isUrbanizationArea = chunk.isUrbanizationArea;
				snapshot.landPatches = chunk.landPatches;
				Array<Chunk> heightSnapshots;
				for (int dz = -1; dz <= 1; ++dz) for (int dx = -1; dx <= 1; ++dx)
				{
					if (const auto* neighbor = world.getChunk(chunk.coord+Point{dx,dz}))
					{
						Chunk heights;
						heights.coord = neighbor->coord;
						heights.heightMap = neighbor->heightMap;
						heightSnapshots << std::move(heights);
					}
				}
				auto quads = getChunkSubtractionQuads(network, chunk.coord);
				TerrainJob job{ key, m_terrainEpoch, m_terrainRevisions[key], {} };
				job.detailedTrees=TreeGeometry::nearChunk(chunk.coord,m_buildingEye);
				job.future = std::async(std::launch::async, [snapshot = std::move(snapshot), quads = std::move(quads), heightSnapshots = std::move(heightSnapshots),detailedTrees=job.detailedTrees]()
				{
					const Stopwatch timer{ StartImmediately::Yes };
					TerrainJobResult result;
					result.batches = buildTerrainMeshData(snapshot, quads);
					result.landscape = buildLandscapeMeshData(snapshot,quads,heightSnapshots,detailedTrees);
					result.milliseconds = timer.msF();
					return result;
				});
				m_terrainJobs << std::move(job);
			}
		}
	}
	else
	{
		if (!m_meshCache.contains(key))
		{
			DBG_LOG(U"[TerrainBool] drawChunk build chunk=({}, {}) dirty={} pending={}"_fmt(
				chunk.coord.x, chunk.coord.y, chunk.meshDirty ? 1 : 0,
				m_pendingTerrainRebuildKeys.contains(key) ? 1 : 0));
			rebuildTerrainBatches();
			m_pendingTerrainRebuildKeys.erase(key);
			chunk.meshDirty = false;
			rebuildBuildingMeshes(key, chunk, world);
		}
		else if (terrainDirty && m_terrainRebuildBudget > 0)
		{
			DBG_LOG(U"[TerrainBool] drawChunk refill chunk=({}, {}) dirty={} pending={} budget={}"_fmt(
				chunk.coord.x, chunk.coord.y, chunk.meshDirty ? 1 : 0,
				m_pendingTerrainRebuildKeys.contains(key) ? 1 : 0, m_terrainRebuildBudget));
			rebuildTerrainBatches();
			m_pendingTerrainRebuildKeys.erase(key);
			chunk.meshDirty = false;
			--m_terrainRebuildBudget;
			rebuildBuildingMeshes(key, chunk, world);
		}
	
	}

	// 急斜面では地形メッシュの薄い断面が見えるため両面描画にする
	const ScopedRenderStates3D cullNone{ RasterizerState::SolidCullNone };
	if (const auto it = m_meshCache.find(key); it != m_meshCache.end())
	{
		const ScopedCustomShader3D shader{ m_terrainShader };
		Graphics3D::SetPSTexture(2, TextureAsset(Asset::CoastSand));
		for (const TerrainMeshBatch& batch : it->second)
		{
			batch.mesh.draw(TextureAsset(terrainMaterialTexture(batch.materialKey)), terrainMaterialColor(batch.materialKey));
		}
	}
	drawCachedBuildings(key);
}

Array<WorldRenderer::TerrainMeshData> WorldRenderer::buildTerrainMeshData(const Chunk& chunk, const Array<TerrainSubtractionQuad>& quads)
{
	const Vec3 worldOrigin = chunk.worldOrigin();
	const Stopwatch buildTimer{ StartImmediately::Yes };
	// Bucket road footprints per terrain cell. Dense towns keep the same geometry as villages.
	Grid<Array<size_t>> cellQuads(HEIGHT_CELLS, HEIGHT_CELLS);
	size_t candidateTests = 0;
	for (size_t index = 0; index < quads.size(); ++index)
	{
		const RectF& bounds = quads[index].bounds;
		const int minCol = Clamp(static_cast<int>(Floor((bounds.x - worldOrigin.x) / kTerrainCellSize)), 0, HEIGHT_CELLS - 1);
		const int maxCol = Clamp(static_cast<int>(Floor((bounds.x + bounds.w - worldOrigin.x) / kTerrainCellSize)), 0, HEIGHT_CELLS - 1);
		const int minRow = Clamp(static_cast<int>(Floor((bounds.y - worldOrigin.z) / kTerrainCellSize)), 0, HEIGHT_CELLS - 1);
		const int maxRow = Clamp(static_cast<int>(Floor((bounds.y + bounds.h - worldOrigin.z) / kTerrainCellSize)), 0, HEIGHT_CELLS - 1);
		for (int row = minRow; row <= maxRow; ++row)
		{
			for (int col = minCol; col <= maxCol; ++col)
			{
				cellQuads[{ col, row }] << index;
			}
		}
	}

	HashTable<int, MeshData> groups;

	auto makeGridVertex = [&](int col, int row)
	{
		const Vec3 pos = worldOrigin + Vec3{
			col * kTerrainCellSize,
			chunk.heightMap[{ col, row }],
			row * kTerrainCellSize
		};
		return TerrainClipVertex{ pos, terrainUvAt(pos) };
	};

	auto processTriangle = [&](int materialKey, const Array<size_t>& candidates, const TerrainClipVertex& a, const TerrainClipVertex& b, const TerrainClipVertex& c)
	{
		Array<Array<TerrainClipVertex>> pieces;
		pieces << Array<TerrainClipVertex>{ a, b, c };

		if (!candidates.isEmpty())
		{
			RectF triBounds = boundsOfPolygon({
				Vec2{ static_cast<float>(a.pos.x), static_cast<float>(a.pos.z) },
				Vec2{ static_cast<float>(b.pos.x), static_cast<float>(b.pos.z) },
				Vec2{ static_cast<float>(c.pos.x), static_cast<float>(c.pos.z) },
			});

			for (const size_t index : candidates)
			{
				++candidateTests;
				const auto& quad = quads[index];
				if (!rectIntersects(triBounds, quad.bounds)) continue;
				// The replacement soil must also remain below the actual sampled terrain,
				// since node Bezier heights and the rendered terrain can differ.
				const float floorY = Min(quad.bedBottomY,
					static_cast<float>(Min(Min(a.pos.y, b.pos.y), c.pos.y)) - 0.04f);

				Array<Array<TerrainClipVertex>> nextPieces;
				for (const auto& piece : pieces)
				{
					Array<TerrainClipVertex> below = clipPolygonByHeight(piece, floorY, true);
					if (below.size() >= 3) nextPieces << std::move(below);

					Array<TerrainClipVertex> above = clipPolygonByHeight(piece, floorY, false);
					if (above.size() >= 3)
					{
						Array<Array<TerrainClipVertex>> kept = subtractConvexPolygonXZ(above, quad.footprint, floorY);
						for (auto& poly : kept)
						{
							if (poly.size() >= 3) nextPieces << std::move(poly);
						}
					}
				}

				pieces = std::move(nextPieces);
				if (pieces.isEmpty()) break;
			}
		}

		MeshData& group = groups[materialKey];
		for (const auto& poly : pieces)
		{
			Array<TerrainClipVertex> materialPoly;
			materialPoly.reserve(poly.size());
			for (const TerrainClipVertex& v : poly) materialPoly << withMaterialUv(v, materialKey);
			appendPolygonAsTriangles(materialPoly, group.vertices, group.indices);
		}
	};

	for (int row = 0; row < HEIGHT_CELLS; ++row)
	{
		for (int col = 0; col < HEIGHT_CELLS; ++col)
		{
			const TerrainClipVertex v00 = makeGridVertex(col, row);
			const TerrainClipVertex v10 = makeGridVertex(col + 1, row);
			const TerrainClipVertex v01 = makeGridVertex(col, row + 1);
			const TerrainClipVertex v11 = makeGridVertex(col + 1, row + 1);

			const int materialKey = terrainMaterialKeyForCell(chunk, col, row);
			processTriangle(materialKey, cellQuads[{ col, row }], v00, v01, v10);
			processTriangle(materialKey, cellQuads[{ col, row }], v10, v01, v11);
		}
	}

	(void)candidateTests;
	(void)buildTimer;
	Array<TerrainMeshData> result;
	result.reserve(groups.size());
	for (auto& [materialKey, meshData] : groups)
	{
		if (meshData.vertices.isEmpty()) continue;
		result << TerrainMeshData{ materialKey, std::move(meshData) };
	}
	return result;
}

const WorldRenderer::TerrainBooleanSubtractor* WorldRenderer::getTerrainSubtractor(
	const RoadNetwork& network, int edgeId)
{
	if (const auto it = m_edgeSubtractorCache.find(edgeId); it != m_edgeSubtractorCache.end())
		return &it->second;

	const RoadEdge* edge = network.getEdge(edgeId);
	if (!edge || (edge->edgeState != EdgeState::Open && edge->edgeState != EdgeState::Existing && edge->edgeState != EdgeState::UnderConstruction) || edge->parts.isEmpty())
		return nullptr;

	const auto bezOpt = network.getBezier(edgeId);
	if (!bezOpt || bezOpt->totalLength <= 0.0f)
		return nullptr;

	TerrainBooleanSubtractor subtractor;
	subtractor.edgeId = edgeId;
	std::unordered_set<Key> touchedChunkSet;
	auto registerSubtractionFootprint = [&](Array<Vec2> footprint, float bedBottomY)
	{
		if (footprint.size() < 3) return;
		if (signedAreaXZ(footprint) < 0.0f)
			footprint.reverse();

		const RectF bounds = boundsOfPolygon(footprint);
		if (bounds.w <= kTerrainClipEpsilon || bounds.h <= kTerrainClipEpsilon)
			return;

		TerrainSubtractionQuad quad;
		quad.footprint = std::move(footprint);
		quad.bounds = bounds;
		quad.bedBottomY = bedBottomY;
		subtractor.quads << quad;

		const int chunkX0 = Clamp(static_cast<int>(Math::Floor(bounds.x / kChunkSizeF)), 0, WORLD_CHUNKS - 1);
		const int chunkX1 = Clamp(static_cast<int>(Math::Floor((bounds.x + bounds.w) / kChunkSizeF)), 0, WORLD_CHUNKS - 1);
		const int chunkZ0 = Clamp(static_cast<int>(Math::Floor(bounds.y / kChunkSizeF)), 0, WORLD_CHUNKS - 1);
		const int chunkZ1 = Clamp(static_cast<int>(Math::Floor((bounds.y + bounds.h) / kChunkSizeF)), 0, WORLD_CHUNKS - 1);
		for (int cz = chunkZ0; cz <= chunkZ1; ++cz)
			for (int cx = chunkX0; cx <= chunkX1; ++cx)
				touchedChunkSet.insert(chunkCoordToKey(Point{ cx, cz }));
	};

	if(edge->useElevation)
	{
		for(const auto& object:network.objects())
		{
			if(object.id<0 || object.parentEdgeId!=edgeId || object.type!=RoadObjectType::Pier) continue;
			const Vec3 position=bezOpt->positionAt(object.arcPos);
			const Vec3 right=tangentToRight(bezOpt->tangentAt(object.arcPos)),along{-right.z,0,right.x};
			const double halfWidth=edge->edgeState==EdgeState::UnderConstruction ? 3.0 : 2.4;
			Array<Vec2> footprint;
			for(const Vec2 corner:{Vec2{-1,-1},Vec2{1,-1},Vec2{1,1},Vec2{-1,1}})
			{
				const Vec3 p=position+right*corner.x*halfWidth+along*corner.y*halfWidth;
				footprint << Vec2{p.x,p.z};
			}
			// Shallow foundation bed; the simulation heightfield remains unchanged.
			registerSubtractionFootprint(std::move(footprint),static_cast<float>(position.y)+100.0f);
		}
		if(subtractor.quads.isEmpty()) return nullptr;
		for(const Key key:touchedChunkSet) subtractor.touchedChunkKeys << key;
		return &m_edgeSubtractorCache.emplace(edgeId,std::move(subtractor)).first->second;
	}

	const CubicBezier& bez = *bezOpt;
	const float startArc = edge->edgeState == EdgeState::UnderConstruction ? 0.0f : Max(0.0f, edge->cutoffA - 0.1f);
	const float endArc = edge->edgeState == EdgeState::UnderConstruction ? bez.totalLength : Min(bez.totalLength, bez.totalLength - edge->cutoffB + 0.1f);
	const float visibleLength = Max(0.0f, endArc - startArc);
	const int sampleCount = Max(2, static_cast<int>(Math::Ceil(visibleLength / kRoadTerrainQuadStep)) + 1);

	struct EdgeSample
	{
		Vec3  center;
		Vec2  left;
		Vec2  right;
		float bedBottomY = 0.0f;
	};

	Array<EdgeSample> samples;
	samples.reserve(sampleCount);

	for (int i = 0; i < sampleCount; ++i)
	{
		const float ft = (sampleCount <= 1) ? 0.0f : (i / static_cast<float>(sampleCount - 1));
		const float s = Math::Lerp(startArc, endArc, ft);
		const Vec3 center = bez.positionAt(s);
		float leftOffset = 0.0f;
		float rightOffset = 0.0f;
		if (!calcRoadCrossSectionRange(*edge, s / bez.totalLength, leftOffset, rightOffset))
			continue;

		const Vec3 rightVec3 = tangentToRight(bez.tangentAt(s));
		const Vec2 rightVec{ static_cast<float>(rightVec3.x), static_cast<float>(rightVec3.z) };
		EdgeSample sample;
		sample.center = center;
		sample.left = Vec2{
			static_cast<float>(center.x + rightVec.x * leftOffset),
			static_cast<float>(center.z + rightVec.y * leftOffset)
		};
		sample.right = Vec2{
			static_cast<float>(center.x + rightVec.x * rightOffset),
			static_cast<float>(center.z + rightVec.y * rightOffset)
		};
		sample.bedBottomY = static_cast<float>(center.y + kRoadSurfaceLift - (edge->edgeState==EdgeState::UnderConstruction ? .25 : kRoadTerrainRelief));
		samples << sample;
	}

	if (samples.size() < 2)
		return nullptr;

	for (size_t i = 0; i + 1 < samples.size(); ++i)
	{
		Array<Vec2> footprint = {
			samples[i].left,
			samples[i + 1].left,
			samples[i + 1].right,
			samples[i].right,
		};
		registerSubtractionFootprint(std::move(footprint),
			(samples[i].bedBottomY + samples[i + 1].bedBottomY) * 0.5f);
	}



	if (subtractor.quads.isEmpty())
		return nullptr;

	for (const Key key : touchedChunkSet)
		subtractor.touchedChunkKeys << key;
	auto [it, inserted] = m_edgeSubtractorCache.emplace(edgeId, std::move(subtractor));
	return &it->second;
}

const WorldRenderer::TerrainNodeSubtractor* WorldRenderer::getTerrainNodeSubtractor(
	const RoadNetwork& network, int nodeId)
{
	if (const auto it = m_nodeSubtractorCache.find(nodeId); it != m_nodeSubtractorCache.end())
		return &it->second;

	const RoadNode* node = network.getNode(nodeId);
	if (!node || node->attachments.size() < 2)
		return nullptr;

	if (node->type != NodeType::Intersection && node->type != NodeType::Diverge && node->type != NodeType::Joint)
		return nullptr;

	bool underground=true;
	for (const auto& attachment : node->attachments) { const auto* edge=network.getEdge(attachment.edgeId); underground &= edge && edge->tunnel; }
	if (underground) { return nullptr; }
	TerrainNodeSubtractor subtractor;
	subtractor.nodeId = nodeId;
	std::unordered_set<Key> touchedChunkSet;
	auto registerSubtractionFootprint = [&](Array<Vec2> footprint, float bedBottomY)
	{
		if (footprint.size() < 3) return;
		if (signedAreaXZ(footprint) < 0.0f)
			footprint.reverse();

		const RectF bounds = boundsOfPolygon(footprint);
		if (bounds.w <= kTerrainClipEpsilon || bounds.h <= kTerrainClipEpsilon)
			return;

		TerrainSubtractionQuad quad;
		quad.footprint = std::move(footprint);
		quad.bounds = bounds;
		quad.bedBottomY = bedBottomY;
		subtractor.quads << quad;

		const int chunkX0 = Clamp(static_cast<int>(Math::Floor(bounds.x / kChunkSizeF)), 0, WORLD_CHUNKS - 1);
		const int chunkX1 = Clamp(static_cast<int>(Math::Floor((bounds.x + bounds.w) / kChunkSizeF)), 0, WORLD_CHUNKS - 1);
		const int chunkZ0 = Clamp(static_cast<int>(Math::Floor(bounds.y / kChunkSizeF)), 0, WORLD_CHUNKS - 1);
		const int chunkZ1 = Clamp(static_cast<int>(Math::Floor((bounds.y + bounds.h) / kChunkSizeF)), 0, WORLD_CHUNKS - 1);
		for (int cz = chunkZ0; cz <= chunkZ1; ++cz)
			for (int cx = chunkX0; cx <= chunkX1; ++cx)
				touchedChunkSet.insert(chunkCoordToKey(Point{ cx, cz }));
	};

	const auto layout = JunctionGeometry::build(network, nodeId);
	if (layout.elevated && !layout.groundConnected) { return nullptr; }
	const MeshData footprint = JunctionGeometry::terrainFootprint(layout);
	// Merge only adjacent triangles whose union remains convex. This preserves the
	// concave kerb exactly while avoiding hundreds of repeated cuts in one terrain cell.
	Array<Array<uint32>> pieces;
	for (const auto& triangle : footprint.indices) { pieces << Array<uint32>{ triangle.i0, triangle.i1, triangle.i2 }; }
	for (size_t i = 0; i < pieces.size(); ++i)
	{
		bool merged = true;
		while (merged)
		{
			merged = false;
			for (size_t j = i + 1; j < pieces.size() && !merged; ++j)
			{
				const auto& a = pieces[i];
				const auto& b = pieces[j];
				if (a.size() + b.size() > 18) { continue; }
				for (size_t ia = 0; ia < a.size() && !merged; ++ia)
				{
					for (size_t ib = 0; ib < b.size() && !merged; ++ib)
					{
						if (a[ia] != b[(ib+1)%b.size()] || a[(ia+1)%a.size()] != b[ib]) { continue; }
						Array<uint32> joined;
						for (size_t offset = 1; offset <= a.size(); ++offset) { joined << a[(ia+offset)%a.size()]; }
						for (size_t offset = 2; offset < b.size(); ++offset) { joined << b[(ib+offset)%b.size()]; }
						double sign = 0.0;
						bool convex = true;
						for (size_t k = 0; k < joined.size(); ++k)
						{
							const auto p = footprint.vertices[joined[k]].pos;
							const auto q = footprint.vertices[joined[(k+1)%joined.size()]].pos;
							const auto r = footprint.vertices[joined[(k+2)%joined.size()]].pos;
							const double turn = (q.x-p.x)*(r.z-q.z) - (q.z-p.z)*(r.x-q.x);
							if (Abs(turn) < 1e-6) { continue; }
							if (sign * turn < 0.0) { convex = false; break; }
							sign = turn;
						}
						if (!convex) { continue; }
						pieces[i] = std::move(joined);
						pieces.erase(pieces.begin() + j);
						merged = true;
					}
				}
			}
		}
	}
	for (const auto& piece : pieces)
	{
		Array<Vec2> polygon;
		float floor = Math::InfF;
		for (const uint32 index : piece)
		{
			const auto& position = footprint.vertices[index].pos;
			polygon << Vec2{ position.x, position.z };
			floor = Min(floor, position.y);
		}
		registerSubtractionFootprint(std::move(polygon), floor - kRoadTerrainRelief);
	}

	if (subtractor.quads.isEmpty())
		return nullptr;

	for (const Key key : touchedChunkSet)
		subtractor.touchedChunkKeys << key;
	auto [it, inserted] = m_nodeSubtractorCache.emplace(nodeId, std::move(subtractor));
	return &it->second;
}

const Array<WorldRenderer::TerrainSubtractionQuad>& WorldRenderer::getChunkSubtractionQuads(
	const RoadNetwork& network, Point chunkCoord)
{
	const Key chunkKey = chunkCoordToKey(chunkCoord);
	if (const auto it = m_chunkSubtractorCache.find(chunkKey); it != m_chunkSubtractorCache.end())
		return it->second;

	const RectF chunkBounds{
		static_cast<float>(chunkCoord.x * CHUNK_SIZE),
		static_cast<float>(chunkCoord.y * CHUNK_SIZE),
		static_cast<float>(CHUNK_SIZE),
		static_cast<float>(CHUNK_SIZE)
	};

	if (!m_chunkSubtractorPrimed)
	{
		primeAllChunkSubtractorCaches(network);
		if (const auto it = m_chunkSubtractorCache.find(chunkKey); it != m_chunkSubtractorCache.end())
			return it->second;
	}

	Array<TerrainSubtractionQuad> assembled;
	for (const auto& opening : m_tunnelOpenings) { if (opening.bounds.intersects(chunkBounds)) { assembled << TerrainSubtractionQuad{opening.footprint,opening.bounds,opening.floor}; } }
	Array<int> edgeContributors;
	for (const auto& entry : m_edgeSubtractorCache)
	{
		const TerrainBooleanSubtractor& subtractor = entry.second;
		bool touchesChunk = false;
		for (const Key key : subtractor.touchedChunkKeys)
		{
			if (key == chunkKey)
			{
				touchesChunk = true;
				break;
			}
		}
		if (!touchesChunk) continue;

		for (const auto& quad : subtractor.quads)
		{
			if (rectIntersects(chunkBounds, quad.bounds))
			{
				assembled << quad;
				if (!edgeContributors.contains(entry.first))
					edgeContributors << entry.first;
			}
		}
	}

	Array<int> nodeContributors;
	for (const auto& entry : m_nodeSubtractorCache)
	{
		const TerrainNodeSubtractor& subtractor = entry.second;
		bool touchesChunk = false;
		for (const Key key : subtractor.touchedChunkKeys)
		{
			if (key == chunkKey)
			{
				touchesChunk = true;
				break;
			}
		}
		if (!touchesChunk) continue;

		for (const auto& quad : subtractor.quads)
		{
			if (rectIntersects(chunkBounds, quad.bounds))
			{
				assembled << quad;
				if (!nodeContributors.contains(entry.first))
					nodeContributors << entry.first;
			}
		}
	}

	if (!assembled.isEmpty())
	{
		DBG_LOG(U"[TerrainBool] chunkSources chunk=({}, {}) quads={} edges={} nodes={}"_fmt(
			chunkCoord.x, chunkCoord.y, assembled.size(),
			formatIntArray(edgeContributors), formatIntArray(nodeContributors)));
	}

	auto [it, inserted] = m_chunkSubtractorCache.emplace(chunkKey, std::move(assembled));
	return it->second;
}

void WorldRenderer::primeAllChunkSubtractorCaches(const RoadNetwork& network)
{
	m_chunkSubtractorCache.clear();

	for (const auto& edge : network.edges())
	{
		if (edge.id < 0) continue;
		(void)getTerrainSubtractor(network, edge.id);
	}
	for (const auto& node : network.nodes())
	{
		if (node.id < 0) continue;
		(void)getTerrainNodeSubtractor(network, node.id);
	}

	for (const auto& [edgeId, subtractor] : m_edgeSubtractorCache)
	{
		for (const Key key : subtractor.touchedChunkKeys)
		{
			auto& bucket = m_chunkSubtractorCache[key];
			for (const auto& quad : subtractor.quads)
				bucket << quad;
		}
	}
	for (const auto& [nodeId, subtractor] : m_nodeSubtractorCache)
	{
		for (const Key key : subtractor.touchedChunkKeys)
		{
			auto& bucket = m_chunkSubtractorCache[key];
			for (const auto& quad : subtractor.quads)
				bucket << quad;
		}
	}

	for (const auto& opening : m_tunnelOpenings)
	{
		for (int z=Max(0,static_cast<int>(opening.bounds.y/CHUNK_SIZE));z<=Min(WORLD_CHUNKS-1,static_cast<int>(opening.bounds.br().y/CHUNK_SIZE));++z)
			for (int x=Max(0,static_cast<int>(opening.bounds.x/CHUNK_SIZE));x<=Min(WORLD_CHUNKS-1,static_cast<int>(opening.bounds.br().x/CHUNK_SIZE));++x)
				m_chunkSubtractorCache[chunkCoordToKey({x,z})] << TerrainSubtractionQuad{opening.footprint,opening.bounds,opening.floor};
	}
	m_chunkSubtractorPrimed = true;
}

void WorldRenderer::invalidateTerrainChunkKeys(const std::unordered_set<Key>& chunkKeys, bool rebuildImmediately)
{
	if (chunkKeys.empty()) return;

	for (const Key key : chunkKeys)
	{
		++m_terrainRevisions[key];
		m_chunkSubtractorCache.erase(key);
		if (rebuildImmediately)
		{
			m_meshCache.erase(key);
			m_pendingTerrainRebuildKeys.erase(key);
		}
		else
		{
			m_pendingTerrainRebuildKeys.insert(key);
		}
	}
}

void WorldRenderer::invalidateTerrainForEdges(const RoadNetwork& network, const Array<int>& edgeIds)
{
	std::unordered_set<Key> dirtyChunkKeys;
	std::unordered_set<int> uniqueEdgeIds;
	for (const int edgeId : edgeIds)
	{
		if (edgeId >= 0)
			uniqueEdgeIds.insert(edgeId);
	}

	for (const int edgeId : uniqueEdgeIds)
	{
		if (const auto it = m_edgeSubtractorCache.find(edgeId); it != m_edgeSubtractorCache.end())
		{
			for (const Key key : it->second.touchedChunkKeys)
				dirtyChunkKeys.insert(key);
			m_edgeSubtractorCache.erase(it);
		}

		if (const TerrainBooleanSubtractor* updated = getTerrainSubtractor(network, edgeId))
		{
			for (const Key key : updated->touchedChunkKeys)
				dirtyChunkKeys.insert(key);
		}
	}

	invalidateTerrainChunkKeys(dirtyChunkKeys, true);
}

void WorldRenderer::invalidateTerrainNearDirtyNodes(const RoadNetwork& network, const Array<int>& nodeIds)
{
	std::unordered_set<Key> dirtyChunkKeys;
	std::unordered_set<int> dirtyEdgeIds;
	std::unordered_set<int> uniqueNodeIds;

	for (const int nodeId : nodeIds)
	{
		if (nodeId < 0 || uniqueNodeIds.contains(nodeId)) continue;
		uniqueNodeIds.insert(nodeId);

		const RoadNode* node = network.getNode(nodeId);
		if (!node) continue;

		const auto nodeKeys = chunkKeysAroundPoint(node->position, 96.0f);
		dirtyChunkKeys.insert(nodeKeys.begin(), nodeKeys.end());

		for (const auto& att : node->attachments)
		{
			dirtyEdgeIds.insert(att.edgeId);
			const RoadEdge* edge = network.getEdge(att.edgeId);
			if (!edge) continue;

			const RoadNode* nodeA = network.getNode(edge->nodeA);
			const RoadNode* nodeB = network.getNode(edge->nodeB);
			if (nodeA)
			{
				const auto keys = chunkKeysAroundPoint(nodeA->position, 96.0f);
				dirtyChunkKeys.insert(keys.begin(), keys.end());
			}
			if (nodeB)
			{
				const auto keys = chunkKeysAroundPoint(nodeB->position, 96.0f);
				dirtyChunkKeys.insert(keys.begin(), keys.end());
			}
		}
	}

	for (const int edgeId : dirtyEdgeIds)
		m_edgeSubtractorCache.erase(edgeId);
	for (const int nodeId : uniqueNodeIds)
		m_nodeSubtractorCache.erase(nodeId);

	for (const int edgeId : dirtyEdgeIds)
		(void)getTerrainSubtractor(network, edgeId);
	for (const int nodeId : uniqueNodeIds)
		(void)getTerrainNodeSubtractor(network, nodeId);

	invalidateTerrainChunkKeys(dirtyChunkKeys);
}

void WorldRenderer::invalidateTerrainNearMovedNode(const RoadNetwork& network, int nodeId, Vec3 oldNodePos)
{
	constexpr float InvalidationRadius = 96.0f;

	const RoadNode* node = network.getNode(nodeId);
	if (!node)
	{
		DBG_LOG(U"[TerrainBool] invalidateMoved node={} missing"_fmt(nodeId));
		invalidateTerrainForNode(nodeId);
		return;
	}

	DBG_LOG(U"[TerrainBool] invalidateMoved node={} old=({:.1f}, {:.1f}, {:.1f}) new=({:.1f}, {:.1f}, {:.1f})"_fmt(
		nodeId, oldNodePos.x, oldNodePos.y, oldNodePos.z, node->position.x, node->position.y, node->position.z));

	std::unordered_set<Key> dirtyChunkKeys;
	const auto oldNodeKeys = chunkKeysAroundPoint(oldNodePos, InvalidationRadius);
	dirtyChunkKeys.insert(oldNodeKeys.begin(), oldNodeKeys.end());

	const auto currentNodeKeys = chunkKeysAroundPoint(node->position, InvalidationRadius);
	dirtyChunkKeys.insert(currentNodeKeys.begin(), currentNodeKeys.end());

	std::unordered_set<int> dirtyEdgeIds;
	std::unordered_set<int> dirtyNodeIds;
	dirtyNodeIds.insert(nodeId);
	for (const auto& att : node->attachments)
	{
		dirtyEdgeIds.insert(att.edgeId);

		const RoadEdge* edge = network.getEdge(att.edgeId);
		if (!edge) continue;
		if (edge->nodeA >= 0) dirtyNodeIds.insert(edge->nodeA);
		if (edge->nodeB >= 0) dirtyNodeIds.insert(edge->nodeB);

		if (const RoadNode* nodeA = network.getNode(edge->nodeA))
		{
			const auto keys = chunkKeysAroundPoint(nodeA->position, InvalidationRadius);
			dirtyChunkKeys.insert(keys.begin(), keys.end());
		}
		if (const RoadNode* nodeB = network.getNode(edge->nodeB))
		{
			const auto keys = chunkKeysAroundPoint(nodeB->position, InvalidationRadius);
			dirtyChunkKeys.insert(keys.begin(), keys.end());
		}
	}

	Array<int> expandedEdgeIds;
	for (const int edgeId : dirtyEdgeIds)
		expandedEdgeIds << edgeId;
	Array<int> expandedNodeIds;
	for (const int dirtyNodeId : dirtyNodeIds)
		expandedNodeIds << dirtyNodeId;
	DBG_LOG(U"[TerrainBool] invalidateMoved expandedEdges={} expandedNodes={}"_fmt(
		formatIntArray(expandedEdgeIds), formatIntArray(expandedNodeIds)));

	for (const int edgeId : dirtyEdgeIds)
	{
		if (const auto it = m_edgeSubtractorCache.find(edgeId); it != m_edgeSubtractorCache.end())
		{
			for (const Key key : it->second.touchedChunkKeys)
				dirtyChunkKeys.insert(key);
			m_edgeSubtractorCache.erase(it);
		}
	}
	for (const int dirtyNodeId : dirtyNodeIds)
	{
		if (const auto it = m_nodeSubtractorCache.find(dirtyNodeId); it != m_nodeSubtractorCache.end())
		{
			for (const Key key : it->second.touchedChunkKeys)
				dirtyChunkKeys.insert(key);
			m_nodeSubtractorCache.erase(it);
		}
	}

	DBG_LOG(U"[TerrainBool] invalidateMoved node={} finalDirtyChunks={}"_fmt(
		nodeId, dirtyChunkKeys.size()));
	DBG_LOG(U"[TerrainBool] invalidateMoved finalChunks={}"_fmt(formatChunkKeySet(dirtyChunkKeys)));

	invalidateTerrainChunkKeys(dirtyChunkKeys);
}

void WorldRenderer::invalidateTerrainForNodes(const RoadNetwork& network, const Array<int>& nodeIds)
{
	std::unordered_set<Key> dirtyChunkKeys;
	std::unordered_set<int> uniqueNodeIds;
	for (const int nodeId : nodeIds)
	{
		if (nodeId >= 0)
			uniqueNodeIds.insert(nodeId);
	}

	for (const int nodeId : uniqueNodeIds)
	{
		if (const auto it = m_nodeSubtractorCache.find(nodeId); it != m_nodeSubtractorCache.end())
		{
			for (const Key key : it->second.touchedChunkKeys)
				dirtyChunkKeys.insert(key);
			m_nodeSubtractorCache.erase(it);
		}

		if (const TerrainNodeSubtractor* updated = getTerrainNodeSubtractor(network, nodeId))
		{
			for (const Key key : updated->touchedChunkKeys)
				dirtyChunkKeys.insert(key);
		}
	}

	invalidateTerrainChunkKeys(dirtyChunkKeys);
}

void WorldRenderer::invalidateTerrainForEdge(int edgeId)
{
	if (edgeId < 0) return;

	std::unordered_set<Key> dirtyChunkKeys;
	if (const auto it = m_edgeSubtractorCache.find(edgeId); it != m_edgeSubtractorCache.end())
	{
		for (const Key key : it->second.touchedChunkKeys)
			dirtyChunkKeys.insert(key);
		m_edgeSubtractorCache.erase(it);
	}

	invalidateTerrainChunkKeys(dirtyChunkKeys);
}

void WorldRenderer::invalidateTerrainForNode(int nodeId)
{
	if (nodeId < 0) return;

	std::unordered_set<Key> dirtyChunkKeys;
	if (const auto it = m_nodeSubtractorCache.find(nodeId); it != m_nodeSubtractorCache.end())
	{
		for (const Key key : it->second.touchedChunkKeys)
			dirtyChunkKeys.insert(key);
		m_nodeSubtractorCache.erase(it);
	}

	invalidateTerrainChunkKeys(dirtyChunkKeys);
}

void WorldRenderer::invalidateAllTerrain()
{
	++m_terrainEpoch;
	m_detailedTreeChunks.clear();
	m_meshCache.clear();
	m_chunkSubtractorCache.clear();
	m_edgeSubtractorCache.clear();
	m_nodeSubtractorCache.clear();
	m_pendingTerrainRebuildKeys.clear();
	m_chunkSubtractorPrimed = false;
}

namespace
{
	/// @brief 非 OBJ 建物（Box 描画）の高さスケール。buildingHeight() はメートル基準。
	constexpr float kLegacyBoxHeightScale = 1.0f;
	/// @brief モデル TOML に scale が無い場合の既定値
	constexpr float kDefaultModelScale = 1.0f;

	String buildingAssetSubDir(BuildingType type)
	{
		return isResidentialBuildingType(type) ? U"residential" : U"commercial";
	}

	float parseModelScale(const TOMLReader& toml)
	{
		const double s = toml[U"scale"].getOr<double>(
			toml[U"render_scale"].getOr<double>(kDefaultModelScale));
		return static_cast<float>(Max(0.001, s));
	}

	/// @brief 建物 Box メッシュの頂点を中心 (cx, cz) まわりに角度 angle で Y 軸回転する
	/// @details rebuildBuildingMeshes と drawBuildingSilhouette で同じ変換を適用するための共通処理
	void rotateBoxVerticesY(MeshData& box, float cx, float cz, float angle)
	{
		if (angle == 0.0f) return;
		const float cosA = Math::Cos(angle);
		const float sinA = Math::Sin(angle);
		for (auto& v : box.vertices)
		{
			const float dx = v.pos.x - cx;
			const float dz = v.pos.z - cz;
			v.pos.x = cx + dx * cosA - dz * sinA;
			v.pos.z = cz + dx * sinA + dz * cosA;
			const float nx = v.normal.x;
			const float nz = v.normal.z;
			v.normal.x = nx * cosA - nz * sinA;
			v.normal.z = nx * sinA + nz * cosA;
		}
	}

	void appendMeshData(MeshData& dst, const MeshData& src)
	{
		const uint32 offset = static_cast<uint32>(dst.vertices.size());
		dst.vertices.append(src.vertices);
		for (const auto& tri : src.indices)
		{
			dst.indices << TriangleIndex32{ tri.i0 + offset, tri.i1 + offset, tri.i2 + offset };
		}
	}

	uint32 cellVisualHash(Point chunkCoord, int col, int row, uint32 salt)
	{
		uint32 value = static_cast<uint32>(chunkCoord.x * 73856093)
			^ static_cast<uint32>(chunkCoord.y * 19349663)
			^ static_cast<uint32>(col * 83492791)
			^ static_cast<uint32>(row * 2654435761u)
			^ salt;
		value ^= value >> 16;
		value *= 0x7FEB352Du;
		value ^= value >> 15;
		value *= 0x846CA68Bu;
		value ^= value >> 16;
		return value;
	}

	void appendRotatedBox(MeshData& dst, float cx, float cy, float cz, float sx, float sy, float sz, float angle)
	{
		MeshData box = MeshData::Box(Float3{ cx, cy, cz }, Float3{ sx, sy, sz });
		rotateBoxVerticesY(box, cx, cz, angle);
		appendMeshData(dst, box);
	}

	int landPatchMaterialKey(LandPatchType type, [[maybe_unused]] uint64 seed)
	{
		switch (type)
		{
		case LandPatchType::ParcelAsphalt: return 119;
		case LandPatchType::ParcelGravel:  return 120;
		case LandPatchType::GardenSoil:    return 122;
		case LandPatchType::Beach:        return 111;
		case LandPatchType::PaddyField:   return 131;
		case LandPatchType::FarmField:    return 130;
		case LandPatchType::Seawall:      return 117;
		default:                    return 100;
		}
	}

	struct LandRoadMask
	{
		RectF bounds;
		Array<Vec2> footprint;
		Polygon shape;
	};
	template<class HeightSource>
	void appendParcelLandscape(HashTable<int, MeshData>& groups, const HeightSource& world,
		const Chunk& chunk, const LandPatch& patch, const Array<LandRoadMask>& roadMasks,bool detailedTrees);
	template<class HeightSource>
	void appendLandPatchSurface(MeshData& dst, const HeightSource& world, const LandPatch& patch,
		const Array<LandRoadMask>& roadMasks)
	{
		if (patch.polygon.size() < 3)
		{
			return;
		}
		const RectF bounds = boundsOfPolygon(patch.polygon);
		Array<const LandRoadMask*> nearbyRoads;
		for (const auto& mask : roadMasks)
		{
			if (rectIntersects(bounds, mask.bounds))
			{
				nearbyRoads << &mask;
			}
		}
		const int materialKey = landPatchMaterialKey(patch.type, patch.materialVariant);
		// Align cultivation rows with the longest plot edge, not with world axes.
		Vec2 rowDirection{1,0};double longest=0;
		for (size_t i=0;i<patch.polygon.size();++i)
		{
			const Vec2 edge=patch.polygon[(i+1)%patch.polygon.size()]-patch.polygon[i];
			if (edge.lengthSq()>longest) { longest=edge.lengthSq();rowDirection=edge.normalized(); }
		}
		const Vec2 rowNormal{-rowDirection.y,rowDirection.x};
		Array<Vec2> outline = patch.polygon;
		UrbanParcel::normalize(outline);
		const Polygon shape{ outline };
		const int minCol = static_cast<int>(Floor(bounds.x / kTerrainCellSize));
		const int maxCol = static_cast<int>(Floor((bounds.x + bounds.w) / kTerrainCellSize));
		const int minRow = static_cast<int>(Floor(bounds.y / kTerrainCellSize));
		const int maxRow = static_cast<int>(Floor((bounds.y + bounds.h) / kTerrainCellSize));
		// Triangulate the parcel first so concave parcel outlines remain valid when clipped.
		for (const auto& triangle : shape.indices())
		{
			const auto& vertices = shape.vertices();
			Array<TerrainClipVertex> source;
			for (const auto index : { triangle.i0, triangle.i1, triangle.i2 })
			{
				const auto& vertex = vertices[index];
				source << TerrainClipVertex{ Vec3{ vertex.x, 0.0, vertex.y }, Float2{ 0, 0 } };
			}
			for (int row = minRow; row <= maxRow; ++row)
			{
				for (int col = minCol; col <= maxCol; ++col)
				{
					const Vec2 a{ col * kTerrainCellSize, row * kTerrainCellSize };
					const Vec2 b = a + Vec2{ kTerrainCellSize, 0 };
					const Vec2 c = a + Vec2{ 0, kTerrainCellSize };
					const Vec2 d = a + Vec2{ kTerrainCellSize, kTerrainCellSize };
					for (const Array<Vec2>& clip : { Array<Vec2>{ a, b, c }, Array<Vec2>{ b, d, c } })
					{
						auto piece = source;
						for (size_t side = 0; side < clip.size() && piece.size() >= 3; ++side)
						{
							piece = clipPolygonByHalfPlaneXZ(piece, clip[side], clip[(side + 1) % clip.size()], true);
						}
						for (auto& vertex : piece)
						{
							vertex.pos.y = world.sampleHeight(static_cast<float>(vertex.pos.x), static_cast<float>(vertex.pos.z)) + patch.elevationOffset;
							vertex.tex = terrainUvAt(vertex.pos, materialKey);
							if (materialKey==130 || materialKey==131)
							{
								const Vec2 local=Vec2{vertex.pos.x,vertex.pos.z}-patch.polygon.front();
								vertex.tex=Float2{static_cast<float>(local.dot(rowNormal)),static_cast<float>(local.dot(rowDirection))};
							}
						}
						Array<Array<TerrainClipVertex>> pieces{ piece };
						const RectF cellBounds{ a.x, a.y, kTerrainCellSize, kTerrainCellSize };
						for (const auto* road : nearbyRoads)
						{
							if (!rectIntersects(cellBounds, road->bounds))
							{
								continue;
							}
							Array<Array<TerrainClipVertex>> remainder;
							for (const auto& candidate : pieces)
							{
								for (auto& fragment : subtractConvexPolygonXZ(candidate, road->footprint))
								{
									remainder << std::move(fragment);
								}
							}
							pieces = std::move(remainder);
						}
						for (const auto& fragment : pieces)
						{
							appendPolygonAsTriangles(fragment, dst.vertices, dst.indices);
						}
					}
				}
			}
		}
	}

	template<class HeightSource>
	void appendLandPatchMesh(HashTable<int, MeshData>& groups, const HeightSource& world, const Chunk& chunk, const LandPatch& patch,
		const Array<LandRoadMask>& roadMasks,bool detailedTrees=true)
	{
		const bool drawSurface = (patch.type == LandPatchType::FarmField
			|| patch.type == LandPatchType::PaddyField
			|| patch.type == LandPatchType::Seawall || patch.type == LandPatchType::ParcelAsphalt
			|| patch.type == LandPatchType::ParcelGravel || patch.type == LandPatchType::GardenSoil);
		if (drawSurface)
		{
			MeshData& surface = groups[landPatchMaterialKey(patch.type, patch.materialVariant)];
			appendLandPatchSurface(surface, world, patch, roadMasks);
		}
		const RectF bounds = boundsOfPolygon(patch.polygon);
		const float cx = static_cast<float>(bounds.x + bounds.w * 0.5);
		const float cz = static_cast<float>(bounds.y + bounds.h * 0.5);
		const float baseY = static_cast<float>(world.sampleHeight(cx, cz)) + patch.elevationOffset;

		if (patch.sourceParcelKey >= 0 && patch.polygon.size() >= 3)
		{
			appendParcelLandscape(groups, world, chunk, patch, roadMasks,detailedTrees);
		}

		if (patch.type == LandPatchType::FarmField || patch.type == LandPatchType::PaddyField)
		{
			// Earth bunds follow the field perimeter; sample at 8 m to follow terrain.
			for (size_t side = 0; side < patch.polygon.size(); ++side)
			{
				const Vec2 a = patch.polygon[side], b = patch.polygon[(side+1)%patch.polygon.size()];
				const double length = a.distanceFrom(b);
				const int pieces = Max(1,static_cast<int>(Ceil(length/8)));
				for (int piece = 0; piece < pieces; ++piece)
				{
					const Vec2 point = a.lerp(b,(piece+.5)/pieces);
					const bool touchesRoad = std::any_of(roadMasks.begin(),roadMasks.end(),[&](const LandRoadMask& road)
					{ return road.bounds.stretched(5).contains(point) && Circle{point,length/pieces*.5+.4}.intersects(road.shape); });
					if (touchesRoad) { continue; }
					const Vec2 start=a.lerp(b,static_cast<double>(piece)/pieces),end=a.lerp(b,static_cast<double>(piece+1)/pieces);
					const Vec2 normal=Vec2{-(b-a).y,(b-a).x}.normalized();
					MeshData bank;
					for (const Vec2 p : {start,end}) for (const auto cross : {Vec2{-.48,.01},Vec2{-.20,.20},Vec2{.20,.20},Vec2{.48,.01}})
					{
						const Vec2 at=p+normal*cross.x;
						bank.vertices << Vertex3D{Float3{static_cast<float>(at.x),world.sampleHeight(static_cast<float>(at.x),static_cast<float>(at.y))+static_cast<float>(cross.y),static_cast<float>(at.y)},Float3{0,1,0},Float2{at*.5}};
					}
					for (uint32 face=0;face<3;++face) { bank.indices << TriangleIndex32{face,face+1,face+4} << TriangleIndex32{face+1,face+5,face+4}; }
					bank.computeNormals();appendMeshData(groups[127],bank);
				}
			}
		}
		if (patch.type == LandPatchType::Seawall)
		{
			appendRotatedBox(groups[117], cx, baseY + 0.70f, cz,
				static_cast<float>(Max(2.4, bounds.w * 1.02)), 1.40f,
				static_cast<float>(Max(1.8, bounds.h * 0.52)), 0.0f);
		}
	}

	void appendGableRoof(MeshData& dst, float cx, float baseY, float cz,
	                     float sx, float depth, float roofH, float angle, bool ridgeAlongX)
	{
		const float hx = sx * 0.5f;
		const float hz = depth * 0.5f;
		const float ridgeX = ridgeAlongX ? hx : 0.0f;
		const float ridgeZ = ridgeAlongX ? 0.0f : hz;
		Array<Float3> local;
		if (ridgeAlongX)
		{
			local = {
				Float3{ -hx, baseY, -hz }, Float3{ hx, baseY, -hz }, Float3{ hx, baseY, hz }, Float3{ -hx, baseY, hz },
				Float3{ -ridgeX, baseY + roofH, 0.0f }, Float3{ ridgeX, baseY + roofH, 0.0f }
			};
		}
		else
		{
			local = {
				Float3{ -hx, baseY, -hz }, Float3{ hx, baseY, -hz }, Float3{ hx, baseY, hz }, Float3{ -hx, baseY, hz },
				Float3{ 0.0f, baseY + roofH, -ridgeZ }, Float3{ 0.0f, baseY + roofH, ridgeZ }
			};
		}

		MeshData md;
		const float cosA = Math::Cos(angle);
		const float sinA = Math::Sin(angle);
		for (const Float3& p : local)
		{
			const float x = ridgeAlongX ? p.x : p.x;
			const float z = p.z;
			Vertex3D v;
			v.pos = Float3{ cx + x * cosA - z * sinA, p.y, cz + x * sinA + z * cosA };
			v.normal = Float3{ 0.0f, 1.0f, 0.0f };
			v.tex = Float2{ 0.0f, 0.0f };
			md.vertices << v;
		}
		md.indices << TriangleIndex32{ 0, 1, 4 };
		md.indices << TriangleIndex32{ 1, 5, 4 };
		md.indices << TriangleIndex32{ 1, 2, 5 };
		md.indices << TriangleIndex32{ 2, 3, 5 };
		md.indices << TriangleIndex32{ 3, 4, 5 };
		md.indices << TriangleIndex32{ 3, 0, 4 };
		appendMeshData(dst, md);
	}

	float targetBuildingModelFootprint(BuildingType type)
	{
		return buildingFootprintXZ(type);
	}

	float targetBuildingModelHeight(BuildingType type)
	{
		// 2階建て住宅の階高と屋根を保つ。シミュレーション上の収容人数は変更しない。
		constexpr float kDetachedModelHeightLimit = 8.5f;
		if (type == BuildingType::Detached)
		{
			return kDetachedModelHeightLimit;
		}
		if (type == BuildingType::PublicFacility)
		{
			constexpr float kCivicModelHeightLimit = 15.0f;
			return kCivicModelHeightLimit;
		}
		if (type == BuildingType::Parking)
		{
			constexpr float kParkingModelHeightLimit = 4.5f;
			return kParkingModelHeightLimit;
		}
		if (type == BuildingType::Office) { return 64.0f; }
		if (type == BuildingType::Shop) { return 36.0f; }
		return Max(1.0f, buildingHeight(type));
	}

	float normalizedObjScale(BuildingType type, const Box& localBounds, float assetScale)
	{
		float scale = assetScale;
		const float localFootprint = static_cast<float>(Max(localBounds.size.x, localBounds.size.z));
		if (localFootprint > 0.001f)
		{
			const float currentFootprint = localFootprint * scale;
			const float targetFootprint = targetBuildingModelFootprint(type);
			if (currentFootprint > targetFootprint || type == BuildingType::Office || type == BuildingType::Shop
				|| type == BuildingType::MidApartment || type == BuildingType::HighApartment)
			{
				scale *= targetFootprint / currentFootprint;
			}
		}

		const float localHeight = static_cast<float>(localBounds.size.y);
		if (localHeight > 0.001f)
		{
			const float currentHeight = localHeight * scale;
			const float targetHeight = targetBuildingModelHeight(type);
			if (currentHeight > targetHeight)
			{
				scale *= targetHeight / currentHeight;
			}
		}
		return scale;
	}
	float boxBuildingFootprintScale(BuildingType type, int gx, int gz)
	{
		const uint32 hash = static_cast<uint32>(gx) * 73856093u ^ static_cast<uint32>(gz) * 19349663u;
		const float variation = 0.86f + static_cast<float>(hash % 29u) * (0.24f / 28.0f);
		switch (type)
		{
		case BuildingType::Factory:        return 1.28f * variation;
		case BuildingType::PublicFacility: return 1.16f * variation;
		case BuildingType::Parking:        return 1.05f * variation;
		case BuildingType::ParkBuilding:   return 0.92f * variation;
		default:                           return variation;
		}
	}

	ColorF detailColorForKey(int key)
	{
		key%=1000;
		switch (key)
		{
		case 100: return ColorF{ 0.44, 0.44, 0.40 };
		case 101: return ColorF{ 0.38, 0.46, 0.29 };
		case 102: return ColorF{ 0.62, 0.60, 0.54 };
		case 103: return ColorF{ 0.42, 0.39, 0.34 };
		case 104: return ColorF{ 0.56, 0.63, 0.42 };
		case 105: return ColorF{ 0.32, 0.24, 0.18 };
		case 125: return ColorF{ 0.33, 0.44, 0.23 }.removeSRGBCurve();
		case 126: return ColorF{ 0.43, 0.54, 0.29 }.removeSRGBCurve();
		case 127: return ColorF{ .24,.20,.105 };
		case 130: case 131: return ColorF{1};
		case 128: return ColorF{.33,.44,.23}.removeSRGBCurve();
		case 129: return ColorF{.43,.54,.29}.removeSRGBCurve();
		case 124:
		case 106: return ColorF{ 0.18, 0.23, 0.11 };
		case 107: return ColorF{ 0.72, 0.74, 0.73 };
		case 108: return ColorF{ 0.35, 0.39, 0.43 };
		case 109: return ColorF{ 0.23, 0.24, 0.23 };
		case 110: return ColorF{ 0.31, 0.39, 0.31 };
		case 111: return ColorF{ 0.76, 0.70, 0.52 };
		case 112: return ColorF{ 0.68, 0.20, 0.16 };
		case 113: return ColorF{ 0.47, 0.44, 0.28 };
		case 114: return ColorF{ 0.66, 0.58, 0.36 };
		case 115: return ColorF{ 0.22, 0.21, 0.19 };
		case 116: return ColorF{ 0.16, 0.23, 0.25 };
		case 117: return ColorF{ 0.54, 0.54, 0.50 };
		case 118: return ColorF{ 0.32, 0.30, 0.23 };
		case 119: return ColorF{ 0.38, 0.42, 0.44 };
		case 120: return ColorF{ 0.25, 0.24, 0.20 };
		case 121: return ColorF{ 0.34, 0.31, 0.22 };
		case 122: return ColorF{ 0.26, 0.30, 0.18 };
		case 123: return ColorF{ 0.28, 0.29, 0.26 };
		default: return ColorF{ 0.60, 0.60, 0.60 };
		}
	}

	void appendLandscapeTree(HashTable<int,MeshData>& groups,Vec2 position,float ground,uint32 variation,double width,double height,bool cedar,bool detailedTrees)
	{
		static const std::array<TreeGeometry::Geometry,8> trees=[]
		{
			std::array<TreeGeometry::Geometry,8> result;
			for (uint32 i=0;i<result.size();++i) { result[i]=TreeGeometry::build(i*31,i>=4); }
			return result;
		}();
		const auto& tree=trees[(variation%4)+(cedar ? 4 : 0)];
		const int leafKey=(variation&1u) ? 126 : 125;
		for (const auto& entry : {std::pair<int,const MeshData*>{105,&tree.wood},{leafKey,&tree.leaves},{leafKey+3,&tree.distant}})
		{
			if (!detailedTrees && entry.first!=128 && entry.first!=129) { continue; }
			MeshData mesh=*entry.second;
			mesh.scale(width,height,width).rotate(Quaternion::RotateY((variation%97)*.065));
			mesh.translate(Float3{static_cast<float>(position.x),ground,static_cast<float>(position.y)});
			if (detailedTrees) { appendMeshData(groups[TreeGeometry::materialKey(entry.first,position)],mesh); }
			// Fully distant chunks submit one merged crown batch per palette, not one per tile.
			if (entry.first==128 || entry.first==129) { appendMeshData(groups[entry.first],mesh); }
		}
	}
	void appendParkTree(HashTable<int, MeshData>& groups, Vec2 position, float groundY, uint32 variation, double heightScale = 1.0,bool detailedTrees=true)
	{
		const double scale=.78+(variation%11)*.028;
		appendLandscapeTree(groups,position,groundY,variation,4.8*scale,4.8*heightScale*scale,false,detailedTrees);
	}
	/// @brief Deterministic woodland batches follow undeveloped slopes, excluding roads and plots.
	void appendWoodland(HashTable<int, MeshData>& groups, const Chunk& chunk,
		const Array<LandRoadMask>& roadMasks,bool detailedTrees=true)
	{
		if (chunk.zoneMap.isEmpty() || chunk.heightMap.isEmpty()) { return; }
		Grid<bool> blocked(ZONE_CELLS,ZONE_CELLS,false);
		for (int row = 0; row < ZONE_CELLS; ++row) for (int col = 0; col < ZONE_CELLS; ++col)
		{
			if (chunk.zoneMap[{col,row}] == ZoneType::Unzoned && chunk.buildingGrid[{col,row}].type == BuildingType::None) { continue; }
			for (int dz = -1; dz <= 1; ++dz) for (int dx = -1; dx <= 1; ++dx)
			{
				if (InRange(col+dx,0,ZONE_CELLS-1) && InRange(row+dz,0,ZONE_CELLS-1)) { blocked[{col+dx,row+dz}] = true; }
			}
		}
		// Index clearance by 16 m cell once; the forest loop never scans all road polygons.
		for (const auto& road : roadMasks)
		{
			const RectF bounds = road.bounds.stretched(9);
			const int left = Clamp(static_cast<int>(Floor((bounds.x-chunk.coord.x*CHUNK_SIZE)/16)),0,ZONE_CELLS-1);
			const int right = Clamp(static_cast<int>(Floor((bounds.x+bounds.w-chunk.coord.x*CHUNK_SIZE)/16)),0,ZONE_CELLS-1);
			const int top = Clamp(static_cast<int>(Floor((bounds.y-chunk.coord.y*CHUNK_SIZE)/16)),0,ZONE_CELLS-1);
			const int bottom = Clamp(static_cast<int>(Floor((bounds.y+bounds.h-chunk.coord.y*CHUNK_SIZE)/16)),0,ZONE_CELLS-1);
			for (int row = top; row <= bottom; ++row) for (int col = left; col <= right; ++col)
			{
				const Vec2 center{chunk.coord.x*CHUNK_SIZE+(col+.5)*16,chunk.coord.y*CHUNK_SIZE+(row+.5)*16};
				if (Circle{center,18}.intersects(road.shape)) { blocked[{col,row}] = true; }
			}
		}
		for (int row = 0; row < ZONE_CELLS; ++row) for (int col = 0; col < ZONE_CELLS; ++col)
		{
			if (blocked[{col,row}]) { continue; }
			const uint32 hash = cellVisualHash(chunk.coord,col,row,179u);
			const float x = chunk.coord.x*CHUNK_SIZE+(col+.5f)*16+static_cast<float>((hash>>8)%141u)*.1f-7.0f;
			const float z = chunk.coord.y*CHUNK_SIZE+(row+.5f)*16+static_cast<float>((hash>>16)%141u)*.1f-7.0f;
			const float ground = chunk.getHeight(x,z);
			const float slope = Max(Abs(chunk.getHeight(x+8,z)-chunk.getHeight(x-8,z)),Abs(chunk.getHeight(x,z+8)-chunk.getHeight(x,z-8)))/16;
			if (ground < 12 || ground > 850 || slope > 1.4f || (ground < 28 && slope < .07f) || hash%100u > 95u) { continue; }
			const float scale = .8f+static_cast<float>((hash>>3)%43u)*.01f;
			const bool cedar = Sin(x*.0043)+Cos(z*.0051)+Sin((x-z)*.0027) > .65;
			appendLandscapeTree(groups,Vec2{x,z},ground,hash,22.0*scale,20.0*scale,cedar,detailedTrees);
		}
	}
	template<class HeightSource>
	void appendParcelLandscape(HashTable<int, MeshData>& groups, const HeightSource& world,
		const Chunk& chunk, const LandPatch& patch, const Array<LandRoadMask>& roadMasks,bool detailedTrees)
	{
		const int col = static_cast<int>(patch.sourceParcelKey & 255);
		const int row = static_cast<int>((patch.sourceParcelKey >> 8) & 255);
		if (col >= ZONE_CELLS || row >= ZONE_CELLS) { return; }
		const Building& building = chunk.buildingGrid[{ col, row }];
		const Vec2 center{ chunk.coord.x * CHUNK_SIZE + (col + 0.5) * 16 + building.offsetX,
			chunk.coord.y * CHUNK_SIZE + (row + 0.5) * 16 + building.offsetZ };
		const Vec2 inward{ -Sin(building.angle), Cos(building.angle) };
		const Vec2 along{ inward.y, -inward.x };
		const double half = buildingFootprintXZ(building.type) * 0.5;
		Array<Vec2> outline = patch.polygon;
		UrbanParcel::normalize(outline);
		const Polygon shape{ outline };
		Array<const LandRoadMask*> adjacentRoads;
		const RectF parcelBounds = boundsOfPolygon(outline);
		for (const auto& road : roadMasks)
		{
			if (rectIntersects(parcelBounds.stretched(3), road.bounds)) { adjacentRoads << &road; }
		}
		auto clearOfRoad = [&](Vec2 position, double radius)
		{
			for (const auto* road : adjacentRoads)
			{
				if (Circle{ position, radius }.intersects(road->shape)) { return false; }
			}
			return true;
		};
		auto fits = [&](Vec2 position, double radius)
		{
			if (!shape.contains(position) || !clearOfRoad(position, radius)) { return false; }
			for (size_t side = 0; side < patch.polygon.size(); ++side)
			{
				const Vec2 a = patch.polygon[side], b = patch.polygon[(side + 1) % patch.polygon.size()];
				const Vec2 delta = b - a;
				const double fraction = Clamp((position - a).dot(delta) / Max(0.001, delta.lengthSq()), 0.0, 1.0);
				if (position.distanceFrom(a + delta * fraction) < radius) { return false; }
			}
			return true;
		};
		if (patch.type == LandPatchType::GardenSoil)
		{
			// Side and rear boundaries leave the original model's street entrance unobstructed.
			for (size_t side = 0; side < patch.polygon.size(); ++side)
			{
				const Vec2 a = patch.polygon[side], b = patch.polygon[(side + 1) % patch.polygon.size()];
				const Vec2 midpoint = (a + b) * 0.5;
				if ((midpoint - center).dot(inward) < -half + 0.4) { continue; }
				const double length = a.distanceFrom(b);
				const int pieces = Max(1, static_cast<int>(Ceil(length / 3.0)));
				for (int segment = 0; segment < pieces; ++segment)
				{
					const Vec2 position = a.lerp(b, (segment + 0.5) / pieces);
					if (!clearOfRoad(position, length / pieces * 0.5 + 0.18)) { continue; }
					appendRotatedBox(groups[123], static_cast<float>(position.x),
						world.sampleHeight(static_cast<float>(position.x), static_cast<float>(position.y)) + 0.30f,
						static_cast<float>(position.y), static_cast<float>(length / pieces), 0.60f, 0.12f,
						static_cast<float>(Atan2(b.y - a.y, b.x - a.x)));
				}
			}
			for (int index = 0; index < 4; ++index)
			{
				const Vec2 position = center + inward * (half + 4.0 + (index / 2) * 5.0)
					+ along * ((index % 2 == 0 ? -1 : 1) * (2.8 + (patch.materialVariant % 7) * 0.12));
				if (fits(position, 2.1) && ((patch.materialVariant >> index) & 3u) != 0)
				{
					appendParkTree(groups, position, world.sampleHeight(static_cast<float>(position.x), static_cast<float>(position.y)), patch.materialVariant + index, chunk.isUrbanizationArea ? 1.0 : 2.0,detailedTrees);
				}
			}
		}
		else if (building.type == BuildingType::Parking || building.type == BuildingType::Factory
			|| (building.type == BuildingType::Shop && (patch.materialVariant % 5) == 0))
		{
			// Real-size rear parking stalls occupy the commercial service yard.
			for (int index = -2; index <= 2; ++index)
			{
				const Vec2 position = center + inward * (half + 4.2) + along * (index * 2.5);
				if (!fits(position, 2.7)) { continue; }
				appendRotatedBox(groups[102], static_cast<float>(position.x),
					world.sampleHeight(static_cast<float>(position.x), static_cast<float>(position.y)) + 0.035f,
					static_cast<float>(position.y), 4.8f, 0.012f, 0.08f,
					static_cast<float>(Atan2(inward.y, inward.x)));
			}
		}
	}
	void appendUrbanLotDetails(HashTable<int, MeshData>& groups, const Chunk& chunk, const World& world,
	                           int col, int row, const Building& building, float cx, float cz,
	                           float cellSize)
	{
		// 駐車枠・精算機・駐車車両は専用OBJに含まれる。
		if (building.type == BuildingType::Parking)
		{
			return;
		}
		const uint32 hash = cellVisualHash(chunk.coord, col, row, static_cast<uint32>(building.type));
		const float angle = building.angle;
		const float baseY = world.sampleHeight(cx, cz);
		const float lotSize = cellSize * (0.74f + static_cast<float>((hash >> 3) % 3u) * 0.010f);
		const bool commercialLike = (building.type == BuildingType::Shop || building.type == BuildingType::Office
			|| building.type == BuildingType::Factory || building.type == BuildingType::PublicFacility);
		const int surfaceKey = (building.type == BuildingType::Parking) ? 119 : (commercialLike ? 120 : (((hash >> 9) % 100u < 8u) ? 121 : 120));
		const float surfaceSize = (building.type == BuildingType::Parking) ? lotSize : (commercialLike ? lotSize * 0.36f : lotSize * 0.22f);
		if (building.type == BuildingType::Parking || commercialLike)
		{
			appendRotatedBox(groups[surfaceKey], cx, baseY + 0.020f, cz, surfaceSize, 0.035f, surfaceSize * (commercialLike ? 0.42f : 0.30f), angle);
		}

		const float cosA = Math::Cos(angle);
		const float sinA = Math::Sin(angle);
		auto worldOffset = [&](float lx, float lz)
		{
			return Vec2{ cx + lx * cosA - lz * sinA, cz + lx * sinA + lz * cosA };
		};

		if (building.type == BuildingType::Parking)
		{
			appendRotatedBox(groups[109], cx, baseY + 0.09f, cz, lotSize * 0.92f, 0.08f, lotSize * 0.74f, angle);
			for (int i = -1; i <= 1; ++i)
			{
				const Vec2 p = worldOffset(static_cast<float>(i) * lotSize * 0.22f,
					((hash >> (10 + i + 1)) & 1u) ? lotSize * 0.08f : -lotSize * 0.13f);
				appendRotatedBox(groups[(i == 0) ? 108 : 107], static_cast<float>(p.x), baseY + 0.30f,
					static_cast<float>(p.y), 1.65f, 0.42f, 3.35f, angle);
			}
			return;
		}

		if (building.type == BuildingType::ParkBuilding)
		{
			appendRotatedBox(groups[101], cx, baseY + 0.08f, cz, lotSize * 0.78f, 0.10f, lotSize * 0.78f, angle);
			for (int i = 0; i < 3; ++i)
			{
				const Vec2 p = worldOffset((static_cast<float>((hash >> (i * 4)) % 9u) - 4.0f) * 0.9f,
					(static_cast<float>((hash >> (i * 5 + 7)) % 9u) - 4.0f) * 0.9f);
				appendParkTree(groups, p, world.sampleHeight(static_cast<float>(p.x), static_cast<float>(p.y)), hash + i);
			}
			return;
		}

		const float frontageZ = -lotSize * 0.43f;
		if (commercialLike)
		{
			appendRotatedBox(groups[119], cx, baseY + 0.040f, cz, lotSize * 0.52f, 0.025f, lotSize * 0.28f, angle);
			const Vec2 sign = worldOffset(lotSize * 0.34f, frontageZ);
			appendRotatedBox(groups[115], static_cast<float>(sign.x), baseY + 1.15f, static_cast<float>(sign.y), 0.16f, 2.30f, 0.16f, angle);
			appendRotatedBox(groups[112], static_cast<float>(sign.x), baseY + 2.45f, static_cast<float>(sign.y), 1.20f, 0.60f, 0.12f, angle);
		}
		else
		{
			if (((hash >> 9) % 100u) < 52u) appendRotatedBox(groups[101], cx, baseY + 0.030f, cz, lotSize * 0.34f, 0.018f, lotSize * 0.22f, angle);
			const Vec2 car = worldOffset(((hash >> 11) & 1u) ? lotSize * 0.25f : -lotSize * 0.25f, lotSize * 0.30f);
			if (((hash >> 6) % 100u) < 52u)
			{
				appendRotatedBox(groups[100], static_cast<float>(car.x), baseY + 0.08f, static_cast<float>(car.y), 2.15f, 0.05f, 4.05f, angle);
				appendRotatedBox(groups[((hash >> 14) & 1u) ? 107 : 108], static_cast<float>(car.x), baseY + 0.32f,
					static_cast<float>(car.y), 1.55f, 0.44f, 3.10f, angle);
			}
			const Vec2 approach = worldOffset(0.0f, frontageZ * 0.54f);
			appendRotatedBox(groups[120], static_cast<float>(approach.x), baseY + 0.036f, static_cast<float>(approach.y),
				lotSize * 0.16f, 0.020f, lotSize * 0.70f, angle);
			if (((hash >> 27) % 100u) < 34u)
			{
				const Vec2 bin = worldOffset(-lotSize * 0.35f, frontageZ + 0.12f);
				appendRotatedBox(groups[117], static_cast<float>(bin.x), baseY + 0.28f, static_cast<float>(bin.y), 0.70f, 0.56f, 0.42f, angle);
			}
		}

		if ((hash % 100u) < 92u)
		{
			const float side = ((hash >> 8) & 1u) ? 1.0f : -1.0f;
			const Vec2 fence = worldOffset(side * lotSize * 0.38f,
				(static_cast<float>((hash >> 12) % 7u) - 3.0f) * 0.65f);
			appendRotatedBox(groups[102], static_cast<float>(fence.x), baseY + 0.34f, static_cast<float>(fence.y),
				0.18f, 0.68f, lotSize * 0.46f, angle);
		}

		if (((hash >> 5) % 100u) < 58u)
		{
			const float side = ((hash >> 17) & 1u) ? 1.0f : -1.0f;
			const Vec2 tree = worldOffset(side * lotSize * 0.36f,
				lotSize * (0.20f + static_cast<float>((hash >> 21) % 18u) * 0.012f));
			appendRotatedBox(groups[105], static_cast<float>(tree.x), baseY + 0.80f, static_cast<float>(tree.y), 0.28f, 1.60f, 0.28f, angle);
			appendRotatedBox(groups[106], static_cast<float>(tree.x), baseY + 1.82f, static_cast<float>(tree.y), 1.65f, 1.45f, 1.65f, angle + static_cast<float>(45.0_deg));
		}


	}
	void appendFarmlandDetails(HashTable<int, MeshData>& groups, const Chunk& chunk, const World& world,
	                           int col, int row, float cx, float cz, float cellSize)
	{
		const uint32 hash = cellVisualHash(chunk.coord, col, row, 0xA6B4C893u);
		const float angle = ((hash & 1u) ? 0.0f : static_cast<float>(90.0_deg))
			+ static_cast<float>((static_cast<int>((hash >> 8) % 7u) - 3) * 0.015f);
		const float baseY = world.sampleHeight(cx, cz);
		const float fieldW = cellSize * (0.78f + static_cast<float>(hash % 18u) * 0.010f);
		const float fieldD = cellSize * (0.56f + static_cast<float>((hash >> 16) % 24u) * 0.010f);
		const int fieldKey = ((hash >> 5) % 100u < 42u) ? 113 : (((hash >> 10) & 1u) ? 104 : 114);
		appendRotatedBox(groups[fieldKey], cx, baseY + 0.025f, cz, fieldW, 0.05f, fieldD, angle);

		const float cosA = Math::Cos(angle);
		const float sinA = Math::Sin(angle);
		for (int i = -3; i <= 3; ++i)
		{
			const float lx = static_cast<float>(i) * fieldW * 0.135f;
			const float px = cx + lx * cosA;
			const float pz = cz + lx * sinA;
			appendRotatedBox(groups[(i == 0 && ((hash >> 20) & 1u)) ? 110 : 101], px, baseY + 0.065f, pz,
				0.18f, 0.045f, fieldD * 0.98f, angle);
		}
		for (int i = -1; i <= 1; ++i)
		{
			const float lz = static_cast<float>(i) * fieldD * 0.29f;
			const float px = cx - lz * sinA;
			const float pz = cz + lz * cosA;
			appendRotatedBox(groups[114], px, baseY + 0.055f, pz, fieldW * 0.94f, 0.035f, 0.16f, angle);
		}

		if (((hash >> 24) % 100u) < 34u)
		{
			const float px = cx + fieldW * 0.48f * cosA;
			const float pz = cz + fieldW * 0.48f * sinA;
			appendRotatedBox(groups[110], px, baseY + 0.05f, pz, 0.42f, 0.04f, fieldD, angle);
		}
	}
	void appendBoxBuildingDetails(HashTable<int, MeshData>& groups, const Chunk& chunk, const World& world,
	                              int col, int row, BuildingType type, float cx, float cz,
	                              float footprint, float height, float angle)
	{
		const uint32 hash = cellVisualHash(chunk.coord, col, row, static_cast<uint32>(type) ^ 0x712A4C3Du);
		const float baseY = world.sampleHeight(cx, cz);
		const float cosA = Math::Cos(angle);
		const float sinA = Math::Sin(angle);
		auto worldOffset = [&](float lx, float lz)
		{
			return Vec2{ cx + lx * cosA - lz * sinA, cz + lx * sinA + lz * cosA };
		};

		if (!isObjBuildingType(type))
		{
			appendRotatedBox(groups[103], cx, baseY + height + 0.08f, cz,
			                 footprint * 0.82f, 0.16f, footprint * 0.82f, angle);
		}

		if (type == BuildingType::Parking || type == BuildingType::ParkBuilding)
		{
			return;
		}

		const bool residential = isResidentialBuildingType(type);
		const bool shop = (type == BuildingType::Shop);
		const bool officeLike = (type == BuildingType::Office || type == BuildingType::PublicFacility);
		const int floorCount = Max(1, static_cast<int>(Floor(height / (residential ? 2.8f : 3.2f))));
		const int visibleFloors = Min(floorCount, residential ? 4 : 7);
		const float frontZ = -footprint * 0.515f;
		const float backZ = footprint * 0.515f;
		const float sideX = footprint * 0.515f;

		for (int floor = 0; floor < visibleFloors; ++floor)
		{
			const float y = baseY + 1.15f + static_cast<float>(floor) * (residential ? 2.65f : 3.0f);
			if (y > baseY + height - 0.55f) break;
			const int windowKey = ((hash >> (floor + 3)) & 1u) ? 116 : 102;
			const float rowWidth = footprint * (residential ? 0.20f : 0.16f);
			for (int w = -1; w <= 1; ++w)
			{
				const float lx = static_cast<float>(w) * footprint * 0.23f;
				const Vec2 front = worldOffset(lx, frontZ);
				appendRotatedBox(groups[windowKey], static_cast<float>(front.x), y, static_cast<float>(front.y),
					rowWidth, residential ? 0.56f : 0.76f, 0.10f, angle);
			}
			if (residential && floor > 0)
			{
				const Vec2 balcony = worldOffset(0.0f, frontZ - 0.18f);
				appendRotatedBox(groups[117], static_cast<float>(balcony.x), y - 0.20f, static_cast<float>(balcony.y),
					footprint * 0.68f, 0.12f, 0.28f, angle);
			}
			if (!residential)
			{
				const Vec2 left = worldOffset(-sideX, 0.0f);
				appendRotatedBox(groups[windowKey], static_cast<float>(left.x), y, static_cast<float>(left.y),
					0.10f, 0.66f, footprint * 0.44f, angle);
				const Vec2 right = worldOffset(sideX, 0.0f);
				appendRotatedBox(groups[windowKey], static_cast<float>(right.x), y, static_cast<float>(right.y),
					0.10f, 0.66f, footprint * 0.44f, angle);
			}
		}

		if (residential)
		{
			const bool ridgeAlongX = ((hash >> 23) & 1u) != 0u;
			const float roofBase = baseY + Max(2.7f, height - (type == BuildingType::Detached ? 1.05f : 0.55f));
			appendGableRoof(groups[103], cx, roofBase, cz, footprint * 0.96f, footprint * 0.86f,
				(type == BuildingType::Detached ? 1.15f : 0.55f), angle, ridgeAlongX);
			const Vec2 eave = worldOffset(0.0f, frontZ - 0.12f);
			appendRotatedBox(groups[103], static_cast<float>(eave.x), baseY + Min(height, 4.8f), static_cast<float>(eave.y),
				footprint * 0.84f, 0.12f, 0.42f, angle);
			const Vec2 unit = worldOffset(sideX + 0.16f, footprint * 0.22f);
			appendRotatedBox(groups[117], static_cast<float>(unit.x), baseY + 1.20f, static_cast<float>(unit.y),
				0.42f, 0.42f, 0.22f, angle);

			if (type == BuildingType::Detached)
			{
				const float wingSide = ((hash >> 4) & 1u) ? 1.0f : -1.0f;
				const Vec2 wing = worldOffset(wingSide * footprint * 0.42f, footprint * 0.10f);
				appendRotatedBox(groups[static_cast<int>(type)], static_cast<float>(wing.x), baseY + 1.15f, static_cast<float>(wing.y),
					footprint * 0.34f, 2.30f, footprint * 0.54f, angle);
				appendGableRoof(groups[103], static_cast<float>(wing.x), baseY + 2.30f, static_cast<float>(wing.y),
					footprint * 0.40f, footprint * 0.62f, 0.62f, angle, !ridgeAlongX);
				const Vec2 shed = worldOffset(-wingSide * footprint * 0.46f, footprint * 0.38f);
				appendRotatedBox(groups[117], static_cast<float>(shed.x), baseY + 0.58f, static_cast<float>(shed.y),
					1.25f, 1.16f, 1.70f, angle);
				appendRotatedBox(groups[103], static_cast<float>(shed.x), baseY + 1.22f, static_cast<float>(shed.y),
					1.45f, 0.16f, 1.95f, angle);
			}
		}

		if (shop)
		{
			const Vec2 sign = worldOffset(0.0f, frontZ - 0.10f);
			appendRotatedBox(groups[112], static_cast<float>(sign.x), baseY + Min(height * 0.74f, 3.15f), static_cast<float>(sign.y),
				footprint * 0.72f, 0.55f, 0.12f, angle);
			const Vec2 awning = worldOffset(0.0f, frontZ - 0.32f);
			appendRotatedBox(groups[118], static_cast<float>(awning.x), baseY + 2.25f, static_cast<float>(awning.y),
				footprint * 0.78f, 0.16f, 0.62f, angle);
			for (int d = -1; d <= 1; ++d)
			{
				const Vec2 door = worldOffset(static_cast<float>(d) * footprint * 0.22f, frontZ - 0.07f);
				appendRotatedBox(groups[116], static_cast<float>(door.x), baseY + 1.10f, static_cast<float>(door.y),
					footprint * 0.16f, 1.35f, 0.10f, angle);
			}
		}

		if (officeLike || type == BuildingType::Factory)
		{
			const Vec2 rear = worldOffset(0.0f, backZ + 0.10f);
			appendRotatedBox(groups[117], static_cast<float>(rear.x), baseY + Max(1.5f, height * 0.42f), static_cast<float>(rear.y),
				footprint * 0.36f, 0.38f, 0.24f, angle);
		}
	}
	/// @brief 住宅 OBJ を全 part 単色で描画する（シルエット用）
	void drawModelSilhouette(Model& model, const Mat4x4& worldMat, const ColorF& color)
	{
		const Transformer3D transform{ worldMat };
		for (const auto& obj : model.objects())
		{
			for (const auto& part : obj.parts)
				part.mesh.draw(color);
		}
	}
}

WorldRenderer::BuildingModelAsset& WorldRenderer::getBuildingModelAsset(BuildingType type, uint8 variant)
{
	// 建物種別ごとの OBJ/TOML を遅延ロードし、以後はモデルとスケールを共有キャッシュで再利用する。
	const uint32 key = (static_cast<uint32>(type) << 8) | static_cast<uint32>(variant);
	auto it = m_buildingModels.find(key);
	if (it != m_buildingModels.end())
	{
		return it->second;
	}

	String stem;
	if (isResidentialBuildingType(type))
	{
		stem = U"residential_{:03d}"_fmt(variant + 1);
	}
	else if (type == BuildingType::Shop)
	{
		stem = U"shop_{:03d}"_fmt(variant + 1);
	}
	else if (type == BuildingType::Factory)
	{
		stem = U"factory_{:03d}"_fmt(variant + 1);
	}
	else if (type == BuildingType::PublicFacility)
	{
		stem = U"public_{:03d}"_fmt(variant + 1);
	}
	else if (type == BuildingType::Parking)
	{
		stem = U"parking_{:03d}"_fmt(variant + 1);
	}
	else if (type == BuildingType::Office)
	{
		stem = U"office_{:03d}"_fmt(variant + 1);
	}
	else
	{
		// OBJ 非対応種別は空アセットを返す
		auto [inserted, _] = m_buildingModels.emplace(key, BuildingModelAsset{});
		return inserted->second;
	}
	const String subDir = buildingAssetSubDir(type);
	const String path = U"assets/buildings/{}/{}.obj"_fmt(subDir, stem);
	const String tomlPath = U"assets/buildings/{}/{}.toml"_fmt(subDir, stem);

	BuildingModelAsset asset;
	const TOMLReader toml{ tomlPath };
	if (toml)
	{
		asset.scale = parseModelScale(toml);
	}
	else
	{
		Console << U"[WorldRenderer] building TOML load failed: " << tomlPath
		        << U" (scale fallback=" << kDefaultModelScale << U")";
	}

	const String distantPath = U"assets/buildings/lod/{}.obj"_fmt(stem);
	if (FileSystem::IsFile(distantPath))
	{
		asset.distantModel = Model{ distantPath };
		Model::RegisterDiffuseTextures(asset.distantModel, TextureDesc::MippedSRGB);
	}
	asset.model = Model{ path };
	if (!asset.model.isEmpty())
	{
		Model::RegisterDiffuseTextures(asset.model, TextureDesc::MippedSRGB);
	}
	else
	{
		Console << U"[WorldRenderer] building model load failed: " << path;
	}

	auto [inserted, _] = m_buildingModels.emplace(key, std::move(asset));
	return inserted->second;
}

Optional<OrientedBox> WorldRenderer::buildingHitBox(const Chunk& chunk, const World& world,
                                                     int col, int row)
{
	if (col < 0 || col >= ZONE_CELLS || row < 0 || row >= ZONE_CELLS) return none;
	const Building& b = chunk.buildingGrid[{ col, row }];
	if (b.type == BuildingType::None || b.type == BuildingType::Farmland) return none;

	constexpr float cellSize  = static_cast<float>(CHUNK_SIZE) / ZONE_CELLS;
	const float footprint = buildingFootprintXZ();
	const Vec3 origin = chunk.worldOrigin();
	const float cx = static_cast<float>(origin.x + (col + 0.5) * cellSize) + b.offsetX;
	const float cz = static_cast<float>(origin.z + (row + 0.5) * cellSize) + b.offsetZ;

	if (isObjBuildingType(b.type))
	{
		const float gy = buildingBaseHeight(world,b,cx,cz);
		const int gx = chunk.coord.x * ZONE_CELLS + col;
		const int gz = chunk.coord.y * ZONE_CELLS + row;
		const uint8 variant = buildingModelVariant(b.type, gx, gz);
		BuildingModelAsset& asset = getBuildingModelAsset(b.type, variant);
		if (asset.model.isEmpty()) return none;
		const float yaw = -b.angle;

		const Box& lb = asset.model.boundingBox();
		const Vec3 localCenter = lb.center;
		const float modelScale = normalizedObjScale(b.type, lb, asset.scale);
		const Vec3 size = lb.size * modelScale;
		// drawCachedBuildings と同じ Mat4x4::RotateY → translate 変換を再現
		const double cosA = Math::Cos(yaw);
		const double sinA = Math::Sin(yaw);
		const Vec3 worldCenter{
			cx + localCenter.x * modelScale * cosA + localCenter.z * modelScale * sinA,
			gy + localCenter.y * modelScale,
			cz - localCenter.x * modelScale * sinA + localCenter.z * modelScale * cosA
		};
		return OrientedBox{ worldCenter, size, Quaternion::RotateY(yaw) };
	}

	if (b.type == BuildingType::Parking || b.type == BuildingType::ParkBuilding)
	{
		return none;
	}

	const float height = buildingHeight(b.type) * kLegacyBoxHeightScale;
	if (height <= 0.0f) return none;
	const int gx = chunk.coord.x * ZONE_CELLS + col;
	const int gz = chunk.coord.y * ZONE_CELLS + row;
	const float visualFootprint = footprint * boxBuildingFootprintScale(b.type, gx, gz);
	const float cy = world.sampleHeight(cx, cz) + height * 0.5f;
	return OrientedBox{ Vec3{ cx, cy, cz }, Vec3{ visualFootprint, height, visualFootprint },
	                   Quaternion::RotateY(-b.angle) };
}

void WorldRenderer::drawBuildingSilhouette(const Chunk& chunk, const World& world,
                                            int col, int row, const ColorF& color)
{
	if (col < 0 || col >= ZONE_CELLS || row < 0 || row >= ZONE_CELLS) return;
	const Building& b = chunk.buildingGrid[{ col, row }];
	if (b.type == BuildingType::None || b.type == BuildingType::Farmland) return;

	constexpr float cellSize  = static_cast<float>(CHUNK_SIZE) / ZONE_CELLS;
	const float footprint = buildingFootprintXZ();
	const Vec3 origin = chunk.worldOrigin();
	const float cx = static_cast<float>(origin.x + (col + 0.5) * cellSize) + b.offsetX;
	const float cz = static_cast<float>(origin.z + (row + 0.5) * cellSize) + b.offsetZ;

	if (isObjBuildingType(b.type))
	{
		const float gy = buildingBaseHeight(world,b,cx,cz);
		const int gx = chunk.coord.x * ZONE_CELLS + col;
		const int gz = chunk.coord.y * ZONE_CELLS + row;
		const uint8 variant = buildingModelVariant(b.type, gx, gz);
		BuildingModelAsset& asset = getBuildingModelAsset(b.type, variant);
		if (asset.model.isEmpty()) return;
		const float yaw = -b.angle;

		drawModelSilhouette(asset.model,
		                    (Mat4x4::Scale(normalizedObjScale(b.type, asset.model.boundingBox(), asset.scale))
		                   * Mat4x4::RotateY(yaw)).translated(cx, gy, cz),
		                    color);
		return;
	}

	if (b.type == BuildingType::Parking || b.type == BuildingType::ParkBuilding)
	{
		return;
	}

	const float height = buildingHeight(b.type) * kLegacyBoxHeightScale;
	if (height <= 0.0f) return;
	const int gx = chunk.coord.x * ZONE_CELLS + col;
	const int gz = chunk.coord.y * ZONE_CELLS + row;
	const float visualFootprint = footprint * boxBuildingFootprintScale(b.type, gx, gz);
	const float cy = world.sampleHeight(cx, cz) + height * 0.5f;

	// rebuildBuildingMeshes と同じパイプライン（MeshData::Box + 頂点手動回転）で描画する
	MeshData box = MeshData::Box(
		Float3{ cx, cy, cz },
		Float3{ visualFootprint, height, visualFootprint });
	rotateBoxVerticesY(box, cx, cz, b.angle);
	Mesh{ box }.draw(color);
}

Array<WorldRenderer::TerrainMeshData> WorldRenderer::buildLandscapeMeshData(
	const Chunk& chunk, const Array<TerrainSubtractionQuad>& quads, const Array<Chunk>& heightSnapshots,bool detailedTrees)
{
	struct HeightSnapshot
	{
		const Array<Chunk>& chunks;
		float sampleHeight(float x, float z) const
		{
			const Point coord{static_cast<int>(Floor(x/CHUNK_SIZE)),static_cast<int>(Floor(z/CHUNK_SIZE))};
			for (const auto& snapshot : chunks)
			{
				if (snapshot.coord == coord) { return snapshot.getHeight(x,z); }
			}
			return 0.0f;
		}
	} heights{heightSnapshots};
	Array<LandRoadMask> masks;
	for (const auto& quad : quads)
	{
		Array<Vec2> outline = quad.footprint;
		UrbanParcel::normalize(outline);
		masks << LandRoadMask{quad.bounds,quad.footprint,Polygon{outline}};
	}
	HashTable<int,MeshData> groups;
	for (const auto& patch : chunk.landPatches) { appendLandPatchMesh(groups,heights,chunk,patch,masks,detailedTrees); }
	appendWoodland(groups,chunk,masks,detailedTrees);
	Array<TerrainMeshData> result;
	for (auto& [key,mesh] : groups) { result << TerrainMeshData{key,std::move(mesh)}; }
	return result;
}

MeshData WorldRenderer::landPatchSurface(const World& world,const RoadNetwork& network,Point coord,const LandPatch& patch)
{
	Array<LandRoadMask> masks;
	for (const auto& quad : getChunkSubtractionQuads(network,coord))
	{
		Array<Vec2> outline=quad.footprint;UrbanParcel::normalize(outline);
		masks << LandRoadMask{quad.bounds,quad.footprint,Polygon{outline}};
	}
	MeshData surface;
	appendLandPatchSurface(surface,world,patch,masks);
	return surface;
}

void WorldRenderer::rebuildBuildingMeshes(Key key, const Chunk& chunk, const World& world)
{
	constexpr float cellSize  = static_cast<float>(CHUNK_SIZE) / ZONE_CELLS;
	const float footprint = buildingFootprintXZ();

	const Vec3 origin = chunk.worldOrigin();

	// 建物種別ごとに MeshData を積み上げる（Box 描画用）
	HashTable<int, MeshData> groups;
	// 住宅 OBJ インスタンス
	Array<BuildingModelInstance> modelInstances;

	Array<LandRoadMask> roadMasks;
	if (const auto masks = m_chunkSubtractorCache.find(key); masks != m_chunkSubtractorCache.end())
	{
		for (const auto& mask : masks->second)
		{
			Array<Vec2> outline = mask.footprint;
			UrbanParcel::normalize(outline);
			roadMasks << LandRoadMask{ mask.bounds, mask.footprint, Polygon{ outline } };
		}
	}
	if (!m_asyncTerrain)
	{
		for (const LandPatch& patch : chunk.landPatches) { appendLandPatchMesh(groups, world, chunk, patch, roadMasks); }
		appendWoodland(groups,chunk,roadMasks);
	}

	for (int row = 0; row < ZONE_CELLS; ++row)
	{
		for (int col = 0; col < ZONE_CELLS; ++col)
		{
			const Building& b = chunk.buildingGrid[{ col, row }];
			const float cellCenterX = static_cast<float>(origin.x + (col + 0.5) * cellSize);
			const float cellCenterZ = static_cast<float>(origin.z + (row + 0.5) * cellSize);
			if (b.type == BuildingType::None)
			{
				continue;
			}

			const float cx = cellCenterX + b.offsetX;
			const float cz = cellCenterZ + b.offsetZ;

			if (b.type == BuildingType::Farmland)
			{
				continue;
			}

			// 住宅系は OBJ で描画する（地表位置に Y 軸回転のみ適用）
			if (isObjBuildingType(b.type))
			{
				const float gy = buildingBaseHeight(world,b,cx,cz);
				const float ground=world.sampleHeight(cx,cz);
				if (gy-ground>.07f && b.type!=BuildingType::Parking)
				{
					const float width=buildingFootprintXZ(b.type)+.12f,height=(gy-ground)*2+.12f;
					appendRotatedBox(groups[126],cx,gy-height*.5f,cz,width,height,width,b.angle);
				}
				const int gx = chunk.coord.x * ZONE_CELLS + col;
				const int gz = chunk.coord.y * ZONE_CELLS + row;
				const uint8 variant = buildingModelVariant(b.type, gx, gz);
				BuildingModelAsset& asset = getBuildingModelAsset(b.type, variant);
				const float modelScale = normalizedObjScale(b.type, asset.model.boundingBox(), asset.scale);
				modelInstances.push_back({
					b.type,
					variant,
					Float3{ cx, gy, cz },
					b.angle,
					modelScale
				});
				continue;
			}

			appendUrbanLotDetails(groups, chunk, world, col, row, b, cx, cz, cellSize);
			if (b.type == BuildingType::Parking || b.type == BuildingType::ParkBuilding)
			{
				continue;
			}

			const float height = buildingHeight(b.type) * kLegacyBoxHeightScale;
			if (height <= 0.0f) continue;
			const int gx = chunk.coord.x * ZONE_CELLS + col;
			const int gz = chunk.coord.y * ZONE_CELLS + row;
			const float visualFootprint = footprint * boxBuildingFootprintScale(b.type, gx, gz);
			const float cy = world.sampleHeight(cx, cz) + height * 0.5f;

			MeshData box = MeshData::Box(
				Float3{ cx, cy, cz },
				Float3{ visualFootprint, height, visualFootprint });

			// 最近傍道路の向きに合わせてY軸回転
			rotateBoxVerticesY(box, cx, cz, b.angle);

			auto& dst = groups[static_cast<int>(b.type)];
			const uint32 offset = static_cast<uint32>(dst.vertices.size());
			dst.vertices.append(box.vertices);
			for (const auto& tri : box.indices)
			{
				dst.indices << TriangleIndex32{
					tri.i0 + offset, tri.i1 + offset, tri.i2 + offset };
			}
			appendBoxBuildingDetails(groups, chunk, world, col, row, b.type, cx, cz,
			                         visualFootprint, height, b.angle);
		}
	}

	auto& batches = m_buildingMeshCache[key];
	batches.clear();
	for (auto& [typeInt, meshData] : groups)
	{
		if (meshData.vertices.isEmpty()) continue;
		const ColorF color = (typeInt >= 100)
			? detailColorForKey(typeInt)
			: buildingColor(static_cast<BuildingType>(typeInt));
		batches.push_back({
			typeInt,
			color,
			Mesh{ meshData }
		});
	}

	m_buildingModelCache[key] = std::move(modelInstances);
}

bool WorldRenderer::drawLandscapeBatch(int materialKey,Key key) const
{
	const Point coord{static_cast<int>(key>>32),static_cast<int>(static_cast<uint32>(key))};
	const bool closeChunk=TreeGeometry::nearChunk(coord,m_buildingEye) && (!m_asyncTerrain || m_detailedTreeChunks.contains(key));
	if (materialKey==128 || materialKey==129) { return !closeChunk; }
	if (materialKey<1000) { return materialKey!=124; }
	if (!closeChunk) { return false; }
	const bool near=TreeGeometry::nearTile(materialKey,coord,m_buildingEye);
	const int material=materialKey%1000;
	return (material==128 || material==129) ? !near : near;
}

void WorldRenderer::drawCachedBuildings(Key key) const
{
	if (const auto it = m_buildingModelCache.find(key); it != m_buildingModelCache.end())
	{
		auto* self = const_cast<WorldRenderer*>(this);
		for (const auto& inst : it->second)
		{
			++m_buildingsConsidered;
			BuildingModelAsset& asset = self->getBuildingModelAsset(inst.type, inst.modelVariant);
			const double modelHeight = asset.model.boundingBox().size.y * inst.scale;
			const Vec3 center = Vec3{ inst.pos } + Vec3{ 0, modelHeight * 0.5, 0 };
			const double radius = Max(12.0, modelHeight * 0.55);
			if (m_buildingFrustum && !m_buildingFrustum->intersects(Sphere{ center, radius }))
			{
				continue;
			}
			++m_buildingsSubmitted;
			if (asset.model.isEmpty()) continue;

			const Mat4x4 worldMat = (Mat4x4::Scale(inst.scale)
			                       * Mat4x4::RotateY(-inst.angle))
				.translated(inst.pos.x, inst.pos.y, inst.pos.z);
			const Model& model = (m_buildingEye.distanceFrom(Vec3{ inst.pos }) > 600 && !asset.distantModel.isEmpty())
				? asset.distantModel : asset.model;
			const auto& materials = model.materials();
			for (const auto& obj : model.objects())
			{
				const Transformer3D transform{ worldMat };
				obj.draw(materials);
			}
		}
	}

	for (const auto* cache : { &m_buildingMeshCache, &m_landscapeMeshCache })
	{
		const auto it = cache->find(key);
		if (it == cache->end()) { continue; }
		for (const auto& batch : it->second)
		{
			if (!drawLandscapeBatch(batch.materialKey,key)) { continue; }
			const int material=batch.materialKey%1000;
			const bool distant=m_distantDetailChunks.contains(key);
			if (distant && material==123) { continue; }
			if ((material==130 && m_fieldShader) || (material==131 && m_paddyShader))
			{
				const ScopedCustomShader3D shader{material==131 ? m_paddyShader : m_fieldShader};
				batch.mesh.draw(TextureAsset(Asset::Sand),ColorF{1});
				continue;
			}
			if ((material==125 || material==126) && m_foliageShader)
			{
				const ScopedCustomShader3D shader{m_foliageShader};
				batch.mesh.draw(batch.color);
				continue;
			}
			if (material == 100)
			{
				batch.mesh.draw(TextureAsset(Asset::Concrete), batch.color);
			}
			else if (material == 119)
			{
				batch.mesh.draw(TextureAsset(Asset::Asphalt), batch.color);
			}
			else if (material == 120)
			{
				batch.mesh.draw(TextureAsset(Asset::Concrete), batch.color);
			}
			else if (material == 101 || material == 113)
			{
				batch.mesh.draw(TextureAsset(Asset::SparseGrass), batch.color);
			}
			else if (material == 127 || material == 106 || material == 110 || material == 122 || material == 124 || material == 125 || material == 126)
			{
				batch.mesh.draw(TextureAsset(Asset::Grass), batch.color);
			}
			else if (material == 111)
			{
				batch.mesh.draw(TextureAsset(Asset::CoastSand), batch.color);
			}
			else if (material == 114)
			{
				batch.mesh.draw(TextureAsset(Asset::Sand), batch.color);
			}
			else if (material == 117)
			{
				batch.mesh.draw(TextureAsset(Asset::Concrete), batch.color);
			}
			else
			{
				batch.mesh.draw(batch.color);
			}
		}
	}
}

void WorldRenderer::renderShadowCasters(Vec3 focus, double radius) const
{
	const double radiusSq = radius * radius;
	for (const auto& [key, instances] : m_buildingModelCache)
	{
		for (const auto& instance : instances)
		{
			const double dx = instance.pos.x - focus.x;
			const double dz = instance.pos.z - focus.z;
			if (dx * dx + dz * dz > radiusSq)
			{
				continue;
			}
			const uint32 assetKey = (static_cast<uint32>(instance.type) << 8) | instance.modelVariant;
			const auto asset = m_buildingModels.find(assetKey);
			if (asset == m_buildingModels.end())
			{
				continue;
			}
			const Mat4x4 transform = (Mat4x4::Scale(instance.scale) * Mat4x4::RotateY(-instance.angle))
				.translated(instance.pos.x, instance.pos.y, instance.pos.z);
			const Transformer3D worldTransform{ transform };
			asset->second.model.draw();
		}
		if (const auto landscape = m_landscapeMeshCache.find(key); landscape != m_landscapeMeshCache.end())
		{
			const Point coord{static_cast<int>(key>>32),static_cast<int>(static_cast<uint32>(key))};
			if (Vec2{(coord.x+.5)*CHUNK_SIZE,(coord.y+.5)*CHUNK_SIZE}.distanceFrom(Vec2{focus.x,focus.z}) < radius+CHUNK_SIZE)
			{
				for (const auto& batch : landscape->second) { if (drawLandscapeBatch(batch.materialKey,key)) { batch.mesh.draw(ColorF{1.0}); } }
			}
		}
		const auto batches = m_buildingMeshCache.find(key);
		if (batches == m_buildingMeshCache.end())
		{
			continue;
		}
		const Point coord{ static_cast<int>(key >> 32), static_cast<int>(static_cast<uint32>(key)) };
		const Vec2 chunkCenter{ (coord.x + 0.5) * CHUNK_SIZE, (coord.y + 0.5) * CHUNK_SIZE };
		if (chunkCenter.distanceFrom(Vec2{ focus.x, focus.z }) > radius + CHUNK_SIZE)
		{
			continue;
		}
		for (const auto& batch : batches->second)
		{
			if (drawLandscapeBatch(batch.materialKey,key)) { batch.mesh.draw(ColorF{ 1.0 }); }
		}
	}
}

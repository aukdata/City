#include "WorldRenderer.hpp"
#include <Siv3D/Profiler.hpp>
#include <Siv3D/ViewFrustum.hpp>

void WorldRenderer::render(World& world, const BasicCamera3D& camera)
{
	// 地形メッシュを動的更新するため W100 警告を抑制する
	Profiler::EnableAssetCreationWarning(false);

	// 草テクスチャを初回のみロードする
	if (m_grassTexture.isEmpty())
		m_grassTexture = Texture{ U"assets/textures/grass.png", TextureDesc::MippedSRGB };

	const Vec3 eye = camera.getEyePosition();
	const int camCx = static_cast<int>(Math::Floor(eye.x / CHUNK_SIZE));
	const int camCz = static_cast<int>(Math::Floor(eye.z / CHUNK_SIZE));
	const Point camChunk{ camCx, camCz };

	const auto& activeChunks = world.getActiveChunks();

	// ソートはカメラチャンクまたはチャンク数が変わった場合のみ実行する
	m_sortedChunks = activeChunks;
	if (camChunk != m_lastSortChunk || activeChunks.size() != m_lastActiveCount)
	{
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

		// カメラが乗っているチャンクは常に描画（足元が消えるのを防止）
		const bool isCameraChunk = (chunk->coord.x == camCx && chunk->coord.y == camCz);

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
		if (!anyVisible && !isCameraChunk) continue;

		drawChunk(*chunk, world);
	}

	// ---- 水面（y=0 の半透明平面）----
	{
		const ScopedRenderStates3D blend{ BlendState::Default2D };
		const ColorF waterColor = ColorF{ 0.15, 0.35, 0.55, 0.7 }.removeSRGBCurve();
		constexpr double cs = static_cast<double>(CHUNK_SIZE);

		for (const Chunk* chunk : m_sortedChunks)
		{
			if (!chunk) continue;
			// 水面下の地形がないチャンクはスキップ
			if (chunk->heightMin > 0.0f) continue;

			const double ox = static_cast<double>(chunk->coord.x) * cs;
			const double oz = static_cast<double>(chunk->coord.y) * cs;
			const double cx = ox + cs * 0.5;
			const double cz = oz + cs * 0.5;
			Box{ cx, -0.5, cz, cs, 1.0, cs }.draw(waterColor);
		}
	}
}

void WorldRenderer::drawChunk(Chunk& chunk, const World& world)
{
	const Key key = chunkCoordToKey(chunk.coord);

	if (!m_meshCache.contains(key))
	{
		// 初回: DynamicMesh を生成して GPU バッファを確保する
		m_meshCache[key] = DynamicMesh{ buildTerrainMeshData(chunk) };
		chunk.meshDirty = false;
		rebuildBuildingMeshes(key, chunk, world);
	}
	else if (chunk.meshDirty)
	{
		// 差分更新: GPU バッファを作り直さず頂点データだけ書き換える（W100 回避）
		m_meshCache[key].fill(buildTerrainMeshData(chunk));
		chunk.meshDirty = false;
		rebuildBuildingMeshes(key, chunk, world);
	}

	// 急斜面では地形メッシュの薄い断面が見えるため両面描画にする
	const ScopedRenderStates3D cullNone{ RasterizerState::SolidCullNone };
	m_meshCache[key].draw(m_grassTexture, ColorF{ 1.0 }.removeSRGBCurve());
	drawCachedBuildings(key);
}

MeshData WorldRenderer::buildTerrainMeshData(const Chunk& chunk)
{
	constexpr float cellSize = static_cast<float>(CHUNK_SIZE) / HEIGHT_CELLS;
	const Vec3 worldOrigin   = chunk.worldOrigin();

	const int gridSize = HEIGHT_CELLS + 1;

	// ワールド空間での1テクスチャタイルのサイズ [m]（= CHUNK_SIZE / kGrassTile = 1024 / 80 ≈ 12.8m）
	// チャンク境界をまたいで連続したUVにするため、ローカルUVではなくワールド座標から計算する
	constexpr float kTileSize = static_cast<float>(CHUNK_SIZE) / (16.0f * 5.0f);

	// 整数倍でないわずかな固定角度回転でグリッド感を崩す
	constexpr float kCosA = 0.97237f;  // cos(13.5°)
	constexpr float kSinA = 0.23345f;  // sin(13.5°)

	Array<Vertex3D> vertices;
	vertices.reserve(gridSize * gridSize);

	for (int row = 0; row < gridSize; ++row)
	{
		for (int col = 0; col < gridSize; ++col)
		{
			const float height = chunk.heightMap[{ col, row }];
			const Vec3 wpos = worldOrigin + Vec3{
				col * cellSize,
				height,
				row * cellSize
			};

			// 隣頂点の高さから中央差分で法線を計算する
			const int c0 = Max(col - 1, 0), c2 = Min(col + 1, HEIGHT_CELLS);
			const int r0 = Max(row - 1, 0), r2 = Min(row + 1, HEIGHT_CELLS);
			const float dhx = chunk.heightMap[{ c2, row }] - chunk.heightMap[{ c0, row }];
			const float dhz = chunk.heightMap[{ col, r2 }] - chunk.heightMap[{ col, r0 }];
			// 接線ベクトル T_x=(2c,dhx,0), T_z=(0,dhz,2c) の外積 → normalize(-dhx, 2c, -dhz)
			const Float3 n = Float3{ -dhx, 2.0f * cellSize, -dhz }.normalized();

			// ワールド座標からUVを計算することでチャンク境界をまたいで連続させる
			const float u = static_cast<float>(wpos.x) / kTileSize;
			const float v = static_cast<float>(wpos.z) / kTileSize;

			Vertex3D vert;
			vert.pos    = Float3{ static_cast<float>(wpos.x), static_cast<float>(wpos.y), static_cast<float>(wpos.z) };
			vert.normal = n;
			vert.tex    = Float2{ kCosA * u - kSinA * v, kSinA * u + kCosA * v };

			vertices << vert;
		}
	}

	Array<TriangleIndex32> indices;
	indices.reserve(HEIGHT_CELLS * HEIGHT_CELLS * 2);

	for (int row = 0; row < HEIGHT_CELLS; ++row)
	{
		for (int col = 0; col < HEIGHT_CELLS; ++col)
		{
			const uint32 i00 = static_cast<uint32>(row       * gridSize + col    );
			const uint32 i10 = static_cast<uint32>(row       * gridSize + col + 1);
			const uint32 i01 = static_cast<uint32>((row + 1) * gridSize + col    );
			const uint32 i11 = static_cast<uint32>((row + 1) * gridSize + col + 1);

			// Siv3D (DirectX) の 3D メッシュは CW = 表面
			indices << TriangleIndex32{ i00, i01, i10 };
			indices << TriangleIndex32{ i10, i01, i11 };
		}
	}

	return MeshData{ vertices, indices };
}

void WorldRenderer::rebuildBuildingMeshes(Key key, const Chunk& chunk, const World& world)
{
	constexpr float cellSize  = static_cast<float>(CHUNK_SIZE) / ZONE_CELLS;
	constexpr float footprint = 10.0f;

	const Vec3 origin = chunk.worldOrigin();

	// 建物種別ごとに MeshData を積み上げる
	HashTable<int, MeshData> groups;

	for (int row = 0; row < ZONE_CELLS; ++row)
	{
		for (int col = 0; col < ZONE_CELLS; ++col)
		{
			const Building& b = chunk.buildingGrid[{ col, row }];
			if (b.type == BuildingType::None || b.type == BuildingType::Farmland)
				continue;

			const float height = buildingHeight(b.type, b.stage);
			if (height <= 0.0f) continue;

			const float cx = static_cast<float>(origin.x + (col + 0.5) * cellSize);
			const float cz = static_cast<float>(origin.z + (row + 0.5) * cellSize);
			const float cy = world.sampleHeight(cx, cz) + height * 0.5f;

			MeshData box = MeshData::Box(
				Float3{ cx, cy, cz },
				Float3{ footprint, height, footprint });

			// 最近傍道路の向きに合わせてY軸回転
			if (b.angle != 0.0f)
			{
				const float cosA = Math::Cos(b.angle);
				const float sinA = Math::Sin(b.angle);
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

			auto& dst = groups[static_cast<int>(b.type)];
			const uint32 offset = static_cast<uint32>(dst.vertices.size());
			dst.vertices.append(box.vertices);
			for (const auto& tri : box.indices)
			{
				dst.indices << TriangleIndex32{
					tri.i0 + offset, tri.i1 + offset, tri.i2 + offset };
			}
		}
	}

	auto& batches = m_buildingMeshCache[key];
	batches.clear();
	for (auto& [typeInt, meshData] : groups)
	{
		if (meshData.vertices.isEmpty()) continue;
		batches.push_back({
			buildingColor(static_cast<BuildingType>(typeInt)).removeSRGBCurve(),
			Mesh{ meshData }
		});
	}
}

void WorldRenderer::drawCachedBuildings(Key key) const
{
	const auto it = m_buildingMeshCache.find(key);
	if (it == m_buildingMeshCache.end()) return;
	for (const auto& batch : it->second)
	{
		batch.mesh.draw(batch.color);
	}
}


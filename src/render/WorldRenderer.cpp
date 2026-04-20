#include "WorldRenderer.hpp"
#include "../asset/AssetRegistrar.hpp"
#include <Siv3D/Profiler.hpp>
#include <Siv3D/ViewFrustum.hpp>

void WorldRenderer::render(World& world, const BasicCamera3D& camera)
{
	// 地形メッシュを動的更新するため W100 警告を抑制する
	Profiler::EnableAssetCreationWarning(false);

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
	m_meshCache[key].draw(TextureAsset(Asset::Grass), ColorF{ 1.0 }.removeSRGBCurve());
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

namespace
{
	/// @brief 住宅建物タイプ → 使用する OBJ モデル群
	bool isResidential(BuildingType t)
	{
		return t == BuildingType::Detached
		    || t == BuildingType::LowApartment
		    || t == BuildingType::MidApartment
		    || t == BuildingType::HighApartment;
	}

	/// @brief Building タイプ + セル座標から決定論的に OBJ インデックス（0〜9）を選ぶ
	uint8 pickResidentialModel(BuildingType t, int gx, int gz)
	{
		const uint32 h = (static_cast<uint32>(gx) * 73856093u)
		               ^ (static_cast<uint32>(gz) * 19349663u);
		switch (t)
		{
		case BuildingType::Detached:      return 0;                          // 001
		case BuildingType::LowApartment:  return static_cast<uint8>(1 + h % 2); // 002-003
		case BuildingType::MidApartment:  return static_cast<uint8>(3 + h % 3); // 004-006
		case BuildingType::HighApartment: return static_cast<uint8>(6 + h % 4); // 007-010
		default:                          return 0;
		}
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

Model& WorldRenderer::getBuildingModel(uint8 idx)
{
	if (m_buildingModels.isEmpty())
	{
		m_buildingModels.resize(10);
	}
	if (m_buildingModels[idx].isEmpty())
	{
		const String path = U"assets/buildings/residential/residential_{:03d}.obj"_fmt(idx + 1);
		m_buildingModels[idx] = Model{ path };
		Model::RegisterDiffuseTextures(m_buildingModels[idx], TextureDesc::MippedSRGB);
	}
	return m_buildingModels[idx];
}

Optional<OrientedBox> WorldRenderer::buildingHitBox(const Chunk& chunk, const World& world,
                                                     int col, int row)
{
	if (col < 0 || col >= ZONE_CELLS || row < 0 || row >= ZONE_CELLS) return none;
	const Building& b = chunk.buildingGrid[{ col, row }];
	if (b.type == BuildingType::None || b.type == BuildingType::Farmland) return none;

	constexpr float cellSize  = static_cast<float>(CHUNK_SIZE) / ZONE_CELLS;
	constexpr float footprint = 10.0f;
	const Vec3 origin = chunk.worldOrigin();
	const float cx = static_cast<float>(origin.x + (col + 0.5) * cellSize);
	const float cz = static_cast<float>(origin.z + (row + 0.5) * cellSize);

	if (isResidential(b.type))
	{
		const float gy = world.sampleHeight(cx, cz);
		const int gx = chunk.coord.x * ZONE_CELLS + col;
		const int gz = chunk.coord.y * ZONE_CELLS + row;
		Model& model = getBuildingModel(pickResidentialModel(b.type, gx, gz));
		if (model.isEmpty()) return none;

		const Box& lb = model.boundingBox();
		const Vec3 localCenter = lb.center;
		const Vec3 size = lb.size;
		// drawCachedBuildings と同じ Mat4x4::RotateY → translate 変換を再現
		const double cosA = Math::Cos(b.angle);
		const double sinA = Math::Sin(b.angle);
		const Vec3 worldCenter{
			cx + localCenter.x * cosA + localCenter.z * sinA,
			gy + localCenter.y,
			cz - localCenter.x * sinA + localCenter.z * cosA
		};
		return OrientedBox{ worldCenter, size, Quaternion::RotateY(b.angle) };
	}

	const float height = buildingHeight(b.type);
	if (height <= 0.0f) return none;
	const float cy = world.sampleHeight(cx, cz) + height * 0.5f;
	return OrientedBox{ Vec3{ cx, cy, cz }, Vec3{ footprint, height, footprint },
	                   Quaternion::RotateY(b.angle) };
}

void WorldRenderer::drawBuildingSilhouette(const Chunk& chunk, const World& world,
                                            int col, int row, const ColorF& color)
{
	if (col < 0 || col >= ZONE_CELLS || row < 0 || row >= ZONE_CELLS) return;
	const Building& b = chunk.buildingGrid[{ col, row }];
	if (b.type == BuildingType::None || b.type == BuildingType::Farmland) return;

	constexpr float cellSize  = static_cast<float>(CHUNK_SIZE) / ZONE_CELLS;
	constexpr float footprint = 10.0f;
	const Vec3 origin = chunk.worldOrigin();
	const float cx = static_cast<float>(origin.x + (col + 0.5) * cellSize);
	const float cz = static_cast<float>(origin.z + (row + 0.5) * cellSize);

	if (isResidential(b.type))
	{
		const float gy = world.sampleHeight(cx, cz);
		const int gx = chunk.coord.x * ZONE_CELLS + col;
		const int gz = chunk.coord.y * ZONE_CELLS + row;
		Model& model = getBuildingModel(pickResidentialModel(b.type, gx, gz));
		if (model.isEmpty()) return;

		drawModelSilhouette(model, Mat4x4::RotateY(b.angle).translated(cx, gy, cz), color);
		return;
	}

	const float height = buildingHeight(b.type);
	if (height <= 0.0f) return;
	const float cy = world.sampleHeight(cx, cz) + height * 0.5f;

	// rebuildBuildingMeshes と同じパイプライン（MeshData::Box + 頂点手動回転）で描画する
	MeshData box = MeshData::Box(
		Float3{ cx, cy, cz },
		Float3{ footprint, height, footprint });
	rotateBoxVerticesY(box, cx, cz, b.angle);
	Mesh{ box }.draw(color);
}

void WorldRenderer::rebuildBuildingMeshes(Key key, const Chunk& chunk, const World& world)
{
	constexpr float cellSize  = static_cast<float>(CHUNK_SIZE) / ZONE_CELLS;
	constexpr float footprint = 10.0f;

	const Vec3 origin = chunk.worldOrigin();

	// 建物種別ごとに MeshData を積み上げる（Box 描画用）
	HashTable<int, MeshData> groups;
	// 住宅 OBJ インスタンス
	Array<BuildingModelInstance> modelInstances;

	for (int row = 0; row < ZONE_CELLS; ++row)
	{
		for (int col = 0; col < ZONE_CELLS; ++col)
		{
			const Building& b = chunk.buildingGrid[{ col, row }];
			if (b.type == BuildingType::None || b.type == BuildingType::Farmland)
				continue;

			const float cx = static_cast<float>(origin.x + (col + 0.5) * cellSize);
			const float cz = static_cast<float>(origin.z + (row + 0.5) * cellSize);

			// 住宅系は OBJ で描画する（地表位置に Y 軸回転のみ適用）
			if (isResidential(b.type))
			{
				const float gy = world.sampleHeight(cx, cz);
				const int gx = chunk.coord.x * ZONE_CELLS + col;
				const int gz = chunk.coord.y * ZONE_CELLS + row;
				modelInstances.push_back({
					pickResidentialModel(b.type, gx, gz),
					Float3{ cx, gy, cz },
					b.angle
				});
				continue;
			}

			const float height = buildingHeight(b.type);
			if (height <= 0.0f) continue;
			const float cy = world.sampleHeight(cx, cz) + height * 0.5f;

			MeshData box = MeshData::Box(
				Float3{ cx, cy, cz },
				Float3{ footprint, height, footprint });

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

	m_buildingModelCache[key] = std::move(modelInstances);
}

void WorldRenderer::drawCachedBuildings(Key key) const
{
	if (const auto it = m_buildingMeshCache.find(key); it != m_buildingMeshCache.end())
	{
		for (const auto& batch : it->second)
		{
			batch.mesh.draw(batch.color);
		}
	}

	if (const auto it = m_buildingModelCache.find(key); it != m_buildingModelCache.end())
	{
		auto* self = const_cast<WorldRenderer*>(this);
		for (const auto& inst : it->second)
		{
			Model& model = self->getBuildingModel(inst.modelIdx);
			if (model.isEmpty()) continue;

			const Mat4x4 worldMat = Mat4x4::RotateY(inst.angle)
				.translated(inst.pos.x, inst.pos.y, inst.pos.z);
			const auto& materials = model.materials();
			for (const auto& obj : model.objects())
			{
				const Transformer3D transform{ worldMat };
				obj.draw(materials);
			}
		}
	}
}


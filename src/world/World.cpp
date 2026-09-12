#include "World.hpp"
#include <cmath>

void World::reserveChunks()
{
	// 全チャンクを先に確保して座標だけ割り当て、後続処理は固定インデックスで参照できるようにする。
	m_chunks.reserve(WORLD_CHUNKS * WORLD_CHUNKS);
	for (int cy = 0; cy < WORLD_CHUNKS; ++cy)
		for (int cx = 0; cx < WORLD_CHUNKS; ++cx)
			m_chunks.emplace_back(Point{ cx, cy });
}

void World::installChunkDirect(Point coord, HeightMapResult&& hmr)
{
	// 生成済み地形結果を指定チャンクへ直接注入し、高さ範囲と開発可否の初期状態も揃える。
	if (!isValidCoord(coord)) return;

	Chunk& chunk = m_chunks[coordToIndex(coord)];
	chunk.heightMap = std::move(hmr.heightMap);
	chunk.heightMin = hmr.heightMin;
	chunk.heightMax = hmr.heightMax;
	chunk.isUrbanizationArea = true;
}

void World::update(Vec3 cameraWorldPos)
{
	// カメラ中心チャンクが変わった時だけ active 範囲を張り直し、描画・更新対象キャッシュを更新する。
	const Point newChunk = worldToChunkCoord(cameraWorldPos);

	if (newChunk == m_cameraChunk)
		return;

	m_cameraChunk = newChunk;
	bool activeChanged = false;

	for (auto& chunk : m_chunks)
	{
		if (chunk.state == ChunkState::Active)
		{
			const int dx = chunk.coord.x - m_cameraChunk.x;
			const int dy = chunk.coord.y - m_cameraChunk.y;
			if (Abs(dx) > ACTIVE_RANGE || Abs(dy) > ACTIVE_RANGE)
			{
				chunk.state = ChunkState::Sleeping;
				activeChanged = true;
			}
		}
	}

	for (int dy = -ACTIVE_RANGE; dy <= ACTIVE_RANGE; ++dy)
	{
		for (int dx = -ACTIVE_RANGE; dx <= ACTIVE_RANGE; ++dx)
		{
			const Point coord{ m_cameraChunk.x + dx, m_cameraChunk.y + dy };
			if (Chunk* chunk = getChunk(coord))
			{
				if (chunk->state != ChunkState::Active)
					activeChanged = true;
				chunk->state = ChunkState::Active;
			}
		}
	}

	if (activeChanged)
		rebuildActiveChunkCache();
}

const Chunk* World::getChunk(Point coord) const
{
	if (!isValidCoord(coord) || static_cast<size_t>(coordToIndex(coord))>=m_chunks.size()) return nullptr;
	const Chunk& chunk = m_chunks[coordToIndex(coord)];
	if (chunk.heightMap.isEmpty()) return nullptr;
	return &chunk;
}

Chunk* World::getChunk(Point coord)
{
	if (!isValidCoord(coord) || static_cast<size_t>(coordToIndex(coord))>=m_chunks.size()) return nullptr;
	Chunk& chunk = m_chunks[coordToIndex(coord)];
	if (chunk.heightMap.isEmpty()) return nullptr;
	return &chunk;
}

void World::rebuildActiveChunkCache()
{
	// Active 状態のチャンク参照だけを配列化し、毎フレームの走査対象を絞る。
	m_activeChunks.clear();
	m_activeChunksConst.clear();
	for (auto& chunk : m_chunks)
	{
		if (chunk.state == ChunkState::Active)
		{
			m_activeChunks << &chunk;
			m_activeChunksConst << &chunk;
		}
	}
}

const Array<Chunk*>& World::getActiveChunks()
{
	return m_activeChunks;
}

const Array<const Chunk*>& World::getActiveChunks() const
{
	return m_activeChunksConst;
}

float World::sampleHeight(float wx, float wz) const
{
	const int cx = static_cast<int>(Math::Floor(wx / CHUNK_SIZE));
	const int cz = static_cast<int>(Math::Floor(wz / CHUNK_SIZE));
	const Chunk* chunk = getChunk({ cx, cz });
	if (!chunk)
		return 0.0f;
	return chunk->getHeight(wx, wz);
}

void World::setGenerationParams(uint64 seed, float mapWidth, float mapDepth)
{
	// 地形生成で参照する乱数種とマップ寸法をまとめて差し替え、Perlin も同じ種で再初期化する。
	m_seed     = seed;
	m_mapWidth = mapWidth;
	m_mapDepth = mapDepth;
	m_perlin   = PerlinNoise{ seed };
	m_rivers=RiverNetwork{};
}

// ─────────────────────────────────────────────────────────────────────────────
// バイオームベース地形生成
// ─────────────────────────────────────────────────────────────────────────────

/// @brief cont ノイズにマップ端距離補正を加える
/// マップ端 → cont が下がり海に。中央 → cont が上がり内陸に。
/// 海は必ずマップ端に接し、小さな内陸海を防ぐ。
float World::adjustContinentalness(float rawCont, float wx, float wz) const
{
	// マップ端からの最短距離 (0 = 端, mapWidth/2 = 中央)
	const float dLeft   = wx;
	const float dRight  = m_mapWidth - wx;
	const float dTop    = wz;
	const float dBottom = m_mapDepth - wz;
	const float edgeDist = Min({ dLeft, dRight, dTop, dBottom });

	// 端からの距離を 0〜1 に正規化（5チャンク=5120m で完全に内陸扱い）
	constexpr float kEdgeZone = 5120.0f;
	const float edgeFactor = Clamp(edgeDist / kEdgeZone, 0.0f, 1.0f);

	// edgeFactor=0(端) → cont を 0.15 下げる（海になりやすい）
	// edgeFactor=1(中央) → cont を 0.15 上げる（海になりにくい）
	return Clamp(rawCont + (edgeFactor - 0.5f) * 0.30f, 0.0f, 1.0f);
}

void World::computeBiomeParams(float wx, float wz, float& outBase, float& outAmp) const
{
	// continentalness と mountainness の 2 軸ノイズから、連続補間で基底高と起伏量を決める。
	// 2つの独立した低周波ノイズ (0〜1)
	const float rawCont = static_cast<float>(
		m_perlin.noise2D0_1(wx * 0.00008 + 1000.0, wz * 0.00008 + 1000.0));
	const float cont = adjustContinentalness(rawCont, wx, wz);
	const float mtn = static_cast<float>(
		m_perlin.noise2D0_1(wx * 0.00012 + 2000.0, wz * 0.00012 + 2000.0));

	// ── 完全連続な二軸補間 ──
	// cont 軸: 海(0) → 海岸(0.3) → 内陸(0.5) → 高地(0.8) → 山脈帯(1.0)
	// mtn  軸: 平坦(0) → 起伏(0.5) → 険峻(1.0)
	// 各軸で基底高・振幅の「低 mtn 時」「高 mtn 時」を連続カーブで求め、mtn で補間する

	// smoothstep で遷移を滑らかにする
	auto smoothstep = [](float edge0, float edge1, float x) -> float
	{
		const float t = Clamp((x - edge0) / (edge1 - edge0), 0.0f, 1.0f);
		return t * t * (3.0f - 2.0f * t);
	};

	// cont 軸: 低 mtn 時の base/amp カーブ（平坦系: 海→海岸平野→平野→高原）
	float baseLow, ampLow;
	{
		const float seaToCoast  = smoothstep(0.20f, 0.35f, cont);  // 海→海岸
		const float coastToLand = smoothstep(0.35f, 0.50f, cont);  // 海岸→内陸
		const float landToHigh  = smoothstep(0.60f, 0.80f, cont);  // 内陸→高地

		baseLow = Math::Lerp(-40.0f, 0.0f, seaToCoast);            // 海→海岸
		baseLow = Math::Lerp(baseLow, 15.0f, coastToLand);         // →平野
		baseLow = Math::Lerp(baseLow, 200.0f, landToHigh);         // →高原

		ampLow = Math::Lerp(8.0f, 15.0f, seaToCoast);
		ampLow = Math::Lerp(ampLow, 20.0f, coastToLand);
		ampLow = Math::Lerp(ampLow, 40.0f, landToHigh);
	}

	// cont 軸: 高 mtn 時の base/amp カーブ（山系: 海→海岸丘陵→山麓→山脈）
	float baseHigh, ampHigh;
	{
		const float seaToCoast  = smoothstep(0.20f, 0.35f, cont);
		const float coastToLand = smoothstep(0.35f, 0.50f, cont);
		const float landToHigh  = smoothstep(0.55f, 0.75f, cont);

		baseHigh = Math::Lerp(-40.0f, 30.0f, seaToCoast);          // 海→海岸丘陵
		baseHigh = Math::Lerp(baseHigh, 120.0f, coastToLand);      // →山麓
		baseHigh = Math::Lerp(baseHigh, 500.0f, landToHigh);       // →山脈

		ampHigh = Math::Lerp(8.0f, 60.0f, seaToCoast);
		ampHigh = Math::Lerp(ampHigh, 200.0f, coastToLand);
		ampHigh = Math::Lerp(ampHigh, 450.0f, landToHigh);
	}

	// mtn 軸で低/高を滑らかに補間
	const float mtnBlend = smoothstep(0.25f, 0.75f, mtn);
	outBase = Math::Lerp(baseLow, baseHigh, mtnBlend);
	outAmp  = Math::Lerp(ampLow, ampHigh, mtnBlend);
}

BiomeType World::getBiome(float wx, float wz) const
{
	// 地形連続値とは別に、描画や生成ルールで使う離散バイオームを閾値ベースで分類する。
	const float rawCont = static_cast<float>(
		m_perlin.noise2D0_1(wx * 0.00008 + 1000.0, wz * 0.00008 + 1000.0));
	const float cont = adjustContinentalness(rawCont, wx, wz);
	const float mtn = static_cast<float>(
		m_perlin.noise2D0_1(wx * 0.00012 + 2000.0, wz * 0.00012 + 2000.0));
	// 湖判定用の独立ノイズ
	const float lake = static_cast<float>(
		m_perlin.noise2D0_1(wx * 0.00015 + 3000.0, wz * 0.00015 + 3000.0));

	if (cont < 0.25f) return BiomeType::Ocean;
	if (cont < 0.38f) return (mtn < 0.5f) ? BiomeType::CoastalPlain : BiomeType::CoastalHill;
	if (cont < 0.62f)
	{
		// 内陸部の低 mtn で lake ノイズが低い → 湖
		if (mtn < 0.30f && lake < 0.20f) return BiomeType::Lake;
		// 低 mtn で lake がやや低い → 窪地
		if (mtn < 0.25f && lake < 0.35f) return BiomeType::Basin;
		if (mtn < 0.30f) return BiomeType::Plain;
		if (mtn < 0.55f) return BiomeType::Hill;
		return BiomeType::Foothill;
	}
	// 高地帯でも lake ノイズが非常に低ければ高原湖
	if (mtn < 0.35f && lake < 0.15f) return BiomeType::Lake;
	if (mtn < 0.35f) return BiomeType::Plateau;
	if (mtn < 0.65f) return BiomeType::Mountain;
	return BiomeType::MountainRange;
}

float World::computeBaseHeight(float wx, float wz) const
{
	// 1. バイオームパラメータ（連続補間）
	float baseHeight, amplitude;
	computeBiomeParams(wx, wz, baseHeight, amplitude);

	// 2. ディテールノイズ（6オクターブ Perlin）
	constexpr double kFreq = 0.00035;
	const float detail = static_cast<float>(
		m_perlin.octave2D0_1(wx * kFreq, wz * kFreq, 6, 0.5));

	// 3. 中周波ノイズ（丘陵ディテール）
	const float midDetail = static_cast<float>(
		m_perlin.octave2D0_1(wx * kFreq * 3.0, wz * kFreq * 3.0, 4, 0.5));

	// 4. 湖ノイズ: 内陸の窪みを水面下に沈める
	const float lake = static_cast<float>(
		m_perlin.noise2D0_1(wx * 0.00015 + 3000.0, wz * 0.00015 + 3000.0));

	float h = baseHeight
		+ (detail - 0.5f) * amplitude * 1.6f
		+ (midDetail - 0.5f) * amplitude * 0.4f;

	// 5. 海・湖は標高をマイナスに保証
	const float rawCont = static_cast<float>(
		m_perlin.noise2D0_1(wx * 0.00008 + 1000.0, wz * 0.00008 + 1000.0));
	const float cont = adjustContinentalness(rawCont, wx, wz);
	const float mtn = static_cast<float>(
		m_perlin.noise2D0_1(wx * 0.00012 + 2000.0, wz * 0.00012 + 2000.0));

	// 海: cont < 0.25 → 確実に水面下
	if (cont < 0.20f)
		h = Min(h, -5.0f);
	else if (cont < 0.30f)
	{
		// 海岸遷移帯: 滑らかに水面下制約を緩和
		const float seaClamp = (cont - 0.20f) / 0.10f;  // 0→1
		h = Min(h, Math::Lerp(-5.0f, h, seaClamp));
	}

	// 湖: 内陸で lake ノイズが低い領域を水面下に沈める
	if (cont >= 0.30f && lake < 0.20f && mtn < 0.35f)
	{
		const float lakeDepth = (0.20f - lake) / 0.20f;  // 0→1 (lake=0.2→0, lake=0→1)
		h = Min(h, Math::Lerp(h, -10.0f, lakeDepth * lakeDepth));
	}

	return h;
}

HeightMapResult World::buildHeightMap(Point chunkCoord) const
{
	// 1 チャンク分の格子点高さをまとめて生成し、最小・最大標高も同時に記録する。
	constexpr float cellSize = static_cast<float>(CHUNK_SIZE) / HEIGHT_CELLS;
	Grid<float> hm(HEIGHT_CELLS + 1, HEIGHT_CELLS + 1, 0.0f);
	float lo =  1e30f;
	float hi = -1e30f;
	for (int row = 0; row <= HEIGHT_CELLS; ++row)
	{
		for (int col = 0; col <= HEIGHT_CELLS; ++col)
		{
			const float wx = chunkCoord.x * CHUNK_SIZE + col * cellSize;
			const float wz = chunkCoord.y * CHUNK_SIZE + row * cellSize;
			const float h  = computeHeight(wx, wz);
			hm[{ col, row }] = h;
			if (h < lo) lo = h;
			if (h > hi) hi = h;
		}
	}
	return { std::move(hm), lo, hi };
}

void World::generateRivers()
{
	m_rivers.generate(m_mapWidth,m_mapDepth,[this](double x,double z) { return computeBaseHeight(static_cast<float>(x),static_cast<float>(z)); });
}

float World::computeHeight(float wx,float wz) const
{
	return static_cast<float>(m_rivers.carveHeight({wx,wz},computeBaseHeight(wx,wz)));
}

void World::setGridHeight(int x,int z,float height)
{
	for (int cz=Max(0,(z-1)/HEIGHT_CELLS);cz<=Min(WORLD_CHUNKS-1,z/HEIGHT_CELLS);++cz)
		for (int cx=Max(0,(x-1)/HEIGHT_CELLS);cx<=Min(WORLD_CHUNKS-1,x/HEIGHT_CELLS);++cx)
		{
			if (auto* chunk=getChunk({cx,cz})) { chunk->heightMap[{x-cx*HEIGHT_CELLS,z-cz*HEIGHT_CELLS}]=height; chunk->meshDirty=true; }
		}
}

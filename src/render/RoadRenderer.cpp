# include "../../stdafx.h"
#include "RoadRenderer.hpp"

// ─────────────────────────────────────────────────────────────────────────────
// 内部ヘルパー（無名名前空間）
// ─────────────────────────────────────────────────────────────────────────────

namespace
{
	/// @brief 接線の XZ 直角右ベクトルを返す（Y=0）
	Vec3 calcRight(const Vec3& tan)
	{
		const double lenXZ = Math::Sqrt(tan.x * tan.x + tan.z * tan.z);
		if (lenXZ > 1e-6)
			return Vec3{ tan.z / lenXZ, 0.0, -tan.x / lenXZ };
		return Vec3{ 1.0, 0.0, 0.0 };
	}

	/// @brief 弧長 s での中心・右ベクトルを返す（Y は地形 + リフト）
	struct SliceInfo { Vec3 center; Vec3 right; };
	SliceInfo makeSlice(const CubicBezier& bez, const World& world, float s, double terrainLift)
	{
		const Vec3  p  = bez.positionAt(s);
		// sampleHeight はチャンク未ロード時に 0 を返すため computeHeight を使う
		const float gy = world.computeHeight(static_cast<float>(p.x), static_cast<float>(p.z));
		return { Vec3{ p.x, gy + terrainLift, p.z }, calcRight(bez.tangentAt(s)) };
	}

	/// @brief Vertex3D を生成する
	Vertex3D makeVert(const Vec3& pos, float u, float v)
	{
		Vertex3D vt;
		vt.pos    = Float3{ static_cast<float>(pos.x), static_cast<float>(pos.y), static_cast<float>(pos.z) };
		vt.normal = Float3{ 0.0f, 1.0f, 0.0f };
		vt.tex    = Float2{ u, v };
		return vt;
	}

	/// @brief 四角形を頂点・インデックス配列に追記する（CW ワインディング）
	void appendQuad(Array<TriangleIndex32>& indices,
	                uint32 iL0, uint32 iR0, uint32 iL1, uint32 iR1)
	{
		indices << TriangleIndex32{ iL0, iL1, iR0 };
		indices << TriangleIndex32{ iR0, iL1, iR1 };
	}

	/// @brief ベジェに沿った破線（または実線）ポリゴン帯を追記する
	/// @param sStart  描画開始弧長 [m]
	/// @param sEnd    描画終了弧長 [m]
	void appendDashedStrip(
		Array<Vertex3D>&        vertices,
		Array<TriangleIndex32>& indices,
		const CubicBezier&      bez,
		const World&            world,
		float offset, float halfLW,
		float dashLength, float gapLength,
		float sStart, float sEnd,
		float terrainLift = 2.05f)
	{
		const float spanLen  = sEnd - sStart;
		if (spanLen <= 0.0f) return;

		const bool  solid    = (dashLength <= 0.0f || gapLength <= 0.0f);
		const float cycleLen = dashLength + gapLength;
		const int   N        = Clamp(static_cast<int>(spanLen / 2.0f) + 1, 5, 200);

		for (int i = 0; i < N; ++i)
		{
			const float s0   = sStart + (i       / static_cast<float>(N)) * spanLen;
			const float s1   = sStart + ((i + 1) / static_cast<float>(N)) * spanLen;
			const float sMid = (s0 + s1) * 0.5f;

			if (!solid && (Math::Fmod(sMid - sStart, cycleLen) >= dashLength))
				continue;

			const uint32 base = static_cast<uint32>(vertices.size());

			for (int j = 0; j <= 1; ++j)
			{
				const float   s  = (j == 0) ? s0 : s1;
				const auto    sl = makeSlice(bez, world, s, terrainLift);
				const Vec3    lc = sl.center + sl.right * offset;
				vertices << makeVert(lc - sl.right * halfLW, 0.0f, s / bez.totalLength);
				vertices << makeVert(lc + sl.right * halfLW, 1.0f, s / bez.totalLength);
			}

			appendQuad(indices, base, base + 1, base + 2, base + 3);
		}
	}
}

// ─────────────────────────────────────────────────────────────────────────────
// RoadRenderer 公開メソッド
// ─────────────────────────────────────────────────────────────────────────────

bool RoadRenderer::loadStyle(FilePathView tomlPath)
{
	return m_styleRegistry.load(tomlPath);
}

void RoadRenderer::render(const RoadNetwork& network, GameTime now, const World& world)
{
	Profiler::EnableAssetCreationWarning(false);

	// 地形変更時は全キャッシュをクリアして再構築する
	for (const Chunk* chunk : world.getActiveChunks())
	{
		if (chunk && chunk->dirty)
		{
			m_meshCache.clear();
			m_laneCache.clear();
			m_marginCache.clear();
			m_nodeCapCache.clear();
			break;
		}
	}

	// ---- エッジ描画（端をノード半幅分カット）----
	for (const RoadEdge& edge : network.edges())
	{
		if (edge.id == -1) continue;

		const auto bez = network.getBezier(edge.id);
		if (!bez) continue;

		const float mA = edgeMargin(edge, edge.nodeA);
		const float mB = edgeMargin(edge, edge.nodeB);

		drawEdge(edge, *bez, mA, mB, world);
	}

	// ---- ノードキャップ描画（交差点フィル）----
	for (const RoadNode& node : network.nodes())
	{
		if (node.id < 0) continue;
		drawNodeCap(network, node.id, world);
	}

	(void)now;
}

void RoadRenderer::markDirty(int edgeId)
{
	m_meshCache.erase(edgeId);
	m_laneCache.erase(edgeId);
	m_marginCache.erase(edgeId);
	m_nodeCapCache.clear();   // 隣接ノードの特定が困難なため全クリア
}

void RoadRenderer::markTopologyChanged()
{
	// cutoff 変化（RoadNetwork 側で更新済み）を拾うためマージンキャッシュと
	// ノードキャップキャッシュをクリアする
	m_marginCache.clear();
	m_nodeCapCache.clear();
}

// ─────────────────────────────────────────────────────────────────────────────
// ヘルパー
// ─────────────────────────────────────────────────────────────────────────────

float RoadRenderer::halfWidthWithShoulder(const RoadEdge& edge) const
{
	return edge.totalWidth() / 2.0f + m_styleRegistry.get(edge.roadType).shoulderWidth;
}

float RoadRenderer::edgeMargin(const RoadEdge& edge, int nodeId)
{
	return (nodeId == edge.nodeA) ? edge.cutoffA : edge.cutoffB;
}

// ─────────────────────────────────────────────────────────────────────────────
// エッジ描画
// ─────────────────────────────────────────────────────────────────────────────

void RoadRenderer::drawEdge(const RoadEdge& edge, const CubicBezier& bezier,
                             float marginA, float marginB, const World& world)
{
	const RoadStyle& style = m_styleRegistry.get(edge.roadType);

	// マージンが変わった場合はキャッシュを破棄して再構築する
	if (auto it = m_marginCache.find(edge.id); it != m_marginCache.end())
	{
		if (it->second.a != marginA || it->second.b != marginB)
		{
			m_meshCache.erase(edge.id);
			m_laneCache.erase(edge.id);
			m_marginCache.erase(edge.id);
			m_nodeCapCache.clear();
		}
	}

	// ---- 路面メッシュ ----
	if (!m_meshCache.contains(edge.id))
	{
		const MeshData md = buildRoadMesh(edge, bezier, style, world, marginA, marginB);
		if (md.vertices.isEmpty()) return;
		m_meshCache.emplace(edge.id, Mesh{ md });
		m_marginCache[edge.id] = { marginA, marginB };
	}

	if (style.surface.surfaceTexture)
		m_meshCache[edge.id].draw(*style.surface.surfaceTexture,
		                          style.surface.surfaceColor.removeSRGBCurve());
	else
		m_meshCache[edge.id].draw(style.surface.surfaceColor.removeSRGBCurve());

	// ---- 車線区画線 ----
	if (!m_laneCache.contains(edge.id))
		m_laneCache[edge.id] = buildLaneLineBatches(edge, bezier, style, world, marginA, marginB);

	for (const auto& b : m_laneCache[edge.id])
		b.mesh.draw(b.color);
}

// ─────────────────────────────────────────────────────────────────────────────
// ノードキャップ描画
// ─────────────────────────────────────────────────────────────────────────────

void RoadRenderer::drawNodeCap(const RoadNetwork& network, int nodeId, const World& world)
{
	const RoadNode* node = network.getNode(nodeId);
	if (!node || node->edgeIds.size() < 2) return;

	if (!m_nodeCapCache.contains(nodeId))
	{
		const MeshData md = buildNodeCapMesh(network, nodeId, world);
		if (md.vertices.isEmpty()) return;
		m_nodeCapCache.emplace(nodeId, Mesh{ md });
	}

	// 最も道路種別の高いエッジのスタイルで描画する
	RoadType maxType = RoadType::LocalRoad;
	for (int eid : node->edgeIds)
	{
		const RoadEdge* e = network.getEdge(eid);
		if (e && static_cast<int>(e->roadType) > static_cast<int>(maxType))
			maxType = e->roadType;
	}
	const RoadStyle& style = m_styleRegistry.get(maxType);

	if (style.surface.surfaceTexture)
		m_nodeCapCache[nodeId].draw(*style.surface.surfaceTexture,
		                            style.surface.surfaceColor.removeSRGBCurve());
	else
		m_nodeCapCache[nodeId].draw(style.surface.surfaceColor.removeSRGBCurve());
}

// ─────────────────────────────────────────────────────────────────────────────
// メッシュ生成
// ─────────────────────────────────────────────────────────────────────────────

MeshData RoadRenderer::buildRoadMesh(const RoadEdge& edge, const CubicBezier& bezier,
                                     const RoadStyle& style, const World& world,
                                     float marginA, float marginB) const
{
	// NodeCap との隙間を防ぐため、エッジメッシュをわずかにキャップ側へ延伸する
	constexpr float kOverlap = 0.1f;
	const float totalLen = bezier.totalLength;
	const float sStart   = Max(marginA - kOverlap, 0.0f);
	const float sEnd     = Min(totalLen - marginB + kOverlap, totalLen);
	if (sStart >= sEnd - 0.1f) return MeshData{};

	const float  halfWidth = halfWidthWithShoulder(edge);
	const float  spanLen   = sEnd - sStart;
	const int    N         = Clamp(static_cast<int>(spanLen / 2.0f) + 1, 5, 100);

	Array<Vertex3D> vertices;
	vertices.reserve((N + 1) * 2);

	for (int i = 0; i <= N; ++i)
	{
		const float s  = sStart + (i / static_cast<float>(N)) * spanLen;
		const auto  sl = makeSlice(bezier, world, s, 2.0);
		const float v  = (s - sStart) * style.surface.surfaceTileV;

		vertices << makeVert(sl.center - sl.right * halfWidth, 0.0f, v);
		vertices << makeVert(sl.center + sl.right * halfWidth, 1.0f, v);
	}

	Array<TriangleIndex32> indices;
	indices.reserve(N * 2);
	for (int i = 0; i < N; ++i)
	{
		appendQuad(indices,
		           static_cast<uint32>(i * 2),
		           static_cast<uint32>(i * 2 + 1),
		           static_cast<uint32>(i * 2 + 2),
		           static_cast<uint32>(i * 2 + 3));
	}

	return MeshData{ vertices, indices };
}

Array<RoadRenderer::LaneLineBatch> RoadRenderer::buildLaneLineBatches(
	const RoadEdge& edge, const CubicBezier& bezier,
	const RoadStyle& style, const World& world,
	float marginA, float marginB) const
{
	constexpr float kOverlap = 0.1f;
	const float sStart = Max(marginA - kOverlap, 0.0f);
	const float sEnd   = Min(bezier.totalLength - marginB + kOverlap, bezier.totalLength);
	if (sStart >= sEnd - 0.1f || edge.lanes.size() < 2) return {};

	int centerBoundaryIdx = -1;
	for (int i = 0; i + 1 < static_cast<int>(edge.lanes.size()); ++i)
	{
		if (edge.lanes[i].dir != edge.lanes[i + 1].dir)
		{
			centerBoundaryIdx = i;
			break;
		}
	}

	const float halfTotal = edge.totalWidth() / 2.0f;
	MeshData    markingData, centerData;
	float       cumFromLeft = 0.0f;

	for (int i = 0; i + 1 < static_cast<int>(edge.lanes.size()); ++i)
	{
		cumFromLeft += edge.lanes[i].width;
		const float offsetFromCenter = cumFromLeft - halfTotal;
		const bool  isCenter         = (i == centerBoundaryIdx);
		const LineMarkStyle& ms      = isCenter ? style.centerLine : style.laneMarking;
		MeshData&   target           = isCenter ? centerData : markingData;

		appendDashedStrip(target.vertices, target.indices,
		                  bezier, world,
		                  offsetFromCenter, ms.lineWidth * 0.5f,
		                  ms.dashLength, ms.gapLength,
		                  sStart, sEnd);
	}

	Array<LaneLineBatch> batches;
	if (!markingData.vertices.isEmpty())
		batches << LaneLineBatch{ style.laneMarking.color.removeSRGBCurve(), Mesh{ markingData } };
	if (!centerData.vertices.isEmpty())
		batches << LaneLineBatch{ style.centerLine.color.removeSRGBCurve(), Mesh{ centerData } };

	return batches;
}

MeshData RoadRenderer::buildNodeCapMesh(const RoadNetwork& network, int nodeId,
                                        const World& world) const
{
	// Pavecity の Intersection::BuildMesh と同一手順で構築する。
	// ① 各接続道路の切断点情報（角・接線）を収集して角度順にソート
	// ② 各隣接ペアの フィレット曲線（DIV+1点）を生成 → fillets[i]
	// ③ BridgeTwoLine: fillet[i] 後半(逆順) ↔ fillet[next] 前半 をクワッドで橋渡し
	// ④ 中心ポリゴン: 各フィレット中点のファン三角形化（N≥3 のみ）
	// 中心頂点を使わないため、傾斜地でのテント状クリースが生じない。

	const RoadNode* node = network.getNode(nodeId);
	if (!node || node->edgeIds.size() < 2) return MeshData{};

	// ---- ① 各エッジの切断点情報を収集 ----
	struct EdgeInfo
	{
		Vec3   capTan;       ///< ノードから外向きの XZ 正規化接線
		Vec3   leftCorner;   ///< 切断点の左端（Y=地形+リフト）
		Vec3   rightCorner;  ///< 切断点の右端（Y=地形+リフト）
		double angle;        ///< XZ 平面上の外向き角度（ソート用）
	};

	Array<EdgeInfo> infos;

	for (int eid : node->edgeIds)
	{
		const RoadEdge* edge = network.getEdge(eid);
		if (!edge) continue;
		const auto bezOpt = network.getBezier(eid);
		if (!bezOpt) continue;
		const CubicBezier& bez = *bezOpt;

		const double halfW  = halfWidthWithShoulder(*edge);
		const float  capRad = (edge->nodeA == nodeId) ? edge->cutoffA : edge->cutoffB;

		// 道路メッシュと同じ s 値（kOverlap 分だけカットオフより手前）を使う
		constexpr float kOverlap = 0.1f;
		Vec3 capPos, capTan;
		if (edge->nodeA == nodeId)
		{
			const float s = Clamp(capRad - kOverlap, 0.0f, bez.totalLength * 0.45f);
			capPos = bez.positionAt(s);
			capTan = bez.tangentAt(s);
		}
		else
		{
			const float s = Clamp(bez.totalLength - capRad + kOverlap,
			                      bez.totalLength * 0.55f, bez.totalLength);
			capPos = bez.positionAt(s);
			capTan = -bez.tangentAt(s);
		}

		const Vec2   tanXZ  = Vec2{ capTan.x, capTan.z };
		const double tanLen = tanXZ.length();
		if (tanLen < 1e-6) continue;
		const Vec2 tanNorm = tanXZ / tanLen;
		capTan = Vec3{ tanNorm.x, 0.0, tanNorm.y };

		const Vec3  right    = calcRight(capTan);
		const float gy       = world.computeHeight(static_cast<float>(capPos.x),
		                                           static_cast<float>(capPos.z));
		const Vec3  capCenter{ capPos.x, gy + 2.0, capPos.z };

		EdgeInfo info;
		info.capTan      = capTan;
		info.leftCorner  = capCenter - right * halfW;
		info.rightCorner = capCenter + right * halfW;
		info.angle       = Math::Atan2(capTan.z, capTan.x);
		infos << info;
	}

	if (static_cast<int>(infos.size()) < 2) return MeshData{};

	infos.sort_by([](const EdgeInfo& a, const EdgeInfo& b) { return a.angle < b.angle; });

	const int N = static_cast<int>(infos.size());

	// ---- ② フィレット曲線を生成 ----
	// fillet[i]: infos[i].leftCorner → infos[(i+1)%N].rightCorner の DIV+1 点
	// Pavecity の bezier{road1_edge, road2_edge, -road1_dir, -road2_dir} に対応
	constexpr int DIV  = 6;   // 偶数
	constexpr int HALF = DIV / 2;

	Array<Array<Vec3>> fillets(N);
	for (int i = 0; i < N; ++i)
	{
		const int       next = (i + 1) % N;
		const EdgeInfo& ei   = infos[i];
		const EdgeInfo& en   = infos[next];

		const Vec2   p0xz{ ei.leftCorner.x,  ei.leftCorner.z  };
		const Vec2   p3xz{ en.rightCorner.x, en.rightCorner.z };
		const double dist  = (p3xz - p0xz).length();
		const double scale = Max(dist / 3.0, 0.5);
		const Vec2   p1xz  = p0xz + Vec2{ -ei.capTan.x, -ei.capTan.z } * scale;
		const Vec2   p2xz  = p3xz + Vec2{ -en.capTan.x, -en.capTan.z } * scale;

		Array<Vec3> pts;
		pts.reserve(DIV + 1);
		for (int k = 0; k <= DIV; ++k)
		{
			// 端点は道路メッシュの角座標をそのまま使い、高さを一致させる
			if (k == 0)
			{
				pts << ei.leftCorner;
				continue;
			}
			if (k == DIV)
			{
				pts << en.rightCorner;
				continue;
			}
			const double t  = k / static_cast<double>(DIV);
			const double mt = 1.0 - t;
			const Vec2   ptXZ = p0xz * (mt*mt*mt)
			                  + p1xz * (3.0*mt*mt*t)
			                  + p2xz * (3.0*mt*t*t)
			                  + p3xz * (t*t*t);
			const double y = ei.leftCorner.y
			               + (en.rightCorner.y - ei.leftCorner.y) * k / DIV;
			pts << Vec3{ ptXZ.x, y, ptXZ.y };
		}
		fillets[i] = std::move(pts);
	}

	// ---- ③ BridgeTwoLine: fillet[i] 後半(逆順) ↔ fillet[next] 前半 ----
	// lineA: fa[DIV], fa[DIV-1], ..., fa[HALF]  (HALF+1 点)
	// lineB: fb[0],   fb[1],    ..., fb[HALF]   (HALF+1 点)
	// Pavecity BridgeTwoLine の三角形ワインディングに合わせる:
	//   {p0,p1,p2} + {p1,p3,p2}  → MergeTrianglesToMesh が反転 → {p2,p1,p0} + {p2,p3,p1}
	Array<Vertex3D>        vertices;
	Array<TriangleIndex32> indices;

	for (int i = 0; i < N; ++i)
	{
		const int          next = (i + 1) % N;
		const Array<Vec3>& fa   = fillets[i];
		const Array<Vec3>& fb   = fillets[next];

		for (int k = 0; k < HALF; ++k)
		{
			const Vec3& p0 = fa[DIV - k];      // lineA[k]   : rightCorner[next] 側
			const Vec3& p1 = fa[DIV - k - 1];  // lineA[k+1] : 中心側
			const Vec3& p2 = fb[k];             // lineB[k]   : leftCorner[next] 側
			const Vec3& p3 = fb[k + 1];         // lineB[k+1] : 中心側

			const uint32 base = static_cast<uint32>(vertices.size());
			vertices << makeVert(p0, 0.0f, 0.0f)   // base+0
			         << makeVert(p1, 0.0f, 1.0f)   // base+1
			         << makeVert(p2, 1.0f, 0.0f)   // base+2
			         << makeVert(p3, 1.0f, 1.0f);  // base+3

			indices << TriangleIndex32{ base + 0, base + 1, base + 2 };
			indices << TriangleIndex32{ base + 1, base + 3, base + 2 };
		}
	}

	// ---- ④ 中心ポリゴン（N≥3 のみ）----
	// Pavecity の mids ファン三角形化に対応
	// mids[i] = fillet[i][HALF]（各フィレットの中点）
	if (N >= 3)
	{
		Array<Vec3> mids;
		for (int i = 0; i < N; ++i)
			mids << fillets[i][HALF];

		while (static_cast<int>(mids.size()) >= 3)
		{
			const uint32 base = static_cast<uint32>(vertices.size());
			vertices << makeVert(mids[0], 0.5f, 0.5f)   // base+0
			         << makeVert(mids[1], 0.5f, 0.5f)   // base+1
			         << makeVert(mids[2], 0.5f, 0.5f);  // base+2
			indices << TriangleIndex32{ base + 2, base + 1, base + 0 };
			mids.erase(mids.begin() + 1);
		}
	}

	return MeshData{ vertices, indices };
}

#include "GuideSign.hpp"
#include "RoadNetwork.hpp"
#include "RoadSign.hpp"
#include "ObjParser.hpp"
#include "../traffic/TrafficCommon.hpp"
#include <queue>

namespace
{
	/// @brief Vertex3D を生成する
	Vertex3D mkVert(double x, double y, double z, float u, float v,
	                double nx, double ny, double nz)
	{
		Vertex3D vt;
		vt.pos    = Float3{ static_cast<float>(x), static_cast<float>(y), static_cast<float>(z) };
		vt.normal = Float3{ static_cast<float>(nx), static_cast<float>(ny), static_cast<float>(nz) };
		vt.tex    = Float2{ u, v };
		return vt;
	}
}

GuideSign::BoardSize GuideSign::computeBoardSizeFor(const GuideSignPlacement& g)
{
	BoardSize sz;
	if (g.kind == GuideSignKind::DirectionArrow)
	{
		// 十字レイアウト（ref1 準拠）: 3.5m × 3.0m 前後
		// elements の Text から最長文字数を推定
		int maxLen = 3;
		for (const auto& el : g.elements)
			if (el.kind == SignElementKind::Text)
				maxLen = Max(maxLen, static_cast<int>(el.text.size()));
		sz.width  = Max(3.5, maxLen * 0.5 * 2 + 1.5);
		sz.height = 3.0;
	}
	else
	{
		// DirectionDistance: Text 要素の行数・文字数から推定
		int rows = 0, maxLen = 3;
		for (const auto& el : g.elements)
		{
			if (el.kind == SignElementKind::Text)
			{
				++rows;
				maxLen = Max(maxLen, static_cast<int>(el.text.size()));
			}
		}
		rows = Max(1, rows);
		const double w = Max(2.5, 1.0 + maxLen * 0.6 + 1.2 + 0.4);
		const double h = 0.25 + 0.55 * rows + 0.15;
		sz = { w, h };
	}
	if (g.widthOverride > 0.0f)  sz.width  = g.widthOverride;
	if (g.heightOverride > 0.0f) sz.height = g.heightOverride;
	return sz;
}

MeshData GuideSign::CreateBoardMesh(double width, double height)
{
	// 板厚は僅かに 0.04m。両面描画し裏面も視認可能に。
	constexpr double thickness = 0.04;
	const double hx = width * 0.5;
	const double hy = height * 0.5;
	const double hz = thickness * 0.5;

	MeshData md;
	md.vertices.reserve(8);
	md.indices.reserve(4);

	// 前面 (+Z)
	// computeSignTransforms は RotateY(yaw+π) を適用し、driver 視点で
	// local +X が左側に、local -X が右側に来る。そのため U=0 を +hx、U=1 を -hx に割当てる
	md.vertices << mkVert( hx,  hy,  hz, 0.0f, 0.0f, 0.0, 0.0, 1.0);
	md.vertices << mkVert(-hx,  hy,  hz, 1.0f, 0.0f, 0.0, 0.0, 1.0);
	md.vertices << mkVert(-hx, -hy,  hz, 1.0f, 1.0f, 0.0, 0.0, 1.0);
	md.vertices << mkVert( hx, -hy,  hz, 0.0f, 1.0f, 0.0, 0.0, 1.0);
	md.indices << TriangleIndex32{ 0, 1, 2 };
	md.indices << TriangleIndex32{ 0, 2, 3 };

	// 背面 (-Z)（裏面でも同じ向きで文字が読めるよう UV を設定）
	md.vertices << mkVert(-hx,  hy, -hz, 0.0f, 0.0f, 0.0, 0.0, -1.0);
	md.vertices << mkVert( hx,  hy, -hz, 1.0f, 0.0f, 0.0, 0.0, -1.0);
	md.vertices << mkVert( hx, -hy, -hz, 1.0f, 1.0f, 0.0, 0.0, -1.0);
	md.vertices << mkVert(-hx, -hy, -hz, 0.0f, 1.0f, 0.0, 0.0, -1.0);
	md.indices << TriangleIndex32{ 4, 5, 6 };
	md.indices << TriangleIndex32{ 4, 6, 7 };

	return md;
}

namespace
{
	GuideSign::PoleMetadata g_guidePoleMetadata;
	bool                    g_guidePoleMetadataLoaded = false;
}

const GuideSign::PoleMetadata& GuideSign::poleMetadata()
{
	if (!g_guidePoleMetadataLoaded)
	{
		const JSON j = JSON::Load(U"assets/signs/guide/guide_pole.json");
		if (j)
		{
			const auto& b = j[U"board"];
			if (b)
			{
				g_guidePoleMetadata.offsetX = b[U"offsetX"].getOr<float>(0.0f);
				g_guidePoleMetadata.offsetY = b[U"offsetY"].getOr<float>(3.3f);
				g_guidePoleMetadata.offsetZ = b[U"offsetZ"].getOr<float>(0.06f);
			}
		}
		g_guidePoleMetadataLoaded = true;
	}
	return g_guidePoleMetadata;
}

void GuideSign::reloadPoleMetadata()
{
	g_guidePoleMetadataLoaded = false;
}


Size GuideSign::guideSignTexSize(double widthM, double heightM)
{
	constexpr double kPxPerM = 180.0;  // リファレンス準拠の高解像度
	const int w = Max(256, static_cast<int>(std::ceil(widthM * kPxPerM)));
	const int h = Max(96,  static_cast<int>(std::ceil(heightM * kPxPerM)));
	return Size{ w, h };
}


namespace
{
	// ── 自由配置モード: 回転矢印描画 ──
	// headBase: 矢頭の基準サイズ（px）。shaftLen: シャフト長（px）。
	// 矢頭サイズは headBase で固定され、shaftLen のみ可変。
	void drawRotatedArrow(double cx, double cy, double angleDeg,
	                      double headBase, double shaftLen, const ColorF& col)
	{
		// 基本形: 上向き矢印 (angle=0)。angleDeg で回転。
		const double rad       = Math::ToRadians(angleDeg);
		const double shaftHalf = headBase * 0.24;  // シャフト幅（半分）
		const double headHalf  = headBase * 0.56;  // 矢頭幅（半分）
		const double headH     = headBase * 0.90;  // 矢頭高さ

		// 全体の半長（中心原点）: 矢頭 + シャフト の合計
		const double totalHalf = (headH + shaftLen) * 0.5;

		// ローカル座標（上向き矢印、中心=原点）
		const Array<Vec2> local = {
			{ -shaftHalf,  totalHalf },              // シャフト下端左
			{ -shaftHalf, -totalHalf + headH },      // 矢頭根元左
			{ -headHalf,  -totalHalf + headH },      // 矢頭翼左
			{  0.0,       -totalHalf },              // 先端
			{  headHalf,  -totalHalf + headH },      // 矢頭翼右
			{  shaftHalf, -totalHalf + headH },      // 矢頭根元右
			{  shaftHalf,  totalHalf },              // シャフト下端右
		};

		// 回転 + 平行移動
		const double cosA = Math::Cos(rad);
		const double sinA = Math::Sin(rad);
		Array<Vec2> world;
		for (const auto& p : local)
			world << Vec2{ cx + p.x * cosA - p.y * sinA,
			               cy + p.x * sinA + p.y * cosA };

		Polygon{ world }.draw(col);
	}

	// ── 背景 + 枠 + 各 element を描画（描画パスは常にこれ一本） ──
	void renderElements(const GuideSignPlacement& g, const Array<SignElement>& elements,
	                    const Size& ts, const Font& fontJa, const Font& fontNum)
	{
		const ColorF bg = (g.bgColor.a > 0.001) ? g.bgColor : ColorF{ 0.07, 0.28, 0.66, 1.0 };
		const ColorF fg{ 1.0, 1.0, 1.0 };

		Rect{ 0, 0, ts }.draw(bg);
		const double border = Max(3.0, ts.y * 0.02);
		const double inset  = border * 1.8;
		RectF{ inset, inset, ts.x - inset * 2, ts.y - inset * 2 }
			.drawFrame(border, 0.0, fg);

		const double baseTextH = ts.y * 0.12;

		for (const auto& el : elements)
		{
			const double px = el.posX * ts.x;
			const double py = el.posY * ts.y;

			if (el.kind == SignElementKind::Arrow)
			{
				const double headBase = baseTextH * 2.2;
				const double shaftLen = baseTextH * el.arrowLength;
				drawRotatedArrow(px, py, el.arrowAngle, headBase, shaftLen, fg);
			}
			else // Text
			{
				if (el.text.isEmpty()) continue;
				// MSDF フォントは drawAt(size) でなく Transformer2D でスケールする
				const double textH = baseTextH * el.scale;
				{
					const Transformer2D tr{ Mat3x2::Scale(textH / 32.0, Vec2{ px, py }) };
					fontJa(el.text).drawAt(Vec2{ px, py }, fg);
				}
				if (g.showReading && !el.reading.isEmpty())
				{
					const double readingH = textH * 0.40;
					const double readingY = py + textH * 0.70;
					const Transformer2D tr{ Mat3x2::Scale(readingH / 24.0, Vec2{ px, readingY }) };
					fontNum(el.reading).drawAt(Vec2{ px, readingY }, fg);
				}
			}
		}
	}
}

void GuideSign::renderContents(const GuideSignPlacement& g, const Size& texSize,
                                const Font& fontJa, const Font& fontNum)
{
	renderElements(g, g.elements, texSize, fontJa, fontNum);
}

MeshData GuideSign::CreatePoleMesh()
{
	// assets/signs/guide/guide_pole.obj を読み込み、複数オブジェクトを単一 MeshData に結合
	const auto parsed = ObjParser::parse(U"assets/signs/guide/guide_pole.obj");
	MeshData md;
	for (const auto& pd : parsed)
	{
		if (pd.isEmpty()) continue;
		const uint32 base = static_cast<uint32>(md.vertices.size());
		md.vertices.insert(md.vertices.end(), pd.vertices.begin(), pd.vertices.end());
		for (const auto& t : pd.indices)
			md.indices << TriangleIndex32{ base + t.i0, base + t.i1, base + t.i2 };
	}
	return md;
}

namespace
{
	/// @brief ノード nodeId における、エッジ incomingEdgeId の外向き接線角度を返す
	Optional<float> incomingTangentAngle(const RoadNetwork& network, int nodeId, int incomingEdgeId)
	{
		const RoadEdge* edge = network.getEdge(incomingEdgeId);
		if (!edge) return none;
		const auto bez = network.getBezier(incomingEdgeId);
		if (!bez) return none;
		const bool atB = (edge->nodeB == nodeId);
		const Vec3 tOut = atB
			? -bez->tangentAt(bez->totalLength)
			:  bez->tangentAt(0.0f);
		return static_cast<float>(Math::Atan2(tOut.x, tOut.z));
	}

	/// @brief Dijkstra で nodeEndId の反対側から名称付き目的地を最大 maxCount 件収集する
	/// @return (地名, 距離km) のペア配列（tier / 距離でソート済み）
	Array<std::pair<String, float>> resolveDestinations(
		const RoadNetwork& network, int edgeId, int nodeEndId, int maxCount)
	{
		Array<std::pair<String, float>> out;
		if (maxCount <= 0) return out;

		const RoadEdge* startEdge = network.getEdge(edgeId);
		if (!startEdge) return out;

		const int farNode = (startEdge->nodeA == nodeEndId) ? startEdge->nodeB : startEdge->nodeA;
		if (farNode < 0) return out;

		const float initialDist_m = Max(0.0f, startEdge->length - GuideSign::kAutoPlaceArcOffset_m);

		struct Entry { float dist; int node; int depth; };
		struct Cmp { bool operator()(const Entry& a, const Entry& b) const { return a.dist > b.dist; } };
		std::priority_queue<Entry, std::vector<Entry>, Cmp> pq;

		HashTable<int, float> bestDist;
		bestDist[farNode] = initialDist_m;
		pq.push({ initialDist_m, farNode, 0 });

		struct Hit { String name; float km; uint8 tier; };
		Array<Hit> hits;
		HashSet<String> hitNames;

		while (!pq.empty())
		{
			const Entry e = pq.top(); pq.pop();
			if (e.dist > GuideSign::kMaxSearchDistance_km * 1000.0f) break;
			if (e.depth > GuideSign::kMaxSearchDepth) continue;
			if (auto it = bestDist.find(e.node); it != bestDist.end() && it->second < e.dist) continue;

			if (const String* name = network.getDestinationName(e.node))
			{
				if (!hitNames.contains(*name))
				{
					hitNames.insert(*name);
					hits << Hit{ *name, e.dist / 1000.0f, network.getDestinationTier(e.node) };
					if (static_cast<int>(hits.size()) >= maxCount) break;
				}
			}

			const RoadNode* node = network.getNode(e.node);
			if (!node) continue;
			for (const auto& att : node->attachments)
			{
				if (e.depth == 0 && att.edgeId == edgeId) continue;
				const RoadEdge* ne = network.getEdge(att.edgeId);
				if (!ne || ne->id < 0) continue;
				const int nextNode = (ne->nodeA == e.node) ? ne->nodeB : ne->nodeA;
				if (nextNode < 0) continue;
				const float nextDist = e.dist + ne->length;
				if (auto it = bestDist.find(nextNode); it != bestDist.end() && it->second <= nextDist)
					continue;
				bestDist[nextNode] = nextDist;
				pq.push({ nextDist, nextNode, e.depth + 1 });
			}
		}

		hits.sort_by([](const Hit& a, const Hit& b) {
			if (std::abs(a.km - b.km) < 2.0f) return a.tier < b.tier;
			return a.km < b.km;
		});

		for (const auto& h : hits)
		{
			if (static_cast<int>(out.size()) >= maxCount) break;
			out << std::make_pair(h.name, h.km);
		}
		return out;
	}

	/// @brief 指定ノードから viaEdgeId 方向に進んだ先の最近接名称付き目的地を返す
	Optional<std::pair<String, float>> resolveNearestDestinationVia(
		const RoadNetwork& network, int nodeId, int viaEdgeId)
	{
		const RoadEdge* via = network.getEdge(viaEdgeId);
		if (!via) return none;
		const int farNode = (via->nodeA == nodeId) ? via->nodeB : via->nodeA;
		if (farNode < 0) return none;

		struct Entry { float dist; int node; int depth; };
		struct Cmp { bool operator()(const Entry& a, const Entry& b) const { return a.dist > b.dist; } };
		std::priority_queue<Entry, std::vector<Entry>, Cmp> pq;

		HashTable<int, float> bestDist;
		bestDist[farNode] = via->length;
		pq.push({ via->length, farNode, 0 });

		while (!pq.empty())
		{
			const Entry e = pq.top(); pq.pop();
			if (e.dist > GuideSign::kMaxSearchDistance_km * 1000.0f) break;
			if (e.depth > GuideSign::kMaxSearchDepth) continue;
			if (auto it = bestDist.find(e.node); it != bestDist.end() && it->second < e.dist) continue;

			if (const String* name = network.getDestinationName(e.node))
				return std::make_pair(*name, e.dist / 1000.0f);

			const RoadNode* node = network.getNode(e.node);
			if (!node) continue;
			for (const auto& att : node->attachments)
			{
				if (e.depth == 0 && att.edgeId == viaEdgeId) continue;
				const RoadEdge* ne = network.getEdge(att.edgeId);
				if (!ne || ne->id < 0) continue;
				const int nextNode = (ne->nodeA == e.node) ? ne->nodeB : ne->nodeA;
				if (nextNode < 0) continue;
				const float nextDist = e.dist + ne->length;
				if (auto it = bestDist.find(nextNode); it != bestDist.end() && it->second <= nextDist)
					continue;
				bestDist[nextNode] = nextDist;
				pq.push({ nextDist, nextNode, e.depth + 1 });
			}
		}
		return none;
	}

	/// @brief DD 用 elements を生成する（(地名, km) ペア配列から）
	Array<SignElement> buildDD_Elements(const Array<std::pair<String, float>>& dests)
	{
		namespace L = GuideSignLayout::DD;
		Array<SignElement> out;

		SignElement arrow;
		arrow.kind        = SignElementKind::Arrow;
		arrow.posX        = L::ArrowColRatio * L::ArrowCxFactor;
		arrow.posY        = 0.50f;
		arrow.arrowLength = 1.6f;
		arrow.arrowAngle  = 0.0f;
		out << arrow;

		const float rightX = arrow.posX + L::ArrowColRatio * L::ArrowCxFactor + L::TextLeftOffset;
		const int rows = static_cast<int>(dests.size());
		for (int i = 0; i < rows; ++i)
		{
			SignElement txt;
			txt.kind    = SignElementKind::Text;
			txt.posX    = rightX + 0.10f;
			txt.posY    = (i + 0.5f) / rows;
			txt.scale   = 1.0f;
			txt.text    = (dests[i].second > 0)
			              ? U"{} {:.0f}km"_fmt(dests[i].first, dests[i].second)
			              : dests[i].first;
			out << txt;
		}
		return out;
	}

	/// @brief DA 用 elements を生成する（方向付きの目的地から）
	/// @param arms (地名, TurnType) のペア配列（左・直進・右 の順でソート済みを想定）
	Array<SignElement> buildDA_Elements(
		const Array<std::pair<String, TurnType>>& arms)
	{
		namespace L = GuideSignLayout::DA;
		Array<SignElement> out;

		// 中央↑
		SignElement upArrow;
		upArrow.kind        = SignElementKind::Arrow;
		upArrow.posX        = 0.50f;
		upArrow.posY        = (L::ArrowTopY + L::ArrowBotY) * 0.5f;
		upArrow.arrowLength = 2.2f;
		upArrow.arrowAngle  = 0.0f;
		out << upArrow;

		const float leftCx  = L::LeftArmEndX * 0.5f;
		const float rightCx = 1.0f - L::RightArmEndX * 0.5f;

		for (const auto& [name, turn] : arms)
		{
			if (turn == TurnType::Straight)
			{
				SignElement t; t.kind = SignElementKind::Text;
				t.posX = 0.50f; t.posY = L::UpTextCy;
				t.text = name;
				out << t;
			}
			else if (turn == TurnType::Left)
			{
				SignElement a; a.kind = SignElementKind::Arrow;
				a.posX = L::LeftArmEndX + 0.05f; a.posY = L::ArmCy;
				a.arrowAngle = 270.0f; a.arrowLength = 1.2f;
				out << a;
				SignElement t; t.kind = SignElementKind::Text;
				t.posX = leftCx; t.posY = L::ArmCy - 0.055f;
				t.text = name;
				out << t;
			}
			else if (turn == TurnType::Right)
			{
				SignElement a; a.kind = SignElementKind::Arrow;
				a.posX = (1.0f - L::RightArmEndX) - 0.05f; a.posY = L::ArmCy;
				a.arrowAngle = 90.0f; a.arrowLength = 1.2f;
				out << a;
				SignElement t; t.kind = SignElementKind::Text;
				t.posX = rightCx; t.posY = L::ArmCy - 0.055f;
				t.text = name;
				out << t;
			}
		}
		return out;
	}
}

Array<GuideSignPlacement> GuideSign::InferAutoForEdge(const RoadEdge& edge, const RoadNetwork& network)
{
	Array<GuideSignPlacement> out;

	const auto rb = RoadSign::roadbedExtentsOf(edge);
	const float leftLateral  = rb.left  - kSideMargin_m;
	const float rightLateral = rb.right + kSideMargin_m;

	// ─── 106: 方面及び距離 ───
	if (edge.length >= kAutoPlaceMinEdgeLen_m)
	{
		const auto place106 = [&](int nodeEndId)
		{
			if (nodeEndId < 0) return;
			const auto dests = resolveDestinations(network, edge.id, nodeEndId, kMaxEntriesPerPanel);
			if (dests.isEmpty()) return;

			GuideSignPlacement gp;
			gp.kind          = GuideSignKind::DirectionDistance;
			gp.nodeEndId     = nodeEndId;
			gp.arcOffset     = kAutoPlaceArcOffset_m;
			// 日本の左側通行: A→B 進行車は左側（leftLateral）に配置
			gp.lateralOffset = (nodeEndId == edge.nodeA) ? leftLateral : rightLateral;
			gp.poleHeight    = kPoleHeight_m;
			gp.autoGenerated = true;
			gp.elements      = buildDD_Elements(dests);
			out << gp;
		};
		place106(edge.nodeA);
		place106(edge.nodeB);
	}

	// ─── 108の2: 方面及び方向 ───
	if (edge.length >= kArrowPlaceMinEdgeLen_m)
	{
		const auto place108 = [&](int nodeEndId)
		{
			if (nodeEndId < 0) return;
			const RoadNode* node = network.getNode(nodeEndId);
			if (!node) return;
			if (static_cast<int>(node->attachments.size()) < kArrowMinAttachments) return;

			const auto inAngleOpt = incomingTangentAngle(network, nodeEndId, edge.id);
			if (!inAngleOpt) return;
			const float driveAngle = *inAngleOpt + static_cast<float>(Math::Pi);

			Array<std::pair<String, TurnType>> arms;
			for (const auto& att : node->attachments)
			{
				if (att.edgeId == edge.id) continue;
				const auto outAngleOpt = incomingTangentAngle(network, nodeEndId, att.edgeId);
				if (!outAngleOpt) continue;
				const TurnType turn = TrafficCommon::classifyTurnByAngles(driveAngle, *outAngleOpt);
				if (turn == TurnType::UTurn) continue;
				const auto dest = resolveNearestDestinationVia(network, nodeEndId, att.edgeId);
				if (!dest) continue;
				arms << std::make_pair(dest->first, turn);
				if (static_cast<int>(arms.size()) >= kMaxEntriesPerPanel) break;
			}
			if (arms.size() < 2) return;

			// 左・直進・右 の順にソート
			arms.sort_by([](const std::pair<String, TurnType>& a, const std::pair<String, TurnType>& b) {
				constexpr int order[] = { 1, 0, 2, 3 };  // Straight=1, Left=0, Right=2, UTurn=3
				return order[static_cast<int>(a.second)] < order[static_cast<int>(b.second)];
			});

			GuideSignPlacement gp;
			gp.kind          = GuideSignKind::DirectionArrow;
			gp.nodeEndId     = nodeEndId;
			gp.arcOffset     = Min(kArrowPlaceArcOffset_m, edge.length * 0.7f);
			gp.lateralOffset = (nodeEndId == edge.nodeA) ? leftLateral : rightLateral;
			gp.poleHeight    = kPoleHeight_m;
			gp.autoGenerated = true;
			gp.elements      = buildDA_Elements(arms);
			out << gp;
		};
		place108(edge.nodeA);
		place108(edge.nodeB);
	}

	return out;
}

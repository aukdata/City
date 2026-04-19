#include "GuideSign.hpp"
#include "RoadNetwork.hpp"
#include "RoadSign.hpp"
#include "ObjParser.hpp"
#include "../traffic/TrafficCommon.hpp"
#include "../asset/AssetRegistrar.hpp"
#include <queue>

namespace
{
	// ===== 共通定数 =====

	/// @brief 案内標識の既定背景色（道路標識青）
	constexpr ColorF kDefaultBgColor{ 0.07, 0.28, 0.66, 1.0 };

	/// @brief 看板テクスチャの解像度（リファレンス準拠の高解像度）
	constexpr double kPxPerM = 180.0;

	/// @brief 板厚（裏面描画用、視覚的にはほぼ平面）
	constexpr double kBoardThickness_m = 0.04;

	/// @brief bgColor.a > kBgColorAlphaEps なら指定色、それ以外はデフォルト青
	constexpr double kBgColorAlphaEps = 0.001;

	ColorF resolveBgColor(const ColorF& specified)
	{
		return (specified.a > kBgColorAlphaEps) ? specified : kDefaultBgColor;
	}

	// ===== Vertex 構築 =====

	Vertex3D makeBoardVertex(double x, double y, double z, float u, float v, double nz)
	{
		Vertex3D vt;
		vt.pos    = Float3{ static_cast<float>(x), static_cast<float>(y), static_cast<float>(z) };
		vt.normal = Float3{ 0.0f, 0.0f, static_cast<float>(nz) };
		vt.tex    = Float2{ u, v };
		return vt;
	}

}

GuideSign::BoardSize GuideSign::computeBoardSizeFor(const GuideSignPlacement& g)
{
	BoardSize boardSize;
	if (g.kind == GuideSignKind::DirectionArrow)
	{
		// 十字レイアウト（ref1 準拠）: 3.5m × 3.0m を基準に最長地名長で拡張
		int maxLen = 3;
		for (const auto& element : g.elements)
		{
			if (element.kind == SignElementKind::Text)
			{
				maxLen = Max(maxLen, static_cast<int>(element.text.size()));
			}
		}
		boardSize.width  = Max(3.5, maxLen * 0.5 * 2 + 1.5);
		boardSize.height = 3.0;
	}
	else
	{
		int rows = 0;
		int maxNameLen = 3;
		for (const auto& element : g.elements)
		{
			if (element.kind == SignElementKind::DestName)
			{
				++rows;
				maxNameLen = Max(maxNameLen, static_cast<int>(element.text.size()));
			}
		}
		rows = Max(1, rows);
		const double height    = GuideSignLayout::DirectionDistance::PaddingM * 2.0 + GuideSignLayout::DirectionDistance::RowHeightM * rows;
		const double textAreaW = maxNameLen * kBoardCharWidth_m + 1.5;
		const double width     = Max(kBoardMinWidth_m, textAreaW / (1.0 - GuideSignLayout::DirectionDistance::ArrowColRatio));
		boardSize = { width, height };
	}
	if (g.widthOverride > 0.0f)
	{
		boardSize.width = g.widthOverride;
	}
	if (g.heightOverride > 0.0f)
	{
		boardSize.height = g.heightOverride;
	}
	return boardSize;
}

MeshData GuideSign::CreateBoardMesh(double width, double height)
{
	const double hx = width  * 0.5;
	const double hy = height * 0.5;

	MeshData md;
	md.vertices.reserve(4);
	md.indices.reserve(2);

	// 片面メッシュ（z=0 平面）。裏面は RoadRenderer の CullFront パスで灰色描画される。
	// computeSignTransforms が RotateY(yaw+π) を適用するため、driver 視点で
	// local +X が左に来る。よって U=0 を +hx、U=1 を -hx に割当てる。
	md.vertices << makeBoardVertex( hx,  hy, 0.0, 0.0f, 0.0f,  1.0);
	md.vertices << makeBoardVertex(-hx,  hy, 0.0, 1.0f, 0.0f,  1.0);
	md.vertices << makeBoardVertex(-hx, -hy, 0.0, 1.0f, 1.0f,  1.0);
	md.vertices << makeBoardVertex( hx, -hy, 0.0, 0.0f, 1.0f,  1.0);
	md.indices << TriangleIndex32{ 0, 1, 2 };
	md.indices << TriangleIndex32{ 0, 2, 3 };

	return md;
}

namespace
{
	GuideSign::PoleMetadata g_guidePoleMetadata;
	bool                    g_guidePoleMetadataLoaded = false;
}

const GuideSign::PoleMetadata& GuideSign::poleMetadata()
{
	if (g_guidePoleMetadataLoaded)
	{
		return g_guidePoleMetadata;
	}
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
	return g_guidePoleMetadata;
}

void GuideSign::reloadPoleMetadata()
{
	g_guidePoleMetadataLoaded = false;
}


Size GuideSign::guideSignTexSize(double widthM, double heightM)
{
	const int w = Max(256, static_cast<int>(std::ceil(widthM  * kPxPerM)));
	const int h = Max(96,  static_cast<int>(std::ceil(heightM * kPxPerM)));
	return Size{ w, h };
}


namespace
{
	/// @brief 矢印ポリゴンを描く
	/// @param x0, y0   始点（シャフト末端）[px]
	/// @param length   全長 [px]（シャフト＋矢頭）
	/// @param angleDeg 先端方向 [度] : 0=上, 90=右, 180=下, 270=左
	void drawArrow(double x0, double y0, double length, double angleDeg, const ColorF& col)
	{
		const double shaftHalf = GuideSign::kArrowShaftWidth * 0.5;
		const double headHalf  = GuideSign::kArrowHeadWidth  * 0.5;
		const double shaftLen  = Max(0.0, length - GuideSign::kArrowHeadHeight);

		// ローカル座標: y=0 が始点、y=-length が矢頭先端（angle=0 で上向き）
		Array<Vec2> pts = {
			{ -shaftHalf,  0.0      },
			{ -shaftHalf, -shaftLen },
			{ -headHalf,  -shaftLen },
			{  0.0,       -length   },
			{ +headHalf,  -shaftLen },
			{ +shaftHalf, -shaftLen },
			{ +shaftHalf,  0.0      },
		};

		const Mat3x2 mat = Mat3x2::Rotate(Math::ToRadians(angleDeg)).translated(x0, y0);
		for (auto& p : pts) p = mat.transformPoint(p);
		Polygon{ pts }.draw(col);
	}

	/// @brief 矢頭なしシャフトを描く（丁字路の直進軸用）
	/// @param x0, y0   始点（シャフト末端）[px]
	/// @param length   全長 [px]
	/// @param angleDeg 先端方向 [度]
	void drawArrowNoHead(double x0, double y0, double length, double angleDeg, const ColorF& col)
	{
		const double shaftHalf = GuideSign::kArrowShaftWidth * 0.5;

		Array<Vec2> pts = {
			{ -shaftHalf,  0.0     },
			{ -shaftHalf, -length  },
			{ +shaftHalf, -length  },
			{ +shaftHalf,  0.0     },
		};

		const Mat3x2 mat = Mat3x2::Rotate(Math::ToRadians(angleDeg)).translated(x0, y0);
		for (auto& p : pts) p = mat.transformPoint(p);
		Polygon{ pts }.draw(col);
	}

	/// @brief 案内標識の統一レンダラ（DirectionDistance / DirectionArrow 共通）
	void renderSign(const GuideSignPlacement& g, const Size& texSize,
	                const Font& fontJa, const Font& fontNum)
	{
		// 板背景 + 白枠（両種別共通）
		Rect{ 0, 0, texSize }.draw(resolveBgColor(g.bgColor));
		const double borderPx = Max(2.0, texSize.y * 0.015);
		const double insetPx  = borderPx * 2.0;
		RectF{ insetPx, insetPx, texSize.x - insetPx * 2, texSize.y - insetPx * 2 }
			.drawFrame(borderPx, 0.0, ColorF{ 1, 1, 1 });
		constexpr ColorF fg{ 1, 1, 1 };

		for (const auto& el : g.elements)
		{
			const double cx = el.posX * texSize.x;
			const double cy = el.posY * texSize.y;

			if (el.kind == SignElementKind::Arrow)
			{
				drawArrow(cx, cy, el.arrowLength, el.arrowAngle, fg);
			}
			else if (el.kind == SignElementKind::ArrowNoHead)
			{
				drawArrowNoHead(cx, cy, el.arrowLength, el.arrowAngle, fg);
			}
			else if (el.kind == SignElementKind::Text)
			{
				const double th = GuideSignLayout::DirectionArrow::UpTextH * texSize.y * el.scale;
				fontJa(el.text).drawAt(th, Vec2{ cx, cy }, fg);
				if (g.showReading && !el.reading.isEmpty())
				{
					fontNum(el.reading).drawAt(th * 0.42, Vec2{ cx, cy + th * 0.68 }, fg);
				}
			}
			else if (el.kind == SignElementKind::DestName)
			{
				const double th = el.scale * texSize.y;
				fontJa(el.text).draw(th, Arg::leftCenter = Vec2{ cx, cy }, fg);
				if (g.showReading && !el.reading.isEmpty())
				{
					fontNum(el.reading).draw(th * 0.42, Arg::leftCenter = Vec2{ cx, cy + th * 0.65 }, fg);
				}
			}
			else if (el.kind == SignElementKind::DestDistance)
			{
				if (el.value > 0.0f)
				{
					const double kmH      = el.scale * texSize.y * (44.0 / 52.0);
					const double kmLabelH = kmH * 0.58;
					const String numStr   = (el.value < 1.0f)
						? U"{:.1f}"_fmt(el.value)
						: U"{:.0f}"_fmt(el.value);
					const double numW     = fontNum(numStr).region(kmH).w;
					const double kmLabelW = fontNum(U"km").region(kmLabelH).w;
					const double gap      = kmH * 0.12;
					const double kmX      = cx - kmLabelW;
					const double numX     = kmX - gap - numW;
					fontNum(numStr).draw(kmH,     Arg::leftCenter = Vec2{ numX, cy               }, fg);
					fontNum(U"km").draw(kmLabelH, Arg::leftCenter = Vec2{ kmX,  cy + kmH * 0.06  }, fg);
				}
			}
			else if (el.kind == SignElementKind::RouteNumber)
			{
				const Texture& iconTex = TextureAsset(Asset::NationalRoadSign);
				if (iconTex)
				{
					const double iconH = texSize.y * el.scale;
					iconTex.resized(iconH).drawAt(Vec2{ cx, cy });
					const double numH = iconH * 0.36;
					fontNum(static_cast<int>(el.value)).drawAt(numH, Vec2{ cx, cy - iconH * 0.02 }, fg);
				}
			}
		}
	}

	void renderEmptyPlaceholder(const GuideSignPlacement& g, const Size& texSize, const Font& fontJa)
	{
		const ColorF bg = resolveBgColor(g.bgColor);
		Rect{ 0, 0, texSize }.draw(bg);
		const double borderPx = Max(2.0, texSize.y * 0.015);
		RectF{ borderPx * 2, borderPx * 2, texSize.x - borderPx * 4, texSize.y - borderPx * 4 }
			.drawFrame(borderPx, 0.0, ColorF{ 1.0 });
		fontJa(U"(空)").drawAt(texSize.y * 0.3, Vec2{ texSize.x * 0.5, texSize.y * 0.5 }, ColorF{ 1.0 });
	}
}

void GuideSign::renderContents(const GuideSignPlacement& g, const Size& texSize,
                                const Font& fontJa, const Font& fontNum)
{
	if (g.elements.isEmpty())
	{
		renderEmptyPlaceholder(g, texSize, fontJa);
		return;
	}
	renderSign(g, texSize, fontJa, fontNum);
}

MeshData GuideSign::CreatePoleMesh()
{
	// guide_pole.obj は複数オブジェクトに分かれているため、単一 MeshData にマージする
	const auto parsedParts = ObjParser::parse(U"assets/signs/guide/guide_pole.obj");
	MeshData md;
	for (const auto& part : parsedParts)
	{
		if (part.isEmpty())
		{
			continue;
		}
		const uint32 base = static_cast<uint32>(md.vertices.size());
		md.vertices.insert(md.vertices.end(), part.vertices.begin(), part.vertices.end());
		for (const auto& tri : part.indices)
		{
			md.indices << TriangleIndex32{ base + tri.i0, base + tri.i1, base + tri.i2 };
		}
	}
	return md;
}

namespace
{
	/// @brief ノード nodeId における、エッジ incomingEdgeId の外向き接線角度を返す
	Optional<float> incomingTangentAngle(const RoadNetwork& network, int nodeId, int incomingEdgeId)
	{
		const RoadEdge* edge = network.getEdge(incomingEdgeId);
		if (!edge)
		{
			return none;
		}
		const auto bezier = network.getBezier(incomingEdgeId);
		if (!bezier)
		{
			return none;
		}
		const bool atB = (edge->nodeB == nodeId);
		const Vec3 tangentOut = atB
			? -bezier->tangentAt(bezier->totalLength)
			:  bezier->tangentAt(0.0f);
		return static_cast<float>(Math::Atan2(tangentOut.x, tangentOut.z));
	}

	/// @brief 目的地情報（距離・読み仮名・階層）
	struct Dest { String name; String reading; float km; uint8 tier; };

	// ===== Dijkstra 共通基盤 =====

	struct DijkstraEntry { float dist; int node; int depth; };
	struct DijkstraGreater
	{
		bool operator()(const DijkstraEntry& a, const DijkstraEntry& b) const { return a.dist > b.dist; }
	};
	using DijkstraQueue = std::priority_queue<DijkstraEntry, std::vector<DijkstraEntry>, DijkstraGreater>;

	/// @brief ノードに到達した目的地として Dest を構築する（名称未登録なら none）
	Optional<Dest> makeDestAt(const RoadNetwork& network, int node, float dist_m)
	{
		const String* name = network.getDestinationName(node);
		if (!name)
		{
			return none;
		}
		const String* reading = network.getDestinationReading(node);
		return Dest{ *name, reading ? *reading : U"", dist_m / 1000.0f, network.getDestinationTier(node) };
	}

	/// @brief Dijkstra の隣接展開を行う（探索打ち切り条件は呼び出し側で判定）
	/// @param skipEdgeIfDepthZero  深度0で除外するエッジ ID（探索開始エッジを再展開しないため）
	void expandNeighbors(const RoadNetwork& network, const DijkstraEntry& current,
	                     int skipEdgeIfDepthZero,
	                     HashTable<int, float>& bestDist, DijkstraQueue& queue)
	{
		const RoadNode* node = network.getNode(current.node);
		if (!node)
		{
			return;
		}
		for (const auto& attachment : node->attachments)
		{
			if (current.depth == 0 && attachment.edgeId == skipEdgeIfDepthZero)
			{
				continue;
			}
			const RoadEdge* nextEdge = network.getEdge(attachment.edgeId);
			if (!nextEdge || nextEdge->id < 0)
			{
				continue;
			}
			const int nextNode = (nextEdge->nodeA == current.node) ? nextEdge->nodeB : nextEdge->nodeA;
			if (nextNode < 0)
			{
				continue;
			}
			const float nextDist = current.dist + nextEdge->length;
			if (auto it = bestDist.find(nextNode); it != bestDist.end() && it->second <= nextDist)
			{
				continue;
			}
			bestDist[nextNode] = nextDist;
			queue.push({ nextDist, nextNode, current.depth + 1 });
		}
	}

	/// @brief Dijkstra で nodeEndId の反対側から名称付き目的地を最大 maxCount 件収集する
	/// @param initialDist_m 看板位置から farNode までの距離（表示距離の起点）
	Array<Dest> resolveDestinations(
		const RoadNetwork& network, int edgeId, int nodeEndId, int maxCount, float initialDist_m)
	{
		Array<Dest> out;
		if (maxCount <= 0)
		{
			return out;
		}

		const RoadEdge* startEdge = network.getEdge(edgeId);
		if (!startEdge)
		{
			return out;
		}

		const int farNode = (startEdge->nodeA == nodeEndId) ? startEdge->nodeB : startEdge->nodeA;
		if (farNode < 0)
		{
			return out;
		}

		HashTable<int, float> bestDist;
		bestDist[farNode] = initialDist_m;
		DijkstraQueue queue;
		queue.push({ initialDist_m, farNode, 0 });

		Array<Dest> hits;
		HashSet<String> hitNames;

		while (!queue.empty())
		{
			const DijkstraEntry entry = queue.top();
			queue.pop();
			if (entry.dist > GuideSign::kMaxSearchDistance_km * 1000.0f)
			{
				break;
			}
			if (entry.depth > GuideSign::kMaxSearchDepth)
			{
				continue;
			}
			if (auto it = bestDist.find(entry.node); it != bestDist.end() && it->second < entry.dist)
			{
				continue;
			}

			if (auto dest = makeDestAt(network, entry.node, entry.dist))
			{
				if (!hitNames.contains(dest->name))
				{
					hitNames.insert(dest->name);
					hits << *dest;
					if (static_cast<int>(hits.size()) >= maxCount)
					{
						break;
					}
				}
			}

			expandNeighbors(network, entry, edgeId, bestDist, queue);
		}

		// 遠い順（上が遠い）。距離差が小さい場合は階層（重要度）優先
		hits.sort_by([](const Dest& a, const Dest& b) {
			if (std::abs(a.km - b.km) < 2.0f)
			{
				return a.tier < b.tier;
			}
			return a.km > b.km;
		});

		for (const auto& h : hits)
		{
			if (static_cast<int>(out.size()) >= maxCount)
			{
				break;
			}
			out << h;
		}
		return out;
	}

	/// @brief 指定ノードから viaEdgeId 方向に進んだ先の最近接名称付き目的地を返す
	Optional<Dest> resolveNearestDestinationVia(
		const RoadNetwork& network, int nodeId, int viaEdgeId)
	{
		const RoadEdge* via = network.getEdge(viaEdgeId);
		if (!via)
		{
			return none;
		}
		const int farNode = (via->nodeA == nodeId) ? via->nodeB : via->nodeA;
		if (farNode < 0)
		{
			return none;
		}

		HashTable<int, float> bestDist;
		bestDist[farNode] = via->length;
		DijkstraQueue queue;
		queue.push({ via->length, farNode, 0 });

		while (!queue.empty())
		{
			const DijkstraEntry entry = queue.top();
			queue.pop();
			if (entry.dist > GuideSign::kMaxSearchDistance_km * 1000.0f)
			{
				break;
			}
			if (entry.depth > GuideSign::kMaxSearchDepth)
			{
				continue;
			}
			if (auto it = bestDist.find(entry.node); it != bestDist.end() && it->second < entry.dist)
			{
				continue;
			}

			if (auto dest = makeDestAt(network, entry.node, entry.dist))
			{
				return dest;
			}

			expandNeighbors(network, entry, viaEdgeId, bestDist, queue);
		}
		return none;
	}

	/// @brief 国道番号アイコン要素を生成する（両標識種別で共通）
	SignElement makeRouteNumberElement(float posX, float posY, float scale, int routeNumber)
	{
		SignElement el;
		el.kind  = SignElementKind::RouteNumber;
		el.posX  = posX;
		el.posY  = posY;
		el.scale = scale;
		el.value = static_cast<float>(routeNumber);
		return el;
	}

	/// @brief DirectionDistance 用 elements を生成する（データ駆動: DestName + DestDistance）
	Array<SignElement> buildDirectionDistance_Elements(const Array<Dest>& dests, int routeNumber = 0)
	{
		Array<SignElement> out;

		const int destCount   = static_cast<int>(dests.size());
		const int layoutRows  = Max(1, destCount);  // 板サイズ計算は最低 1 行ぶん確保

		// 板サイズを computeBoardSizeFor / guideSignTexSize と同じ式で推定
		int maxNameLen = 3;
		for (const auto& d : dests)
			maxNameLen = Max(maxNameLen, static_cast<int>(d.name.size()));
		const double boardH    = GuideSignLayout::DirectionDistance::PaddingM * 2.0 + GuideSignLayout::DirectionDistance::RowHeightM * layoutRows;
		const double textAreaW = maxNameLen * GuideSign::kBoardCharWidth_m + 1.5;
		const double boardW    = Max(GuideSign::kBoardMinWidth_m, textAreaW / (1.0 - GuideSignLayout::DirectionDistance::ArrowColRatio));
		const double tsX       = Max(256.0, std::ceil(boardW  * kPxPerM));
		const double tsY       = Max(96.0,  std::ceil(boardH  * kPxPerM));

		const double borderPx  = Max(2.0, tsY * 0.015);
		const double insetPx   = borderPx * 2.0;
		const double interiorH = tsY - insetPx * 2.0;
		const double interiorW = tsX - insetPx * 2.0;
		const double arrowColW = interiorW * GuideSignLayout::DirectionDistance::ArrowColRatio;

		const double pxPerM    = interiorH / (GuideSignLayout::DirectionDistance::PaddingM * 2.0 + GuideSignLayout::DirectionDistance::RowHeightM * layoutRows);
		const double padPx     = GuideSignLayout::DirectionDistance::PaddingM * pxPerM;
		const double rowPx     = GuideSignLayout::DirectionDistance::RowHeightM * pxPerM;
		const double contentY0 = insetPx + padPx;
		const double contentH  = interiorH - 2.0 * padPx;
		const double nameH     = rowPx * 0.52;
		const double textLeft  = insetPx + arrowColW + padPx * 0.5;
		const double textRight = tsX - insetPx - padPx;

		// Arrow 要素（1個。中央↑）始点 = コンテンツ下端
		const float arrowPosX  = static_cast<float>((insetPx + arrowColW * 0.5) / tsX);
		const float arrowTailY = static_cast<float>((insetPx + padPx + contentH) / tsY);
		SignElement arrow;
		arrow.kind        = SignElementKind::Arrow;
		arrow.posX        = arrowPosX;
		arrow.posY        = arrowTailY;
		arrow.arrowAngle  = 0.0f;
		arrow.arrowLength = static_cast<float>(contentH);  // 全長 [px]
		out << arrow;

		// 国道番号アイコン（矢印の中央に重ねて表示）
		if (routeNumber > 0)
		{
			const float iconScale = static_cast<float>(arrowColW * 0.64 / tsY);
			const float iconCy    = static_cast<float>((insetPx + padPx + contentH * 0.5) / tsY);
			out << makeRouteNumberElement(arrowPosX, iconCy, iconScale, routeNumber);
		}

		for (int i = 0; i < destCount; ++i)
		{
			const double rowCy = contentY0 + rowPx * (i + 0.5);

			// DestName 要素
			SignElement nameEl;
			nameEl.kind    = SignElementKind::DestName;
			nameEl.posX    = static_cast<float>(textLeft / tsX);
			nameEl.posY    = static_cast<float>(rowCy / tsY);
			nameEl.scale   = static_cast<float>(nameH / tsY);
			nameEl.text    = dests[i].name;
			nameEl.reading = dests[i].reading;
			out << nameEl;

			// DestDistance 要素
			SignElement distEl;
			distEl.kind  = SignElementKind::DestDistance;
			distEl.posX  = static_cast<float>(textRight / tsX);
			distEl.posY  = static_cast<float>(rowCy / tsY);
			distEl.scale = static_cast<float>(nameH / tsY);
			distEl.text  = dests[i].name;  // 参照用
			distEl.value = dests[i].km;
			out << distEl;
		}
		return out;
	}

	struct Arm { String name; String reading; TurnType turn; int routeNumber = 0; };

	/// @brief DirectionArrow 用 elements を生成する
	Array<SignElement> buildDirectionArrow_Elements(const Array<Arm>& arms, int signRouteNumber = 0)
	{
		// computeBoardSizeFor + guideSignTexSize と同じ式でサイン寸法を推定する
		// （板上の正規化座標を決めるためにピクセル寸法が必要）
		int maxLen = 3;
		for (const auto& arm : arms)
		{
			maxLen = Max(maxLen, static_cast<int>(arm.name.size()));
		}
		const double tsX      = Max(256.0, std::ceil(Max(3.5, maxLen * 1.0 + 1.5) * kPxPerM));
		constexpr double tsY  = 540.0;
		const double insetPx  = Max(3.0, tsY * 0.02) * 1.8;
		const double upCx     = tsX * 0.5;
		const double laX      = insetPx + tsX * GuideSignLayout::DirectionArrow::LeftArmEndX;  // 左アーム先端 x [px]
		const double laTailX  = upCx - GuideSign::kArrowShaftWidth * 0.5;  // 左アーム始点 x [px]（シャフト左端）
		const double sideLen  = laTailX - laX;              // 側方アーム長 [px]

		// 直進矢印: 始点 = ArrowBotY（下端）、全長 [px]
		const float upArrowLen    = static_cast<float>((GuideSignLayout::DirectionArrow::ArrowBotY
		                          - GuideSignLayout::DirectionArrow::ArrowTopY) * tsY);
		// 側方アーム: 始点 = シャフト隣接端（laTailX）、全長 [px]
		const float leftArmTailX  = static_cast<float>(laTailX / tsX);
		const float rightArmTailX = 1.0f - leftArmTailX;
		// アイコン配置用（アームの中心 x）
		const float leftIconCx    = static_cast<float>((laX + laTailX) * 0.5 / tsX);
		const float rightIconCx   = 1.0f - leftIconCx;
		const float leftTextCx    = static_cast<float>((insetPx + laX) * 0.5 / tsX);
		const float rightTextCx   = 1.0f - leftTextCx;

		// 側方アーム用テキストの Y 座標（ArmCy より少し上に表示）
		constexpr float kSideTextDy = 0.055f;

		Array<SignElement> out;

		// 丁字路（直進アームなし）は上向きシャフトを矢頭なしにし、左右矢印の位置(ArmCy)までの長さに収める
		const bool hasThrough = arms.any([](const Arm& a){ return a.turn == TurnType::Straight; });

		SignElement upArrow;
		upArrow.kind       = hasThrough ? SignElementKind::Arrow : SignElementKind::ArrowNoHead;
		upArrow.posX       = 0.50f;
		upArrow.posY       = GuideSignLayout::DirectionArrow::ArrowBotY;  // 始点（下端）
		upArrow.arrowAngle = 0.0f;
		if (hasThrough)
		{
			upArrow.arrowLength = upArrowLen;
		}
		else
		{
			// 丁字路: ArmCy（側方アームと半シャフト分重なる）～ ArrowBotY
			upArrow.arrowLength = static_cast<float>(
				(GuideSignLayout::DirectionArrow::ArrowBotY - GuideSignLayout::DirectionArrow::ArmCy) * tsY
				+ GuideSign::kArrowShaftWidth * 0.5);
		}
		out << upArrow;

		// 国道アイコンは最後にまとめて追加するため一時バッファに収集する
		Array<SignElement> iconElements;

		// 国道上の看板：直進矢印の上から1/4の位置にアイコンを表示
		if (signRouteNumber > 0)
		{
			constexpr float botY = GuideSignLayout::DirectionArrow::ArrowBotY;
			constexpr float topY = GuideSignLayout::DirectionArrow::ArrowTopY;
			const float iconPosY = botY - (botY - topY) * 0.25f;
			iconElements << makeRouteNumberElement(0.50f, iconPosY, 0.165f, signRouteNumber);
		}

		// 側方アーム（左/右）の要素を生成する共通処理
		// tailX: 矢印始点（シャフト末端、中央シャフト側）、angle=270 で左向き / angle=90 で右向き
		const auto appendSideArm = [&](const Arm& arm, float tailX, float textCx, float arrowAngle)
		{
			SignElement a;
			a.kind        = SignElementKind::Arrow;
			a.posX        = tailX;
			a.posY        = GuideSignLayout::DirectionArrow::ArmCy;
			a.arrowAngle  = arrowAngle;
			a.arrowLength = static_cast<float>(sideLen);
			out << a;
			SignElement t;
			t.kind    = SignElementKind::Text;
			t.posX    = textCx;
			t.posY    = GuideSignLayout::DirectionArrow::ArmCy - kSideTextDy;
			t.text    = arm.name;
			t.reading = arm.reading;
			out << t;
		};

		for (const auto& arm : arms)
		{
			if (arm.turn == TurnType::Straight)
			{
				SignElement t;
				t.kind    = SignElementKind::Text;
				t.posX    = 0.50f;
				t.posY    = GuideSignLayout::DirectionArrow::UpTextCy;
				t.text    = arm.name;
				t.reading = arm.reading;
				out << t;
			}
			else if (arm.turn == TurnType::Left)
			{
				appendSideArm(arm, leftArmTailX, leftTextCx, 270.0f);
				if (signRouteNumber == 0 && arm.routeNumber > 0)
					iconElements << makeRouteNumberElement(leftIconCx, GuideSignLayout::DirectionArrow::ArmCy, 0.165f, arm.routeNumber);
			}
			else if (arm.turn == TurnType::Right)
			{
				appendSideArm(arm, rightArmTailX, rightTextCx, 90.0f);
				if (signRouteNumber == 0 && arm.routeNumber > 0)
					iconElements << makeRouteNumberElement(rightIconCx, GuideSignLayout::DirectionArrow::ArmCy, 0.165f, arm.routeNumber);
			}
		}

		// 国道アイコンを最後に追加（常に最前面に描画）
		out.insert(out.end(), iconElements.begin(), iconElements.end());
		return out;
	}

	// ─── チェーン探索 ───

	struct PlacementLocus
	{
		int   edgeId;
		int   nodeEndId;  ///< arcOffset の基準ノード（このノード端から計測）
		float arcOffset;
	};

	/// @brief startEdgeId の startNodeId 端から反対方向へ targetDist_m 進んだ位置を返す。
	/// @details エッジが足りない場合、直線的に続く隣接エッジを辿る（最大 maxHops 本まで）。
	///   交差点・行き止まり・ホップ上限に達した場合はその時点の末端位置を返す。
	PlacementLocus locateArcAlongChain(
		const RoadNetwork& network,
		int startEdgeId,
		int startNodeId,
		float targetDist_m,
		int maxHops)
	{
		int   curEdgeId    = startEdgeId;
		int   curStartNode = startNodeId;
		float remaining    = targetDist_m;

		for (int hop = 0; hop <= maxHops; ++hop)
		{
			const RoadEdge* edge = network.getEdge(curEdgeId);
			if (!edge)
			{
				return PlacementLocus{ curEdgeId, curStartNode, 0.0f };
			}

			if (remaining <= edge->length)
			{
				// このエッジ内に収まる
				return PlacementLocus{ curEdgeId, curStartNode, remaining };
			}

			// 直線的に続くエッジを選ぶ
			const int farNode = (curStartNode == edge->nodeA) ? edge->nodeB : edge->nodeA;
			const RoadNode* farNodePtr = network.getNode(farNode);
			if (!farNodePtr)
			{
				return PlacementLocus{ curEdgeId, curStartNode, edge->length };
			}

			const auto driveAngleOpt = incomingTangentAngle(network, farNode, curEdgeId);
			if (!driveAngleOpt)
			{
				return PlacementLocus{ curEdgeId, curStartNode, edge->length };
			}
			const float driveAngle = *driveAngleOpt + static_cast<float>(Math::Pi);

			int   bestEdgeId = -1;
			float bestDiff   = static_cast<float>(Math::Pi);

			for (const auto& att : farNodePtr->attachments)
			{
				if (att.edgeId == curEdgeId)
				{
					continue;
				}
				const RoadEdge* nextEdge = network.getEdge(att.edgeId);
				if (!nextEdge)
				{
					continue;
				}

				const auto outAngleOpt = incomingTangentAngle(network, farNode, att.edgeId);
				if (!outAngleOpt)
				{
					continue;
				}

				const TurnType turn = TrafficCommon::classifyTurnByAngles(driveAngle, *outAngleOpt);
				if (turn != TurnType::Straight)
				{
					continue;
				}

				// 角度差を [-π, π] に正規化
				float rawDiff = *outAngleOpt - driveAngle;
				rawDiff = std::fmod(rawDiff + static_cast<float>(Math::Pi), static_cast<float>(Math::TwoPi));
				if (rawDiff < 0.0f)
				{
					rawDiff += static_cast<float>(Math::TwoPi);
				}
				rawDiff -= static_cast<float>(Math::Pi);
				const float diff = std::abs(rawDiff);
				if (diff < bestDiff)
				{
					bestDiff   = diff;
					bestEdgeId = att.edgeId;
				}
			}

			// 直進エッジなし（交差点・行き止まり）またはホップ上限 → 現エッジ末端に配置
			if (bestEdgeId < 0 || hop == maxHops)
			{
				return PlacementLocus{ curEdgeId, curStartNode, edge->length };
			}

			remaining   -= edge->length;
			curStartNode = farNode;
			curEdgeId    = bestEdgeId;
		}

		return PlacementLocus{ curEdgeId, curStartNode, targetDist_m };
	}
}

Array<GuideSignPlacement> GuideSign::InferAutoForEdge(const RoadEdge& edge, const RoadNetwork& network)
{
	Array<GuideSignPlacement> out;

	// 国道の交差点かどうかを判定する（接続エッジのいずれかが国道に属していれば true）
	const auto isNationalRouteIntersection = [&](const RoadNode* node) -> bool
	{
		for (const auto& att : node->attachments)
		{
			const RoadEdge* e = network.getEdge(att.edgeId);
			if (!e) continue;
			for (int routeId : e->routeIds)
			{
				const RoadRoute* route = network.getRoute(routeId);
				if (route && route->kind == RoadRouteKind::NationalRoute)
					return true;
			}
		}
		return false;
	};

	// 国道の交差点ごとに 108の2（手前 300m）と 106（通過後 300m）を配置する
	const auto processIntersectionNode = [&](int nodeId)
	{
		if (nodeId < 0)
		{
			return;
		}
		const RoadNode* node = network.getNode(nodeId);
		if (!node)
		{
			return;
		}
		if (static_cast<int>(node->attachments.size()) < kArrowMinAttachments)
		{
			return;
		}
		if (!isNationalRouteIntersection(node))
		{
			return;
		}

		const int oppositeNodeId = (nodeId == edge.nodeA) ? edge.nodeB : edge.nodeA;
		if (oppositeNodeId < 0)
		{
			return;
		}

		// lateral 原則（locus エッジ基準）: nodeEndId==locusEdge.nodeA → left, nodeEndId==nodeB → right
		const auto lateralForLocus = [&](const RoadEdge* locusEdge, int locusNodeEndId) -> float {
			if (!locusEdge) return 0.0f;
			const auto rb = RoadSign::roadbedExtentsOf(*locusEdge);
			return (locusNodeEndId == locusEdge->nodeA)
				? rb.left  - kSideMargin_m
				: rb.right + kSideMargin_m;
		};

		// 国道番号を edge.routeIds から抽出する
		int routeNumber = 0;
		for (int routeId : edge.routeIds)
		{
			const RoadRoute* route = network.getRoute(routeId);
			if (route && route->kind == RoadRouteKind::NationalRoute && route->number > 0)
			{
				routeNumber = route->number;
				break;
			}
		}

		// 106 / 108の2 は同じ物理位置（nodeId から oppositeNodeId 方向へ 300m）に立つ。
		// 106 は outbound 向き（nodeEndId = locus.nodeEndId）、
		// 108の2 は inbound 向き（nodeEndId を反転して逆側から計測）。
		constexpr int kMaxHops = 2;
		const auto locus = locateArcAlongChain(network, edge.id, nodeId, kArrowPlaceArcOffset_m, kMaxHops);
		const RoadEdge* locusEdge = network.getEdge(locus.edgeId);

		// 108の2 用に nodeEndId と arcOffset を反転（同位置・逆向き）
		const int   nodeEndFor108 = locusEdge
			? ((locus.nodeEndId == locusEdge->nodeA) ? locusEdge->nodeB : locusEdge->nodeA)
			: locus.nodeEndId;
		const float arcFor108 = locusEdge ? locusEdge->length - locus.arcOffset : locus.arcOffset;

		// ─── 106: 方面及び距離（交差点通過後 300m） ───
		{
			const float initDist = locusEdge ? Max(0.0f, locusEdge->length - locus.arcOffset) : 0.0f;
			const auto dests = resolveDestinations(
				network, locus.edgeId, locus.nodeEndId, kMaxEntriesPerPanel, initDist);
			if (!dests.isEmpty())
			{
				GuideSignPlacement p;
				p.kind          = GuideSignKind::DirectionDistance;
				p.nodeEndId     = locus.nodeEndId;
				p.arcOffset     = locus.arcOffset;
				p.lateralOffset = lateralForLocus(locusEdge, locus.nodeEndId);
				p.poleHeight    = kPoleHeight_m;
				p.autoGenerated = true;
				p.sourceNodeId  = nodeId;
				p.parentEdgeId  = locus.edgeId;
				p.elements      = buildDirectionDistance_Elements(dests, routeNumber);
				out << p;
			}
		}

		// ─── 108の2: 方面及び方向（交差点 300m 手前） ───
		// 106 と同じ物理位置・逆向き（nodeEndId 反転）
		{
			const auto inAngleOpt = incomingTangentAngle(network, nodeId, edge.id);
			if (inAngleOpt)
			{
				const float driveAngle = *inAngleOpt + static_cast<float>(Math::Pi);

				Array<Arm> arms;
				for (const auto& attachment : node->attachments)
				{
					if (attachment.edgeId == edge.id)
					{
						continue;
					}
					const auto outAngleOpt = incomingTangentAngle(network, nodeId, attachment.edgeId);
					if (!outAngleOpt)
					{
						continue;
					}
					const TurnType turn = TrafficCommon::classifyTurnByAngles(driveAngle, *outAngleOpt);
					if (turn == TurnType::UTurn)
					{
						continue;
					}
					const auto dest = resolveNearestDestinationVia(network, nodeId, attachment.edgeId);
					if (!dest)
					{
						continue;
					}
					// 接続エッジの国道番号を取得
					int armRouteNum = 0;
					const RoadEdge* attEdge = network.getEdge(attachment.edgeId);
					if (attEdge) {
						for (int rId : attEdge->routeIds) {
							const RoadRoute* r = network.getRoute(rId);
							if (r && r->kind == RoadRouteKind::NationalRoute && r->number > 0)
							{ armRouteNum = r->number; break; }
						}
					}
					arms << Arm{ dest->name, dest->reading, turn, armRouteNum };
					if (static_cast<int>(arms.size()) >= kMaxEntriesPerPanel)
					{
						break;
					}
				}

				if (arms.size() >= 2)
				{
					arms.sort_by([](const Arm& a, const Arm& b) {
						constexpr int order[] = { 1, 0, 2, 3 };
						return order[static_cast<int>(a.turn)] < order[static_cast<int>(b.turn)];
					});

					GuideSignPlacement p;
					p.kind          = GuideSignKind::DirectionArrow;
					p.nodeEndId     = nodeEndFor108;
					p.arcOffset     = arcFor108;
					p.lateralOffset = lateralForLocus(locusEdge, nodeEndFor108);
					p.poleHeight    = kPoleHeight_m;
					p.autoGenerated = true;
					p.sourceNodeId  = nodeId;
					p.parentEdgeId  = locus.edgeId;
					p.elements      = buildDirectionArrow_Elements(arms, routeNumber);
					out << p;
				}
			}
		}
	};

	processIntersectionNode(edge.nodeA);
	processIntersectionNode(edge.nodeB);

	return out;
}

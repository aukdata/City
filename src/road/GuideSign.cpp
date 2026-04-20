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
	// 背景色定数 kDefaultBgColor / kBgColorAlphaEps / resolveBgColor() は
	// GuideSign.hpp に公開されている（RoadRenderer / GuideSignEditor からも参照するため）

	using GuideSign::resolveBgColor;

	/// @brief 看板テクスチャの解像度（リファレンス準拠の高解像度）
	constexpr double kPxPerM = 180.0;

	// ===== Vertex 構築 =====

	Vertex3D makeBoardVertex(double x, double y, double z, float u, float v)
	{
		Vertex3D vt;
		vt.pos    = Float3{ static_cast<float>(x), static_cast<float>(y), static_cast<float>(z) };
		vt.normal = Float3{ 0.0f, 0.0f, 1.0f };
		vt.tex    = Float2{ u, v };
		return vt;
	}

	/// @brief DirectionDistance 用の板寸法を計算する（rows / maxNameLen から）
	/// @details computeBoardSizeFor と buildDirectionDistance_Elements で共有
	GuideSign::BoardSize computeDirectionDistanceBoardSize(int rows, int maxNameLen)
	{
		const double height    = GuideSignLayout::DirectionDistance::PaddingM * 2.0
		                       + GuideSignLayout::DirectionDistance::RowHeightM * Max(1, rows);
		const double textAreaW = maxNameLen * GuideSign::kBoardCharWidth_m + 1.5;
		const double width     = Max(GuideSign::kBoardMinWidth_m,
		                             textAreaW / (1.0 - GuideSignLayout::DirectionDistance::ArrowColRatio));
		return { width, height };
	}

	/// @brief DirectionArrow 用の板寸法を計算する
	GuideSign::BoardSize computeDirectionArrowBoardSize(int maxNameLen)
	{
		return { Max(3.5, maxNameLen * 0.5 * 2 + 1.5), 3.0 };
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
		boardSize = computeDirectionArrowBoardSize(maxLen);
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
		boardSize = computeDirectionDistanceBoardSize(rows, maxNameLen);
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
	md.vertices << makeBoardVertex( hx,  hy, 0.0, 0.0f, 0.0f);
	md.vertices << makeBoardVertex(-hx,  hy, 0.0, 1.0f, 0.0f);
	md.vertices << makeBoardVertex(-hx, -hy, 0.0, 1.0f, 1.0f);
	md.vertices << makeBoardVertex( hx, -hy, 0.0, 0.0f, 1.0f);
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
	/// @details 板背景を全面塗りしてから白枠と elements を重ねる。
	///   3D 側で国道アイコン PNG（Mipped / Unorm 読み込み）と明るさを揃えるため、
	///   bg は SRGB 符号化の linear 入力として扱う（removeSRGBCurve しない）。
	void renderSign(const GuideSignPlacement& g, const Size& texSize,
	                const Font& fontJa, const Font& fontNum)
	{
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

	/// @brief 空要素時のプレースホルダ描画（板背景も含めて全て描画）
	void renderEmptyPlaceholder(const GuideSignPlacement& g, const Size& texSize, const Font& fontJa)
	{
		Rect{ 0, 0, texSize }.draw(resolveBgColor(g.bgColor));
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
		const auto   boardSize = computeDirectionDistanceBoardSize(layoutRows, maxNameLen);
		const Size   texSize   = GuideSign::guideSignTexSize(boardSize.width, boardSize.height);
		const double tsX       = static_cast<double>(texSize.x);
		const double tsY       = static_cast<double>(texSize.y);

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

	/// @brief DirectionArrow の幾何学的配置情報
	struct ArrowLayoutGeom
	{
		float armCy;            ///< 側方アーム Y 比（中心線）
		float leftArmTailX;     ///< 左アーム始点（シャフト隣接端） X 比
		float rightArmTailX;    ///< 右アーム始点 X 比
		float leftTextCx;       ///< 左アーム用テキスト X 比
		float rightTextCx;      ///< 右アーム用テキスト X 比
		float leftIconCx;       ///< 左アーム国道アイコン X 比
		float rightIconCx;      ///< 右アーム国道アイコン X 比
		float upArrowLength;    ///< 直進矢印の全長 [px]
		float sideArmLength;    ///< 側方アームの全長 [px]
		bool  hasThrough;       ///< 直進アームを持つ（= 有頭矢印）
	};

	/// @brief arms 内容から ArrowLayoutGeom を計算する
	ArrowLayoutGeom computeArrowLayout(const Array<Arm>& arms)
	{
		// computeBoardSizeFor + guideSignTexSize と同じ式でサイン寸法を推定する
		// （板上の正規化座標を決めるためにピクセル寸法が必要）
		int maxLen = 3;
		for (const auto& arm : arms)
		{
			maxLen = Max(maxLen, static_cast<int>(arm.name.size()));
		}
		const double tsX      = Max(256.0, std::ceil(Max(3.5, maxLen * 1.0 + 1.5) * kPxPerM));
		constexpr double tsY  = GuideSignLayout::DirectionArrow::ReferenceTexHeight;
		const double insetPx  = Max(3.0, tsY * 0.02) * 1.8;
		const double upCx     = tsX * 0.5;

		// 左アーム先端 x [px]
		const double laX      = insetPx + tsX * GuideSignLayout::DirectionArrow::LeftArmEndX;
		// 左アーム始点 x [px]（シャフト左端）
		const double laTailX  = upCx - GuideSign::kArrowShaftWidth * 0.5;
		// 左右矢印は基準長の 1.2 倍で描画する（見た目のボリューム感向上）
		const double sideLen  = (laTailX - laX) * GuideSignLayout::DirectionArrow::SideArmLengthScale;
		// 矢印の先端 x [px]（シャフト端から sideLen 分だけ外側）
		const double laTipX   = laTailX - sideLen;

		constexpr float kArrowBotY = GuideSignLayout::DirectionArrow::ArrowBotY;
		constexpr float kArrowTopY = GuideSignLayout::DirectionArrow::ArrowTopY;

		// 丁字路（直進アームなし）は上向きシャフトを矢頭なしにし、左右矢印の位置(ArmCy)までの長さに収める
		const bool hasThrough = arms.any([](const Arm& a){ return a.turn == TurnType::Straight; });

		ArrowLayoutGeom g;
		g.hasThrough = hasThrough;

		// 側方アームの Y 位置:
		//   直進あり: 直進矢印の下から 1/4（矢印下端側に寄せる）
		//   直進なし: 上から 1/4（丁字路）
		g.armCy = hasThrough
			? (kArrowBotY - (kArrowBotY - kArrowTopY) * 0.25f)
			: (kArrowTopY + (kArrowBotY - kArrowTopY) * 0.25f);

		g.leftArmTailX  = static_cast<float>(laTailX / tsX);
		g.rightArmTailX = 1.0f - g.leftArmTailX;
		g.leftIconCx    = static_cast<float>((laTipX + laTailX) * 0.5 / tsX);
		g.rightIconCx   = 1.0f - g.leftIconCx;
		// テキストは矢印の外側（先端より更に外側の余白）に配置する
		g.leftTextCx    = static_cast<float>((insetPx + laTipX) * 0.5 / tsX);
		g.rightTextCx   = 1.0f - g.leftTextCx;

		g.upArrowLength = static_cast<float>((kArrowBotY - kArrowTopY) * tsY);
		g.sideArmLength = static_cast<float>(sideLen);

		return g;
	}

	/// @brief 直進矢印（中央上向き）の要素を生成する
	SignElement makeUpArrowElement(const ArrowLayoutGeom& geom)
	{
		constexpr float kArrowBotY = GuideSignLayout::DirectionArrow::ArrowBotY;
		constexpr double tsY       = GuideSignLayout::DirectionArrow::ReferenceTexHeight;

		SignElement el;
		el.kind       = geom.hasThrough ? SignElementKind::Arrow : SignElementKind::ArrowNoHead;
		el.posX       = 0.50f;
		el.posY       = kArrowBotY;  // 始点（下端）
		el.arrowAngle = 0.0f;
		if (geom.hasThrough)
		{
			el.arrowLength = geom.upArrowLength;
		}
		else
		{
			// 丁字路: armCy（側方アームと半シャフト分重なる）～ ArrowBotY
			el.arrowLength = static_cast<float>(
				(kArrowBotY - geom.armCy) * tsY
				+ GuideSign::kArrowShaftWidth * 0.5);
		}
		return el;
	}

	/// @brief 側方アーム（矢印 + テキスト）を out に追加する
	/// @param tailX      矢印始点（シャフト末端、中央シャフト側）X 比
	/// @param textCx     テキスト中心 X 比
	/// @param arrowAngle 270=左向き / 90=右向き
	void appendSideArmElements(Array<SignElement>& out, const Arm& arm,
	                           const ArrowLayoutGeom& geom,
	                           float tailX, float textCx, float arrowAngle)
	{
		SignElement a;
		a.kind        = SignElementKind::Arrow;
		a.posX        = tailX;
		a.posY        = geom.armCy;
		a.arrowAngle  = arrowAngle;
		a.arrowLength = geom.sideArmLength;
		out << a;

		SignElement t;
		t.kind    = SignElementKind::Text;
		t.posX    = textCx;
		t.posY    = geom.armCy - GuideSignLayout::DirectionArrow::SideTextDy;
		t.text    = arm.name;
		t.reading = arm.reading;
		out << t;
	}

	/// @brief 直進アーム（中央上部のラベル）を out に追加する
	void appendStraightLabelElement(Array<SignElement>& out, const Arm& arm)
	{
		SignElement t;
		t.kind    = SignElementKind::Text;
		t.posX    = 0.50f;
		t.posY    = GuideSignLayout::DirectionArrow::UpTextCy;
		t.text    = arm.name;
		t.reading = arm.reading;
		out << t;
	}

	/// @brief 直進シャフト上に重ねる国道アイコン要素を生成する
	/// @details 直進あり: (ArrowTopY + ArrowBotY) / 2、丁字路: (armCy + ArrowBotY) / 2
	SignElement makeShaftRouteIconElement(const ArrowLayoutGeom& geom, int routeNumber)
	{
		constexpr float kArrowBotY = GuideSignLayout::DirectionArrow::ArrowBotY;
		constexpr float kArrowTopY = GuideSignLayout::DirectionArrow::ArrowTopY;
		const float shaftTopY = geom.hasThrough ? kArrowTopY : geom.armCy;
		const float iconPosY  = (shaftTopY + kArrowBotY) * 0.5f;
		return makeRouteNumberElement(0.50f, iconPosY,
		                              GuideSignLayout::DirectionArrow::RouteIconScale, routeNumber);
	}

	/// @brief DirectionArrow 用 elements を生成する
	Array<SignElement> buildDirectionArrow_Elements(const Array<Arm>& arms, int signRouteNumber = 0)
	{
		const ArrowLayoutGeom geom = computeArrowLayout(arms);

		Array<SignElement> out;
		out << makeUpArrowElement(geom);

		// 国道アイコンは最後にまとめて追加するため一時バッファに収集する（常に最前面）
		Array<SignElement> iconElements;

		if (signRouteNumber > 0)
		{
			iconElements << makeShaftRouteIconElement(geom, signRouteNumber);
		}

		for (const auto& arm : arms)
		{
			switch (arm.turn)
			{
			case TurnType::Straight:
				appendStraightLabelElement(out, arm);
				break;
			case TurnType::Left:
				appendSideArmElements(out, arm, geom, geom.leftArmTailX, geom.leftTextCx, 270.0f);
				if (signRouteNumber == 0 && arm.routeNumber > 0)
				{
					iconElements << makeRouteNumberElement(geom.leftIconCx, geom.armCy,
					                                       GuideSignLayout::DirectionArrow::RouteIconScale,
					                                       arm.routeNumber);
				}
				break;
			case TurnType::Right:
				appendSideArmElements(out, arm, geom, geom.rightArmTailX, geom.rightTextCx, 90.0f);
				if (signRouteNumber == 0 && arm.routeNumber > 0)
				{
					iconElements << makeRouteNumberElement(geom.rightIconCx, geom.armCy,
					                                       GuideSignLayout::DirectionArrow::RouteIconScale,
					                                       arm.routeNumber);
				}
				break;
			default:
				break;
			}
		}

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

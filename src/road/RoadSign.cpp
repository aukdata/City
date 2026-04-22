#include "RoadSign.hpp"
#include "RoadNetwork.hpp"
#include "ObjParser.hpp"
#include "../asset/AssetRegistrar.hpp"  // Asset::StopSign 等
#include "../traffic/TrafficCommon.hpp" // classifyTurnByAngles / TurnType

namespace
{
	/// @brief Vertex3D を生成する
	Vertex3D makeVert(double x, double y, double z, float u, float v,
	                double nx = 0.0, double ny = 1.0, double nz = 0.0)
	{
		Vertex3D vt;
		vt.pos    = Float3{ static_cast<float>(x), static_cast<float>(y), static_cast<float>(z) };
		vt.normal = Float3{ static_cast<float>(nx), static_cast<float>(ny), static_cast<float>(nz) };
		vt.tex    = Float2{ u, v };
		return vt;
	}

	/// @brief XZ 平面の半幅 r、高さ h の円柱を生成（底面 Y=0、上面 Y=h）
	MeshData makeCylinder(double r, double h, int segments = 12)
	{
		MeshData md;
		md.vertices.reserve((segments + 1) * 2);
		md.indices.reserve(segments * 2);

		// 側面
		for (int i = 0; i <= segments; ++i)
		{
			const double theta = (i / static_cast<double>(segments)) * Math::TwoPi;
			const double cx = Math::Cos(theta) * r;
			const double cz = Math::Sin(theta) * r;
			const float u = static_cast<float>(i) / segments;
			md.vertices << makeVert(cx, 0.0, cz, u, 0.0f, cx / r, 0.0, cz / r);
			md.vertices << makeVert(cx, h,   cz, u, 1.0f, cx / r, 0.0, cz / r);
		}

		for (int i = 0; i < segments; ++i)
		{
			const uint32 b = static_cast<uint32>(i * 2);
			md.indices << TriangleIndex32{ b,     b + 2, b + 1 };
			md.indices << TriangleIndex32{ b + 1, b + 2, b + 3 };
		}

		return md;
	}

	/// @brief OBJ を読み込んで複数オブジェクトを単一 MeshData に結合
	MeshData loadBoardObj(const FilePathView path)
	{
		const auto parsed = ObjParser::parse(path);
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
}

MeshData RoadSign::CreatePoleMesh(float poleHeight)
{
	return makeCylinder(kPoleRadius_m, static_cast<double>(poleHeight), 10);
}

namespace
{
	RoadSign::PoleMetadata g_poleMetadata;
	bool                   g_poleMetadataLoaded = false;
}

const RoadSign::PoleMetadata& RoadSign::poleMetadata()
{
	if (!g_poleMetadataLoaded)
	{
		const JSON j = JSON::Load(U"assets/signs/sign_pole.json");
		if (j)
		{
			g_poleMetadata.poleHeight = j[U"poleHeight"].getOr<float>(2.5f);
			const auto& b = j[U"board"];
			if (b)
			{
				g_poleMetadata.offsetX = b[U"offsetX"].getOr<float>(0.0f);
				g_poleMetadata.offsetY = b[U"offsetY"].getOr<float>(2.1f);
				g_poleMetadata.offsetZ = b[U"offsetZ"].getOr<float>(0.05f);
			}
		}
		g_poleMetadataLoaded = true;
	}
	return g_poleMetadata;
}

void RoadSign::reloadPoleMetadata()
{
	g_poleMetadataLoaded = false;
}

const RoadSign::SignVisual& RoadSign::visualOf(RoadSignType type)
{
	// 各種別の (category, 形状OBJ, テクスチャ) を一元管理。
	// 形状 OBJ は種別をまたいで共用可（例: Stop と Yield は inverted_triangle.obj）。
	static const SignVisual kNone{
		RoadSignCategory::Regulatory, U"", U"" };
	static const SignVisual kStop{
		RoadSignCategory::Regulatory,
		U"assets/signs/regulatory/inverted_triangle.obj",
		Asset::StopSign };
	static const SignVisual kYield{
		RoadSignCategory::Regulatory,
		U"assets/signs/regulatory/inverted_triangle.obj",
		Asset::YieldSign };
	static const SignVisual kNoEntry{
		RoadSignCategory::Regulatory,
		U"assets/signs/regulatory/circle.obj",
		Asset::NoEntrySign };
	static const SignVisual kDirectionalRestriction{
		RoadSignCategory::Regulatory,
		U"assets/signs/regulatory/circle.obj",
		Asset::DirectionalRestrictionSign };
	static const SignVisual kNationalRoute{
		RoadSignCategory::Guide,
		U"assets/signs/guide/onigiri.obj",
		U"" };  // テクスチャは号数合成（m_routeSignTexCache）で動的解決

	switch (type)
	{
	case RoadSignType::Stop:                   return kStop;
	case RoadSignType::Yield:                  return kYield;
	case RoadSignType::NoEntry:                return kNoEntry;
	case RoadSignType::DirectionalRestriction: return kDirectionalRestriction;
	case RoadSignType::NationalRoute:          return kNationalRoute;
	default:                                   return kNone;
	}
}

MeshData RoadSign::CreateBoardMesh(RoadSignType type)
{
	const auto& vis = visualOf(type);
	if (vis.shapeObjPath.isEmpty()) return {};
	return loadBoardObj(vis.shapeObjPath);
}

double RoadSign::BoardCenterFromPoleTop(RoadSignType type)
{
	// 国道おにぎりは板サイズの半分、その他（逆三角・円 等）は共通オフセット
	return (visualOf(type).category == RoadSignCategory::Guide)
		? kRouteBoardSize_m * 0.5
		: kBoardCenterFromPoleTop_m;
}

RoadSign::RoadbedExtents RoadSign::roadbedExtentsOf(const RoadEdge& edge)
{
	const float halfWidth = edge.totalWidth() * 0.5f;
	RoadbedExtents ext{ -halfWidth, halfWidth };
	bool found = false;
	for (const auto& part : edge.parts)
	{
		if (part.type != RoadPartType::Roadbed) continue;
		if (!found)
		{
			ext.left  = part.offsetL();
			ext.right = part.offsetR();
			found = true;
		}
		else
		{
			ext.left  = Min(ext.left,  part.offsetL());
			ext.right = Max(ext.right, part.offsetR());
		}
	}
	return ext;
}

Array<RoadSignPlacement> RoadSign::InferAutoForEdge(const RoadEdge& edge, const RoadNetwork& network)
{
	Array<RoadSignPlacement> out;

	const auto rb = roadbedExtentsOf(edge);
	const float leftLateral  = rb.left  - static_cast<float>(kSideMargin_m);  // 道路左端外側（負）
	const float rightLateral = rb.right + static_cast<float>(kSideMargin_m);  // 道路右端外側（正）

	// nodeA, nodeB それぞれについて attachment.control を見て自動生成
	const auto checkNode = [&](int nodeId) {
		const RoadNode* node = network.getNode(nodeId);
		if (!node) return;
		const EdgeAttachment* att = node->getAttachment(edge.id);
		if (!att) return;

		if (att->control == TrafficControl::Stop)
		{
			RoadSignPlacement sp;
			sp.type      = RoadSignType::Stop;
			sp.nodeEndId = nodeId;
			sp.arcOffset = 0.0f;  // cutoff 位置
			// driver の左側 = Roadbed 左端外側（左右が逆だったため反転 + Roadbed 基準）:
			//   nodeId == nodeB (driver A→B): driver 左 = 世界 -側 = leftLateral
			//   nodeId == nodeA (driver B→A): driver 左 = 世界 +側 = rightLateral
			sp.lateralOffset = (nodeId == edge.nodeB) ? leftLateral : rightLateral;
			sp.poleHeight    = kDefaultPoleHeight_m;
			sp.autoGenerated = true;
			out << sp;
		}

		// 車両進入禁止（303）: このノードから当該エッジへ進入できる車線が
		// 1本もなければ、「こちら側から入るな」という意味で自動配置。
		// 進入車（nodeId から入ろうとする車）の視点で右側に立てる。
		if (!edge.lanes.isEmpty())
		{
			bool canEnter = false;
			for (const auto& L : edge.lanes)
			{
				if (L.op != OpState::Open && L.op != OpState::Provisional) continue;
				const bool entersFromHere =
					(nodeId == edge.nodeA && L.dir == LaneDir::Forward) ||
					(nodeId == edge.nodeB && L.dir == LaneDir::Backward);
				if (entersFromHere) { canEnter = true; break; }
			}
			if (!canEnter)
			{
				RoadSignPlacement sp;
				sp.type      = RoadSignType::NoEntry;
				sp.nodeEndId = nodeId;
				sp.arcOffset = 0.0f;
				// 進入車の左側に配置（左右が逆だったため反転 + Roadbed 基準）:
				//   nodeId == nodeA → 進入車 A→B → 左 = 世界 -側 = leftLateral
				//   nodeId == nodeB → 進入車 B→A → 左 = 世界 +側 = rightLateral
				sp.lateralOffset = (nodeId == edge.nodeA) ? leftLateral : rightLateral;
				sp.poleHeight    = kDefaultPoleHeight_m;
				sp.autoGenerated = true;
				out << sp;
			}
		}
		// 指定方向外進行禁止（311）:
		// ノード N における幾何的な出口方向集合 expected と、
		// エッジ E から laneConnections 経由で実際に進める方向集合 actual を比較し、
		// actual ⊊ expected であれば「行けない方向がある」ため 311 を配置する。
		if (!edge.lanes.isEmpty())
		{
			const auto bezIn = network.getBezier(edge.id);
			if (bezIn)
			{
				const bool isAtB = (nodeId == edge.nodeB);
				const Vec3 tIn = isAtB
					? bezIn->tangentAt(bezIn->totalLength)
					: -bezIn->tangentAt(0.0f);
				const float inAngle = static_cast<float>(Math::Atan2(tIn.x, tIn.z));

				auto classify = [&](int outEdgeId) -> Optional<TurnType>
				{
					if (outEdgeId == edge.id) return TurnType::UTurn;
					const RoadEdge* outE = network.getEdge(outEdgeId);
					if (!outE) return none;
					const auto bezOut = network.getBezier(outEdgeId);
					if (!bezOut) return none;
					const bool outAtA = (outE->nodeA == nodeId);
					const Vec3 tOut = outAtA
						? bezOut->tangentAt(0.0f)
						: -bezOut->tangentAt(bezOut->totalLength);
					const float outAngle = static_cast<float>(Math::Atan2(tOut.x, tOut.z));
					return TrafficCommon::classifyTurnByAngles(inAngle, outAngle);
				};

				// expected: U ターンを除く他エッジの幾何方向
				uint32 expectedMask = 0;
				for (const auto& a : node->attachments)
				{
					if (a.edgeId == edge.id) continue;
					if (auto t = classify(a.edgeId))
						expectedMask |= (1u << static_cast<int>(*t));
				}

				// actual: エッジ E のレーンから実際に到達する方向
				uint32 actualMask = 0;
				for (const auto& c : node->laneConnections)
				{
					if (c.fromEdgeId != edge.id) continue;
					if (auto t = classify(c.toEdgeId))
						actualMask |= (1u << static_cast<int>(*t));
				}

				// E から N へ進入可能 (actual != 0) かつ 幾何的方向の一部が欠ける場合
				if (actualMask != 0 && expectedMask != 0
					&& (actualMask & expectedMask) != expectedMask)
				{
					RoadSignPlacement sp;
					sp.type      = RoadSignType::DirectionalRestriction;
					sp.nodeEndId = nodeId;
					sp.arcOffset = 0.0f;
					// Stop と同じ規約で driver の左側（Roadbed 基準）
					sp.lateralOffset = (nodeId == edge.nodeB) ? leftLateral : rightLateral;
					sp.poleHeight    = kDefaultPoleHeight_m;
					sp.autoGenerated = true;
					out << sp;
				}
			}
		}
	};

	if (edge.nodeA >= 0) checkNode(edge.nodeA);
	if (edge.nodeB >= 0) checkNode(edge.nodeB);

	return out;
}

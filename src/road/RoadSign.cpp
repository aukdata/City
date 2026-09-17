#include "../asset/ModelLodPath.hpp"
#include "RoadSign.hpp"
#include "SignArtwork.hpp"
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

	/// @brief OBJ の厚み・UV規約に依存せず、local -Zの表面だけを取り出す。
	MeshData boardFront(const MeshData& source)
	{
		if (source.vertices.isEmpty()) { return {}; }
		Float3 lower{1e9f,1e9f,1e9f},upper{-1e9f,-1e9f,-1e9f};
		for (const auto& vertex : source.vertices)
		{
			lower.x=Min(lower.x,vertex.pos.x);lower.y=Min(lower.y,vertex.pos.y);lower.z=Min(lower.z,vertex.pos.z);
			upper.x=Max(upper.x,vertex.pos.x);upper.y=Max(upper.y,vertex.pos.y);
		}
		MeshData face;
		for (const auto triangle : source.indices)
		{
			const Vec3 a{source.vertices[triangle.i0].pos},b{source.vertices[triangle.i1].pos},c{source.vertices[triangle.i2].pos};
			if (Abs(a.z-lower.z)>1e-5 || Abs(b.z-lower.z)>1e-5 || Abs(c.z-lower.z)>1e-5) { continue; }
			const double winding=(b-a).cross(c-a).z;if (Abs(winding)<1e-10) { continue; }
			const uint32 base=static_cast<uint32>(face.vertices.size());
			for (const Vec3 point : {a,b,c})
			{
				face.vertices << makeVert(point.x,point.y,-.01,static_cast<float>((point.x-lower.x)/(upper.x-lower.x)),
					static_cast<float>((upper.y-point.y)/(upper.y-lower.y)),0,0,-1);
			}
			face.indices << (winding<0 ? TriangleIndex32{base,base+1,base+2} : TriangleIndex32{base,base+2,base+1});
		}
		return face;
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

MeshData RoadSign::CreatePoleMesh(float poleHeight, int lod)
{
	// 共有する実寸モデルには天蓋・根元のカラー・板面の固定帯を含める。
	static std::array<MeshData, 3> levels;
	lod = Clamp(lod, 0, 2);
	if (levels[lod].vertices.isEmpty()) { levels[lod] = loadBoardObj(modelLodPath(U"assets/signs/sign_pole.obj", lod)); }
	const auto& kPole = levels[lod];
	if (kPole.vertices.isEmpty()) { return makeCylinder(kPoleRadius_m, poleHeight, 32); }
	MeshData result = kPole;
	const float scale = Max(.01f, poleHeight)/2.5f;
	for (auto& vertex : result.vertices)
	{
		vertex.pos.y *= scale;
		vertex.normal.y /= scale;
		vertex.normal = vertex.normal.normalized();
	}
	return result;
}

namespace
{
	RoadSign::PoleMetadata g_poleMetadata;
	bool                   g_poleMetadataLoaded = false;
}

const RoadSign::PoleMetadata& RoadSign::poleMetadata()
{
	// ポールと看板の相対位置は JSON から 1 回だけ読み込み、以後はキャッシュを共有する。
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
	// 標識種別ごとのカテゴリ・形状・テクスチャ対応をここへ集約し、描画側の分岐を減らす。
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
		U"" };
	static const SignVisual kSpeed{RoadSignCategory::Regulatory,U"assets/signs/regulatory/circle.obj",U""};
	static const SignVisual kOneWay{RoadSignCategory::Regulatory,U"",U""};
	static const SignVisual kCurve{RoadSignCategory::Warning,U"",U""};
	static const SignVisual kLabel{RoadSignCategory::Guide,U"",U""};
	static const SignVisual kNationalRoute{
		RoadSignCategory::Guide,
		U"assets/signs/guide/onigiri.obj",
		U"" };  // テクスチャは号数合成（m_routeSignTexCache）で動的解決

	switch (type)
	{
	case RoadSignType::RoadName:
	case RoadSignType::Municipality:
	case RoadSignType::PrefectureRoute: return kLabel;
	case RoadSignType::SteepGrade:
	case RoadSignType::NarrowRoad: return kCurve;
	case RoadSignType::SpeedLimit: return kSpeed;
	case RoadSignType::OneWay: return kOneWay;
	case RoadSignType::CurveWarning: return kCurve;
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
	if (type==RoadSignType::OneWay || type==RoadSignType::CurveWarning || type==RoadSignType::SteepGrade || type==RoadSignType::NarrowRoad || type==RoadSignType::PrefectureRoute || SignArtwork::wide(type))
	{
		MeshData mesh;
		const double halfWidth=SignArtwork::wide(type) ? 1.2 : type==RoadSignType::OneWay ? .23 : .45,halfHeight=SignArtwork::wide(type) ? .4 : .45;
		mesh.vertices << makeVert(-halfWidth,halfHeight,0,0,0,0,0,-1) << makeVert(halfWidth,halfHeight,0,1,0,0,0,-1)
			<< makeVert(-halfWidth,-halfHeight,0,0,1,0,0,-1) << makeVert(halfWidth,-halfHeight,0,1,1,0,0,-1);
		mesh.indices << TriangleIndex32{0,1,2} << TriangleIndex32{2,1,3};
		if (type==RoadSignType::CurveWarning || type==RoadSignType::SteepGrade || type==RoadSignType::NarrowRoad || type==RoadSignType::PrefectureRoute)
		{
			mesh=MeshData{};
			const Array<Vec2> shape=type==RoadSignType::PrefectureRoute
				? Array<Vec2>{{-.23,.45},{.23,.45},{.45,0},{.23,-.45},{-.23,-.45},{-.45,0}}
				: Array<Vec2>{{0,.45},{.45,0},{0,-.45},{-.45,0}};
			mesh.vertices << makeVert(0,0,0,.5f,.5f,0,0,-1);
			for (Vec2 point:shape) { mesh.vertices << makeVert(point.x,point.y,0,static_cast<float>(point.x/.9+.5),static_cast<float>(.5-point.y/.9),0,0,-1); }
			for (uint32 index=1;index<=shape.size();++index) { mesh.indices << TriangleIndex32{0,index,index==shape.size() ? 1u : index+1}; }
		}
		return mesh;
	}
	const auto& vis = visualOf(type);
	if (vis.shapeObjPath.isEmpty()) return {};
	return boardFront(loadBoardObj(vis.shapeObjPath));
}

MeshData RoadSign::CreateBoardBackingMesh(const MeshData& front)
{
	MeshData back;
	struct Edge { Vec3 a,b; int count=1; }; Array<Edge> outline;
	for (const auto& vertex : front.vertices)
	{
		auto copy=vertex;copy.pos.z+=.02f;copy.normal={0,0,1};back.vertices<<copy;
	}
	for (const auto triangle : front.indices)
	{
		back.indices<<TriangleIndex32{triangle.i0,triangle.i2,triangle.i1};
		const std::array<uint32,3> points{triangle.i0,triangle.i1,triangle.i2};
		for (int index=0;index<3;++index)
		{
			const Vec3 a{front.vertices[points[index]].pos},b{front.vertices[points[(index+1)%3]].pos};
			bool found=false;
			for (auto& edge : outline)
			{
				if ((edge.a.distanceFromSq(a)<1e-10 && edge.b.distanceFromSq(b)<1e-10)
					|| (edge.a.distanceFromSq(b)<1e-10 && edge.b.distanceFromSq(a)<1e-10)) { ++edge.count;found=true;break; }
			}
			if (!found) { outline<<Edge{a,b}; }
		}
	}
	for (const auto& edge : outline)
	{
		if (edge.count!=1) { continue; }
		const Vec3 normal=Vec3{0,0,1}.cross(edge.b-edge.a).normalized();
		const uint32 base=static_cast<uint32>(back.vertices.size());
		for (const Vec3 point : {edge.a,edge.b,edge.b+Vec3{0,0,.02},edge.a+Vec3{0,0,.02}})
		{ back.vertices<<makeVert(point.x,point.y,point.z,0,0,normal.x,normal.y,normal.z); }
		back.indices<<TriangleIndex32{base,base+2,base+1}<<TriangleIndex32{base,base+3,base+2};
	}
	return back;
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
	// エッジ両端の交通規制と接続関係を見て、必要な規制標識だけを自動配置候補として生成する。
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

		const bool towardNode=edge.lanes.any([&](const Lane& lane) { return nodeId==edge.nodeB ? lane.dir==LaneDir::Forward : lane.dir==LaneDir::Backward; });
		const bool oneWay=!edge.lanes.isEmpty() && edge.lanes.all([&](const Lane& lane) { return lane.dir==edge.lanes.front().dir; });
		const float usable=edge.length-edge.cutoffA-edge.cutoffB;
		const auto add=[&](RoadSignType type,float offset,int value,bool entering)
		{
			RoadSignPlacement sign; sign.type=type; sign.nodeEndId=nodeId; sign.arcOffset=offset;
			const bool left=(nodeId==edge.nodeB)!=entering;
			sign.lateralOffset=left ? leftLateral : rightLateral;
			sign.poleHeight=kDefaultPoleHeight_m; sign.auxValue=value; sign.autoGenerated=true; out << sign;
		};
		if (oneWay && !towardNode && usable>20 && node->attachments.size()>=3) { add(RoadSignType::OneWay,8,0,true); }
		if (towardNode && usable>95 && node->attachments.size()>=3)
		{
			add(RoadSignType::SpeedLimit,40,Clamp(static_cast<int>(Round(edge.speedLimit)),1,140),false);
		}
		if (towardNode && usable>130)
		{
			if (const auto curve=network.getBezier(edge.id))
			{
				const Vec3 a=curve->tangent(0),b=curve->tangent(1);
				if (a.dot(b)<.90)
				{
					const double turn=(a.x*b.z-a.z*b.x)*(nodeId==edge.nodeB ? 1 : -1);
					add(RoadSignType::CurveWarning,usable*.72f,turn>0 ? 1 : 2,false);
				}
			}
		}
		if (att->control == TrafficControl::Stop && towardNode)
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

		if (att->control==TrafficControl::Yield && towardNode) { add(RoadSignType::Yield,5,0,false); }
		if (towardNode && usable>90)
		{
			const auto curve=network.getBezier(edge.id);
			if (curve)
			{
				const Vec3 rise=curve->p3-curve->p0;
				const double grade=100*rise.y/Max(1.0,Vec2{rise.x,rise.z}.length())*(nodeId==edge.nodeB ? 1 : -1);
				if (Abs(grade)>=5) { add(RoadSignType::SteepGrade,usable*.55f,static_cast<int>(Round(grade)),false); }
			}
		}
		// 幅員減少はこの先の道路が実際に狭くなるときだけ予告する。
		if (towardNode && usable>70 && node->attachments.size()==2)
		{
			for (const auto& other:node->attachments)
			{
				const auto* next=network.getEdge(other.edgeId);
				if (next && next->id!=edge.id && next->totalWidth()+2<edge.totalWidth()) { add(RoadSignType::NarrowRoad,30,0,false); }
			}
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
					sp.auxValue  = static_cast<int>(actualMask);
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

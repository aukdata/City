#include "RoadRenderer.hpp"
#include "../road/LocationSigns.hpp"
#include "../road/SignArtwork.hpp"
#include "../road/RoadSign.hpp"
#include "../road/RoadGeometry.hpp"
#include "../road/RoadEnvironment.hpp"
#include "../asset/AssetRegistrar.hpp"

bool RoadRenderer::computeSignTransforms(const CubicBezier& bezier, const World& world,
	float arcLen, float lateralOffset, bool boardFacesTan,
	float boardOffsetX, float boardOffsetY, float boardOffsetZ,
	const RoadEdge& edge, Mat4x4& outPole, Mat4x4& outBoard, Vec3& outPoleTop)
{
	if (arcLen < 0.0f || arcLen > bezier.totalLength) { return false; }

	const Vec3 roadPos = bezier.positionAt(arcLen);
	if (RoadEnvironment::coveredAt(world,roadPos,edge.useElevation || edge.tunnel)) { return false; }
	const Vec3 rawTan  = bezier.tangentAt(arcLen);
	const Vec3 right   = tangentToRight(rawTan);

	const double anchorX = roadPos.x + right.x * static_cast<double>(lateralOffset);
	const double anchorZ = roadPos.z + right.z * static_cast<double>(lateralOffset);
	// 標識の接地 Y は Roadbed の高さに合わせる（路面リフト込み）
	// 高架: bezier Y / 非高架: 地形 Y
	const double baseY = edge.usesDesignHeight()
		? roadPos.y
		: static_cast<double>(world.sampleHeight(static_cast<float>(anchorX), static_cast<float>(anchorZ)));
	const double groundY = baseY + kRoadSurfaceLift;

	const Vec3   frontDir = boardFacesTan ? rawTan : -rawTan;
	const double dLen = Math::Sqrt(frontDir.x * frontDir.x + frontDir.z * frontDir.z);
	if (dLen < 1e-6) { return false; }
	const float yaw = static_cast<float>(Math::Atan2(frontDir.x / dLen, frontDir.z / dLen));

	// ポール: OBJ 実寸で配置（スケール無し）、yaw のみ適用
	outPole = Mat4x4::RotateY(yaw)
		* Mat4x4::Translate(Float3{
			static_cast<float>(anchorX),
			static_cast<float>(groundY),
			static_cast<float>(anchorZ) });

	// 看板: local (offsetX, offsetY, offsetZ) → world
	// 行ベクトル規約 (v' = v * M) で以下の順に適用:
	//   1) 支柱より手前（local -Z）へ板を出し、柱が文字を遮らないようにする
	//   2) RotateY(rotated) — yaw+π で local +Z を driver 反対方向（= -frontDir）に向け、
	//      看板正面（local -Z）が driver 方向を向くようにする
	//   3) anchor へ平行移動
	// 軸: X=driver から見た横方向, Y=垂直, Z=道路の長手方向（driver 逆向き）
	const float rotated = yaw + static_cast<float>(Math::Pi);
	outBoard = Mat4x4::Translate(Float3{ boardOffsetX, boardOffsetY, -boardOffsetZ })
		* Mat4x4::RotateY(rotated)
		* Mat4x4::Translate(Float3{
			static_cast<float>(anchorX),
			static_cast<float>(groundY),
			static_cast<float>(anchorZ) });
	outPoleTop = Vec3{ anchorX, groundY + boardOffsetY, anchorZ };
	return true;
}

Array<RoadRenderer::SignDraw> RoadRenderer::buildEdgeSignMeshes(
	const RoadNetwork& network, int edgeId, const World& world) const
{
	const RoadEdge* edge = network.getEdge(edgeId);
	if (!edge || !edge->isRoadbedBuilt()) return {};
	const auto bez = network.getBezier(edgeId);
	if (!bez) return {};

	Array<SignDraw> batches;

	for (const auto& sp : edge->signs)
	{
		if (sp.type == RoadSignType::None) { continue; }

		const bool atA = (sp.nodeEndId == edge->nodeA);
		const bool atB = (sp.nodeEndId == edge->nodeB);
		if (!atA && !atB) { continue; }

		// 弧長位置: cutoff + arcOffset を内側方向に
		const float cutoff = atA ? edge->cutoffA : edge->cutoffB;
		const float arc = atA
			? (cutoff + sp.arcOffset)
			: (bez->totalLength - cutoff - sp.arcOffset);

		// 看板は driver に向ける: nodeA 側 → driver は B→A → 看板正面は +tan
		//                          nodeB 側 → driver は A→B → 看板正面は -tan
		const bool entering=sp.type==RoadSignType::NoEntry || sp.type==RoadSignType::OneWay;
		const bool boardFacesTan = entering ? !atA : atA;

		Mat4x4 poleMat, boardMat;
		Vec3   poleTop;
		const auto& meta = RoadSign::poleMetadata();
		if (!computeSignTransforms(*bez, world, arc, sp.lateralOffset, boardFacesTan,
		                           meta.offsetX, meta.offsetY, meta.offsetZ,
		                           *edge, poleMat, boardMat, poleTop))
		{
			continue;
		}

		SignDraw signDraw;
		signDraw.poleMat  = poleMat;
		signDraw.boardMat = boardMat;
		signDraw.poleTop  = poleTop;
		signDraw.type     = sp.type;
		signDraw.auxNumber = sp.auxValue;
		batches << signDraw;
	}

	for (const auto& boundary:LocationSigns::boundaries(*bez,m_municipalityLookup))
	{
		const auto ext=RoadSign::roadbedExtentsOf(*edge);
		const auto& meta=RoadSign::poleMetadata();
		for (bool forward:{true,false})
		{
			if (!edge->lanes.any([&](const Lane& lane) { return lane.dir==(forward ? LaneDir::Forward : LaneDir::Backward); })) { continue; }
			SignDraw sign; sign.type=RoadSignType::Municipality; sign.label=forward ? boundary.after : boundary.before;
			const float lateral=forward ? ext.left-1.4f : ext.right+1.4f;
			if (computeSignTransforms(*bez,world,boundary.arc,lateral,!forward,meta.offsetX,meta.offsetY,meta.offsetZ,
				*edge,sign.poleMat,sign.boardMat,sign.poleTop)) { batches << sign; }
		}
	}
	return batches;
}

Array<RoadRenderer::SignDraw> RoadRenderer::buildRouteSignDraws(
	const RoadRoute& route,const RoadNetwork& network,const World& world) const
{
	Array<SignDraw> out;
	Array<std::pair<int,float>> anchors=network.routeSignAnchors(route);
	if (anchors.isEmpty())
	{
		float interval=1000;
		for (int id:route.edgeIds)
		{
			const auto* edge=network.getEdge(id);
			if (!edge || !edge->isRoadbedBuilt()) { continue; }
			interval+=edge->length;
			if (interval>=1000 && edge->length>70) { anchors.emplace_back(id,edge->length*.5f); interval=0; }
		}
	}
	const auto& meta=RoadSign::poleMetadata();
	for (const auto& [id,arc]:anchors)
	{
		const auto curve=network.getBezier(id); const auto* edge=network.getEdge(id);
		if (!curve || !edge) { continue; }
		const auto ext=RoadSign::roadbedExtentsOf(*edge);
		for (bool forward:{true,false})
		{
			if (!edge->lanes.any([&](const Lane& lane) { return lane.dir==(forward ? LaneDir::Forward : LaneDir::Backward); })) { continue; }
			const auto add=[&](RoadSignType type,String label,float offset)
			{
				SignDraw sign;sign.type=type;sign.auxNumber=route.number;sign.label=std::move(label);
				const float margin=SignArtwork::wide(type) ? 1.5f : .9f;
				const float lateral=forward ? ext.left-margin : ext.right+margin;
				if (computeSignTransforms(*curve,world,Clamp(arc+offset,5.0f,curve->totalLength-5),lateral,!forward,
					meta.offsetX,meta.offsetY,meta.offsetZ,*edge,sign.poleMat,sign.boardMat,sign.poleTop)) { out << sign; }
			};
			const bool numbered=route.number>0 && (route.kind==RoadRouteKind::NationalRoute || route.kind==RoadRouteKind::PrefectureRoute);
			if (numbered) { add(route.kind==RoadRouteKind::NationalRoute ? RoadSignType::NationalRoute : RoadSignType::PrefectureRoute,U"",0); }
			if (!route.name.isEmpty()) { add(RoadSignType::RoadName,route.name,numbered ? 8.0f : 0.0f); }
		}
	}
	return out;
}

const Mesh* RoadRenderer::getSignBoardMesh(RoadSignType type)
{
	if (type == RoadSignType::None) { return nullptr; }
	const auto& vis = RoadSign::visualOf(type);

	const String key=vis.shapeObjPath.isEmpty() ? U"generated:{}"_fmt(static_cast<int>(type)) : String{vis.shapeObjPath};
	if (auto it = m_signBoardMeshes.find(key); it != m_signBoardMeshes.end())
	{
		return &it->second;
	}

	const MeshData md = RoadSign::CreateBoardMesh(type);
	if (md.vertices.isEmpty()) { return nullptr; }
	m_signBoardBacks.emplace(key,Mesh{RoadSign::CreateBoardBackingMesh(md)});
	return &m_signBoardMeshes.emplace(key, Mesh{ md }).first->second;
}

void RoadRenderer::drawSigns(const Array<SignDraw>& draws)
{
	if (draws.isEmpty()) { return; }

	if (!m_signPoleMesh) { m_signPoleMesh=Mesh{RoadSign::CreatePoleMesh(RoadSign::kDefaultPoleHeight_m)}; }

	// ポール
	for (const auto& signDraw : draws)
	{
		const double distance = signDraw.poleTop.distanceFromSq(Vec3{Graphics3D::GetEyePosition()});
		const int lod = distance < 80*80 ? 0 : (distance < 200*200 ? 1 : 2);
		if (lod > 0 && !m_signPoleLods[lod-1]) { m_signPoleLods[lod-1] = Mesh{RoadSign::CreatePoleMesh(RoadSign::kDefaultPoleHeight_m, lod)}; }
		const auto& mesh = lod == 0 ? m_signPoleMesh : m_signPoleLods[lod-1];
		if (mesh) { mesh->draw(signDraw.poleMat, RoadSign::kPoleColor.removeSRGBCurve()); }
	}

	// 裏板と縁は無地の金属。表面用テクスチャを共有しない。
	{
		const ScopedRenderStates3D states{ RasterizerState::SolidCullBack };
		for (const auto& signDraw : draws)
		{
			const Mesh* boardMesh = getSignBoardMesh(signDraw.type);
			if (!boardMesh) { continue; }
			const auto& visual=RoadSign::visualOf(signDraw.type);
			const String key=visual.shapeObjPath.isEmpty() ? U"generated:{}"_fmt(static_cast<int>(signDraw.type)) : String{visual.shapeObjPath};
			if (const auto found=m_signBoardBacks.find(key);found!=m_signBoardBacks.end()) { found->second.draw(signDraw.boardMat,ColorF{.55}.removeSRGBCurve()); }
		}
	}

	// 看板前面: テクスチャ（裏面ポリゴンをカリングして前面のみ表示）
	{
		const ScopedRenderStates3D states{ BlendState::Default2D, RasterizerState::SolidCullBack };
		for (const auto& signDraw : draws)
		{
			const Mesh* boardMesh = getSignBoardMesh(signDraw.type);
			if (!boardMesh) { continue; }

			const auto& vis = RoadSign::visualOf(signDraw.type);
			if (!vis.textureAssetName.isEmpty())
			{
				boardMesh->draw(signDraw.boardMat, TextureAsset(vis.textureAssetName));
				continue;
			}
			if (SignArtwork::dynamic(signDraw.type))
			{
				const String key=SignArtwork::textureKey(signDraw.type,signDraw.auxNumber,signDraw.label);
				if (auto found=m_signTextures.find(key);found!=m_signTextures.end()) { boardMesh->draw(signDraw.boardMat,found->second); }
				else { m_pendingSignTextures[key] = SignTextureContent{ signDraw.type, signDraw.auxNumber, signDraw.label }; }
			}
		}
	}
}

namespace
{
	/// @brief 国道標識を生成・描画する対象となる route かどうか
	bool isDrawableNamedRoute(const RoadRoute& route)
	{
		return route.id >= 0
			&& (!route.name.isEmpty() || route.number > 0);
	}
}

void RoadRenderer::prepareSignTextures()
{
	// 実際に画面へ出た看板だけを合成する。文字・番号・種別で共有する。
	constexpr uint32 kFaceSize = 256;
	constexpr uint32 kWideFaceWidth = 768;
	for (const auto& [key, sign] : m_pendingSignTextures)
	{
		RenderTexture texture{ SignArtwork::wide(sign.type) ? kWideFaceWidth : kFaceSize, kFaceSize,
			ColorF{0, 0}, TextureFormat::R8G8B8A8_Unorm_SRGB, HasDepth::No, HasMipMap::Yes };
		{
			const ScopedRenderTarget2D target{ texture };
			const ScopedRenderStates2D blend{ BlendState::Opaque };
			SignArtwork::draw(sign.type, sign.auxNumber, FontAsset(Asset::CJK14), sign.label);
			Graphics2D::Flush();
		}
		texture.generateMips();
		m_signTextures.emplace(key, std::move(texture));
	}
	m_pendingSignTextures.clear();
}

void RoadRenderer::drawRouteSigns(const RoadNetwork& network, const World& world, Vec3 cameraPos)
{
	constexpr double kSignDistanceSquared = 800.0 * 800.0;
	for (const auto& route : network.routes())
	{
		if (!isDrawableNamedRoute(route)) { continue; }
		if (!m_routeSignCache.contains(route.id))
		{
			m_routeSignCache[route.id] = buildRouteSignDraws(route, network, world);
		}
		Array<SignDraw> visible;
		for (const auto& sign : m_routeSignCache[route.id])
		{
			if (sign.poleTop.distanceFromSq(cameraPos) < kSignDistanceSquared) { visible << sign; }
		}
		drawSigns(visible);
	}
}

// =============================================================================
// 案内標識（GuideSign）
// =============================================================================

namespace
{
	/// @brief GuideSignPlacement の内容からテクスチャキャッシュキーを計算
	uint64 guideSignTexKey(const GuideSignPlacement& g)
	{
		// FNV-1a 64bit
		uint64 h = 14695981039346656037ull;
		auto mix = [&](uint64 v) {
			h ^= v;
			h *= 1099511628211ull;
		};
		auto mixStr = [&](const String& s) {
			for (char32_t c : s) { mix(static_cast<uint64>(c)); }
			mix(0xff);  // 区切り
		};
		auto mixFloat = [&](float f) {
			mix(static_cast<uint64>(static_cast<int64>(f * 1000.0f + (f >= 0 ? 0.5f : -0.5f))));
		};
		mix(static_cast<uint64>(g.kind));
		mixFloat(g.widthOverride);
		mixFloat(g.heightOverride);
		mixFloat(static_cast<float>(g.bgColor.r));
		mixFloat(static_cast<float>(g.bgColor.g));
		mixFloat(static_cast<float>(g.bgColor.b));
		mixFloat(static_cast<float>(g.bgColor.a));
		mix(g.showReading ? 1 : 0);
		mix(static_cast<uint64>(g.elements.size()));
		for (const auto& el : g.elements)
		{
			mix(static_cast<uint64>(el.kind));
			mixFloat(el.posX);
			mixFloat(el.posY);
			mixFloat(el.scale);
			mixFloat(el.arrowAngle);
			mixStr(el.text);
			mixStr(el.reading);
		}
		return h;
	}

}

namespace
{
	/// @brief 1基分の案内標識テクスチャを合成する
	/// @details SRGB format でサンプリング時に線形空間へ自動変換させる（ルート看板と色合いを合わせる）
	///   HasMipMap なし（Test 検証: HasMipMap::Yes だと描画内容がキャプチャ先に反映されない）
	RenderTexture buildGuideSignTexture(const GuideSignPlacement& g, const Font& fontJa, const Font& fontNum)
	{
		const auto   bs      = GuideSign::computeBoardSizeFor(g);
		const Size   texSize = GuideSign::guideSignTexSize(bs.width, bs.height);
		const ColorF bg      = GuideSign::resolveBgColor(g.bgColor);

		// SRGB フォーマットへのクリア色は linear 値として解釈されて sRGB 符号化される。
		// bgColor (21/255, 87/255, 161/255) を linear とみなして encode した結果が物理バイトに
		// 入るため、3D でサンプリングしたとき国道アイコン PNG（Mipped / Unorm 読み込み）と
		// 同じ明るめの青 ≒ (94,193,230) で表示され両者の色味が一致する。
		RenderTexture rt{ static_cast<uint32>(texSize.x), static_cast<uint32>(texSize.y), bg,
		                  TextureFormat::R8G8B8A8_Unorm_SRGB };
		{
			const ScopedRenderTarget2D target{ rt };
			const ScopedRenderStates2D blend{ BlendState::Default2D };
			GuideSign::renderContents(g, texSize, fontJa, fontNum);
			Graphics2D::Flush();
		}
		rt.generateMips();
		return rt;
	}
}

void RoadRenderer::prepareGuideSignTextures(const RoadNetwork& network)
{
	// 全テクスチャ準備済みなら即リターン（毎フレーム 5000 件を走査するコストを回避）
	if (m_guideSignTexAllReady) { return; }

	// 地名は太字で表示（現物に合わせる）
	const Font& fontJa  = FontAsset(Asset::CJK32Bold);
	const Font& fontNum = FontAsset(Asset::Arial24);
	if (!fontJa || !fontNum) { return; }

	int missCount = 0;

	for (const auto& g : network.guideSigns())
	{
		if (g.id < 0) { continue; }
		if (g.elements.isEmpty()) { continue; }

		// 板面内容のハッシュ単位でテクスチャ化し、同一内容の標識はエッジをまたいで共有する。
		const uint64 key = guideSignTexKey(g);
		if (m_guideSignTexCache.contains(key)) { continue; }
		++missCount;

		m_guideSignTexCache[key] = buildGuideSignTexture(g, fontJa, fontNum);
	}

	// キャッシュミスがなくなったら準備完了フラグをセット
	if (missCount == 0)
	{
		m_guideSignTexAllReady = true;
	}
}

Optional<RoadRenderer::GuideSignDraw> RoadRenderer::buildGuideSignDraw(
	const GuideSignPlacement& sign, const RoadNetwork& network, const World& world) const
{
	const auto* edge = network.getEdge(sign.parentEdgeId);
	if (!edge || !edge->isRoadbedBuilt()) { return none; }
	const auto curve = network.getBezier(sign.parentEdgeId);
	if (!curve) { return none; }
	const bool atA = sign.nodeEndId == edge->nodeA;
	if (!atA && sign.nodeEndId != edge->nodeB) { return none; }
	const float cutoff = atA ? edge->cutoffA : edge->cutoffB;
	const float arc = atA ? cutoff + sign.arcOffset : curve->totalLength - cutoff - sign.arcOffset;
	const auto& meta = GuideSign::poleMetadata();
	GuideSignDraw draw;
	if (!computeSignTransforms(*curve, world, arc, sign.lateralOffset, !atA,
		meta.offsetX, meta.offsetY, meta.offsetZ, *edge,
		draw.poleMat, draw.boardMat, draw.poleTop)) { return none; }
	const auto size = GuideSign::computeBoardSizeFor(sign);
	const MeshData mesh = GuideSign::CreateBoardMesh(size.width, size.height);
	if (!mesh.vertices.isEmpty()) { draw.boardMesh=Mesh{mesh};draw.backMesh=Mesh{RoadSign::CreateBoardBackingMesh(mesh)}; }
	draw.texKey = guideSignTexKey(sign);
	return draw;
}

Array<RoadRenderer::GuideSignDraw> RoadRenderer::buildEdgeGuideSignDraws(
	const RoadNetwork& network, int edgeId, const World& world) const
{
	const auto* edge = network.getEdge(edgeId);
	if (!edge || !edge->isRoadbedBuilt()) { return {}; }
	Array<GuideSignDraw> draws;
	for (const auto& sign : network.guideSigns())
	{
		if (sign.id < 0 || sign.parentEdgeId != edgeId || sign.elements.isEmpty()) { continue; }
		if (auto draw = buildGuideSignDraw(sign, network, world); draw && !draw->boardMesh.isEmpty())
		{
			draws << std::move(*draw);
		}
	}
	return draws;
}

bool RoadRenderer::ensureGuidePoleMesh()
{
	if (!m_guidePoleMesh)
	{
		const MeshData mesh = GuideSign::CreatePoleMesh();
		if (!mesh.vertices.isEmpty()) { m_guidePoleMesh = Mesh{ mesh }; }
	}
	return m_guidePoleMesh.has_value();
}

const Texture* RoadRenderer::getGuideSignCachedTexture(const GuideSignPlacement& g) const
{
	const uint64 key = guideSignTexKey(g);
	if (auto it = m_guideSignTexCache.find(key); it != m_guideSignTexCache.end())
	{
		return &it->second;
	}
	return nullptr;
}

// =============================================================================
// 選択アウトライン用シルエット描画（書き込み先を差し替え、通常パスと同じメッシュ・変換で描画）
// =============================================================================

void RoadRenderer::drawGuideSignSilhouette(int signId, const RoadNetwork& network,
	const World& world, const ColorF& color)
{
	for (const auto& sign : network.guideSigns())
	{
		if (sign.id != signId) { continue; }
		const auto draw = buildGuideSignDraw(sign, network, world);
		if (!draw) { return; }
		const ScopedRenderStates3D states{ RasterizerState::SolidCullNone };
		if (ensureGuidePoleMesh()) { m_guidePoleMesh->draw(draw->poleMat, color); }
		if (!draw->boardMesh.isEmpty()) { draw->boardMesh.draw(draw->boardMat, color); }
		return;
	}
}

void RoadRenderer::drawGuideSigns(const Array<GuideSignDraw>& draws)
{
	if (draws.isEmpty()) { return; }

	if (!ensureGuidePoleMesh()) { return; }

	// ポール
	{
		const ScopedRenderStates3D states{ RasterizerState::SolidCullNone };
		for (const auto& d : draws)
		{
			const Vec3 position{d.poleMat.transformPoint(Float3{0,0,0})};
			const double distance = position.distanceFromSq(Vec3{Graphics3D::GetEyePosition()});
			const int lod = distance < 120*120 ? 0 : (distance < 300*300 ? 1 : 2);
			if (lod > 0 && !m_guidePoleLods[lod-1]) { m_guidePoleLods[lod-1] = Mesh{GuideSign::CreatePoleMesh(lod)}; }
			const auto& mesh = lod == 0 ? m_guidePoleMesh : m_guidePoleLods[lod-1];
			mesh->draw(d.poleMat, RoadSign::kPoleColor.removeSRGBCurve());
		}
	}

	// 裏板と縁は無地の金属。表面用テクスチャを共有しない。
	{
		const ScopedRenderStates3D states{ RasterizerState::SolidCullBack };
		for (const auto& d : draws)
			d.backMesh.draw(d.boardMat, ColorF{ 0.55 }.removeSRGBCurve());
	}

	// 看板前面: テクスチャ（裏面ポリゴンをカリングして前面のみ表示）
	{
		const ScopedRenderStates3D states{ BlendState::Default2D, RasterizerState::SolidCullBack };
		for (const auto& d : draws)
		{
			if (auto itRt = m_guideSignTexCache.find(d.texKey); itRt != m_guideSignTexCache.end())
			{
				d.boardMesh.draw(d.boardMat, itRt->second);
			}
		}
	}
}


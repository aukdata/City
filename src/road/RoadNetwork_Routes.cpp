#include "RoadNetwork.hpp"

ColorF RoadNetwork::defaultRouteColor(RoadRouteKind kind)
{
	switch (kind)
	{
	case RoadRouteKind::Expressway:      return ColorF{ 0.0, 0.6, 0.0 };
	case RoadRouteKind::NationalRoute:   return ColorF{ 0.8, 0.1, 0.1 };
	case RoadRouteKind::PrefectureRoute: return ColorF{ 0.1, 0.35, 0.75 };
	case RoadRouteKind::CityRoute:       return ColorF{ 0.95, 0.7, 0.2 };
	case RoadRouteKind::Named:           return ColorF{ 0.55 };
	}
	return ColorF{ 0.55 };
}

String RoadNetwork::generateAutoRouteName(RoadRouteKind kind, int* outNumber) const
{
	// 番号系 kind: kind 内でユニークな 1〜400 の番号
	const bool isNumbered =
		(kind == RoadRouteKind::NationalRoute) ||
		(kind == RoadRouteKind::PrefectureRoute) ||
		(kind == RoadRouteKind::CityRoute);

	int number = 0;
	if (isNumbered)
	{
		HashSet<int> used;
		for (const auto& r : m_routes)
			if (r.id >= 0 && r.kind == kind && r.number > 0)
				used.insert(r.number);

		// 1〜400 からランダムに 50 回試行
		for (int i = 0; i < 50; ++i)
		{
			const int n = Random(1, 400);
			if (!used.contains(n)) { number = n; break; }
		}
		// フォールバック: 線形走査
		if (number == 0)
		{
			for (int n = 1; n <= 400; ++n)
			{
				if (!used.contains(n)) { number = n; break; }
			}
		}
	}

	if (outNumber) *outNumber = number;

	switch (kind)
	{
	case RoadRouteKind::NationalRoute:   return U"国道{}号"_fmt(number);
	case RoadRouteKind::PrefectureRoute: return U"県道{}号"_fmt(number);
	case RoadRouteKind::CityRoute:       return U"市道{}号"_fmt(number);
	case RoadRouteKind::Expressway:      return U"○○自動車道";  // v2 で起点-終点合成
	case RoadRouteKind::Named:           return U"通り";         // v2 で地名連動
	}
	return U"名称未設定";
}

int RoadNetwork::addRoute(RoadRouteKind kind, String name, Array<int> edgeIds, int number)
{
	// 路線本体の登録と、各エッジから見た routeIds 逆引きの更新を同時に行う。
	// 空名なら自動命名
	if (name.isEmpty())
	{
		int autoN = 0;
		name = generateAutoRouteName(kind, &autoN);
		if (number == 0) number = autoN;
	}

	RoadRoute route;
	route.id      = m_nextRouteId++;
	route.kind    = kind;
	route.name    = std::move(name);
	route.number  = number;
	route.edgeIds = std::move(edgeIds);
	route.color   = defaultRouteColor(kind);

	// スロット割当
	int idx;
	if (!m_freeRouteSlots.isEmpty())
	{
		idx = m_freeRouteSlots.back();
		m_freeRouteSlots.pop_back();
		m_routes[idx] = route;
	}
	else
	{
		idx = static_cast<int>(m_routes.size());
		m_routes << route;
	}
	m_routeIdToIdx[route.id] = idx;

	// 各 edge に逆引き登録
	for (const int eid : m_routes[idx].edgeIds)
	{
		if (RoadEdge* e = getEdge(eid))
		{
			if (!e->routeIds.contains(route.id))
				e->routeIds << route.id;
		}
	}

	return route.id;
}

void RoadNetwork::addRouteRaw(const RoadRoute& route)
{
	if (route.id < 0 || m_routeIdToIdx.contains(route.id)) return;

	int idx;
	if (!m_freeRouteSlots.isEmpty())
	{
		idx = m_freeRouteSlots.back();
		m_freeRouteSlots.pop_back();
		m_routes[idx] = route;
	}
	else
	{
		idx = static_cast<int>(m_routes.size());
		m_routes << route;
	}
	m_routeIdToIdx[route.id] = idx;
	m_nextRouteId = Max(m_nextRouteId, route.id + 1);
}

void RoadNetwork::removeRoute(int routeId)
{
	const int idx = routeIndex(routeId);
	if (idx < 0) return;
	// 各 edge の routeIds からも除去
	for (const int eid : m_routes[idx].edgeIds)
	{
		if (RoadEdge* e = getEdge(eid))
		{
			e->routeIds.remove(routeId);
		}
	}
	m_routes[idx].id = -1;
	m_routes[idx].edgeIds.clear();
	m_routeIdToIdx.erase(routeId);
	m_freeRouteSlots << idx;

	for (auto& plan : m_plans)
	{
		if (plan.id >= 0 && plan.routeId == routeId)
			plan.routeId = -1;
	}
}

RoadRoute* RoadNetwork::getRoute(int id)
{
	const int idx = routeIndex(id);
	return (idx >= 0) ? &m_routes[idx] : nullptr;
}

const RoadRoute* RoadNetwork::getRoute(int id) const
{
	const int idx = routeIndex(id);
	return (idx >= 0) ? &m_routes[idx] : nullptr;
}

void RoadNetwork::rebuildEdgeRouteIndex()
{
	// セーブ復元や一括編集後に、路線→エッジ情報からエッジ側の逆引きを再構築する。
	// 全 edge の routeIds をクリア
	for (auto& e : m_edges)
	{
		if (e.id >= 0) e.routeIds.clear();
	}
	// 各 route の edgeIds から逆引きを構築
	for (const auto& r : m_routes)
	{
		if (r.id < 0) continue;
		for (const int eid : r.edgeIds)
		{
			if (RoadEdge* e = getEdge(eid))
			{
				if (!e->routeIds.contains(r.id))
					e->routeIds << r.id;
			}
		}
	}
}

Array<std::pair<int, float>> RoadNetwork::routeSignAnchors(
	const RoadRoute& route,
	float distFromJunction_m,
	float minEdgeLen_m) const
{
	Array<std::pair<int, float>> result;

	for (const int edgeId : route.edgeIds)
	{
		const RoadEdge* edge = getEdge(edgeId);
		if (!edge || edge->id < 0) continue;

		// Planned / UnderConstruction は対象外
		if (edge->edgeState == EdgeState::Planned ||
		    edge->edgeState == EdgeState::UnderConstruction)
			continue;

		const auto bezier = getBezier(edgeId);
		if (!bezier) continue;

		const float totalLen = bezier->totalLength;
		if (totalLen < minEdgeLen_m) continue;

		// 信号のある交差点の先 50m のみに配置する
		// nodeA 側: nodeA に信号があるとき
		{
			const RoadNode* nodeA = getNode(edge->nodeA);
			if (nodeA && nodeA->signalPlacement.has_value())
			{
				const float arc = Min(distFromJunction_m, totalLen * 0.5f);
				result.emplace_back(edgeId, arc);
			}
		}

		// nodeB 側: nodeB に信号があるとき
		{
			const RoadNode* nodeB = getNode(edge->nodeB);
			if (nodeB && nodeB->signalPlacement.has_value())
			{
				const float arc = totalLen - Min(distFromJunction_m, totalLen * 0.5f);
				result.emplace_back(edgeId, arc);
			}
		}
	}

	return result;
}

void RoadNetwork::onEdgeRemovedFromRoutes(int edgeId)
{
	// 路線の途中エッジが消えたときは、末端短縮か中間分割で経路の連続性を保ち直す。
	// edge が所属する route の順序を保持したまま処理するため、
	// m_routes のインデックスを走査（新 route 追加時に m_routes が拡張される点に注意）
	// ここでは id ベースの snapshot を取ってから処理する
	Array<int> affectedRouteIds;
	for (const auto& r : m_routes)
	{
		if (r.id >= 0 && r.edgeIds.contains(edgeId))
			affectedRouteIds << r.id;
	}

	for (const int rid : affectedRouteIds)
	{
		RoadRoute* r = getRoute(rid);
		if (!r) continue;

		// 該当 edgeId の位置を探す（複数出現は本来禁止だが、安全に全削除処理）
		// 先に最初の出現のみを扱う
		auto it = std::find(r->edgeIds.begin(), r->edgeIds.end(), edgeId);
		if (it == r->edgeIds.end()) continue;
		const int pos = static_cast<int>(it - r->edgeIds.begin());
		const int lastIdx = static_cast<int>(r->edgeIds.size()) - 1;

		if (pos == 0 || pos == lastIdx)
		{
			// 端: 短縮のみ
			r->edgeIds.remove_at(pos);
			if (r->edgeIds.isEmpty())
			{
				removeRoute(rid);  // 空になったら削除
			}
		}
		else
		{
			// 中間: 2 つに分割
			// 元 route は前半 [0..pos-1] に短縮
			Array<int> backHalf(r->edgeIds.begin() + pos + 1, r->edgeIds.end());
			r->edgeIds.resize(pos);

			// 新 route を作成（同じ kind/name/number/color、新 id）
			if (!backHalf.isEmpty())
			{
				const int newId = m_nextRouteId++;
				RoadRoute newR;
				newR.id      = newId;
				newR.kind    = r->kind;
				newR.name    = r->name;
				newR.number  = r->number;
				newR.edgeIds = backHalf;
				newR.color   = r->color;

				int newIdx;
				if (!m_freeRouteSlots.isEmpty())
				{
					newIdx = m_freeRouteSlots.back();
					m_freeRouteSlots.pop_back();
					m_routes[newIdx] = newR;
				}
				else
				{
					newIdx = static_cast<int>(m_routes.size());
					m_routes << newR;
				}
				m_routeIdToIdx[newId] = newIdx;

				// 後半 edge 群の routeIds を更新（元 id を除去 + 新 id を追加）
				// 注: 元 id は edgeId の除去処理の一部として後半 edge の routeIds からも消す必要あり。
				for (const int eid : backHalf)
				{
					if (RoadEdge* e = getEdge(eid))
					{
						e->routeIds.remove(rid);
						if (!e->routeIds.contains(newId))
							e->routeIds << newId;
					}
				}
			}
		}
	}
}


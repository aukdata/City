#include "SubsurfaceView.hpp"
#include "TrainRenderer.hpp"
#include "../road/RoadGeometry.hpp"

bool SubsurfaceView::below(Vec3 point, const World& world)
{
	return point.y < world.sampleHeight(static_cast<float>(point.x), static_cast<float>(point.z)) - kCover;
}

MeshData SubsurfaceView::clip(const MeshData& source, const World& world, bool underground)
{
	MeshData result;
	const auto distance = [&](const Vertex3D& vertex)
	{
		const double cover = world.sampleHeight(vertex.pos.x, vertex.pos.z) - vertex.pos.y - kCover;
		return underground ? cover : -cover;
	};
	for (const auto& triangle : source.indices)
	{
		Array<Vertex3D> polygon{
			source.vertices[triangle.i0], source.vertices[triangle.i1], source.vertices[triangle.i2]},
			clipped;
		for (size_t i = 0; i < polygon.size(); ++i)
		{
			const auto& a = polygon[i];
			const auto& b = polygon[(i + 1) % polygon.size()];
			const double da = distance(a), db = distance(b);
			if (da >= 0)
			{
				clipped << a;
			}
			if ((da >= 0) != (db >= 0))
			{
				const float t = static_cast<float>(da / (da - db));
				Vertex3D vertex;
				vertex.pos = a.pos + (b.pos - a.pos) * t;
				vertex.normal = a.normal + (b.normal - a.normal) * t;
				vertex.tex = a.tex + (b.tex - a.tex) * t;
				clipped << vertex;
			}
		}
		if (clipped.size() < 3)
		{
			continue;
		}
		const uint32 first = static_cast<uint32>(result.vertices.size());
		result.vertices.append(clipped);
		for (uint32 i = 1; i + 1 < clipped.size(); ++i)
		{
			result.indices << TriangleIndex32{first, first + i, first + i + 1};
		}
	}
	return result;
}

Optional<double> SubsurfaceView::hitDistance(const MeshData& data, const Ray& ray)
{
	Optional<double> best;
	for (const auto& index : data.indices)
	{
		if (const auto distance = ray.intersects(
				Triangle3D{data.vertices[index.i0].pos, data.vertices[index.i1].pos, data.vertices[index.i2].pos}))
		{
			if (!best || *distance < *best)
			{
				best = *distance;
			}
		}
	}
	return best;
}

void SubsurfaceView::add(Item& item, MeshData data, ColorF color)
{
	if (data.indices.isEmpty())
	{
		return;
	}
	for (const auto& vertex : data.vertices)
	{
		item.radius = Max(item.radius, Vec3{vertex.pos}.distanceFrom(item.center));
	}
	Mesh mesh{data};
	item.parts << Part{std::move(data), std::move(mesh), color};
}

void SubsurfaceView::prepare(const World& world, const RoadNetwork& roads, const TrainNetwork& railway)
{
	if (!m_dirty && m_edgeCount == roads.edges().size() && m_stationCount == railway.nodes().size())
	{
		return;
	}
	m_dirty = false;
	m_edgeCount = roads.edges().size();
	m_stationCount = railway.nodes().size();
	m_edges.clear();
	m_stations.clear();
	for (const auto& edge : roads.edges())
	{
		if (edge.id < 0)
		{
			continue;
		}
		const auto curve = roads.getBezier(edge.id);
		if (!curve)
		{
			continue;
		}
		bool covered = false;
		const int samples = Max(2, static_cast<int>(Ceil(curve->totalLength / kEdgeSampleLength)));
		for (int i = 0; i <= samples; ++i)
		{
			covered |= below(curve->positionAt(curve->totalLength * i / samples), world);
		}
		if (!covered)
		{
			continue;
		}
		Item item;
		item.center = curve->positionAt(curve->totalLength * .5f);
		const ColorF color = edge.isRoadbedBuilt()
								 ? (edge.hasRoadLanes() ? ColorF{.26, .29, .32} : ColorF{.40, .43, .45})
								 : ColorF{.18, .65, .72};
		add(item, clip(RoadGeometry::roadbedSurface(edge, *curve, world), world), color);
		if (edge.hasRailLanes())
		{
			add(item, clip(TrainRenderer::trackGeometry(edge, *curve), world), ColorF{.72, .77, .79});
		}
		if (!item.parts.isEmpty())
		{
			m_edges.emplace(edge.id, std::move(item));
		}
	}
	for (const auto& node : railway.nodes())
	{
		if (node.type != TrackNodeType::Station || node.stationKind != StationKind::Underground)
		{
			continue;
		}
		Item item;
		item.center = node.position;
		const auto geometry = RailFacilities::station(railway, world, node.id);
		for (size_t material = 0; material < RailFacilities::Count; ++material)
		{
			add(item, clip(geometry.parts[material], world), RailFacilities::color(material));
		}
		m_stations.emplace(node.id, std::move(item));
	}
}

void SubsurfaceView::draw(Vec3 eye) const
{
	const ScopedRenderStates3D culling{RasterizerState::SolidCullNone};
	for (const auto* items : {&m_edges, &m_stations})
	{
		for (const auto& [id, item] : *items)
		{
			if (item.center.distanceFrom(eye) > kDrawDistance + item.radius)
			{
				continue;
			}
			for (const auto& part : item.parts)
			{
				part.mesh.draw(part.color);
			}
		}
	}
}
void SubsurfaceView::drawSelection(int id, bool station, ColorF color) const
{
	const auto& items = station ? m_stations : m_edges;
	const auto found = items.find(id);
	if (found == items.end())
	{
		return;
	}
	const ScopedRenderStates3D culling{RasterizerState::SolidCullNone};
	for (const auto& part : found->second.parts)
	{
		part.mesh.draw(color);
	}
}
Optional<SubsurfaceView::Hit> SubsurfaceView::hit(const Ray& ray) const
{
	Optional<Hit> best;
	for (int kind = 0; kind < 2; ++kind)
	{
		const auto& items = kind == 1 ? m_stations : m_edges;
		for (const auto& [id, item] : items)
		{
			if (!Sphere{item.center, item.radius}.intersects(ray))
			{
				continue;
			}
			for (const auto& part : item.parts)
			{
				if (const auto distance = hitDistance(part.data, ray);
					distance && (!best || *distance < best->distance))
				{
					best = Hit{Vec3{ray.point_at(static_cast<float>(*distance))}, kind == 0 ? Optional<int>{id} : none,
						kind == 1 ? Optional<int>{id} : none, *distance};
				}
			}
		}
	}
	return best;
}
Array<MeshData> SubsurfaceView::stationGeometry(
	const World& world, const TrainNetwork& railway, int id, bool underground)
{
	Array<MeshData> result;
	const auto geometry = RailFacilities::station(railway, world, id);
	for (const auto& part : geometry.parts)
	{
		auto clipped = clip(part, world, underground);
		if (!clipped.indices.isEmpty())
		{
			result << std::move(clipped);
		}
	}
	return result;
}
Optional<SubsurfaceView::Hit> SubsurfaceView::stationHit(
	const World& world, const TrainNetwork& railway, const Ray& ray, bool underground)
{
	Optional<Hit> best;
	for (const auto& node : railway.nodes())
	{
		if (node.type != TrackNodeType::Station || (underground && node.stationKind != StationKind::Underground))
		{
			continue;
		}
		const Vec3 center = !underground && node.entrance ? *node.entrance : node.position;
		if (!Sphere{center, kStationPickRadius}.intersects(ray))
		{
			continue;
		}
		for (const auto& data : stationGeometry(world, railway, node.id, underground))
		{
			if (const auto distance = hitDistance(data, ray); distance && (!best || *distance < best->distance))
			{
				best = Hit{Vec3{ray.point_at(static_cast<float>(*distance))}, none, node.id, *distance};
			}
		}
	}
	return best;
}

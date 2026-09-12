#include "JunctionGeometry.hpp"
#include "RoadGeometry.hpp"
#include "../debug/DebugLog.hpp"

namespace JunctionGeometry
{
	namespace
	{
		struct Approach
		{
			const RoadEdge* edge = nullptr;
			Vec3 center, direction, right;
			double left = 0.0, rightOffset = 0.0, angle = 0.0;
			float fraction = 0.0f;
			bool reversed = false;
		};
		double cross(const Vec2& a, const Vec2& b) { return a.x * b.y - a.y * b.x; }
		Vertex3D vertex(const Vec3& position)
		{
			return Vertex3D{ Float3{ position }, Float3{ 0, 1, 0 }, Float2{ static_cast<float>(position.x * 0.5), static_cast<float>(position.z * 0.5) } };
		}
		Array<Band> sideBands(const Approach& approach, bool leftSide)
		{
			Array<Band> bands;
			for (const auto& part : approach.edge->parts)
			{
				if (!RoadGeometry::isStructuralStrip(part) || part.type == RoadPartType::Roadbed) { continue; }
				double left = RoadGeometry::partOffsetAt(part, approach.fraction, true);
				double right = RoadGeometry::partOffsetAt(part, approach.fraction, false);
				if (approach.reversed) { const double oldLeft = left; left = -right; right = -oldLeft; }
				const double near = leftSide ? approach.left - right : left - approach.rightOffset;
				const double far = leftSide ? approach.left - left : right - approach.rightOffset;
				if (near < -0.01 || far <= near + 0.001) { continue; }
				Band band;
				band.part = part;
				band.nearStart = Max(0.0, near);
				band.farStart = far;
				bands << band;
			}
			bands.sort_by([](const Band& a, const Band& b) { return a.nearStart < b.nearStart; });
			return bands;
		}
	}

	Vec3 bandPosition(const Section& section, const Band& band, double across)
	{
		const double near = Math::Lerp(band.nearStart, band.nearEnd, section.fraction);
		const double far = Math::Lerp(band.farStart, band.farEnd, section.fraction);
		return section.position + section.outward * Math::Lerp(near, far, across);
	}

	Layout build(const RoadNetwork& network, int nodeId, bool onlyOpenEdges)
	{
		Layout layout;
		const auto* node = network.getNode(nodeId);
		if (!node) { return layout; }
		Array<Approach> approaches;
		for (const auto& attachment : node->attachments)
		{
			const auto* edge = network.getEdge(attachment.edgeId);
			if (!edge || (onlyOpenEdges && edge->edgeState != EdgeState::Open && edge->edgeState != EdgeState::Existing)) { continue; }
			const auto bezier = network.getBezier(edge->id);
			if (!bezier || bezier->totalLength < 0.1f) { continue; }
			Approach approach;
			approach.edge = edge;
			approach.reversed = edge->nodeB == nodeId;
			const float arc = approach.reversed ? Min(bezier->totalLength, bezier->totalLength - edge->cutoffB + 0.1f) : Max(0.0f, edge->cutoffA - 0.1f);
			approach.fraction = arc / bezier->totalLength;
			const auto range = RoadGeometry::roadbedRangeAt(*edge, approach.fraction);
			if (!range.valid) { continue; }
			approach.center = bezier->positionAt(arc);
			approach.center.y += kRoadSurfaceLift;
			Vec3 direction = bezier->tangentAt(arc) * (approach.reversed ? -1.0 : 1.0);
			direction.y = 0.0;
			if (direction.lengthSq() < 1e-8) { continue; }
			approach.direction = direction.normalized();
			approach.right = tangentToRight(approach.direction);
			approach.left = approach.reversed ? -range.right : range.left;
			approach.rightOffset = approach.reversed ? -range.left : range.right;
			approach.angle = Math::Atan2(direction.z, direction.x);
			approaches << approach;
			layout.elevated = layout.elevated || edge->useElevation;
		}
		if (approaches.size() < 2) { return layout; }
		approaches.sort_by([](const Approach& a, const Approach& b) { return a.angle < b.angle; });
		Array<Vec3> outline;
		for (size_t index = 0; index < approaches.size(); ++index)
		{
			const Approach& a = approaches[index];
			const Approach& b = approaches[(index + 1) % approaches.size()];
			Corner corner;
			corner.bands = sideBands(a, true);
			for (const auto& endBand : sideBands(b, false))
			{
				auto found = std::find_if(corner.bands.begin(), corner.bands.end(), [&](const Band& band) { return band.part.type == endBand.part.type && band.farEnd == 0.0; });
				if (found == corner.bands.end())
				{
					Band band;
					band.part = endBand.part;
					band.nearEnd = endBand.nearStart;
					band.farEnd = endBand.farStart;
					corner.bands << band;
				}
				else { found->nearEnd = endBand.nearStart; found->farEnd = endBand.farStart; }
			}
			// Missing strips taper at their neighbouring strip boundary, not at the
			// asphalt edge, so a disappearing sidewalk cannot overlap a continuing gutter.
			for (auto& band : corner.bands)
			{
				if (band.farEnd == band.nearEnd)
				{
					double anchor = 0.0;
					for (const auto& other : corner.bands)
					{
						if (&other != &band && other.farStart <= band.nearStart + 0.001) { anchor = Max(anchor, other.farEnd); }
					}
					band.nearEnd = band.farEnd = anchor;
				}
				if (band.farStart == band.nearStart)
				{
					double anchor = 0.0;
					for (const auto& other : corner.bands)
					{
						if (&other != &band && other.farEnd <= band.nearEnd + 0.001) { anchor = Max(anchor, other.farStart); }
					}
					band.nearStart = band.farStart = anchor;
				}
			}
			const Vec3 start = a.center + a.right * a.left;
			const Vec3 end = b.center + b.right * b.rightOffset;
			const Vec2 p0{ start.x, start.z }, p3{ end.x, end.z };
			const Vec2 da{ a.direction.x, a.direction.z }, db{ b.direction.x, b.direction.z };
			Array<Vec2> points;
			auto appendLine = [&](Vec2 from, Vec2 to)
			{
				const int segments = Max(1, static_cast<int>(Ceil(from.distanceFrom(to) / 3.0)));
				for (int i = 0; i < segments; ++i) { points << from.lerp(to, i / static_cast<double>(segments)); }
			};
			double gap = b.angle - a.angle;
			if (gap <= 0.0) { gap += Math::TwoPi; }
			const double denominator = cross(da, db);
			bool rounded = false;
			if (Abs(denominator) > 1e-5 && gap < Math::Pi - 0.01)
			{
				const double alongA = cross(p0 - p3, db) / denominator;
				const double alongB = cross(p0 - p3, da) / denominator;
				if (alongA > 0.05 && alongB > 0.05)
				{
					double roadsideWidth = 0.0;
					for (const auto& band : corner.bands) { roadsideWidth = Max(roadsideWidth, Max(band.farStart, band.farEnd)); }
					const double radius = Min(Max(4.0, roadsideWidth + 2.0), Min(alongA, alongB) * Math::Tan(gap * 0.5) * 0.9);
					const double trim = radius / Math::Tan(gap * 0.5);
					const Vec2 intersection = p0 - da * alongA;
					const Vec2 q0 = intersection + da * trim, q3 = intersection + db * trim;
					appendLine(p0, q0);
					const double handle = radius * (4.0 / 3.0) * Math::Tan((Math::Pi - gap) * 0.25);
					const Vec2 q1 = q0 - da * handle, q2 = q3 - db * handle;
					for (int i = 0; i < 12; ++i)
					{
						const double t = i / 12.0, u = 1.0 - t;
						points << q0 * (u*u*u) + q1 * (3*u*u*t) + q2 * (3*u*t*t) + q3 * (t*t*t);
					}
					appendLine(q3, p3);
					rounded = true;
				}
			}
			if (!rounded)
			{
				// Opposing approaches have a straight kerb, including the back of a T junction.
				if (Abs(gap - Math::Pi) < 0.02) { appendLine(p0, p3); }
				else
				{
					const double handle = p0.distanceFrom(p3) / 3.0;
					const Vec2 p1 = p0 - da * handle, p2 = p3 - db * handle;
					for (int i = 0; i < 16; ++i)
					{
						const double t = i / 16.0, u = 1.0 - t;
						points << p0 * (u*u*u) + p1 * (3*u*u*t) + p2 * (3*u*t*t) + p3 * (t*t*t);
					}
				}
			}
			points << p3;
			double total = 0.0;
			for (size_t i = 1; i < points.size(); ++i) { total += points[i].distanceFrom(points[i-1]); }
			double distance = 0.0;
			for (size_t i = 0; i < points.size(); ++i)
			{
				if (i > 0) { distance += points[i].distanceFrom(points[i-1]); }
				const double fraction = total > 1e-6 ? distance / total : 0.0;
				Vec3 outward;
				if (i == 0) { outward = -a.right; }
				else if (i + 1 == points.size()) { outward = b.right; }
				else
				{
					const Vec2 tangent = points[i+1] - points[i-1];
					outward = tangent.lengthSq() > 1e-10 ? Vec3{ tangent.y, 0, -tangent.x }.normalized() : -a.right;
				}
				const Vec3 position{ points[i].x, Math::Lerp(start.y, end.y, fraction), points[i].y };
				corner.sections << Section{ position, outward, fraction };
				if (outline.isEmpty() || outline.back().distanceFromSq(position) > 1e-8) { outline << position; }
			}
			layout.corners << std::move(corner);
		}
		Array<Vec2> localOutline;
		for (const auto& p : outline) { localOutline << Vec2{ p.x - node->position.x, p.z - node->position.z }; }
		if (!Geometry2D::IsClockwise(localOutline)) { localOutline.reverse(); outline.reverse(); }
		const Polygon polygon{ localOutline };
		Array<Polygon> regions;
		if (polygon) { regions << polygon; }
		else
		{
			layout.repaired = true;
			regions = Polygon::Correct(localOutline);
			// Folded or overlapping approaches can produce several valid regions. Keep
			// every region, and include each cut mouth so repair cannot drop a road end.
			for (const auto& approach : approaches)
			{
				const Vec3 left = approach.center + approach.right*approach.left;
				const Vec3 right = approach.center + approach.right*approach.rightOffset;
				const Vec3 innerLeft = node->position + approach.right*approach.left;
				const Vec3 innerRight = node->position + approach.right*approach.rightOffset;
				Array<Vec2> support{ Vec2{left.x-node->position.x,left.z-node->position.z}, Vec2{right.x-node->position.x,right.z-node->position.z},
					Vec2{innerRight.x-node->position.x,innerRight.z-node->position.z}, Vec2{innerLeft.x-node->position.x,innerLeft.z-node->position.z} };
				if (!Geometry2D::IsClockwise(support)) { support.reverse(); }
				const Polygon strip{ support };
				if (!strip || strip.area() < 1e-6) { continue; }
				Polygon pending = strip;
				for (size_t i = 0; i < regions.size();)
				{
					const auto united = Geometry2D::Or(pending, regions[i]);
					if (united.size() == 1)
					{
						pending = united.front();
						regions.erase(regions.begin()+i);
						i = 0;
					}
					else { ++i; }
				}
				regions << pending;
			}
			static HashSet<int> reported;
			if (reported.insert(nodeId).second)
			{
				DBG_LOG(U"[JunctionGeometry] repaired node={} approaches={} regions={} sourceFailure={}"_fmt(nodeId, approaches.size(), regions.size(), static_cast<int>(Polygon::Validate(localOutline))));
			}
		}
		for (const auto& region : regions)
		{
			const uint32 base = static_cast<uint32>(layout.asphalt.vertices.size());
			for (const auto& p : region.vertices())
			{
				// Repair adds intersection vertices. Interpolate along their nearest source
				// boundary segment so elevated cut mouths retain the authored grade.
				double y = node->position.y + kRoadSurfaceLift, best = Math::Inf;
				for (size_t i = 0; i < localOutline.size(); ++i)
				{
					const size_t next = (i+1)%localOutline.size();
					const Vec2 segment = localOutline[next]-localOutline[i];
					const double fraction = segment.lengthSq() > 1e-12 ? Clamp((Vec2{p}-localOutline[i]).dot(segment)/segment.lengthSq(),0.0,1.0) : 0.0;
					const double distance = localOutline[i].lerp(localOutline[next],fraction).distanceFromSq(Vec2{p});
					if (distance < best) { best = distance; y = Math::Lerp(outline[i].y,outline[next].y,fraction); }
				}
				layout.asphalt.vertices << vertex(Vec3{ node->position.x+p.x,y,node->position.z+p.y });
			}
			for (const auto& triangle : region.indices())
			{
				layout.asphalt.indices << TriangleIndex32{ base+triangle.i0,base+triangle.i1,base+triangle.i2 };
			}
		}
		return layout;
	}

	MeshData terrainFootprint(const Layout& layout)
	{
		// Triangulate the outer envelope once. Overlapping asphalt/curb/gutter triangles
		// repeatedly split the same terrain fragments and cause exponential work.
		MeshData mesh;
		if (layout.corners.isEmpty() || layout.asphalt.vertices.isEmpty()) { return mesh; }
		const Vec3 origin = layout.corners.front().sections.front().position;
		Array<Vec2> outline;
		Array<Vec3> positions;
		for (const auto& corner : layout.corners)
		{
			for (const auto& section : corner.sections)
			{
				double distance = 0.0;
				for (const auto& band : corner.bands)
				{
					distance = Max(distance, Math::Lerp(band.farStart, band.farEnd, section.fraction));
				}
				const Vec3 position = section.position + section.outward * distance;
				const Vec2 point{ position.x-origin.x, position.z-origin.z };
				if (!outline.isEmpty() && outline.back().distanceFromSq(point) < 1e-8) { continue; }
				outline << point;
				positions << position;
			}
		}
		if (!Geometry2D::IsClockwise(outline)) { outline.reverse(); positions.reverse(); }
		const Polygon polygon{ outline };
		const Array<Polygon> regions = polygon ? Array<Polygon>{ polygon } : Polygon::Correct(outline);
		if (regions.isEmpty()) { return layout.asphalt; }
		for (const auto& region : regions)
		{
			const uint32 base = static_cast<uint32>(mesh.vertices.size());
			for (const auto& point : region.vertices())
			{
				double best = Math::Inf;
				Vec3 position;
				for (size_t i = 0; i < outline.size(); ++i)
				{
					const double distance = outline[i].distanceFromSq(Vec2{ point });
					if (distance < best) { best = distance; position = positions[i]; }
				}
				position.x = origin.x + point.x;
				position.z = origin.z + point.y;
				mesh.vertices << vertex(position);
			}
			for (const auto& triangle : region.indices())
			{
				mesh.indices << TriangleIndex32{ base+triangle.i0,base+triangle.i1,base+triangle.i2 };
			}
		}
		if (layout.repaired)
		{
			const uint32 base=static_cast<uint32>(mesh.vertices.size());
			mesh.vertices.append(layout.asphalt.vertices);
			for (const auto& triangle : layout.asphalt.indices)
			{
				mesh.indices << TriangleIndex32{base+triangle.i0,base+triangle.i1,base+triangle.i2};
			}
		}
		return mesh;
	}
}

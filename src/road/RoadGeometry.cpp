#include "RoadGeometry.hpp"
#include "../debug/DebugLog.hpp"
#include "BezierUtil.hpp"
#include "../world/World.hpp"

namespace RoadGeometry
{
	Optional<SignalAnchor> signalAnchor(const RoadEdge& edge, const CubicBezier& bezier, int nodeId)
	{
		if (nodeId != edge.nodeA && nodeId != edge.nodeB) { return none; }
		const bool isNodeA = nodeId == edge.nodeA;
		const float arc = isNodeA ? edge.cutoffA : bezier.totalLength - edge.cutoffB;
		const Vec3 roadPosition = bezier.positionAt(arc);
		const Vec3 tangent = bezier.tangentAt(arc);
		const Vec3 facing = isNodeA ? tangent : -tangent;
		const Vec3 right = tangentToRight(tangent);

		// A/B端の断面を参照する。路肩・歩道幅を車道端と取り違えないよう Roadbed のみを調べる。
		// 描画とクリック選択を同じ計算に通し、非対称の道路や端点ごとの幅変更でも一致させる。
		Optional<float> offset;
		for (const auto& part : edge.parts)
		{
			if (part.type != RoadPartType::Roadbed) { continue; }
			const float candidate = isNodeA ? part.offsetA_L : part.offsetB_R;
			if (!offset) { offset = candidate; }
			else { offset = isNodeA ? Min(*offset, candidate) : Max(*offset, candidate); }
		}
		const float lateral = offset.value_or((isNodeA ? -1.0f : 1.0f) * edge.totalWidth() * 0.5f);
		return SignalAnchor{roadPosition,
			{roadPosition.x - right.x * lateral, roadPosition.y, roadPosition.z - right.z * lateral},
			static_cast<float>(Math::Atan2(facing.x, facing.z))};
	}

	bool isStructuralStrip(const RoadPart& part)
	{
		return part.build == BuildState::Built
			&& part.placement == RoadPartPlacement::Strip
			&& part.envelopeRole == RoadPartEnvelopeRole::Structural;
	}

	bool isRenderableStrip(const RoadPart& part)
	{
		return part.build == BuildState::Built
			&& part.placement == RoadPartPlacement::Strip
			&& part.type != RoadPartType::RoadsideObject
			&& part.type != RoadPartType::UtilityPole;
	}

	bool isRoadOwnedObject(const RoadPart& part)
	{
		return part.build == BuildState::Built
			&& part.envelopeRole == RoadPartEnvelopeRole::RoadOwnedObject;
	}

	float partOffsetAt(const RoadPart& part, float t, bool leftSide)
	{
		return leftSide
			? Math::Lerp(part.offsetA_L, part.offsetB_L, t)
			: Math::Lerp(part.offsetA_R, part.offsetB_R, t);
	}

	LateralRange structuralRangeAt(const RoadEdge& edge, float t)
	{
		LateralRange range;
		for (const auto& part : edge.parts)
		{
			if (!isStructuralStrip(part))
			{
				continue;
			}

			const float left = partOffsetAt(part, t, true);
			const float right = partOffsetAt(part, t, false);
			if (!range.valid)
			{
				range.left = Min(left, right);
				range.right = Max(left, right);
				range.valid = true;
			}
			else
			{
				range.left = Min(range.left, Min(left, right));
				range.right = Max(range.right, Max(left, right));
			}
		}
		return range;
	}

	LateralRange roadbedRangeAt(const RoadEdge& edge, float t)
	{
		LateralRange range;
		for (const auto& part : edge.parts)
		{
			if (!isRenderableStrip(part) || part.type != RoadPartType::Roadbed)
			{
				continue;
			}

			const float left = partOffsetAt(part, t, true);
			const float right = partOffsetAt(part, t, false);
			if (!range.valid)
			{
				range.left = Min(left, right);
				range.right = Max(left, right);
				range.valid = true;
			}
			else
			{
				range.left = Min(range.left, Min(left, right));
				range.right = Max(range.right, Max(left, right));
			}
		}
		return range;
	}

	float structuralWidth(const RoadEdge& edge)
	{
		const LateralRange range = structuralRangeAt(edge, 0.5f);
		return range.width();
	}

	double surfaceY(const RoadEdge& edge, const Vec3& roadPosition, double terrainHeight)
	{
		return (edge.usesDesignHeight() ? roadPosition.y : terrainHeight) + kRoadSurfaceLift;
	}

	double markingY(const RoadEdge& edge, const Vec3& roadPosition, double terrainHeight)
	{
		return (edge.usesDesignHeight() ? roadPosition.y : terrainHeight) + kRoadLineLift;
	}

	double furnitureBaseY(const RoadEdge& edge, const Vec3& roadPosition, double terrainHeight)
	{
		return surfaceY(edge, roadPosition, terrainHeight);
	}
	MeshData roadbedSurface(const RoadEdge& edge, const CubicBezier& bezier, const World& world)
	{
		MeshData mesh;
		const float start = Max(0.0f, edge.cutoffA - 0.1f);
		const float end = Min(bezier.totalLength, bezier.totalLength - edge.cutoffB + 0.1f);
		const float span = end - start;
		if (span <= 0.1f) { return mesh; }
		const int count = Clamp(static_cast<int>(span / 2.0f) + 1, 3, 100);
		for (const auto& part : edge.parts)
		{
			if (!isRenderableStrip(part) || part.type != RoadPartType::Roadbed) { continue; }
			const uint32 base = static_cast<uint32>(mesh.vertices.size());
			for (int i = 0; i <= count; ++i)
			{
				const float arc = start + (i / static_cast<float>(count)) * span;
				const Vec3 center = bezier.positionAt(arc);
				const Vec3 right = tangentToRight(bezier.tangentAt(arc));
				for (const bool left : { true, false })
				{
					Vec3 position = center + right * partOffsetAt(part, arc / bezier.totalLength, left);
					position.y = surfaceY(edge, center, world.sampleHeight(static_cast<float>(center.x), static_cast<float>(center.z)));
					mesh.vertices << Vertex3D{ Float3{ position }, Float3{ 0, 1, 0 }, Float2{ 0, 0 } };
				}
			}
			for (int i = 0; i < count; ++i)
			{
				const uint32 index = base + i * 2;
				mesh.indices << TriangleIndex32{ index, index+2, index+1 } << TriangleIndex32{ index+1, index+2, index+3 };
			}
		}
		return mesh;
	}

	MeshData projectMarking(const MeshData& paint, const MeshData& surface)
	{
		MeshData result;
		auto cross = [](Vec2 a, Vec2 b) { return a.x * b.y - a.y * b.x; };
		for (const auto& markingTriangle : paint.indices)
		{
			const auto& p0 = paint.vertices[markingTriangle.i0].pos;
			const auto& p1 = paint.vertices[markingTriangle.i1].pos;
			const auto& p2 = paint.vertices[markingTriangle.i2].pos;
			const double minX = Min(Min(p0.x, p1.x), p2.x), maxX = Max(Max(p0.x, p1.x), p2.x);
			const double minZ = Min(Min(p0.z, p1.z), p2.z), maxZ = Max(Max(p0.z, p1.z), p2.z);
			for (const auto& roadTriangle : surface.indices)
			{
				const Float3 a = surface.vertices[roadTriangle.i0].pos;
				const Float3 b = surface.vertices[roadTriangle.i1].pos;
				const Float3 c = surface.vertices[roadTriangle.i2].pos;
				if (maxX < Min(Min(a.x,b.x),c.x) || minX > Max(Max(a.x,b.x),c.x)
					|| maxZ < Min(Min(a.z,b.z),c.z) || minZ > Max(Max(a.z,b.z),c.z)) { continue; }
				// Work relative to the triangle origin to retain centimetre precision at map edges.
				const Vec2 origin{ a.x, a.z }, ab{ b.x-a.x, b.z-a.z }, ac{ c.x-a.x, c.z-a.z };
				const double area = cross(ab, ac);
				if (Abs(area) < 1e-8) { continue; }
				Array<Vec2> clipped{ Vec2{ p0.x,p0.z }-origin, Vec2{ p1.x,p1.z }-origin, Vec2{ p2.x,p2.z }-origin };
				const Array<Vec2> triangle{ Vec2{0,0}, ab, ac };
				const double sign = area > 0.0 ? 1.0 : -1.0;
				for (int side = 0; side < 3 && !clipped.isEmpty(); ++side)
				{
					const Vec2 first = triangle[side], direction = triangle[(side+1)%3]-first;
					Array<Vec2> output;
					Vec2 previous = clipped.back();
					double previousDistance = sign * cross(direction, previous-first);
					for (const auto& current : clipped)
					{
						const double distance = sign * cross(direction, current-first);
						if ((distance >= 0.0) != (previousDistance >= 0.0))
						{
							output << previous.lerp(current, previousDistance / (previousDistance-distance));
						}
						if (distance >= 0.0) { output << current; }
						previous = current;
						previousDistance = distance;
					}
					clipped = std::move(output);
				}
				if (clipped.size() < 3) { continue; }
				const uint32 base = static_cast<uint32>(result.vertices.size());
				for (const auto& point : clipped)
				{
					const double u = cross(point, ac)/area, v = cross(ab, point)/area;
					const double y = a.y + u*(b.y-a.y) + v*(c.y-a.y) + (kRoadLineLift-kRoadSurfaceLift);
					result.vertices << Vertex3D{ Float3{ static_cast<float>(origin.x+point.x), static_cast<float>(y), static_cast<float>(origin.y+point.y) }, Float3{ 0,1,0 }, Float2{ 0,0 } };
				}
				for (uint32 i = 1; i+1 < clipped.size(); ++i)
				{
					TriangleIndex32 triangleIndex{ base, base+i, base+i+1 };
					const Float3 normal = (result.vertices[base+i].pos-result.vertices[base].pos).cross(result.vertices[base+i+1].pos-result.vertices[base].pos);
					if (normal.y < 0.0f) { std::swap(triangleIndex.i1, triangleIndex.i2); }
					if (Abs(normal.y) > 1e-8f) { result.indices << triangleIndex; }
				}
			}
		}
		return result;
	}
	MeshData excludeSurface(const MeshData& strips, const MeshData& pavement)
	{
		const Stopwatch timer{StartImmediately::Yes};
		MeshData result;
		auto cross = [](Vec2 a, Vec2 b) { return a.x*b.y - a.y*b.x; };
		auto hasArea = [](const Array<Vertex3D>& polygon)
		{
			if (polygon.size() < 3) { return false; }
			for (size_t i = 1; i+1 < polygon.size(); ++i)
			{
				if ((polygon[i].pos-polygon[0].pos).cross(polygon[i+1].pos-polygon[0].pos).lengthSq() > 1e-12f) { return true; }
			}
			return false;
		};
		// A strip triangle can only be affected by mask triangles in its XZ bounds.
		// Index once instead of re-testing the full neighbouring road for every face.
		constexpr double kClipCellSize = 16.0;
		HashTable<int64,Array<uint32>> cells;
		auto cellKey = [](int x, int z) { return static_cast<int64>((static_cast<uint64>(static_cast<uint32>(x))<<32)|static_cast<uint32>(z)); };
		auto bounds = [](const MeshData& mesh, const TriangleIndex32& triangle)
		{
			const auto a = mesh.vertices[triangle.i0].pos, b = mesh.vertices[triangle.i1].pos, c = mesh.vertices[triangle.i2].pos;
			return RectF{Min(Min(a.x,b.x),c.x),Min(Min(a.z,b.z),c.z),Max(Max(a.x,b.x),c.x)-Min(Min(a.x,b.x),c.x),Max(Max(a.z,b.z),c.z)-Min(Min(a.z,b.z),c.z)};
		};
		for (uint32 index = 0; index < pavement.indices.size(); ++index)
		{
			const RectF box = bounds(pavement,pavement.indices[index]);
			for (int z = static_cast<int>(Floor(box.y/kClipCellSize)); z <= static_cast<int>(Floor((box.y+box.h)/kClipCellSize)); ++z)
			{
				for (int x = static_cast<int>(Floor(box.x/kClipCellSize)); x <= static_cast<int>(Floor((box.x+box.w)/kClipCellSize)); ++x) { cells[cellKey(x,z)] << index; }
			}
		}
		for (const auto& sourceTriangle : strips.indices)
		{
			Array<Array<Vertex3D>> fragments{ Array<Vertex3D>{ strips.vertices[sourceTriangle.i0], strips.vertices[sourceTriangle.i1], strips.vertices[sourceTriangle.i2] } };
			const RectF box = bounds(strips,sourceTriangle);
			Array<uint32> candidates;
			for (int z = static_cast<int>(Floor(box.y/kClipCellSize)); z <= static_cast<int>(Floor((box.y+box.h)/kClipCellSize)); ++z)
			{
				for (int x = static_cast<int>(Floor(box.x/kClipCellSize)); x <= static_cast<int>(Floor((box.x+box.w)/kClipCellSize)); ++x)
				{
					if (const auto found = cells.find(cellKey(x,z)); found != cells.end()) { candidates.append(found->second); }
				}
			}
			candidates.sort();
			candidates.erase(std::unique(candidates.begin(),candidates.end()),candidates.end());
			for (const uint32 maskIndex : candidates)
			{
				const auto& maskTriangle = pavement.indices[maskIndex];
				const RectF maskBounds = bounds(pavement,maskTriangle);
				if (box.x+box.w < maskBounds.x || box.x > maskBounds.x+maskBounds.w
					|| box.y+box.h < maskBounds.y || box.y > maskBounds.y+maskBounds.h) { continue; }
				const auto a = pavement.vertices[maskTriangle.i0].pos;
				const auto b = pavement.vertices[maskTriangle.i1].pos;
				const auto c = pavement.vertices[maskTriangle.i2].pos;
				const Array<Vec2> mask{ Vec2{a.x,a.z},Vec2{b.x,b.z},Vec2{c.x,c.z} };
				const double area = cross(mask[1]-mask[0],mask[2]-mask[0]);
				if (Abs(area) < 1e-8) { continue; }
				const double sign = area > 0.0 ? 1.0 : -1.0;
				Array<Array<Vertex3D>> outside;
				for (const auto& fragment : fragments)
				{
					double minX=Math::Inf,minZ=Math::Inf,maxX=-Math::Inf,maxZ=-Math::Inf;
					for (const auto& vertex : fragment)
					{
						minX=Min(minX,static_cast<double>(vertex.pos.x)); maxX=Max(maxX,static_cast<double>(vertex.pos.x));
						minZ=Min(minZ,static_cast<double>(vertex.pos.z)); maxZ=Max(maxZ,static_cast<double>(vertex.pos.z));
					}
					if (maxX < Min(Min(a.x,b.x),c.x) || minX > Max(Max(a.x,b.x),c.x)
						|| maxZ < Min(Min(a.z,b.z),c.z) || minZ > Max(Max(a.z,b.z),c.z)) { outside << fragment; continue; }
					Array<Vertex3D> remaining=fragment;
					for (int side=0; side<3 && remaining.size()>=3; ++side)
					{
						const Vec2 origin=mask[side],direction=mask[(side+1)%3]-origin;
						auto distance = [&](const Vertex3D& vertex) { return sign*cross(direction,Vec2{vertex.pos.x,vertex.pos.z}-origin); };
						Array<Vertex3D> kept,inside;
						Vertex3D previous=remaining.back();
						double previousDistance=distance(previous);
						for (const auto& current : remaining)
						{
							const double currentDistance=distance(current);
							if ((currentDistance>1e-5)!=(previousDistance>1e-5))
							{
								const float fraction=static_cast<float>(Clamp(previousDistance/(previousDistance-currentDistance),0.0,1.0));
								Vertex3D hit=previous;
								hit.pos=previous.pos+(current.pos-previous.pos)*fraction;
								hit.normal=previous.normal+(current.normal-previous.normal)*fraction;
								hit.tex=previous.tex+(current.tex-previous.tex)*fraction;
								kept << hit; inside << hit;
							}
							if (currentDistance>1e-5) { inside << current; } else { kept << current; }
							previous=current; previousDistance=currentDistance;
						}
						if (hasArea(kept)) { outside << std::move(kept); }
						remaining=std::move(inside);
						if (!hasArea(remaining)) { remaining.clear(); }
					}
				}
				fragments=std::move(outside);
				if (fragments.isEmpty()) { break; }
			}
			for (const auto& fragment : fragments)
			{
				const uint32 base=static_cast<uint32>(result.vertices.size());
				result.vertices.append(fragment);
				for (uint32 i=1;i+1<fragment.size();++i)
				{
					if ((fragment[i].pos-fragment[0].pos).cross(fragment[i+1].pos-fragment[0].pos).lengthSq()>1e-12f)
					{
						result.indices << TriangleIndex32{base,base+i,base+i+1};
					}
				}
			}
		}
		if (timer.msF() > 5.0) { DBG_LOG(U"[RoadClip] input={} mask={} output={} ms={:.2f}"_fmt(strips.indices.size(),pavement.indices.size(),result.indices.size(),timer.msF())); }
		return result;
	}
}

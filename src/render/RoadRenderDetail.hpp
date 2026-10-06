#pragma once
#include "RoadRenderer.hpp"
#include "MeshGeometry.hpp"
#include "../road/RoadGeometry.hpp"

/// @brief 道路描画実装だけで共有する断面・頂点・重なりの計算。
namespace RoadRenderDetail
{
	using MeshGeometry::appendMeshData;
	struct SliceInfo { Vec3 center; Vec3 right; };
	struct StripRange { float sStart; float sEnd; };
	constexpr float kWorldUV = 0.5f;

	inline SliceInfo makeSlice(const CubicBezier& bez, const World& world,
	                    float s, double terrainLift, bool useElevation = false)
	{
		const Vec3  p  = bez.positionAt(s);
		double y;
		if (useElevation)
			y = p.y + terrainLift;  // ノード Y は地形高さなので、通常と同じ terrainLift を加算
		else
		{
			const float gy = world.sampleHeight(static_cast<float>(p.x), static_cast<float>(p.z));
			y = gy + terrainLift;
		}
		return { Vec3{ p.x, y, p.z }, tangentToRight(bez.tangentAt(s)) };
	}

	/// @brief Vertex3D を生成する
	inline Vertex3D makeVert(const Vec3& pos, float u, float v)
	{
		Vertex3D vt;
		vt.pos    = Float3{ static_cast<float>(pos.x), static_cast<float>(pos.y), static_cast<float>(pos.z) };
		vt.normal = Float3{ 0.0f, 1.0f, 0.0f };
		vt.tex    = Float2{ u, v };
		return vt;
	}

	/// @brief ワールド XZ 座標から世界空間 UV を生成
	inline Vertex3D makeVertWorldUV(const Vec3& pos)
	{
		return makeVert(pos,
			static_cast<float>(pos.x) * kWorldUV,
			static_cast<float>(pos.z) * kWorldUV);
	}

	inline Vertex3D makeVertWorldUVWithNormal(const Vec3& pos, const Float3& normal)
	{
		Vertex3D vt = makeVertWorldUV(pos);
		vt.normal = normal;
		return vt;
	}

	/// @brief 四角形を頂点・インデックス配列に追記する（CW ワインディング）
	inline void appendQuad(Array<TriangleIndex32>& indices,
	                uint32 iL0, uint32 iR0, uint32 iL1, uint32 iR1)
	{
		indices << TriangleIndex32{ iL0, iL1, iR0 };
		indices << TriangleIndex32{ iR0, iL1, iR1 };
	}

	/// @brief Imported road parts and junction patches must face their authored normals.
	inline void orientRoadFaces(MeshData& mesh)
	{
		for (auto& triangle : mesh.indices)
		{
			const auto& a = mesh.vertices[triangle.i0];
			const auto& b = mesh.vertices[triangle.i1];
			const auto& c = mesh.vertices[triangle.i2];
			if ((b.pos - a.pos).cross(c.pos - a.pos).dot(a.normal + b.normal + c.normal) < 0.0f)
			{
				std::swap(triangle.i1, triangle.i2);
			}
		}
	}

	inline MeshData adjacentPavement(const RoadNetwork& network, const RoadEdge& edge, const World& world)
	{
		MeshData mask;
		const auto bezier = network.getBezier(edge.id);
		if (!bezier) { return mask; }
		HashSet<int> included;
		for (const int nodeId : { edge.nodeA,edge.nodeB })
		{
			const auto* node = network.getNode(nodeId);
			if (!node || node->attachments.size() < 2) { continue; }
			const bool atStart = edge.nodeA == nodeId;
			Vec3 direction = bezier->tangentAt(atStart ? edge.cutoffA : bezier->totalLength-edge.cutoffB) * (atStart ? 1.0 : -1.0);
			direction.y = 0.0;
			if (direction.lengthSq() < 1e-8) { continue; }
			direction.normalize();
			for (const auto& attachment : node->attachments)
			{
				const auto* other = network.getEdge(attachment.edgeId);
				if (!other || other->id == edge.id || other->useElevation != edge.useElevation || !other->isRoadbedBuilt()
					|| (other->edgeState != EdgeState::Open && other->edgeState != EdgeState::Existing)) { continue; }
				const auto otherBezier = network.getBezier(other->id);
				if (!otherBezier) { continue; }
				const bool otherStart = other->nodeA == nodeId;
				Vec3 otherDirection = otherBezier->tangentAt(otherStart ? other->cutoffA : otherBezier->totalLength-other->cutoffB) * (otherStart ? 1.0 : -1.0);
				otherDirection.y = 0.0;
				if (otherDirection.lengthSq() < 1e-8 || direction.dot(otherDirection.normalized()) < 0.80) { continue; }
				if (included.insert(other->id).second)
				{
					RoadEdge covered = *other;
					if (other->id < edge.id)
					{
						// Give overlapping roadside surfaces one owner to avoid coplanar
						// curb/sidewalk faces fighting after two near-parallel roads merge.
						for (auto& part : covered.parts)
						{
							if (RoadGeometry::isStructuralStrip(part)) { part.type = RoadPartType::Roadbed; }
						}
					}
					appendMeshData(mask,RoadGeometry::roadbedSurface(covered,*otherBezier,world));
				}
			}
		}
		return mask;
	}

	inline bool calcStripRange(float totalLen, float marginA, float marginB, StripRange& out)
	{
		constexpr float kOverlap = 0.1f;
		out.sStart = Max(marginA - kOverlap, 0.0f);
		out.sEnd   = Min(totalLen - marginB + kOverlap, totalLen);
		return (out.sStart < out.sEnd - 0.1f);
	}

}

#include "TreeInstances.hpp"
#include "ShaderAsset.hpp"
#include "TreeGeometry.hpp"
#include "LandscapeMaterials.hpp"
#include "../debug/DebugLog.hpp"

namespace
{
	constexpr uint32 kNearCapacity = TreeInstanceRenderer::kNearCapacity;
	constexpr uint32 kFarCapacity = TreeInstanceRenderer::kFarCapacity;
	struct SharedPart { Mesh mesh; uint32 triangles = 0; };
	struct SharedTree { SharedPart wood, leaves, distant; };
	/// @brief 個体スロットだけをUVへ焼き込み、全チャンクで同じGPUバッチを使う。
	SharedPart pack(const MeshData& source, uint32 capacity)
	{
		if (source.indices.isEmpty()) { return {}; }
		MeshData packed;
		packed.vertices.reserve(source.vertices.size() * capacity);
		packed.indices.reserve(source.indices.size() * capacity);
		for (uint32 instance = 0; instance < capacity; ++instance)
		{
			const uint32 offset = static_cast<uint32>(packed.vertices.size());
			for (auto vertex : source.vertices)
			{
				vertex.tex.x = static_cast<float>(instance);
				packed.vertices << vertex;
			}
			for (const auto& face : source.indices)
			{
				packed.indices << TriangleIndex32{face.i0 + offset, face.i1 + offset, face.i2 + offset};
			}
		}
		return {Mesh{packed}, static_cast<uint32>(source.indices.size())};
	}
	struct SharedTrees
	{
		std::array<SharedTree, TreeInstanceRenderer::kModelCount> models;
		VertexShader nearShader{ShaderAsset::vertex(U"shaders/hlsl/tree_instances.hlsl", U"NearVS")};
		VertexShader farShader{ShaderAsset::vertex(U"shaders/hlsl/tree_instances.hlsl", U"FarVS")};
		SharedTrees()
		{
			const Stopwatch timer{StartImmediately::Yes};
			if (!nearShader || !farShader)
			{
				DBG_LOG(U"[TreeModels] shaderFailed path=shaders/hlsl/tree_instances.hlsl NearVS={} FarVS={}"_fmt(
					static_cast<bool>(nearShader), static_cast<bool>(farShader)));
				throw Error{U"共有樹木シェーダーの読み込みに失敗しました"};
			}
			for (uint32 i = 0; i < models.size(); ++i)
			{
				const bool shrub = i == TreeInstanceRenderer::kShrubModel;
				const auto source = shrub ? TreeGeometry::build(37, false)
					: TreeGeometry::build((i % 8) * 31, (i % 8) >= 4, i >= 8);
				auto& model = models[i];
				if (!shrub)
				{
					model.wood = pack(source.wood, kNearCapacity);
					model.distant = pack(source.distant, kFarCapacity);
				}
				model.leaves = pack(source.leaves, kNearCapacity);
			}
			DBG_LOG(U"[TreeModels] models={} nearCapacity={} farCapacity={} preloadMs={:.2f}"_fmt(
				models.size(), kNearCapacity, kFarCapacity, timer.msF()));
		}
	};
	const SharedTrees& sharedTrees()
	{
		static const SharedTrees result;
		return result;
	}
}

void TreeInstanceRenderer::preload() { (void)sharedTrees(); }

void TreeInstanceRenderer::clear()
{
	for (auto& bucket : m_buckets) { bucket.clear(); }
	m_nearInstances = m_farInstances = 0;
}

Box TreeInstanceRenderer::bounds(const TreeInstance& tree)
{
	const auto& position = tree.transform.positionWidth;
	const double width = Abs(position.w), height = Abs(tree.transform.heightRotation.x);
	// 原型は半径 .6、Y=-.1～1.1 に収まる。風・丸め誤差へ 1 m の余裕を残す。
	constexpr double kExtent = .6, kMargin = 1.0;
	return Box{Vec3{position.x,position.y + height * .5,position.z},
		Vec3{2 * (width * kExtent + kMargin), 2 * (height * kExtent + kMargin), 2 * (width * kExtent + kMargin)}};
}

void TreeInstanceRenderer::append(const Array<TreeInstance>& trees, Point chunk, Vec2 heights, Vec3 eye, double renderDistance)
{
	std::array<bool, 16> near;
	for (int tile = 0; tile < 16; ++tile)
	{
		near[tile] = TreeGeometry::nearTile(1000 * (tile + 1), chunk, eye, heights);
	}
	for (const auto& tree : trees)
	{
		if (renderDistance != RenderDistance::kDefault && !RenderDistance::contains(eye,bounds(tree),renderDistance)) { continue; }
		const bool detailed = near[tree.tile];
		if (!detailed && tree.model == kShrubModel) { continue; }
		const size_t bucket = (tree.model * 2 + tree.palette) * 2 + (detailed ? 1 : 0);
		m_buckets[bucket] << tree.transform;
		if (detailed) { ++m_nearInstances; }
		else { ++m_farInstances; }
	}
}

void TreeInstanceRenderer::draw(const PixelShader& foliage, bool shadowPass)
{
	if (m_nearInstances + m_farInstances == 0) { return; }
	const auto& shared = sharedTrees();
	const ScopedRenderStates3D rasterizer{RasterizerState::SolidCullNone};
	for (size_t bucket = 0; bucket < m_buckets.size(); ++bucket)
	{
		const auto& transforms = m_buckets[bucket];
		if (transforms.isEmpty()) { continue; }
		const auto& model = shared.models[bucket / 4];
		const bool detailed = (bucket % 2) != 0;
		const int palette = static_cast<int>((bucket / 2) % 2);
		const uint32 capacity = detailed ? kNearCapacity : kFarCapacity;
		const ScopedCustomShader3D shader{detailed ? shared.nearShader : shared.farShader};
		for (size_t start = 0; start < transforms.size(); start += capacity)
		{
			const uint32 count = static_cast<uint32>(Min<size_t>(capacity, transforms.size() - start));
			if (detailed)
			{
				for (uint32 i = 0; i < count; ++i) { m_nearInstancesBuffer->values[i] = transforms[start + i]; }
				Graphics3D::SetVSConstantBuffer(4, m_nearInstancesBuffer);
			}
			else
			{
				for (uint32 i = 0; i < count; ++i) { m_farInstancesBuffer->values[i] = transforms[start + i]; }
				Graphics3D::SetVSConstantBuffer(5, m_farInstancesBuffer);
			}
			if (detailed && !model.wood.mesh.isEmpty())
			{
				model.wood.mesh.drawSubset(0, count * model.wood.triangles, LandscapeMaterials::detailColorForKey(105));
			}
			Optional<ScopedCustomShader3D> leavesShader;
			if (!shadowPass && foliage) { leavesShader.emplace(foliage); }
			const auto& leaves = detailed ? model.leaves : model.distant;
			leaves.mesh.drawSubset(0, count * leaves.triangles, LandscapeMaterials::detailColorForKey(125 + palette));
		}
	}
}

#pragma once
#include "ModelBatch.hpp"

/// @brief 無テクスチャの遠景車両専用。頂点は不変、個体の変換行列だけGPUへ送る。
class VehicleInstanceBatch
{
public:
	void clear() { m_transforms.clear(); }
	void append(const ModelMeshSource& source, const Mat4x4& transform)
	{
		if (m_mesh.isEmpty())
		{
			if (!source.material.diffuseTextureName.isEmpty()) { throw Error{U"遠景車両バッチは無テクスチャLODが必要です"}; }
			MeshData packed;
			for (uint32 instance = 0; instance < kCapacity; ++instance)
			{
				const uint32 offset = static_cast<uint32>(packed.vertices.size());
				for (auto vertex : source.geometry.vertices)
				{
					vertex.tex = Float2{static_cast<float>(instance), 0}; packed.vertices << vertex;
				}
				for (const auto& face : source.geometry.indices) { packed.indices << TriangleIndex32{face.i0 + offset, face.i1 + offset, face.i2 + offset}; }
			}
			m_mesh = Mesh{packed}; m_triangles = static_cast<uint32>(source.geometry.indices.size()); m_material = source.material;
		}
		m_transforms << transform;
	}
	uint32 draw()
	{
		if (m_transforms.isEmpty()) { return 0; }
		static const VertexShader shader = VertexShader::HLSL(U"shaders/hlsl/vehicle_instances.hlsl", U"VS");
		if (shader.isEmpty()) { throw Error{U"車両LODの頂点シェーダーを読み込めません"}; }
		const ScopedCustomShader3D scope{shader};
		uint32 calls = 0;
		for (size_t start = 0; start < m_transforms.size(); start += kCapacity)
		{
			const uint32 count = static_cast<uint32>(Min<size_t>(kCapacity, m_transforms.size() - start));
			for (uint32 i = 0; i < count; ++i) { m_matrices->at(i) = m_transforms[start + i]; }
			Graphics3D::SetVSConstantBuffer(4, m_matrices);
			m_mesh.drawSubset(0, count * m_triangles, PhongMaterial{m_material, HasDiffuseTexture::No}); ++calls;
		}
		return calls;
	}
	[[nodiscard]] size_t triangles() const { return m_transforms.size() * m_triangles; }
private:
	static constexpr uint32 kCapacity = 256;
	Mesh m_mesh;
	Material m_material;
	uint32 m_triangles = 0;
	Array<Mat4x4> m_transforms;
	ConstantBuffer<std::array<Mat4x4, kCapacity>> m_matrices;
};

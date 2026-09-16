#pragma once
#include "ModelBatch.hpp"

/// @brief 無テクスチャの遠景車両専用。頂点は不変、個体の変換行列だけGPUへ送る。
class VehicleInstanceBatch
{
public:
	void clear() { m_transforms.clear(); m_colors.clear(); m_hasPaint=false; }
	void append(const ModelMeshSource& source, const Mat4x4& transform, Float4 paint = {1,1,1,1})
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
		m_transforms << transform; m_colors << paint;
		m_hasPaint |= paint!=Float4{1,1,1,1};
	}
	uint32 draw()
	{
		if (m_transforms.isEmpty()) { return 0; }
		static const VertexShader shader = VertexShader::HLSL(U"shaders/hlsl/vehicle_instances.hlsl", U"VS");
		if (shader.isEmpty()) { throw Error{U"車両LODの頂点シェーダーを読み込めません"}; }
		const ScopedCustomShader3D scope{shader};
		static const PixelShader paintShader{HLSL{U"shaders/hlsl/city_forward.hlsl",U"VehicleInstance_PS"}};
		Optional<ScopedCustomShader3D> paintScope;
		if (m_hasPaint) { paintScope.emplace(paintShader); }
		uint32 calls = 0;
		for (size_t start = 0; start < m_transforms.size(); start += kCapacity)
		{
			const uint32 count = static_cast<uint32>(Min<size_t>(kCapacity, m_transforms.size() - start));
			for (uint32 i = 0; i < count; ++i) { m_instances->matrices[i] = m_transforms[start + i]; m_instances->colors[i]=m_colors[start+i]; }
			Graphics3D::SetVSConstantBuffer(4, m_instances);
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
	struct Instances { std::array<Mat4x4,kCapacity> matrices; std::array<Float4,kCapacity> colors; };
	ConstantBuffer<Instances> m_instances;
	Array<Float4> m_colors;
	bool m_hasPaint=false;
};

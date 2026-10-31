#pragma once
#include "ModelLod.hpp"
#include "../road/ObjParser.hpp"

/// @brief ネイティブModelと同じ材質を保持したCPU側LOD。静的・動的バッチで共有する。
struct ModelMeshSource
{
	MeshData geometry;
	Material material;
};

/// @brief 三角化・法線付きで出力したLODを、Siv3D Modelと同じ座標系で読む。
inline Array<ModelMeshSource> loadModelMeshSource(FilePathView path, const Model& model)
{
	Array<ModelMeshSource> result;
	for (auto& parsed : ObjParser::parse(path, true))
	{
		if (parsed.indices.isEmpty()) { continue; }
		Material material;
		for (const auto& candidate : model.materials())
		{
			if (candidate.name == parsed.name) { material = candidate; break; }
		}
		MeshData geometry; geometry.vertices = std::move(parsed.vertices); geometry.indices = std::move(parsed.indices);
		bool missingNormal = false;
		for (const auto& vertex : geometry.vertices) { missingNormal |= vertex.normal.lengthSq() < 1e-12f; }
		if (missingNormal)
		{
			// Native Model recomputes the entire material part when any normal is absent.
			MeshData flat;
			for (const auto& face : geometry.indices)
			{
				const uint32 offset = static_cast<uint32>(flat.vertices.size());
				flat.vertices << geometry.vertices[face.i0] << geometry.vertices[face.i1] << geometry.vertices[face.i2];
				flat.indices << TriangleIndex32{offset, offset + 1, offset + 2};
			}
			flat.computeNormals(); geometry = std::move(flat);
		}
		result << ModelMeshSource{std::move(geometry), material};
	}
	return result;
}

/// @brief ModelObject::draw と同じ描画を、材質ごとの TextureAsset 名前検索なしで行う。
/// 読み込めたテクスチャだけを保持し、空のものは次回また引き直す。
class ModelTextureCache
{
public:
	void draw(const ModelObject& object, const Array<Material>& materials)
	{
		if (m_textures.size() < materials.size()) { m_textures.resize(materials.size()); }
		for (const auto& part : object.parts)
		{
			if (!part.materialID) { part.mesh.draw(); continue; }
			const Material& material = materials[*part.materialID];
			if (material.diffuseTextureName.isEmpty()) { part.mesh.draw(PhongMaterial{ material, HasDiffuseTexture::No }); continue; }
			Texture& texture = m_textures[*part.materialID];
			if (!texture) { texture = TextureAsset{ material.diffuseTextureName }; }
			part.mesh.draw(texture, PhongMaterial{ material, HasDiffuseTexture::Yes });
		}
	}
private:
	Array<Texture> m_textures;
};

/// @brief 材質ごとに結合した不動のモデル。
struct StaticModelBatch
{
	Mesh mesh;
	Material material;
	uint32 triangles = 0;
	void draw() const
	{
		if (!material.diffuseTextureName.isEmpty())
		{
			mesh.draw(TextureAsset{material.diffuseTextureName}, PhongMaterial{material, HasDiffuseTexture::Yes});
		}
		else { mesh.draw(PhongMaterial{material, HasDiffuseTexture::No}); }
	}
};


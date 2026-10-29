#pragma once
#include <Siv3D.hpp>
#include <limits>

/// @brief Bound each native upload while preserving triangle order and every vertex attribute.
namespace MeshUploadSlices
{
	inline constexpr size_t kVertices=16384, kTriangles=8192;
	inline Array<MeshData> split(MeshData source,size_t vertices=kVertices,size_t triangles=kTriangles)
	{
		Array<MeshData> result;
		if(source.indices.isEmpty()) { return result; }
		vertices=Max(size_t{3},vertices);triangles=Max(size_t{1},triangles);
		if(source.vertices.size()<=vertices && source.indices.size()<=triangles) { result << std::move(source); return result; }
		constexpr uint32 absent=std::numeric_limits<uint32>::max();
		Array<uint32> remap(source.vertices.size(),absent), used;
		MeshData batch;
		auto finish=[&]()
		{
			if(!batch.indices.isEmpty()) { result << std::move(batch);batch={}; }
			for(uint32 index:used) { remap[index]=absent; } used.clear();
		};
		for(const auto& face:source.indices)
		{
			const std::array<uint32,3> original{face.i0,face.i1,face.i2};
			size_t needed=0;for(uint32 index:original) { needed+=remap[index]==absent; }
			if(batch.vertices.size()+needed>vertices || batch.indices.size()>=triangles) { finish(); }
			std::array<uint32,3> local;
			for(size_t i=0;i<3;++i)
			{
				const uint32 index=original[i];
				if(remap[index]==absent) { remap[index]=static_cast<uint32>(batch.vertices.size());used << index;batch.vertices << source.vertices[index]; }
				local[i]=remap[index];
			}
			batch.indices << TriangleIndex32{local[0],local[1],local[2]};
		}
		finish();return result;
	}
}

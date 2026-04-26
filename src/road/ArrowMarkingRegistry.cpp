#include "ArrowMarkingRegistry.hpp"

namespace
{
	const HashTable<String, RoadArrowType> kTypeMap = {
		{ U"Straight",      RoadArrowType::Straight      },
		{ U"Left",          RoadArrowType::Left          },
		{ U"Right",         RoadArrowType::Right         },
		{ U"StraightLeft",  RoadArrowType::StraightLeft  },
		{ U"StraightRight", RoadArrowType::StraightRight },
		{ U"LeftRight",     RoadArrowType::LeftRight     },
		{ U"All",           RoadArrowType::All           },
		{ U"UTurn",         RoadArrowType::UTurn         },
	};
}

bool ArrowMarkingRegistry::load(FilePathView dirPath)
{
	// 矢羽根定義ディレクトリを走査し、ID から種別を確定できるものだけをメッシュ化して登録する。
	int count = 0;

	for (const auto& path : FileSystem::DirectoryContents(dirPath))
	{
		if (FileSystem::Extension(path) != U"toml")
			continue;

		const TOMLReader toml{ path };
		if (!toml)
		{
			Console << U"[ArrowMarkingRegistry] failed to open: " << path;
			continue;
		}

		const String id = toml[U"id"].getString();
		const auto type = parseType(id);
		if (!type)
		{
			Console << U"[ArrowMarkingRegistry] unknown id: " << id;
			continue;
		}

		Array<Vec2> verts;
		for (const auto& row : toml[U"verts"].arrayView())
		{
			if (row.arrayCount() < 2) continue;
			const auto arr = row.arrayView();
			verts << Vec2{ arr[0].get<double>(), arr[1].get<double>() };
		}

		const Polygon poly{ verts };
		if (poly.isEmpty())
		{
			Console << U"[ArrowMarkingRegistry] invalid polygon for: " << id;
			continue;
		}

		m_entries[static_cast<uint8>(*type)] = Entry{ buildMesh(poly) };
		++count;
	}

	return count > 0;
}

const MeshData* ArrowMarkingRegistry::getMesh(RoadArrowType type) const
{
	const auto it = m_entries.find(static_cast<uint8>(type));
	return (it != m_entries.end()) ? &it->second.mesh : nullptr;
}

Optional<RoadArrowType> ArrowMarkingRegistry::parseType(StringView id)
{
	if (const auto it = kTypeMap.find(String{ id }); it != kTypeMap.end())
		return it->second;
	return none;
}

MeshData ArrowMarkingRegistry::buildMesh(const Polygon& poly)
{
	// 2D ポリゴン定義を道路面上に寝かせた 3D メッシュへ変換し、描画用頂点形式に揃える。
	const auto& outerVerts = poly.outer();
	const auto& triIndices = poly.indices();

	MeshData md;
	md.vertices.reserve(outerVerts.size());
	for (const auto& v : outerVerts)
	{
		Vertex3D vert;
		// ローカル座標: X=進行方向, Z=lateral（XZ 平面に展開）, Y=0
		vert.pos    = Float3{ static_cast<float>(v.x), 0.0f, static_cast<float>(v.y) };
		vert.normal = Float3{ 0.0f, 1.0f, 0.0f };
		vert.tex    = Float2{ static_cast<float>(v.x), static_cast<float>(v.y) };
		md.vertices << vert;
	}

	md.indices.reserve(triIndices.size());
	for (const auto& tri : triIndices)
		md.indices << TriangleIndex32{ tri.i0, tri.i1, tri.i2 };

	return md;
}

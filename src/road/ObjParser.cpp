#include "ObjParser.hpp"

namespace
{
	/// @brief 面のインデックス（v/vt/vn、1-based → 0-based 変換済み）
	struct FaceVertex
	{
		int v  = -1;  ///< 頂点位置インデックス
		int vt = -1;  ///< テクスチャ座標インデックス
		int vn = -1;  ///< 法線インデックス
	};

	/// @brief "v/vt/vn" 形式の文字列をパース
	FaceVertex parseFaceVertex(const String& token)
	{
		FaceVertex fv;
		const auto parts = token.split(U'/');

		if (parts.size() >= 1 && !parts[0].isEmpty())
			fv.v = ParseOr<int>(parts[0], 0) - 1;

		if (parts.size() >= 2 && !parts[1].isEmpty())
			fv.vt = ParseOr<int>(parts[1], 0) - 1;

		if (parts.size() >= 3 && !parts[2].isEmpty())
			fv.vn = ParseOr<int>(parts[2], 0) - 1;

		return fv;
	}

	/// @brief FaceVertex から Vertex3D を構築
	Vertex3D makeVertex(const FaceVertex& fv,
	                    const Array<Float3>& positions,
	                    const Array<Float2>& texCoords,
	                    const Array<Float3>& normals)
	{
		Vertex3D vtx;
		vtx.pos    = (fv.v  >= 0 && fv.v  < static_cast<int>(positions.size()))
		           ? positions[fv.v] : Float3{ 0, 0, 0 };
		vtx.tex    = (fv.vt >= 0 && fv.vt < static_cast<int>(texCoords.size()))
		           ? texCoords[fv.vt] : Float2{ 0, 0 };
		vtx.normal = (fv.vn >= 0 && fv.vn < static_cast<int>(normals.size()))
		           ? normals[fv.vn] : Float3{ 0, 1, 0 };
		return vtx;
	}

	/// @brief inner を X 軸ミラーして outer を生成
	PartModelData mirrorX(const PartModelData& src, const String& newName)
	{
		PartModelData dst;
		dst.name = newName;
		dst.vertices.reserve(src.vertices.size());
		dst.indices = src.indices;

		for (const auto& v : src.vertices)
		{
			Vertex3D mv = v;
			mv.pos.x    *= -1.0f;
			mv.normal.x *= -1.0f;
			dst.vertices << mv;
		}

		// ミラーするとワインディング順序が反転するため、三角形の頂点順を入れ替え
		for (auto& tri : dst.indices)
			std::swap(tri.i0, tri.i1);

		return dst;
	}
}

Array<PartModelData> ObjParser::parse(FilePathView path)
{
	// OBJ をオブジェクト単位へ分割しつつ、face ごとの頂点参照を Siv3D の MeshData 形式へ展開する。
	TextReader reader{ path };
	if (!reader)
	{
		Console << U"[ObjParser] Failed to open: " << path;
		return {};
	}

	Array<Float3> positions;
	Array<Float2> texCoords;
	Array<Float3> normals;

	Array<PartModelData> objects;
	PartModelData* current = nullptr;

	// 重複頂点を検出するためのマップ（FaceVertex の組み合わせ → 頂点インデックス）
	HashTable<int64, uint32> vertexCache;

	auto ensureCurrentObject = [&]()
	{
		if (!current)
		{
			objects << PartModelData{ U"default", {}, {} };
			current = &objects.back();
			vertexCache.clear();
		}
	};

	String line;
	while (reader.readLine(line))
	{
		line = line.trimmed();
		if (line.isEmpty() || line[0] == U'#')
			continue;

		auto tokens = line.replace(U'\t', U' ').split(U' ');
		tokens.remove_if([](const String& token) { return token.isEmpty(); });
		if (tokens.isEmpty())
			continue;

		const auto& cmd = tokens[0];

		if (cmd == U"o" && tokens.size() >= 2)
		{
			// 新オブジェクト開始
			objects << PartModelData{ tokens[1], {}, {} };
			current = &objects.back();
			vertexCache.clear();
		}
		else if (cmd == U"v" && tokens.size() >= 4)
		{
			positions << Float3{
				ParseOr<float>(tokens[1], 0.0f),
				ParseOr<float>(tokens[2], 0.0f),
				ParseOr<float>(tokens[3], 0.0f)
			};
		}
		else if (cmd == U"vt" && tokens.size() >= 3)
		{
			texCoords << Float2{
				ParseOr<float>(tokens[1], 0.0f),
				ParseOr<float>(tokens[2], 0.0f)
			};
		}
		else if (cmd == U"vn" && tokens.size() >= 4)
		{
			normals << Float3{
				ParseOr<float>(tokens[1], 0.0f),
				ParseOr<float>(tokens[2], 0.0f),
				ParseOr<float>(tokens[3], 0.0f)
			};
		}
		else if (cmd == U"f" && tokens.size() >= 4)
		{
			ensureCurrentObject();

			// 面の頂点を収集
			Array<uint32> faceIndices;
			for (size_t i = 1; i < tokens.size(); ++i)
			{
				if (tokens[i].isEmpty()) continue;

				const FaceVertex fv = parseFaceVertex(tokens[i]);

				// キーを生成（v, vt, vn の組み合わせ）
				const int64 key = (static_cast<int64>(fv.v + 1) << 40)
				                | (static_cast<int64>(fv.vt + 1) << 20)
				                | static_cast<int64>(fv.vn + 1);

				if (auto it = vertexCache.find(key); it != vertexCache.end())
				{
					faceIndices << it->second;
				}
				else
				{
					const uint32 idx = static_cast<uint32>(current->vertices.size());
					current->vertices << makeVertex(fv, positions, texCoords, normals);
					vertexCache[key] = idx;
					faceIndices << idx;
				}
			}

			// 三角形化（ファン方式: 0-1-2, 0-2-3, 0-3-4, ...）
			for (size_t i = 2; i < faceIndices.size(); ++i)
			{
				current->indices << TriangleIndex32{
					faceIndices[0],
					faceIndices[i - 1],
					faceIndices[i]
				};
			}
		}
		// mtllib, usemtl, s, g は無視
	}

	return objects;
}

RoadPartModel ObjParser::buildModel(const Array<PartModelData>& meshes, bool symmetric)
{
	// 命名規約に従って inner / center / outer と LOD を振り分け、必要なら outer を鏡像生成する。
	RoadPartModel model;

	// LOD レベルごとの inner/center/outer を分類
	// 命名規約: inner, center, outer, inner_lod1, center_lod1, outer_lod1, ...
	HashTable<int, RoadPartModel*> lodModels; // LOD レベル → モデル

	auto getOrCreateLod = [&](int level) -> RoadPartModel*
	{
		if (level == 0) return &model;
		while (static_cast<int>(model.lods.size()) < level)
			model.lods << RoadPartModel{};
		return &model.lods[level - 1];
	};

	for (const auto& mesh : meshes)
	{
		const String& name = mesh.name;

		// LOD レベルを判定
		int lodLevel = 0;
		String baseName = name;

		if (const auto lodPos = name.indexOf(U"_lod"); lodPos != String::npos)
		{
			const String lodStr = name.substr(lodPos + 4);
			lodLevel = ParseOr<int>(lodStr, 1);
			baseName = name.substr(0, lodPos);
		}

		RoadPartModel* target = getOrCreateLod(lodLevel);

		if (baseName == U"inner")
			target->inner = mesh;
		else if (baseName == U"center")
			target->center = mesh;
		else if (baseName == U"outer")
			target->outer = mesh;
		// その他の名前は無視
	}

	// symmetric 対応: outer が空なら inner をミラー
	if (symmetric)
	{
		if (model.outer.isEmpty() && !model.inner.isEmpty())
			model.outer = mirrorX(model.inner, U"outer");

		for (size_t i = 0; i < model.lods.size(); ++i)
		{
			auto& lod = model.lods[i];
			if (lod.outer.isEmpty() && !lod.inner.isEmpty())
				lod.outer = mirrorX(lod.inner, U"outer_lod" + Format(i + 1));
		}
	}

	return model;
}

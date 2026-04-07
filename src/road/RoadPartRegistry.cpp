#include "RoadPartRegistry.hpp"

namespace
{
	/// @brief TOML 配列から ColorF を読む
	ColorF parseColorArray(const TOMLValue& v, ColorF def)
	{
		if (v.isEmpty() || v.arrayCount() < 3) return def;
		const auto arr = v.arrayView();
		return ColorF{
			arr[0].getOr<double>(def.r),
			arr[1].getOr<double>(def.g),
			arr[2].getOr<double>(def.b)
		};
	}

	/// @brief 文字列から RoadPartType を逆引き
	Optional<RoadPartType> parsePartType(const String& s)
	{
		static const HashTable<String, RoadPartType> map = {
			{ U"Roadbed",   RoadPartType::Roadbed   },
			{ U"Shoulder",  RoadPartType::Shoulder  },
			{ U"Median",    RoadPartType::Median    },
			{ U"Sidewalk",  RoadPartType::Sidewalk  },
			{ U"Gutter",    RoadPartType::Gutter    },
			{ U"Guardrail", RoadPartType::Guardrail },
			{ U"Wall",      RoadPartType::Wall      },
			{ U"Curb",      RoadPartType::Curb      },
			{ U"Slope",     RoadPartType::Slope     },
			{ U"BikeLane",  RoadPartType::BikeLane  },
		};
		if (auto it = map.find(s); it != map.end())
			return it->second;
		return none;
	}

	/// @brief 文字列から TilingMode を逆引き
	TilingMode parseTilingMode(const String& s)
	{
		if (s == U"Longitudinal") return TilingMode::Longitudinal;
		return TilingMode::CrossSection;
	}
}

bool RoadPartRegistry::load(FilePathView dirPath)
{
	int count = 0;

	for (const auto& entry : FileSystem::DirectoryContents(dirPath))
	{
		if (FileSystem::Extension(entry) != U"toml")
			continue;

		const String baseDir = FileSystem::ParentPath(entry);
		if (auto e = loadEntry(entry, baseDir))
		{
			const String id = e->def.id;
			m_entries[id] = std::move(*e);
			++count;
		}
	}

	Console << U"[RoadPartRegistry] Loaded " << count << U" part definitions from " << dirPath;
	return count > 0;
}

const RoadPartDef& RoadPartRegistry::get(StringView defId) const
{
	if (auto it = m_entries.find(String{ defId }); it != m_entries.end())
		return it->second.def;
	return m_fallbackDef;
}

const RoadPartModel& RoadPartRegistry::getModel(StringView defId) const
{
	if (auto it = m_entries.find(String{ defId }); it != m_entries.end())
		return it->second.model;
	return m_fallbackModel;
}

Array<String> RoadPartRegistry::defIds() const
{
	Array<String> ids;
	ids.reserve(m_entries.size());
	for (const auto& [key, _] : m_entries)
		ids << key;
	return ids;
}

Optional<RoadPartRegistry::Entry> RoadPartRegistry::loadEntry(FilePathView tomlPath, FilePathView baseDir)
{
	const TOMLReader toml{ tomlPath };
	if (!toml)
	{
		Console << U"[RoadPartRegistry] Failed to parse TOML: " << tomlPath;
		return none;
	}

	RoadPartDef def;

	// 必須フィールド
	def.id   = toml[U"id"].getString();
	def.name = toml[U"name"].getOr<String>(def.id);

	if (def.id.isEmpty())
	{
		Console << U"[RoadPartRegistry] Missing 'id' in " << tomlPath;
		return none;
	}

	// 部品種別
	if (auto t = parsePartType(toml[U"type"].getOr<String>(U"Roadbed")))
		def.type = *t;

	// タイリングモード
	def.tiling = parseTilingMode(toml[U"tiling"].getOr<String>(U"CrossSection"));

	// 寸法
	def.modelUnitWidth = static_cast<float>(toml[U"model_unit_width"].getOr<double>(0.5));
	def.modelUnitLen   = static_cast<float>(toml[U"model_unit_len"].getOr<double>(1.0));
	def.heightOffset   = static_cast<float>(toml[U"height_offset"].getOr<double>(0.0));
	def.symmetric      = toml[U"symmetric"].getOr<bool>(false);

	// モデルパス
	const String modelFile = toml[U"model"].getOr<String>(U"");
	if (!modelFile.isEmpty())
		def.modelPath = String{ baseDir } + modelFile;

	// マテリアル
	const auto mat = toml[U"material"];
	if (!mat.isEmpty())
	{
		const String texFile = mat[U"texture"].getOr<String>(U"");
		if (!texFile.isEmpty())
			def.texturePath = String{ baseDir } + texFile;

		def.color = parseColorArray(mat[U"color"], def.color);
	}

	// LOD
	const auto lod = toml[U"lod"];
	if (!lod.isEmpty())
	{
		if (const auto names = lod[U"mesh_names"]; !names.isEmpty())
		{
			for (size_t i = 0; i < names.arrayCount(); ++i)
				def.lodMeshNames << names.arrayView()[i].getString();
		}
		if (const auto dists = lod[U"distances"]; !dists.isEmpty())
		{
			for (size_t i = 0; i < dists.arrayCount(); ++i)
				def.lodDistances << static_cast<float>(dists.arrayView()[i].getOr<double>(800.0));
		}
	}

	// テクスチャロード（同一パスのアセットは TextureAsset で共有される）
	if (!def.texturePath.isEmpty() && FileSystem::Exists(def.texturePath))
	{
		TextureAsset::Register(def.texturePath, def.texturePath, TextureDesc::MippedSRGB);
		def.texture = TextureAsset(def.texturePath);
	}

	// OBJ モデルロード
	RoadPartModel model;
	if (!def.modelPath.isEmpty() && FileSystem::Exists(def.modelPath))
	{
		const auto meshes = ObjParser::parse(def.modelPath);
		model = ObjParser::buildModel(meshes, def.symmetric);
		Console << U"[RoadPartRegistry] " << def.id
		        << U": inner=" << model.inner.vertices.size()
		        << U" center=" << model.center.vertices.size()
		        << U" outer=" << model.outer.vertices.size() << U" verts";
	}

	return Entry{ std::move(def), std::move(model) };
}

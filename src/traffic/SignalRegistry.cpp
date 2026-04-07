#include "SignalRegistry.hpp"

namespace
{
	Float4 parseUvRect(const TOMLValue& v)
	{
		if (v.isEmpty() || v.arrayCount() < 4) return { 0, 0, 0, 0 };
		const auto a = v.arrayView();
		return {
			static_cast<float>(a[0].getOr<double>(0)),
			static_cast<float>(a[1].getOr<double>(0)),
			static_cast<float>(a[2].getOr<double>(0)),
			static_cast<float>(a[3].getOr<double>(0))
		};
	}

	Float3 parseFloat3(const TOMLValue& v, Float3 def = { 0, 0, 0 })
	{
		if (v.isEmpty() || v.arrayCount() < 3) return def;
		const auto a = v.arrayView();
		return {
			static_cast<float>(a[0].getOr<double>(0)),
			static_cast<float>(a[1].getOr<double>(0)),
			static_cast<float>(a[2].getOr<double>(0))
		};
	}

	ColorF parseEmission(const TOMLValue& v, ColorF def = ColorF{ 1.0 })
	{
		if (v.isEmpty() || v.arrayCount() < 3) return def;
		const auto a = v.arrayView();
		return ColorF{
			a[0].getOr<double>(def.r),
			a[1].getOr<double>(def.g),
			a[2].getOr<double>(def.b)
		};
	}
}

bool SignalRegistry::load(FilePathView dirPath)
{
	int count = 0;
	for (const auto& entry : FileSystem::DirectoryContents(dirPath))
	{
		if (FileSystem::Extension(entry) != U"toml") continue;
		const String baseDir = FileSystem::ParentPath(entry);
		if (auto e = loadEntry(entry, baseDir))
		{
			const String id = e->def.id;
			m_entries[id] = std::move(*e);
			++count;
		}
	}
	Console << U"[SignalRegistry] Loaded " << count << U" signal definitions from " << dirPath;
	return count > 0;
}

const SignalDef* SignalRegistry::getDef(StringView id) const
{
	if (auto it = m_entries.find(String{ id }); it != m_entries.end())
		return &it->second.def;
	return nullptr;
}

const SignalModel* SignalRegistry::getModel(StringView id) const
{
	if (auto it = m_entries.find(String{ id }); it != m_entries.end())
		return &it->second.model;
	return nullptr;
}

Array<String> SignalRegistry::defIds() const
{
	Array<String> ids;
	ids.reserve(m_entries.size());
	for (const auto& [key, _] : m_entries) ids << key;
	return ids;
}

Optional<SignalRegistry::Entry> SignalRegistry::loadEntry(FilePathView tomlPath, FilePathView baseDir)
{
	const TOMLReader toml{ tomlPath };
	if (!toml)
	{
		Console << U"[SignalRegistry] Failed to parse: " << tomlPath;
		return none;
	}

	Console << U"[SignalRegistry] TOML opened OK";
	Console << U"[SignalRegistry] Reading id...";

	SignalDef def;
	def.id   = toml[U"id"].getOr<String>(U"");
	Console << U"[SignalRegistry] id = " << def.id;
	Console << U"[SignalRegistry] Reading name...";
	def.name = toml[U"name"].getOr<String>(def.id);

	if (def.id.isEmpty())
	{
		Console << U"[SignalRegistry] Missing 'id' in " << tomlPath;
		return none;
	}

	Console << U"[SignalRegistry] Reading model path...";
	// モデル・テクスチャパス
	const String modelFile = toml[U"model"].getOr<String>(U"");
	if (!modelFile.isEmpty())
		def.modelPath = String{ baseDir } + modelFile;

	def.bodyMeshName = toml[U"body_mesh"].getOr<String>(U"body");

	Console << U"[SignalRegistry] Reading material...";
	const auto mat = toml[U"material"];
	if (!mat.isEmpty())
	{
		const String texFile = mat[U"texture"].getOr<String>(U"");
		if (!texFile.isEmpty())
			def.texturePath = String{ baseDir } + texFile;
	}

	Console << U"[SignalRegistry] Reading lamps...";
	// ランプ定義（[[lamps]] = テーブル配列）
	if (const auto lampsArr = toml[U"lamps"]; lampsArr.isTableArray())
	{
		for (const auto& lamp : lampsArr.tableArrayView())
		{
			SignalLampDef ld;
			ld.meshName = lamp[U"mesh"].getOr<String>(U"");

			if (const auto stArr = lamp[U"states"]; !stArr.isEmpty())
			{
				for (size_t j = 0; j < stArr.arrayCount(); ++j)
					ld.stateIds << stArr.arrayView()[j].getString();
			}
			def.lamps << std::move(ld);
		}
	}

	Console << U"[SignalRegistry] Reading sub_lamp...";
	// sub_lamp 定義
	if (const auto sub = toml[U"sub_lamp"]; !sub.isEmpty())
	{
		SignalSubLampDef sld;
		sld.meshName     = sub[U"mesh"].getOr<String>(U"sub_lamp");
		sld.bodyMeshName = sub[U"body_mesh"].getOr<String>(U"sub_body");
		sld.colStride    = parseFloat3(sub[U"col_stride"]);
		sld.rowStride    = parseFloat3(sub[U"row_stride"]);
		sld.cols         = sub[U"cols"].getOr<int>(3);

		if (const auto stArr = sub[U"states"]; !stArr.isEmpty())
		{
			for (size_t j = 0; j < stArr.arrayCount(); ++j)
				sld.stateIds << stArr.arrayView()[j].getString();
		}
		def.subLamp = std::move(sld);
	}

	// 状態定義 [[state]]（テーブル配列）
	Console << U"[SignalRegistry] Parsing states...";
	if (const auto stateArr = toml[U"state"]; stateArr.isTableArray())
	{
		for (const auto& entry : stateArr.tableArrayView())
		{
			SignalState st;
			st.id       = entry[U"id"].getOr<String>(U"");
			Console << U"[SignalRegistry]   state: " << st.id;
			st.uvRect   = parseUvRect(entry[U"uv"]);
			st.emission = parseEmission(entry[U"emission"], ColorF{ 1.0 });
			if (!st.id.isEmpty())
				def.states[st.id] = std::move(st);
		}
	}
	Console << U"[SignalRegistry] States parsed: " << def.states.size();

	Console << U"[SignalRegistry] Loading OBJ model...";
	// OBJ モデルロード
	SignalModel model;
	if (!def.modelPath.isEmpty() && FileSystem::Exists(def.modelPath))
	{
		auto meshes = ObjParser::parse(def.modelPath);

		// Blender(右手系) → DirectX(左手系) 変換: X反転 + UV V反転
		for (auto& m : meshes)
		{
			for (auto& v : m.vertices)
			{
				v.pos.x    *= -1.0f;
				v.normal.x *= -1.0f;
				v.tex.y = 1.0f - v.tex.y;
			}
			// X反転でワインディングが逆になるため三角形の頂点順を入れ替え
			for (auto& tri : m.indices)
				std::swap(tri.i0, tri.i1);
		}

		// ランプ / sub_lamp メッシュの UV を 0-1 に正規化
		// （TextureRegion で状態切り替えするため）
		HashSet<String> lampMeshNames;
		for (const auto& ld : def.lamps) lampMeshNames.insert(ld.meshName);
		if (def.subLamp) lampMeshNames.insert(def.subLamp->meshName);

		for (auto& m : meshes)
		{
			if (lampMeshNames.contains(m.name) && !m.vertices.isEmpty())
			{
				// UV バウンディングボックスを求める
				float uMin = 1e9f, uMax = -1e9f, vMin = 1e9f, vMax = -1e9f;
				for (const auto& v : m.vertices)
				{
					uMin = Min(uMin, v.tex.x); uMax = Max(uMax, v.tex.x);
					vMin = Min(vMin, v.tex.y); vMax = Max(vMax, v.tex.y);
				}
				const float uRange = (uMax - uMin > 1e-6f) ? (uMax - uMin) : 1.0f;
				const float vRange = (vMax - vMin > 1e-6f) ? (vMax - vMin) : 1.0f;
				for (auto& v : m.vertices)
				{
					v.tex.x = (v.tex.x - uMin) / uRange;
					v.tex.y = (v.tex.y - vMin) / vRange;
				}
			}
		}

		for (const auto& m : meshes)
			model.meshes[m.name] = m;
		Console << U"[SignalRegistry] " << def.id << U": " << meshes.size() << U" meshes loaded";
	}

	Console << U"[SignalRegistry] Loading texture...";
	// テクスチャロード
	if (!def.texturePath.isEmpty() && FileSystem::Exists(def.texturePath))
	{
		model.texture = Texture{ def.texturePath, TextureDesc::MippedSRGB };
	}

	return Entry{ std::move(def), std::move(model) };
}

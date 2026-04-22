#include "RoadPreset.hpp"
#include "RoadNetwork.hpp"

// =============================================================================
// JSON シリアライズ ヘルパー（内部使用）
// =============================================================================

namespace
{
	static JSON partToJson(const RoadPart& part)
	{
		JSON j;
		j[U"defId"]     = part.defId;
		j[U"offsetA_L"] = part.offsetA_L;
		j[U"offsetA_R"] = part.offsetA_R;
		j[U"offsetB_L"] = part.offsetB_L;
		j[U"offsetB_R"] = part.offsetB_R;
		j[U"build"]     = static_cast<uint8>(part.build);
		j[U"type"]      = static_cast<uint8>(part.type);
		return j;
	}

	static RoadPart partFromJson(const JSON& j)
	{
		RoadPart part;
		part.defId     = j[U"defId"].getString();
		part.offsetA_L = j[U"offsetA_L"].get<float>();
		part.offsetA_R = j[U"offsetA_R"].get<float>();
		part.offsetB_L = j[U"offsetB_L"].get<float>();
		part.offsetB_R = j[U"offsetB_R"].get<float>();
		part.build     = static_cast<BuildState>(j[U"build"].get<uint8>());
		part.type      = static_cast<RoadPartType>(j[U"type"].get<uint8>());
		return part;
	}

	static JSON laneToJson(const Lane& lane)
	{
		JSON j;
		j[U"offsetA_L"]          = lane.offsetA_L;
		j[U"offsetA_R"]          = lane.offsetA_R;
		j[U"offsetB_L"]          = lane.offsetB_L;
		j[U"offsetB_R"]          = lane.offsetB_R;
		j[U"dir"]                = static_cast<uint8>(lane.dir);
		j[U"op"]                 = static_cast<uint8>(lane.op);
		j[U"canChangeLaneLeft"]  = lane.canChangeLaneLeft;
		j[U"canChangeLaneRight"] = lane.canChangeLaneRight;
		j[U"lineLeft"]           = static_cast<uint8>(lane.lineLeft);
		j[U"lineRight"]          = static_cast<uint8>(lane.lineRight);
		j[U"nominalWidth"]       = lane.nominalWidth;
		j[U"type"]               = static_cast<uint8>(lane.type);
		return j;
	}

	static Lane laneFromJson(const JSON& j)
	{
		Lane lane;
		lane.offsetA_L          = j[U"offsetA_L"].get<float>();
		lane.offsetA_R          = j[U"offsetA_R"].get<float>();
		lane.offsetB_L          = j[U"offsetB_L"].get<float>();
		lane.offsetB_R          = j[U"offsetB_R"].get<float>();
		lane.dir                = static_cast<LaneDir>(j[U"dir"].get<uint8>());
		lane.op                 = static_cast<OpState>(j[U"op"].get<uint8>());
		lane.canChangeLaneLeft  = j[U"canChangeLaneLeft"].get<bool>();
		lane.canChangeLaneRight = j[U"canChangeLaneRight"].get<bool>();
		lane.lineLeft           = static_cast<LineType>(j[U"lineLeft"].get<uint8>());
		lane.lineRight          = static_cast<LineType>(j[U"lineRight"].get<uint8>());
		lane.nominalWidth       = j[U"nominalWidth"].get<float>();
		lane.type               = static_cast<LaneType>(j[U"type"].get<uint8>());
		return lane;
	}

	static StringView roadTypeName(RoadType rt)
	{
		switch (rt)
		{
		case RoadType::LocalRoad:  return U"市道";
		case RoadType::Arterial:   return U"幹線";
		case RoadType::Expressway: return U"高速";
		case RoadType::Highway:    return U"有料";
		}
		return U"道路";
	}
}

// =============================================================================
// RoadTemplatePreset
// =============================================================================

RoadTemplatePreset RoadTemplatePreset::fromEdge(const RoadEdge& edge, const String& name)
{
	RoadTemplatePreset preset;
	preset.name       = name;
	preset.roadType   = edge.roadType;
	preset.speedLimit = edge.speedLimit;
	preset.parts      = edge.parts;
	preset.lanes      = edge.lanes;
	return preset;
}

void RoadTemplatePreset::applyTo(RoadEdge& dst) const
{
	// id / ノード / 制御点 / 弧長 / planId / cutoff は変更しない
	dst.roadType   = roadType;
	dst.speedLimit = speedLimit;
	dst.parts      = parts;
	dst.lanes      = lanes;
}

String RoadTemplatePreset::autoName(const RoadEdge& edge)
{
	return U"{} {}車線 {}km/h"_fmt(
		roadTypeName(edge.roadType),
		edge.lanes.size(),
		static_cast<int>(edge.speedLimit));
}

// =============================================================================
// RoadPresetStore
// =============================================================================

void RoadPresetStore::load()
{
	constexpr StringView kPath = U"user_data/draw_presets.json";
	if (!FileSystem::Exists(kPath))
	{
		return;
	}

	const JSON json = JSON::Load(kPath);
	if (!json)
	{
		Logger.writeln(U"[RoadPreset] JSON::Load failed: {}"_fmt(kPath));
		return;
	}

	m_favorites.clear();
	if (json.hasElement(U"favorites"))
	{
		for (const auto& [idx, entry] : json[U"favorites"])
		{
			(void)idx;
			RoadTemplatePreset preset;
			preset.name       = entry[U"name"].getString();
			preset.roadType   = static_cast<RoadType>(entry[U"roadType"].get<uint8>());
			preset.speedLimit = entry[U"speedLimit"].get<float>();

			if (entry.hasElement(U"parts"))
			{
				for (const auto& [pi, pj] : entry[U"parts"])
				{
					(void)pi;
					preset.parts << partFromJson(pj);
				}
			}
			if (entry.hasElement(U"lanes"))
			{
				for (const auto& [li, lj] : entry[U"lanes"])
				{
					(void)li;
					preset.lanes << laneFromJson(lj);
				}
			}
			m_favorites << preset;
		}
	}
}

bool RoadPresetStore::save() const
{
	constexpr StringView kPath = U"user_data/draw_presets.json";
	FileSystem::CreateDirectories(U"user_data/");

	JSON json;
	json[U"version"] = 1;

	JSON favArray;
	for (const auto& preset : m_favorites)
	{
		JSON entry;
		entry[U"name"]       = preset.name;
		entry[U"roadType"]   = static_cast<uint8>(preset.roadType);
		entry[U"speedLimit"] = preset.speedLimit;

		JSON partsArray;
		for (const auto& part : preset.parts)
		{
			partsArray.push_back(partToJson(part));
		}
		entry[U"parts"] = partsArray;

		JSON lanesArray;
		for (const auto& lane : preset.lanes)
		{
			lanesArray.push_back(laneToJson(lane));
		}
		entry[U"lanes"] = lanesArray;
		favArray.push_back(entry);
	}
	json[U"favorites"] = favArray;

	if (!json.save(kPath))
	{
		Logger.writeln(U"[RoadPreset] Failed to save: {}"_fmt(kPath));
		return false;
	}
	return true;
}

void RoadPresetStore::addFavorite(RoadTemplatePreset preset)
{
	// 名前重複時に "(2)", "(3)" ... を付加する
	const String baseName = preset.name;
	int suffix = 2;
	while (true)
	{
		const bool duplicate = m_favorites.any([&](const RoadTemplatePreset& p) {
			return p.name == preset.name;
		});
		if (!duplicate) { break; }
		preset.name = U"{} ({})"_fmt(baseName, suffix);
		++suffix;
	}
	m_favorites << preset;
}

void RoadPresetStore::removeFavoriteAt(size_t index)
{
	if (index < m_favorites.size())
	{
		m_favorites.erase(m_favorites.begin() + index);
	}
}

Array<RoadTemplatePreset> RoadPresetStore::buildDefaults()
{
	// ラムダで一時 RoadEdge を構築し fromEdge で変換する
	const auto makePreset = [](RoadType rt, int numLanes, float speed)
	{
		RoadEdge edge;
		edge.roadType   = rt;
		edge.speedLimit = speed;
		edge.lanes      = RoadNetwork::buildDefaultLanes(numLanes, rt);
		RoadNetwork::buildDefaultParts(edge);
		return RoadTemplatePreset::fromEdge(edge, RoadTemplatePreset::autoName(edge));
	};

	return {
		makePreset(RoadType::LocalRoad,  2, 60.0f),
		makePreset(RoadType::LocalRoad,  4, 60.0f),
		makePreset(RoadType::Arterial,   4, 80.0f),
		makePreset(RoadType::Expressway, 4, 100.0f),
	};
}

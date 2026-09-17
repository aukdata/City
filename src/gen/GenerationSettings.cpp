#include "GenerationSettings.hpp"
#include <cmath>
#include <type_traits>

namespace GenerationSettings
{
	Values load(FilePathView directory)
	{
		Values result;
		HashTable<String, JSON> files;
		HashTable<String, HashSet<String>> keys;
		const auto number = [&]<class T>(T& target, StringView group, StringView key, double minimum, double maximum)
		{
			const String file{group};
			if (!files.contains(file))
			{
				const FilePath path = FilePath{directory} + U"/" + file + U".json";
				const JSON json = JSON::Load(path);
				if (!json || !json.isObject()) { throw Error{U"生成設定を読み込めません: " + path}; }
				files.emplace(file, json);
			}
			const auto& json = files.at(file);
			const String label = file + U"." + key;
			if (!json.contains(key)) { throw Error{U"生成設定に必須項目がありません: " + label}; }
			const auto value = json[key].getOpt<double>();
			if (!value || !std::isfinite(*value) || *value < minimum || *value > maximum
				|| (std::is_integral_v<T> && std::trunc(*value) != *value))
			{
				throw Error{U"生成設定の型または範囲が不正です: " + label};
			}
			target = static_cast<T>(*value); keys[file].insert(String{key});
		};
#define GENERATION_SETTING(type, field, group, key, minimum, maximum) number(result.field, group, key, minimum, maximum);
#include "GenerationSettings.def"
#undef GENERATION_SETTING
		for (const auto& [group, json] : files)
		{
			for (const auto item : json)
			{
				if (!keys.at(group).contains(item.key)) { throw Error{U"未知の生成設定: " + group + U"." + item.key}; }
			}
		}
		if (result.terrain_centralMountainThreshold > result.terrain_centralLakeThreshold
			|| result.terrain_centralLakeThreshold > result.terrain_centralBayThreshold
			|| result.terrain_centralBayThreshold > 1) { throw Error{U"中央地形の累積確率は昇順で1以下にしてください"}; }
		const auto require = [](bool valid, StringView reason) { if (!valid) { throw Error{U"生成設定の組み合わせが不正です: " + String{reason}}; } };
		require(result.traffic_minimumTripSteps <= result.traffic_maximumTripSteps
			&& result.traffic_maximumTripSteps <= result.traffic_tripSearchSteps, U"traffic trip steps (minimum <= maximum <= search)");
		require(result.traffic_minimumFreightShare <= result.traffic_maximumFreightShare, U"traffic freight share");
		require(result.roads_surfaceTolerance < Min(result.roads_maximumFill, result.roads_maximumCut), U"roads surface tolerance < fill / cut");
		require(result.districtRoads_castleHalfMin <= result.districtRoads_castleHalfMax, U"districtRoads castle size");
		require(result.rivers_minimumHalfWidth <= result.rivers_maximumHalfWidth, U"rivers width");
		require(result.rivers_bankBlendStart + result.rivers_bankBlendWidth <= result.rivers_carveExtent, U"rivers bank blend inside carve extent");
		require(result.routing_minimumStep <= result.routing_maximumStep, U"routing step size");
		require(result.network_mediumRouteThreshold <= result.network_longRouteThreshold, U"network route thresholds");
		require(result.agriculture_plotWidth>2*result.agriculture_bundWidth+result.agriculture_drainWidth, U"agriculture usable field depth");
		require(result.agriculture_trackStep<=result.agriculture_trackReach, U"agriculture track step <= reach");
		require(result.agriculture_homeSpacing<result.agriculture_homeMaximumDistance, U"agriculture houses within field service radius");
		require(result.vegetation_minimumForestAltitude <= result.vegetation_maximumForestAltitude, U"vegetation altitude range");
		require(result.vegetation_maximumForestAltitude < result.vegetation_treeLine
			&& result.vegetation_treeLine <= result.vegetation_snowStart
			&& result.vegetation_snowStart < result.vegetation_snowFull, U"vegetation forest < tree line <= snow start < snow full");
		for (const auto item : files.at(U"streetProfiles"))
		{
			if (!item.key.ends_with(U"_lanes")) { continue; }
			const int lanes=item.value.get<int>();
			require(lanes==1 || lanes%2==0, U"streetProfiles lanes: one-way 1, two-way 2 / 4 / 6");
		}
		require(result.pedestrians_initialParkedCars<=result.pedestrians_parkingSpaces, U"pedestrians parked cars <= spaces");
		require(result.pedestrians_detailedDrawDistance<=result.pedestrians_drawDistance, U"pedestrians detailed draw distance <= maximum");
		return result;
	}

	const Values& get()
	{
		static const Values values = load(FileSystem::IsDirectory(U"assets/generation")
			? U"assets/generation" : U"../../App/assets/generation");
		return values;
	}
}

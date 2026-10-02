#pragma once
#include <Siv3D.hpp>
#include "scene/SceneCommon.hpp"
#include "debug/DebugLog.hpp"

/// @brief Startup arguments shared by the game and regression tests.
namespace GameLaunch
{
	/// @brief Process-only flags applied before the initial scene starts.
	struct Options
	{
		bool directStart = false;
		bool captureCityRenders = false;
		bool captureRoadRenders = false;
		bool uncapped = false;
		Optional<uint64> seedOverride;
	};

	/// @brief Apply explicit arguments over already loaded local preferences.
	[[nodiscard]] inline Options parseCommandLine(const Array<String>& args, SceneData& data)
	{
		Options options;
		for (size_t i = 0; i < args.size(); ++i)
		{
			if (args[i] == U"--seed" && i + 1 < args.size())
			{
				options.seedOverride = ParseOpt<uint64>(args[i + 1]);
				++i;
				continue;
			}

			if (args[i] == U"--audit-road-integrity") { data.auditRoadIntegrity = true; continue; }
			if (args[i] == U"--capture-folder" && i+1<args.size()) { data.captureFolder=args[++i]; continue; }
			if (args[i] == U"--capture-first-person") { data.captureFirstPerson = true; options.captureCityRenders = true; continue; }
			if (args[i] == U"--capture-rail-signs") { data.captureTransportObjects=true; continue; }
			if (args[i] == U"--capture-transport") { data.captureTransport=true; continue; }
			if (args[i] == U"--capture-construction") { data.captureConstruction = true; continue; }
			if (args[i] == U"--capture-road-ux") { data.captureRoadPlanUx = true; continue; }
			if (args[i] == U"--benchmark-streaming") { data.benchmarkStreaming = true; continue; }
			if (args[i] == U"--benchmark-navigation") { data.benchmarkNavigation = true; continue; }
			if (args[i] == U"--playtest-commands" && i+1<args.size()) { data.playtestCommands=args[++i];data.playtest=true;continue; }
			if (args[i] == U"--playtest") { data.playtest = true; continue; }
			if (args[i] == U"--sync-roads") { data.syncRoads = true; continue; }
			if (args[i] == U"--sync-terrain") { data.syncTerrain = true; continue; }

			if (args[i] == U"--render-distance")
			{
				if (i + 1 < args.size())
				{
					if (const auto meters = RenderDistance::parse(args[i + 1]))
					{
						data.renderDistance = *meters;
						++i;
						continue;
					}
				}
				DebugLog::print(U"[RenderDistance] --render-distance requires 0 (default) or 100–20000 meters; unchanged");
				continue;
			}

			if (args[i] == U"--low-spec") { data.lowSpec = true; continue; }

			if (args[i] == U"--uncapped") { options.uncapped = true; continue; }

			if (args[i] == U"--capture-roads")
			{
				options.captureRoadRenders = options.captureCityRenders = true;
				continue;
			}

			if (args[i] == U"--capture-node" && i+1 < args.size())
			{
				data.captureNode = ParseOpt<int>(args[++i]).value_or(-1);
				options.captureRoadRenders = options.captureCityRenders = true;
				continue;
			}

			if (args[i] == U"--capture-city")
			{
				options.captureCityRenders = true;
				continue;
			}

			if (args[i] == U"--inspect-node" && i + 1 < args.size())
			{
				data.inspectNode = ParseOpt<int>(args[++i]).value_or(-1);
				continue;
			}

			// Preserve the first startup target while still applying later ordinary options.
			if (args[i] == U"--new")
			{
				if (!options.directStart)
				{
					data.isNewGame = true;
					data.saveName.clear();
					options.directStart = true;
					DebugLog::print(U"[GameApp] direct start: new game");
				}
				continue;
			}

			if (args[i] == U"--load" && i + 1 < args.size())
			{
				const auto& saveName = args[++i];
				if (!options.directStart)
				{
					data.isNewGame   = false;
					data.saveName    = saveName;
					data.sandboxMode = true;
					options.directStart = true;
					DebugLog::print(U"[GameApp] direct start: load '{}'"_fmt(data.saveName));
				}
				continue;
			}
		}
		return options;
	}
}

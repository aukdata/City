#pragma once
#include "TestRunner.hpp"
#include "src/GameLaunchOptions.hpp"

/// @brief Exercise the actual game argument loop without starting a world.
inline void registerLaunchOptionsTests(TestRunner& runner)
{
	runner.add(U"LaunchOptions.LoadFlagOrder", [](TestContext& context)
	{
		SceneData before, after;
		const auto first = GameLaunch::parseCommandLine({U"--low-spec", U"--render-distance", U"100",
			U"--playtest", U"--seed", U"42", U"--uncapped", U"--load", U"saved_city"}, before);
		const auto last = GameLaunch::parseCommandLine({U"--load", U"saved_city", U"--low-spec",
			U"--render-distance", U"100", U"--playtest", U"--seed", U"42", U"--uncapped"}, after);
		context.expect(first.directStart && last.directStart && !before.isNewGame && !after.isNewGame
			&& before.saveName == U"saved_city" && after.saveName == before.saveName,
			U"Loading the same save is independent of option position");
		context.expect(before.lowSpec && after.lowSpec, U"Low-spec is applied before and after --load");
		context.expectNear(before.renderDistance, 100, 0, U"Distance before --load is applied");
		context.expectNear(after.renderDistance, 100, 0, U"Distance after --load is applied");
		context.expect(before.playtest && after.playtest && first.uncapped && last.uncapped,
			U"Diagnostic options after --load are still processed");
		context.expect(first.seedOverride == Optional<uint64>{42} && last.seedOverride == first.seedOverride,
			U"Seed override after --load is retained");
	});

	runner.add(U"LaunchOptions.NewFlagOrder", [](TestContext& context)
	{
		SceneData before, after;
		before.isNewGame = after.isNewGame = false;
		before.saveName = after.saveName = U"stale_save";
		const auto first = GameLaunch::parseCommandLine({U"--low-spec", U"--render-distance", U"1500",
			U"--seed", U"7", U"--playtest-commands", U"commands.json", U"--new"}, before);
		const auto last = GameLaunch::parseCommandLine({U"--new", U"--low-spec", U"--render-distance",
			U"1500", U"--seed", U"7", U"--playtest-commands", U"commands.json"}, after);
		context.expect(first.directStart && last.directStart && before.isNewGame && after.isNewGame
			&& before.saveName.isEmpty() && after.saveName.isEmpty(),
			U"New-game selection clears the old save in either argument order");
		context.expect(before.lowSpec && after.lowSpec, U"Low-spec is applied before and after --new");
		context.expectNear(before.renderDistance, 1500, 0, U"Distance before --new is applied");
		context.expectNear(after.renderDistance, 1500, 0, U"Distance after --new is applied");
		context.expect(first.seedOverride == Optional<uint64>{7} && last.seedOverride == first.seedOverride,
			U"Seed override after --new is retained");
		context.expect(before.playtest && after.playtest && before.playtestCommands == U"commands.json"
			&& after.playtestCommands == before.playtestCommands,
			U"Valued diagnostic options after --new are still processed");
	});

	runner.add(U"LaunchOptions.FirstTargetWins", [](TestContext& context)
	{
		SceneData loadFirst;
		const auto loaded = GameLaunch::parseCommandLine({U"--load", U"first_city", U"--new",
			U"--load", U"second_city", U"--low-spec"}, loadFirst);
		context.expect(loaded.directStart && !loadFirst.isNewGame && loadFirst.saveName == U"first_city",
			U"The first load keeps ownership of the startup target");
		context.expect(loadFirst.lowSpec, U"Conflicting targets do not suppress later ordinary options");
		SceneData newFirst;
		const auto generated = GameLaunch::parseCommandLine({U"--new", U"--load", U"--capture-city",
			U"--new", U"--render-distance", U"2000"}, newFirst);
		context.expect(generated.directStart && newFirst.isNewGame && newFirst.saveName.isEmpty(),
			U"A later load cannot turn a new game into a save load");
		context.expect(!generated.captureCityRenders,
			U"A later load consumes its operand even when the first target remains selected");
		context.expectNear(newFirst.renderDistance, 2000, 0,
			U"Ordinary options following ignored targets are still applied");
	});

	runner.add(U"LaunchOptions.PreservesPreferences", [](TestContext& context)
	{
		SceneData preferences;
		preferences.lowSpec = true;
		preferences.renderDistance = 900;
		preferences.effectVolume = .25;
		const auto title = GameLaunch::parseCommandLine({}, preferences);
		context.expect(!title.directStart && preferences.lowSpec,
			U"No arguments preserve the title screen and persisted quality");
		context.expectNear(preferences.renderDistance, 900, 0, U"Absent CLI distance preserves preferences");
		context.expectNear(preferences.effectVolume, .25, 0, U"CLI parsing leaves persisted volume alone");
		const auto loaded = GameLaunch::parseCommandLine({U"--load", U"saved_city"}, preferences);
		context.expect(loaded.directStart && preferences.lowSpec && !preferences.isNewGame,
			U"A load without rendering flags preserves quality preferences");
		context.expectNear(preferences.renderDistance, 900, 0, U"A load alone preserves distance preferences");
		SceneData invalid;
		invalid.renderDistance = 900;
		const auto invalidOptions = GameLaunch::parseCommandLine({U"--load", U"saved_city",
			U"--render-distance", U"invalid", U"--low-spec", U"--render-distance"}, invalid);
		context.expect(invalidOptions.directStart && invalid.lowSpec,
			U"An invalid or missing distance after load does not suppress other flags");
		context.expectNear(invalid.renderDistance, 900, 0,
			U"Invalid and missing CLI distances preserve the previous valid preference");
	});
}

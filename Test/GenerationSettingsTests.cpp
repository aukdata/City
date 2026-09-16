#include "TestCases.hpp"
#include "TestRunner.hpp"
#include "src/gen/GenerationSettings.hpp"

void registerGenerationSettingsTests(TestRunner& runner)
{
	runner.add(U"GenerationSettings.RequiredValuesAndValidation", [](TestContext& context)
	{
		const String source = U"../../App/assets/generation";
		const String directory = U"TestResults/generation_settings";
		FileSystem::CreateDirectories(directory);
		for (const auto& path : FileSystem::DirectoryContents(source))
		{
			if (FileSystem::Extension(path) == U"json") { JSON::Load(path).save(directory + U"/" + FileSystem::FileName(path)); }
		}
		const auto defaults = GenerationSettings::load(source);
		context.expect(defaults.terrain_detailOctaves > 0, U"External settings load all required typed fields");
		JSON terrain = JSON::Load(source + U"/terrain.json");
		const auto verify = [&](const JSON& changed, bool valid, double expected = 0)
		{
			changed.save(directory + U"/terrain.json");
			bool accepted = false; double height = 0;
			try { const auto values = GenerationSettings::load(directory); height = values.terrain_firstRangeHeight; accepted = true; }
			catch (const Error&) {}
			context.expect(accepted == valid, U"Malformed/missing/unknown settings fail before generation");
			if (valid) { context.expectNear(height, expected, 0, U"An asset edit changes the loaded value without recompilation"); }
		};
		JSON changed = terrain; changed[U"firstRangeHeight"] = 1234; verify(changed, true, 1234);
		changed = terrain; changed[U"detailOctaves"] = 2.5; verify(changed, false);
		changed = terrain; changed[U"detailFrequency"] = 0; verify(changed, false);
		changed = terrain; changed[U"firstRangeHeight"] = U"high"; verify(changed, false);
		changed = terrain; changed[U"unknownOption"] = 1; verify(changed, false);
		changed = terrain; changed[U"centralLakeThreshold"] = .01; verify(changed, false);
		JSON missing;
		for (const auto item : terrain) { if (item.key != U"firstRangeHeight") { missing[item.key] = item.value; } }
		verify(missing, false);
		terrain.save(directory + U"/terrain.json");
		const auto rejectCombination = [&](StringView file, StringView key, double value)
		{
			const String path=directory+U"/"+file+U".json"; const JSON original=JSON::Load(path); JSON changedGroup=original;
			changedGroup[key]=value; changedGroup.save(path); bool rejected=false;
			try { GenerationSettings::load(directory); } catch (const Error&) { rejected=true; }
			context.expect(rejected,U"Invalid cross-field setting rejected: "+String{file}+U"."+key); original.save(path);
		};
		rejectCombination(U"streetProfiles",U"local_laneWidth",0);
		rejectCombination(U"streetProfiles",U"local_lanes",3);
		rejectCombination(U"traffic",U"minimumTripSteps",80);
		rejectCombination(U"traffic",U"maximumFreightShare",1.5);
		rejectCombination(U"agriculture",U"plotWidth",2);
		rejectCombination(U"rivers",U"minimumHalfWidth",300);
		rejectCombination(U"roads",U"surfaceTolerance",20);
		rejectCombination(U"vegetation",U"treeLine",2100);
		rejectCombination(U"vegetation",U"snowFull",1800);
	});
}

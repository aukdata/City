#include <Siv3D.hpp>
#include <process.h>
#include "TestCases.hpp"
#include "TestRunner.hpp"

void Main()
{
	TestRunner runner;
	registerBezierBuildingFrontageTests(runner);
	registerSaveTransactionTests(runner);
	registerBuildingAssetTests(runner);
	registerCityGenerationTests(runner);
	registerRoadPlanUxTests(runner);
	registerRoadConstructionTests(runner);
	registerRoadIntegrityTests(runner);
	registerUrbanUsabilityTests(runner);
	registerUrbanMorphologyTests(runner);
	const int exitCode = runner.run();
	::_exit(exitCode);
}
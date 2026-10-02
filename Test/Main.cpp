#include <Siv3D.hpp>
#if SIV3D_PLATFORM(WINDOWS)
#include <process.h>
#else
#include <unistd.h>
#endif
#include "TestCases.hpp"
#include "TestRunner.hpp"

void Main()
{
	TestRunner runner;
	registerBezierBuildingFrontageTests(runner);
	registerSaveTransactionTests(runner);
	registerDevelopmentSnapshotTests(runner);
	registerBuildingAssetTests(runner);
	registerCityGenerationTests(runner);
	registerRoadPlanUxTests(runner);
	registerTransportPlanningTests(runner);
	registerRoadConstructionTests(runner);
	registerZoneDevelopmentTests(runner);
	registerRailwayTimetableTests(runner);
	registerMapTransportTests(runner);
	registerRoadIntegrityTests(runner);
	registerUrbanUsabilityTests(runner);
	registerUrbanMorphologyTests(runner);
	registerUrbanFabricTests(runner);
	registerUrbanStructureTests(runner);
	registerUrbanFacilitiesTests(runner);
	registerGenerationRevisionTests(runner);
	registerRegionalTerrainTests(runner);
	registerRiverGenerationTests(runner);
	registerPlayabilityTests(runner);
	registerFringeAgricultureTests(runner);
	registerRoadsideLifeTests(runner);
	registerTrafficScalingTests(runner);
	registerPedestrianTests(runner);
	registerModelLodTests(runner);
	registerGenerationSettingsTests(runner);
	registerRuralVisualTests(runner);
	registerTreeStreamingTests(runner);
	registerComprehensiveTests(runner);
	registerRefactoringTests(runner);
	registerIntegrationRefactoringTests(runner);
	registerCityHudTests(runner);
	registerSharedTransportTests(runner);
	registerMountainRoadTests(runner);
	registerDrivingTests(runner);
	registerSoundEffectsTests(runner);
	const int exitCode = runner.run();
	::_exit(exitCode);
}

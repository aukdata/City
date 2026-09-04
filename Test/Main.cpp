#include <Siv3D.hpp>
#include <process.h>
#include "TestCases.hpp"
#include "TestRunner.hpp"

void Main()
{
	TestRunner runner;
	registerBezierBuildingFrontageTests(runner);
	registerSaveTransactionTests(runner);
	const int exitCode = runner.run();
	::_exit(exitCode);
}

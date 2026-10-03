#pragma once
#include <cmath>
#include <fstream>
#include <set>
#include <sstream>
#include <stdexcept>
#include "TestRunner.hpp"
#include "src/world/World.hpp"

/// @brief Sample fixed public terrain queries before and after river generation.
namespace TerrainFoundationProbe
{
	struct Sample
	{
		String label;
		double requestedX=0, requestedZ=0;
		float queryX=0, queryZ=0, before=0, after=0;
	};
	inline void run(TestContext& context)
	{
		const FilePath inputPath=FileSystem::FullPath(U"../fixtures/terrain_foundation_probe_points.csv");
		std::ifstream input{Unicode::ToUTF8(inputPath)};
		context.expect(input.is_open(),U"Fixed terrain probe CSV opens");
		if (!input.is_open()) { return; }
		std::string line; std::getline(input,line);
		if (!line.empty() && line.back()=='\r') { line.pop_back(); }
		context.expect(line=="probe,x_m,z_m,sampling,purpose",U"Probe CSV schema matches the reviewed plan");
		Array<Sample> samples; std::set<std::pair<double,double>> unique;
		try
		{
			while (std::getline(input,line))
			{
				if (line.empty()) { continue; }
				std::istringstream row{line}; std::string label,x,z;
				if (!std::getline(row,label,',') || !std::getline(row,x,',') || !std::getline(row,z,',')) { throw std::runtime_error("Incomplete probe row"); }
				size_t endX=0,endZ=0; const double wx=std::stod(x,&endX),wz=std::stod(z,&endZ);
				if (endX!=x.size() || endZ!=z.size() || !std::isfinite(wx) || !std::isfinite(wz)
					|| wx<0 || wz<0 || wx>WORLD_SIZE || wz>WORLD_SIZE) { throw std::runtime_error("Invalid probe coordinate"); }
				samples << Sample{Unicode::FromUTF8(label),wx,wz,static_cast<float>(wx),static_cast<float>(wz),0,0};
				unique.emplace(wx,wz);
			}
		}
		catch (const std::exception& error)
		{
			context.expect(false,U"Probe CSV parse failed: "+Unicode::FromUTF8(error.what())); return;
		}
		context.expectEqual(samples.size(),2729,U"All reviewed probe entries are present");
		context.expectEqual(unique.size(),2572,U"The reviewed unique coordinate count is preserved");
		if (samples.size()!=2729 || unique.size()!=2572) { return; }
		World world; world.setGenerationParams(42,WORLD_SIZE,WORLD_SIZE);
		int invalid=0;
		for (auto& sample:samples)
		{
			sample.before=world.computeHeight(sample.queryX,sample.queryZ);
			invalid+=!std::isfinite(sample.before);
		}
		world.generateRivers();
		for (auto& sample:samples)
		{
			sample.after=world.computeHeight(sample.queryX,sample.queryZ);
			invalid+=!std::isfinite(sample.after);
		}
		context.expectEqual(invalid,0,U"Every before/after terrain sample is finite");
		context.expect(!world.rivers().reaches.isEmpty(),U"Post-river samples use an actual generated network");
		if (invalid) { return; }
		JSON report; report[U"seed"]=42;report[U"entries"]=samples.size();report[U"uniqueCoordinates"]=unique.size();
		report[U"worldExtentMetres"]=WORLD_SIZE;report[U"sourceCsv"]=U"Test/fixtures/terrain_foundation_probe_points.csv";
		report[U"scope"]=U"CPU public computeHeight queries on one World; before generateRivers then after; no chunks or renderer";
		report[U"coordinates"]=U"Requested CSV doubles cast explicitly to float32 for both World queries; both values retained";
		report[U"reachCount"]=world.rivers().reaches.size();
		for (size_t i=0;i<samples.size();++i)
		{
			const auto& s=samples[i];JSON row;row[U"probe"]=s.label;
			row[U"requestedX"]=s.requestedX;row[U"requestedZ"]=s.requestedZ;row[U"queryX"]=s.queryX;row[U"queryZ"]=s.queryZ;
			row[U"beforeRivers"]=s.before;row[U"afterRivers"]=s.after;row[U"riverHeightChange"]=static_cast<double>(s.after)-s.before;
			report[U"samples"][i]=row;
		}
		context.expect(report.save(U"TestResults/terrain_foundation_probe_seed42.json"),U"All paired terrain samples are saved");
	}
}

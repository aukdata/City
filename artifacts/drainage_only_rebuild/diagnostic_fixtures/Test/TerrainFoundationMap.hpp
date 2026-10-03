#pragma once
#include <cmath>
#include "TestRunner.hpp"
#include "src/world/World.hpp"

/// @brief CPU-only map of actual generated heights and emitted river coordinates.
namespace TerrainFoundationMap
{
	inline void run(TestContext& context)
	{
		constexpr int size = 512;
		constexpr uint64 seed = 42;
		const double span = static_cast<double>(WORLD_SIZE), step = span / (size - 1);
		World world; world.setGenerationParams(seed, WORLD_SIZE, WORLD_SIZE); world.generateRivers();
		Grid<float> heights(size, size);
		double low = 1e30, high = -1e30;
		int invalid = 0;
		for (int z = 0; z < size; ++z) { for (int x = 0; x < size; ++x)
		{
			const float h = world.computeHeight(static_cast<float>(x * step), static_cast<float>(z * step));
			heights[{x, z}] = h;
			if (!std::isfinite(h)) { ++invalid; continue; }
			low = Min(low, static_cast<double>(h)); high = Max(high, static_cast<double>(h));
		} }
		context.expectEqual(invalid, 0, U"Every generated map height is finite");
		context.expect(low < 0 && high > 600, U"The seed42 map contains actual sea and mountain relief, not a missing-chunk zero field");
		context.expect(!world.rivers().reaches.isEmpty(), U"Map overlays actual emitted reaches");
		if (invalid) { return; }
		const Array<double> breaks{0, 50, 150, 600, 1500, 3000};
		const Array<ColorF> colors{ColorF{.43,.66,.43},ColorF{.62,.75,.49},ColorF{.77,.76,.56},ColorF{.64,.53,.40},ColorF{.69,.67,.63},ColorF{.95,.94,.91}};
		Image terrain{size, size, Palette::Black};
		for (int z = 0; z < size; ++z) { for (int x = 0; x < size; ++x)
		{
			const double h = heights[{x,z}];
			ColorF base{.12,.34,.53};
			if (h >= 0)
			{
				size_t band = 0; while (band + 1 < breaks.size() && h > breaks[band+1]) { ++band; }
				base = colors[band];
				if (band + 1 < breaks.size())
				{
					const double t = Clamp((h-breaks[band])/(breaks[band+1]-breaks[band]),0.0,1.0);
					base = ColorF{base.r+(colors[band+1].r-base.r)*t,base.g+(colors[band+1].g-base.g)*t,base.b+(colors[band+1].b-base.b)*t};
				}
				const int lx=Max(0,x-1),rx=Min(size-1,x+1),uz=Max(0,z-1),dz=Min(size-1,z+1);
				const double dx=(heights[{rx,z}]-heights[{lx,z}])/((rx-lx)*step);
				const double dy=(heights[{x,dz}]-heights[{x,uz}])/((dz-uz)*step);
				const double lighting=Clamp((.816496580927726+.408248290463863*dx+.408248290463863*dy)/std::sqrt(1+dx*dx+dy*dy),0.0,1.0);
				const double shade=.65+.35*lighting;
				base=ColorF{base.r*shade,base.g*shade,base.b*shade};
			}
			terrain[{x,z}]=base.toColor();
		} }
		FileSystem::CreateDirectories(U"Screenshot");
		context.expect(terrain.save(U"Screenshot/terrain_foundation_seed42_heights.png"), U"CPU terrain map is saved");
		Image overlay=terrain;
		JSON report; report[U"seed"]=seed; report[U"widthPixels"]=size;report[U"heightPixels"]=size;
		report[U"extentMetres"]=span; report[U"sampleSpacingMetres"]=step;
		report[U"axis"]=U"x increases right, z increases down; full [0,WORLD_SIZE] extent";
		report[U"scope"]=U"CPU map from post-river World::computeHeight; no chunks, buildings, GPU scene, or first-person verification";
		report[U"rawHeightFormat"]=U"512x512 native little-endian float32, x fastest then increasing z; Linux x86-64";
		report[U"rawExpectedBytes"]=size*size*4;
		report[U"coordinateQueries"]=U"x,z = float32(index * (double(WORLD_SIZE)/511)), including both extent endpoints";
		report[U"waterColor"]=U"RGB(31,87,135) below zero";
		report[U"elevationBreaksMetres"]=breaks;
		for(size_t i=0;i<colors.size();++i) { report[U"elevationRgbNormalizedDirectToColor"][i]=Array<double>{colors[i].r,colors[i].g,colors[i].b}; }
		report[U"hillshade"]=U"normal=(-dh/dx,1,-dh/dz), light=(-1,2,-1)/sqrt(6), multiplier=.65+.35*clamp(dot,0,1); one-pixel centered differences";
		report[U"minimumHeight"]=low; report[U"maximumHeight"]=high;
		report[U"riverOverlay"]=U"Uniform1px cyan centerline, schematic; does not depict physical width or bank appearance";
		report[U"scaleBarMetres"]=10000;report[U"scaleBarStartPixel"]=Array<int>{20,490};report[U"scaleBarLengthPixels"]=10000/step;
		for (size_t i=0;i<world.rivers().reaches.size();++i)
		{
			const auto& reach=world.rivers().reaches[i];
			const Vec2 a{reach.start.x/step,reach.start.z/step},b{reach.end.x/step,reach.end.z/step};
			Line{a,b}.overwrite(overlay,1,Color{35,195,235},Antialiased::Yes);
			report[U"reaches"][i]=Array<double>{reach.start.x,reach.start.y,reach.start.z,reach.end.x,reach.end.y,reach.end.z,reach.halfWidth};
		}
		report[U"reachCount"]=world.rivers().reaches.size();
		Line{20,490,20+10000/step,490}.overwrite(overlay,5,Palette::White);
		Line{20,490,20+10000/step,490}.overwrite(overlay,2,Palette::Black);
		context.expect(overlay.save(U"Screenshot/terrain_foundation_seed42_rivers.png"), U"River-overlay map is saved");
		BinaryWriter raw{U"TestResults/terrain_foundation_map_seed42.f32"};
		context.expect(raw.isOpen(),U"Raw height output opens successfully");
		bool written=raw.isOpen();
		for(int z=0;z<size;++z) { for(int x=0;x<size;++x) { const bool ok=raw.write(heights[{x,z}]); written=written && ok; } }
		context.expect(written,U"Every raw float height is written successfully");
		context.expectEqual(raw.size(),int64{size}*size*4,U"Raw height file contains exactly512x512 float32 samples");
		raw.close();
		context.expectEqual(FileSystem::FileSize(U"TestResults/terrain_foundation_map_seed42.f32"),int64{size}*size*4,U"Closed raw file has the expected byte count");
		context.expect(report.save(U"TestResults/terrain_foundation_map_seed42.json"),U"Map provenance and exact river coordinates are saved");
	}
}

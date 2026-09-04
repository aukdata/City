#include <Siv3D.hpp>
#include "TestCases.hpp"
#include "TestRunner.hpp"
#include "../src/gen/MapGenerator.hpp"
#include "../src/gen/DistrictRoads.hpp"

namespace
{
	enum class VisualLandUse
	{
		None,
		Agriculture,
		LowResidential,
		Residential,
		Commercial,
		Industrial,
	};

	struct PreviewViewport
	{
		Vec2 center{ WORLD_SIZE * 0.5f, WORLD_SIZE * 0.5f };
		float span = WORLD_SIZE;
	};

	uint32 visualHash(uint64 seed, int settlementIndex, int gx, int gz)
	{
		uint64 value = seed ^ (static_cast<uint64>(settlementIndex) * 0x9E3779B97F4A7C15ULL);
		value ^= static_cast<uint64>(gx) * 0xBF58476D1CE4E5B9ULL;
		value ^= static_cast<uint64>(gz) * 0x94D049BB133111EBULL;
		value ^= value >> 30;
		value *= 0xBF58476D1CE4E5B9ULL;
		value ^= value >> 27;
		value *= 0x94D049BB133111EBULL;
		value ^= value >> 31;
		return static_cast<uint32>(value);
	}

	float visualHash01(uint64 seed, int settlementIndex, int gx, int gz)
	{
		return static_cast<float>(visualHash(seed, settlementIndex, gx, gz) & 0xFFFFu) / 65535.0f;
	}

	Vec2 visualAxisX(const MapGenerator::Settlement& settlement, uint64 seed, int settlementIndex)
	{
		if (settlement.gridAxisX.lengthSq() > 1e-6f)
		{
			Vec2 axis = settlement.gridAxisX;
			axis.normalize();
			return axis;
		}
		const float angle = static_cast<float>((seed + settlementIndex * 97) % 6283) * 0.001f;
		return Vec2{ Math::Cos(angle), Math::Sin(angle) };
	}

	VisualLandUse pickVisualLandUse(uint64 seed, int settlementIndex,
		const MapGenerator::Settlement& settlement, const World& world, float wx, float wz)
	{
		if (world.computeHeight(wx, wz) < 0.0f) return VisualLandUse::None;

		const Vec2 axisX = visualAxisX(settlement, seed, settlementIndex);
		const Vec2 axisZ{ -axisX.y, axisX.x };
		const Vec2 delta{ wx - static_cast<float>(settlement.center.x), wz - static_cast<float>(settlement.center.y) };
		const float lx = static_cast<float>(delta.dot(axisX));
		const float lz = static_cast<float>(delta.dot(axisZ));
		const int gx = static_cast<int>(wx / 32.0f);
		const int gz = static_cast<int>(wz / 32.0f);
		const float n = visualHash01(seed, settlementIndex, gx / 2, gz / 2);
		const float fine = visualHash01(seed ^ 0xA53A9E11ULL, settlementIndex, gx, gz);

		if (settlement.kind == MapGenerator::SettlementKind::CastleTown)
		{
			const float halfX = Max(920.0f, settlement.radius * 1.20f) * (0.94f + n * 0.14f);
			const float halfZ = Max(720.0f, settlement.radius * 0.94f) * (0.90f + n * 0.16f);
			const float nx = Math::Abs(lx) / halfX;
			const float nz = Math::Abs(lz) / halfZ;
			const float roundedShape = Math::Pow(nx, 1.45f) + Math::Pow(nz, 1.45f);
			const float cornerBite = (nx > 0.62f && nz > 0.62f) ? 0.22f + fine * 0.16f : 0.0f;
			const float boundaryNoise = (n - 0.5f) * 0.34f + Math::Sin((lx + lz) * 0.003f) * 0.07f;
			const float corridor = Min(Math::Abs(lz) / 145.0f + Math::Abs(lx) / (halfX * 1.46f),
				Math::Abs(lx) / 135.0f + Math::Abs(lz) / (halfZ * 1.38f));
			const bool urbanCorridor = corridor < 1.0f;
			const bool urbanPocket = roundedShape < (1.22f + boundaryNoise - cornerBite);
			if (!urbanPocket && !urbanCorridor)
			{
				const float fieldShape = Math::Pow(nx, 1.20f) + Math::Pow(nz, 1.20f);
				return (fieldShape < 1.62f && fine < 0.70f) ? VisualLandUse::Agriculture : VisualLandUse::None;
			}
			if (roundedShape > 0.84f && fine < 0.24f) return VisualLandUse::Industrial;
			if (urbanCorridor || (Math::Abs(lx) < 380.0f && Math::Abs(lz) < 300.0f && fine < 0.74f)) return VisualLandUse::Commercial;
			if (roundedShape < 0.70f || fine < 0.56f) return VisualLandUse::Residential;
			return VisualLandUse::LowResidential;
		}

		if (settlement.kind == MapGenerator::SettlementKind::PostTown)
		{
			const float length = Max(520.0f, settlement.radius * 2.05f) * (0.90f + n * 0.20f);
			const float width = Max(210.0f, settlement.radius * 0.88f) * (0.88f + n * 0.20f);
			const float ribbon = Math::Abs(lx) / length + Math::Abs(lz) / width;
			if (ribbon > 1.68f) return fine < 0.68f ? VisualLandUse::Agriculture : VisualLandUse::None;
			if (ribbon > 1.10f && fine < 0.16f) return VisualLandUse::Industrial;
			if (Math::Abs(lz) < 82.0f && Math::Abs(lx) < length * 0.86f) return VisualLandUse::Commercial;
			return (ribbon < 1.10f) ? VisualLandUse::Residential : VisualLandUse::LowResidential;
		}

		const float hamlet = Math::Abs(lx) / Max(210.0f, settlement.radius * 1.45f)
			+ Math::Abs(lz) / Max(90.0f, settlement.radius * 0.70f);
		if (hamlet < 0.86f) return VisualLandUse::LowResidential;
		if (hamlet < 1.12f && fine < 0.40f) return VisualLandUse::Residential;
		const float fieldRadius = Max(420.0f, settlement.radius * 3.2f) * (0.90f + n * 0.18f);
		if (delta.length() < fieldRadius) return VisualLandUse::Agriculture;
		return VisualLandUse::None;
	}

	int landUsePriority(VisualLandUse use)
	{
		switch (use)
		{
		case VisualLandUse::Commercial: return 5;
		case VisualLandUse::Residential: return 4;
		case VisualLandUse::LowResidential: return 3;
		case VisualLandUse::Industrial: return 2;
		case VisualLandUse::Agriculture: return 1;
		default: return 0;
		}
	}

	Color rasterColorForHeight(float h)
	{
		if (h < 0.0f) return Color{ 88, 137, 171 };
		if (h < 12.0f) return Color{ 176, 190, 132 };
		if (h < 80.0f) return Color{ 137, 155, 105 };
		if (h < 220.0f) return Color{ 104, 125, 88 };
		return Color{ 80, 92, 75 };
	}

	Color landUseColor(VisualLandUse use, uint32 hash)
	{
		const int jitter = static_cast<int>(hash % 15u) - 7;
		switch (use)
		{
		case VisualLandUse::Agriculture: return Color{ static_cast<uint8>(154 + jitter), static_cast<uint8>(170 + jitter), static_cast<uint8>(94 + jitter) };
		case VisualLandUse::LowResidential: return Color{ static_cast<uint8>(174 + jitter), static_cast<uint8>(168 + jitter), static_cast<uint8>(151 + jitter) };
		case VisualLandUse::Residential: return Color{ static_cast<uint8>(188 + jitter), static_cast<uint8>(185 + jitter), static_cast<uint8>(174 + jitter) };
		case VisualLandUse::Commercial: return Color{ static_cast<uint8>(209 + jitter), static_cast<uint8>(208 + jitter), static_cast<uint8>(200 + jitter) };
		case VisualLandUse::Industrial: return Color{ static_cast<uint8>(224 + jitter), static_cast<uint8>(224 + jitter), static_cast<uint8>(216 + jitter) };
		default: return Color{ 0, 0, 0 };
		}
	}

	Color rasterRoadColor(RoadType type)
	{
		if (type == RoadType::Arterial) return Color{ 246, 241, 219 };
		if (type == RoadType::Expressway || type == RoadType::Highway) return Color{ 255, 213, 106 };
		return Color{ 198, 196, 188 };
	}

	String svgColorForHeight(float h)
	{
		if (h < 0.0f) return U"#5d8fb0";
		if (h < 12.0f) return U"#b6c58b";
		if (h < 80.0f) return U"#8fa36f";
		if (h < 220.0f) return U"#6f845c";
		return U"#56624f";
	}

	String roadColor(RoadType type)
	{
		if (type == RoadType::Arterial) return U"#f5f1dc";
		if (type == RoadType::Expressway || type == RoadType::Highway) return U"#ffd56a";
		return U"#d8d2be";
	}

	String settlementColor(MapGenerator::SettlementKind kind)
	{
		if (kind == MapGenerator::SettlementKind::CastleTown) return U"#d94a38";
		if (kind == MapGenerator::SettlementKind::PostTown) return U"#e29338";
		return U"#f0d077";
	}

	Vec2 worldToPixel(float wx, float wz, const PreviewViewport& viewport, int size)
	{
		const float minX = static_cast<float>(viewport.center.x) - viewport.span * 0.5f;
		const float minZ = static_cast<float>(viewport.center.y) - viewport.span * 0.5f;
		const double scale = static_cast<double>(size) / viewport.span;
		return Vec2{ (wx - minX) * scale, (wz - minZ) * scale };
	}

	void blendRect(Image& image, int x0, int y0, int w, int h, Color color, float alpha)
	{
		const int x1 = Min(static_cast<int>(image.width()), x0 + w);
		const int y1 = Min(static_cast<int>(image.height()), y0 + h);
		for (int y = Max(0, y0); y < y1; ++y)
		{
			for (int x = Max(0, x0); x < x1; ++x)
			{
				Color& dst = image[y][x];
				dst.r = static_cast<uint8>(dst.r * (1.0f - alpha) + color.r * alpha);
				dst.g = static_cast<uint8>(dst.g * (1.0f - alpha) + color.g * alpha);
				dst.b = static_cast<uint8>(dst.b * (1.0f - alpha) + color.b * alpha);
			}
		}
	}

	void drawLandUse(Image& image, const World& world, const Array<MapGenerator::Settlement>& settlements,
		uint64 seed, const PreviewViewport& viewport)
	{
		const int size = static_cast<int>(image.width());
		const double worldPerPixel = viewport.span / size;
		const int tilePx = Max(3, static_cast<int>(Round(48.0 / worldPerPixel)));
		const float minX = static_cast<float>(viewport.center.x) - viewport.span * 0.5f;
		const float minZ = static_cast<float>(viewport.center.y) - viewport.span * 0.5f;
		for (int py = 0; py < size; py += tilePx)
		{
			for (int px = 0; px < size; px += tilePx)
			{
				const float wx = minX + static_cast<float>((px + tilePx * 0.5) * worldPerPixel);
				const float wz = minZ + static_cast<float>((py + tilePx * 0.5) * worldPerPixel);
				VisualLandUse chosen = VisualLandUse::None;
				uint32 chosenHash = 0;
				for (int si = 0; si < static_cast<int>(settlements.size()); ++si)
				{
					const VisualLandUse candidate = pickVisualLandUse(seed, si, settlements[si], world, wx, wz);
					if (landUsePriority(candidate) >= landUsePriority(chosen))
					{
						chosen = candidate;
						chosenHash = visualHash(seed, si, static_cast<int>(wx / 32.0f), static_cast<int>(wz / 32.0f));
					}
				}
				if (chosen == VisualLandUse::None) continue;
				const Color base = landUseColor(chosen, chosenHash);
				const float alpha = chosen == VisualLandUse::Agriculture ? 0.66f : 0.82f;
				blendRect(image, px, py, tilePx, tilePx, base, alpha);

				if (chosen != VisualLandUse::Agriculture && (chosenHash % 100u) < 82u)
				{
					const int inset = chosen == VisualLandUse::Industrial ? 0 : Max(1, tilePx / 5);
					const int roof = chosen == VisualLandUse::Industrial ? tilePx : Max(2, tilePx / 2 + static_cast<int>(chosenHash % 3u));
					blendRect(image, px + inset, py + inset, roof, roof, Color{ 236, 235, 228 }, 0.88f);
				}
				else if (chosen == VisualLandUse::Agriculture && (chosenHash % 100u) < 36u)
				{
					Line{ Vec2{ static_cast<double>(px), static_cast<double>(py + tilePx / 2) },
						Vec2{ static_cast<double>(px + tilePx), static_cast<double>(py + tilePx / 2) } }
						.overwrite(image, 1, Color{ 132, 150, 82 });
				}
			}
		}
	}

	void writePreviewPng(const FilePath& path, const World& world,
		const RoadNetwork& network, const Array<MapGenerator::Settlement>& settlements,
		uint64 seed, const PreviewViewport& viewport)
	{
		constexpr int size = 1024;
		Image image{ size, size, Color{ 116, 132, 97 } };
		const float minX = static_cast<float>(viewport.center.x) - viewport.span * 0.5f;
		const float minZ = static_cast<float>(viewport.center.y) - viewport.span * 0.5f;
		const double worldPerPixel = viewport.span / size;

		for (int py = 0; py < size; ++py)
		{
			for (int px = 0; px < size; ++px)
			{
				const float wx = minX + static_cast<float>((px + 0.5) * worldPerPixel);
				const float wz = minZ + static_cast<float>((py + 0.5) * worldPerPixel);
				image[py][px] = rasterColorForHeight(world.computeHeight(wx, wz));
			}
		}

		drawLandUse(image, world, settlements, seed, viewport);

		for (const auto& edge : network.edges())
		{
			if (edge.id < 0) continue;
			const auto bez = network.getBezier(edge.id);
			if (!bez) continue;
			const Color color = rasterRoadColor(edge.roadType);
			const int thickness = edge.roadType == RoadType::Arterial ? 5 : 1;
			constexpr int segments = 48;
			for (int i = 0; i < segments; ++i)
			{
				const Vec3 a = bez->positionAt(bez->totalLength * (static_cast<float>(i) / segments));
				const Vec3 b = bez->positionAt(bez->totalLength * (static_cast<float>(i + 1) / segments));
				Line{ worldToPixel(static_cast<float>(a.x), static_cast<float>(a.z), viewport, size),
					worldToPixel(static_cast<float>(b.x), static_cast<float>(b.z), viewport, size) }
					.overwrite(image, thickness, color);
			}
		}

		image.save(path);
	}

	void writePreviewSvg(const FilePath& path, const World& world,
		const RoadNetwork& network, const Array<MapGenerator::Settlement>& settlements)
	{
		TextWriter writer{ path };
		if (!writer) return;
		constexpr int size = 1600;
		constexpr double scale = static_cast<double>(size) / WORLD_SIZE;
		writer.writeln(U"<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"{}\" height=\"{}\" viewBox=\"0 0 {} {}\">"_fmt(size, size, size, size));
		writer.writeln(U"<rect width=\"100%\" height=\"100%\" fill=\"#748461\"/>");

		constexpr int terrainSamples = 64;
		constexpr double tile = static_cast<double>(size) / terrainSamples;
		for (int z = 0; z < terrainSamples; ++z)
		{
			for (int x = 0; x < terrainSamples; ++x)
			{
				const float wx = static_cast<float>((x + 0.5) * WORLD_SIZE / terrainSamples);
				const float wz = static_cast<float>((z + 0.5) * WORLD_SIZE / terrainSamples);
				const float h = world.computeHeight(wx, wz);
				writer.writeln(U"<rect x=\"{:.2f}\" y=\"{:.2f}\" width=\"{:.2f}\" height=\"{:.2f}\" fill=\"{}\" opacity=\"0.62\"/>"_fmt(
					x * tile, z * tile, tile + 0.2, tile + 0.2, svgColorForHeight(h)));
			}
		}

		writer.writeln(U"<g fill=\"none\" stroke-linecap=\"round\" stroke-linejoin=\"round\">");
		for (const auto& edge : network.edges())
		{
			if (edge.id < 0) continue;
			const RoadNode* a = network.getNode(edge.nodeA);
			const RoadNode* b = network.getNode(edge.nodeB);
			if (!a || !b) continue;
			const double width = edge.roadType == RoadType::Arterial ? 1.8 : 0.72;
			writer.writeln(U"<path d=\"M {:.2f} {:.2f} C {:.2f} {:.2f}, {:.2f} {:.2f}, {:.2f} {:.2f}\" stroke=\"{}\" stroke-width=\"{:.2f}\" opacity=\"0.88\"/>"_fmt(
				a->position.x * scale, a->position.z * scale,
				edge.ctrlA.x * scale, edge.ctrlA.z * scale,
				edge.ctrlB.x * scale, edge.ctrlB.z * scale,
				b->position.x * scale, b->position.z * scale,
				roadColor(edge.roadType), width));
		}
		writer.writeln(U"</g>");

		for (const auto& settlement : settlements)
		{
			const double r = settlement.kind == MapGenerator::SettlementKind::CastleTown ? 4.6
				: settlement.kind == MapGenerator::SettlementKind::PostTown ? 2.8 : 1.5;
			writer.writeln(U"<circle cx=\"{:.2f}\" cy=\"{:.2f}\" r=\"{:.2f}\" fill=\"{}\" opacity=\"0.78\"/>"_fmt(
				settlement.center.x * scale, settlement.center.y * scale, r, settlementColor(settlement.kind)));
		}
		writer.writeln(U"</svg>");
	}

	PreviewViewport cityViewport(const Array<MapGenerator::Settlement>& settlements)
	{
		for (const auto& settlement : settlements)
		{
			if (settlement.kind == MapGenerator::SettlementKind::CastleTown)
			{
				return PreviewViewport{ Vec2{ settlement.center.x, settlement.center.y }, Max(2600.0f, settlement.radius * 3.1f) };
			}
		}
		return PreviewViewport{};
	}
}

void registerCityGenerationVisualTests(TestRunner& runner)
{
	runner.add(U"CityGeneration.RealisticPreview", [](TestContext& context)
	{
		constexpr uint64 seed = 20260904ULL;
		World world;
		world.reserveChunks();
		world.setGenerationParams(seed, WORLD_SIZE, WORLD_SIZE);
		for (int chunkY = 0; chunkY < WORLD_CHUNKS; ++chunkY)
		{
			for (int chunkX = 0; chunkX < WORLD_CHUNKS; ++chunkX)
			{
				world.installChunkDirect(Point{ chunkX, chunkY }, world.buildHeightMap(Point{ chunkX, chunkY }));
			}
		}

		Array<MapGenerator::Settlement> settlements = MapGenerator::placeAllSettlements(seed, world);
		RoadNetwork network;
		MapGenerator::generateGlobalRoads(seed, settlements, world, network);
		MapGenerator::generateDistrictRoads(seed, settlements, world, network);
		DistrictRoads::straightenCastleTownRoads(settlements, world, network);

		int castleTowns = 0;
		int postTowns = 0;
		int villages = 0;
		for (const auto& settlement : settlements)
		{
			if (settlement.kind == MapGenerator::SettlementKind::CastleTown) ++castleTowns;
			else if (settlement.kind == MapGenerator::SettlementKind::PostTown) ++postTowns;
			else ++villages;
		}

		int arterialEdges = 0;
		int localEdges = 0;
		for (const auto& edge : network.edges())
		{
			if (edge.id < 0) continue;
			if (edge.roadType == RoadType::Arterial) ++arterialEdges;
			if (edge.roadType == RoadType::LocalRoad) ++localEdges;
		}

		FileSystem::CreateDirectories(U"TestResults");
		writePreviewSvg(U"TestResults/city_generation_preview.svg", world, network, settlements);
		writePreviewPng(U"TestResults/city_generation_preview.png", world, network, settlements, seed, PreviewViewport{});
		writePreviewPng(U"TestResults/city_generation_detail.png", world, network, settlements, seed, cityViewport(settlements));

		context.expect(settlements.size() >= 80, U"settlement count should be enough to create a lived-in region");
		context.expect(castleTowns >= 2, U"at least two castle towns should anchor regional structure");
		context.expect(postTowns >= 1, U"post towns should appear around the arterial chain");
		context.expect(villages > castleTowns + postTowns, U"villages should dominate the rural landscape");
		context.expect(arterialEdges >= 20, U"arterial road chain should be visible in preview");
		context.expect(localEdges > arterialEdges, U"local roads should outnumber arterials for urban grain");
		context.expect(FileSystem::IsFile(U"TestResults/city_generation_preview.svg"), U"preview SVG should be written for visual review");
		context.expect(FileSystem::IsFile(U"TestResults/city_generation_preview.png"), U"regional PNG should be written for visual review");
		context.expect(FileSystem::IsFile(U"TestResults/city_generation_detail.png"), U"city detail PNG should be written for visual review");
	});
}
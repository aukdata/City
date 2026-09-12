#pragma once
#include "RoadNetwork.hpp"
#include "../world/World.hpp"
#include "../gen/ParcelGeometry.hpp"
#include <functional>

/// @brief Saved game time drives construction; rendering never advances the simulation.
namespace RoadConstruction
{
	enum class Stage { Clearance, Earthwork, BaseCourse, Paving, Marking, Complete };
	struct Progress
	{
		Stage stage = Stage::Clearance;
		double total = 0.0;
		double fraction = 0.0;
		bool elevated = false,tunnel=false;
		[[nodiscard]] String name() const;
	};
	[[nodiscard]] Progress progress(double elapsed, double duration, bool elevated);
	[[nodiscard]] Progress progress(const RoadNetwork& network, const RoadEdge& edge, GameTime now);
	struct Bounds { Vec3 center; Vec3 size; double angle = 0; };
	using BoundsResolver = std::function<Optional<Bounds>(const Chunk&, int, int)>;
	struct Cell { Point chunk; Point cell; bool building = false; };
	/// @brief Actual curved cross section, rotated building bounds, deck clearance and pier foundations.
	[[nodiscard]] Array<Cell> affectedCells(const RoadNetwork& network, const Array<int>& edges,
		const World& world, const BoundsResolver& resolver = {});
	/// @brief Tombstones survive procedural building regeneration, even when a road is later deleted.
	class ClearanceLedger
	{
	public:
		int clear(World& world, const Array<Cell>& cells);
		void apply(World& world) const;
		[[nodiscard]] bool save(FilePathView path) const;
		bool load(FilePathView path);
		[[nodiscard]] size_t size() const { return m_cells.size(); }
	private:
		Array<Cell> m_cells;
		HashSet<uint64> m_keys;
	};
}

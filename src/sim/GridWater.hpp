#pragma once
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <vector>

namespace GridWaterPrototype
{
	/// @brief A point source, in cubic metres per simulation second.
	struct Source
	{
		std::size_t cell = 0;
		double rate = 0;
	};

	/// @brief Conservative, overdamped water routing on a square heightfield.
	/// @details This is a game-oriented height redistribution model, not a shallow-water momentum solver.
	/// Standard containers intentionally keep this isolated experiment independent of Siv3D.
	class GridWater
	{
	public:
		GridWater(int width, int height, double cellSize, double diffusivity = 4.0);
		/// @brief Apply sources and synchronous face fluxes, subdividing above the stability bound.
		void advance(double seconds);
		/// @brief Strict four-neighbour diffusion bound with a 10% margin.
		double stableStep() const { return 0.9 * cellSize * cellSize / (4.0 * diffusivity); }
		std::size_t index(int x, int z) const { return static_cast<std::size_t>(z * width + x); }
		double volume() const;
		double surface(int x, int z) const { const auto i = index(x, z); return bed[i] + depth[i]; }
		std::size_t allocatedBytes() const;
		/// @brief Prescribed drain cells may receive water but never donate it to land.
		void setSinkOnlyMask(std::vector<std::uint8_t> mask);

		int width;
		int height;
		double cellSize;
		double diffusivity;
		std::vector<double> bed;
		std::vector<double> depth;
		std::vector<Source> sources;
		bool rightOutflow = false; ///< Only the explicitly selected outer edge drains.
		double exteriorHead = -1.0; ///< Sea or downstream reservoir surface; sink-only in this prototype.
		double injected = 0;
		double drained = 0;
		double lastMaxDepthChange = 0;
		std::size_t substeps = 0;
		std::size_t limitedCells = 0;

	private:
		void step(double seconds);
		double faceFlux(std::size_t a, std::size_t b) const;
		std::vector<std::uint8_t> m_sinkOnlyMask;
		std::vector<double> m_fluxX;
		std::vector<double> m_fluxZ;
		std::vector<double> m_donorScale;
		std::vector<double> m_delta;
	};
}

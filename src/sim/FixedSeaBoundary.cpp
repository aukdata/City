#include "FixedSeaBoundary.hpp"
#include <utility>

namespace GridWaterPrototype
{
	FixedSeaBoundary::FixedSeaBoundary(GridWater& grid, std::vector<std::uint8_t> oceanMask, double seaLevel, bool allowInflow)
		: m_grid(grid), m_oceanMask(std::move(oceanMask)), m_seaLevel(seaLevel)
	{
		if (m_oceanMask.size() != m_grid.depth.size() || !std::isfinite(m_seaLevel))
		{
			throw std::invalid_argument("Invalid fixed sea boundary");
		}
		for (std::size_t i = 0; i < m_oceanMask.size(); ++i)
		{
			if (m_oceanMask[i] != 0)
			{
				if (!(m_grid.bed[i] < m_seaLevel))
				{
					throw std::invalid_argument("An ocean reservoir cell must be below sea level");
				}
				m_oceanCells.push_back(i);
			}
		}
		m_grid.setSinkOnlyMask(allowInflow ? std::vector<std::uint8_t>{} : m_oceanMask);
		initialize();
	}

	void FixedSeaBoundary::initialize()
	{
		for (const auto i : m_oceanCells)
		{
			m_grid.depth[i] = std::max(0.0, m_seaLevel - m_grid.bed[i]);
		}
		m_oceanOutflow = 0;
		m_oceanInflow = 0;
		m_initialOwnedVolume = ownedVolume();
		m_initialInjected = m_grid.injected;
		m_initialDrained = m_grid.drained;
	}

	void FixedSeaBoundary::advance(double seconds)
	{
		if (!std::isfinite(seconds) || seconds < 0)
		{
			throw std::invalid_argument("Timestep must be finite and nonnegative");
		}
		if (seconds == 0)
		{
			return;
		}
		for (const auto& source : m_grid.sources)
		{
			if (source.cell >= m_oceanMask.size() || m_oceanMask[source.cell] != 0)
			{
				throw std::invalid_argument("Water sources must be in owned land cells");
			}
		}
		// Stay at or below the core bound after floating-point division.
		const double safeStep = std::nextafter(m_grid.stableStep(), 0.0);
		const auto count = static_cast<std::size_t>(std::ceil(seconds / safeStep));
		const double dt = seconds / static_cast<double>(count);
		const double area = m_grid.cellSize * m_grid.cellSize;
		for (std::size_t step = 0; step < count; ++step)
		{
			m_grid.advance(dt);
			for (const auto i : m_oceanCells)
			{
				const double prescribedDepth = std::max(0.0, m_seaLevel - m_grid.bed[i]);
				const double volume = (m_grid.depth[i] - prescribedDepth) * area;
				m_oceanOutflow += std::max(0.0, volume);
				m_oceanInflow += std::max(0.0, -volume);
				m_grid.depth[i] = prescribedDepth;
			}
		}
	}

	double FixedSeaBoundary::ownedVolume() const
	{
		long double depthSum = 0;
		for (std::size_t i = 0; i < m_grid.depth.size(); ++i)
		{
			if (m_oceanMask[i] == 0)
			{
				depthSum += m_grid.depth[i];
			}
		}
		return static_cast<double>(depthSum * m_grid.cellSize * m_grid.cellSize);
	}

	double FixedSeaBoundary::balanceError() const
	{
		return ownedVolume() + m_oceanOutflow - m_oceanInflow + (m_grid.drained - m_initialDrained)
			- (m_grid.injected - m_initialInjected) - m_initialOwnedVolume;
	}
}

#include "GridWater.hpp"
#include <limits>
#include <numeric>

namespace GridWaterPrototype
{
	GridWater::GridWater(int gridWidth, int gridHeight, double gridCellSize, double routingDiffusivity)
		: width(gridWidth), height(gridHeight), cellSize(gridCellSize), diffusivity(routingDiffusivity)
	{
		if (width <= 0 || height <= 0 || !(cellSize > 0) || !(diffusivity > 0))
		{
			throw std::invalid_argument("Invalid grid dimensions or routing coefficient");
		}
		const auto count = static_cast<std::size_t>(width) * static_cast<std::size_t>(height);
		bed.resize(count);
		depth.resize(count);
		m_fluxX.resize(count);
		m_fluxZ.resize(count);
		m_donorScale.resize(count);
		m_delta.resize(count);
	}

	double GridWater::faceFlux(std::size_t a, std::size_t b) const
	{
		const double headDifference = (bed[a] + depth[a]) - (bed[b] + depth[b]);
		// Q [m^3/s] = D [m^2/s] * head [m]. Donor-depth clipping limits steep/dry fronts.
		const double flux = diffusivity * std::clamp(headDifference, -depth[b], depth[a]);
		if (!m_sinkOnlyMask.empty() && ((flux > 0.0 && m_sinkOnlyMask[a]) || (flux < 0.0 && m_sinkOnlyMask[b])))
		{
			return 0.0;
		}
		return flux;
	}

	void GridWater::setSinkOnlyMask(std::vector<std::uint8_t> mask)
	{
		if (!mask.empty() && mask.size() != depth.size())
		{
			throw std::invalid_argument("Drain mask size differs from the water grid");
		}
		m_sinkOnlyMask = std::move(mask);
	}

	void GridWater::advance(double seconds)
	{
		if (!std::isfinite(seconds) || seconds < 0)
		{
			throw std::invalid_argument("Timestep must be finite and nonnegative");
		}
		if (seconds == 0)
		{
			return;
		}
		const auto count = static_cast<std::size_t>(std::ceil(seconds / stableStep()));
		const double dt = seconds / static_cast<double>(count);
		for (std::size_t i = 0; i < count; ++i)
		{
			step(dt);
		}
	}

	void GridWater::step(double seconds)
	{
		const double area = cellSize * cellSize;
		for (const auto& source : sources)
		{
			if (source.cell >= depth.size() || !std::isfinite(source.rate) || source.rate < 0)
			{
				throw std::invalid_argument("Invalid water source");
			}
			const double volume = source.rate * seconds;
			depth[source.cell] += volume / area;
			injected += volume;
		}
		std::fill(m_donorScale.begin(), m_donorScale.end(), 0.0);
		std::fill(m_delta.begin(), m_delta.end(), 0.0);
		// Read only the old state. Each interior face is owned exactly once.
		for (int z = 0; z < height; ++z)
		{
			for (int x = 0; x < width; ++x)
			{
				const auto i = index(x, z);
				const double qx = x + 1 < width ? faceFlux(i, i + 1)
					: (rightOutflow ? diffusivity * std::clamp(bed[i] + depth[i] - exteriorHead, 0.0, depth[i]) : 0.0);
				const double qz = z + 1 < height ? faceFlux(i, i + static_cast<std::size_t>(width)) : 0.0;
				m_fluxX[i] = qx;
				m_fluxZ[i] = qz;
				m_donorScale[i] += std::max(0.0, qx) + std::max(0.0, qz);
				if (qx < 0)
				{
					m_donorScale[i + 1] -= qx;
				}
				if (qz < 0)
				{
					m_donorScale[i + static_cast<std::size_t>(width)] -= qz;
				}
			}
		}
		for (std::size_t i = 0; i < depth.size(); ++i)
		{
			const double requestedVolume = seconds * m_donorScale[i];
			m_donorScale[i] = requestedVolume > 0 ? std::min(1.0, depth[i] * area / requestedVolume) : 1.0;
			limitedCells += m_donorScale[i] < 1.0 ? 1 : 0;
		}
		// Debit and credit the identical transferred volume. No traversal-order water creation.
		for (int z = 0; z < height; ++z)
		{
			for (int x = 0; x < width; ++x)
			{
				const auto i = index(x, z);
				const auto right = x + 1 < width ? i + 1 : i;
				const auto down = z + 1 < height ? i + static_cast<std::size_t>(width) : i;
				const double vx = seconds * m_fluxX[i] * m_donorScale[m_fluxX[i] >= 0 ? i : right];
				const double vz = seconds * m_fluxZ[i] * m_donorScale[m_fluxZ[i] >= 0 ? i : down];
				m_delta[i] -= vx + vz;
				if (x + 1 < width)
				{
					m_delta[right] += vx;
				}
				else
				{
					drained += vx;
				}
				if (z + 1 < height)
				{
					m_delta[down] += vz;
				}
			}
		}
		lastMaxDepthChange = 0;
		for (std::size_t i = 0; i < depth.size(); ++i)
		{
			const double change = m_delta[i] / area;
			depth[i] += change;
			lastMaxDepthChange = std::max(lastMaxDepthChange, std::abs(change));
		}
		++substeps;
	}

	double GridWater::volume() const
	{
		const long double sum = std::accumulate(depth.begin(), depth.end(), static_cast<long double>(0));
		return static_cast<double>(sum * cellSize * cellSize);
	}

	std::size_t GridWater::allocatedBytes() const
	{
		return (bed.capacity() + depth.capacity() + m_fluxX.capacity() + m_fluxZ.capacity()
			+ m_donorScale.capacity() + m_delta.capacity()) * sizeof(double)
			+ sources.capacity() * sizeof(Source) + m_sinkOnlyMask.capacity() * sizeof(std::uint8_t);
	}
}

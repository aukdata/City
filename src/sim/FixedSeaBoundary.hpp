#pragma once
#include "GridWater.hpp"
#include <cstdint>

namespace GridWaterPrototype
{
	/// @brief Fixed-head ocean reservoir around the unchanged conservative routing core.
	/// @details Ocean cells are prescribed, not finite owned water. Every post-substep reset is
	/// accounted as an external inflow or outflow. A mask cell must be below the sea level.
	class FixedSeaBoundary
	{
	public:
		FixedSeaBoundary(GridWater& grid, std::vector<std::uint8_t> oceanMask, double seaLevel = 0.0, bool allowInflow = true);
		/// @brief Prescribe ocean depths and begin a new balance ledger after a scenario reset.
		void initialize();
		/// @brief Advance, restoring and accounting for sea level after EVERY internal substep.
		void advance(double seconds);
		/// @brief Persist the full external-exchange ledger along with owned depth.
		struct Ledger { double outflow=0, inflow=0, initialOwned=0, initialInjected=0, initialDrained=0; };
		Ledger ledger() const { return {m_oceanOutflow,m_oceanInflow,m_initialOwnedVolume,m_initialInjected,m_initialDrained}; }
		void restoreLedger(const Ledger& value)
		{
			m_oceanOutflow=value.outflow; m_oceanInflow=value.inflow; m_initialOwnedVolume=value.initialOwned;
			m_initialInjected=value.initialInjected; m_initialDrained=value.initialDrained;
		}
		double ownedVolume() const;
		double balanceError() const;
		double oceanOutflow() const { return m_oceanOutflow; }
		double oceanInflow() const { return m_oceanInflow; }
		bool isOcean(std::size_t cell) const { return m_oceanMask.at(cell) != 0; }
		std::size_t oceanCellCount() const { return m_oceanCells.size(); }

	private:
		GridWater& m_grid;
		std::vector<std::uint8_t> m_oceanMask;
		std::vector<std::size_t> m_oceanCells;
		double m_seaLevel;
		double m_oceanOutflow = 0;
		double m_oceanInflow = 0;
		double m_initialOwnedVolume = 0;
		double m_initialInjected = 0;
		double m_initialDrained = 0;
	};
}

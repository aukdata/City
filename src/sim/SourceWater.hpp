#pragma once
#include "GridWater.hpp"
#include "FixedSeaBoundary.hpp"
#include <functional>
#include <memory>
#include <istream>
#include <limits>
#include <vector>
#include <ostream>
#include <unordered_map>

/// @brief Lightweight whole-map source water, with a sink-only fixed sea level.
class SourceWater
{
public:
	struct Source { double x = 0.0, z = 0.0, rate = 64.0; }; ///< Point inlet, m and m³/s.
	struct Sample { double head = 0.0, depth = -0.05; bool ocean = false; }; ///< Signed bank clipping.
	using Height = std::function<double(double,double)>;
	using Marine = std::function<bool(double,double)>;
	/// @brief Construct one persistent coarse map; camera movement never closes its boundaries.
	SourceWater(double width, double height, const Height& terrain, const Marine& marine,
		const std::vector<Source>& sources, double spacing = 128.0);
	/// @brief A routed river segment. Water enters only through point sources and tributaries.
	struct Channel
	{
		double x0=0,z0=0,x1=0,z1=0,head0=0,head1=0,width0=0,width1=0,depth=1;
		int downstream=-1; ///< -1: closed land end, -2: sink-only sea outlet.
		double inlet=0; ///< Point-source supply in m³/s; never distributed rainfall.
	};
	/// @brief Start every riverbed dry; only point sources can create its subsequently routed water.
	void configureChannels(std::vector<Channel> channels);
	const std::vector<Channel>& channels() const { return m_channels; }
	double channelDepth(std::size_t index) const { return m_channelVolume.at(index)/m_channelArea.at(index); }
	double channelResidence(std::size_t index) const { return m_channelResidence.at(index); }
	double channelFlow(std::size_t index) const { return m_channelFlow.at(index); }
	/// @brief Supply control used to verify that a river drains when its sources stop.
	void setSupplyScale(double value);
	double injected() const { return m_grid->injected+m_channelInjected; }
	int snapshotVersion() const { return m_channels.empty() ? 1 : 2; }
	/// @brief Advance owned land water; prescribed ocean cells are excluded from its ledger.
	void advance(double seconds);
	/// @brief Reconstruct a shared head against the actual visible terrain height.
	Sample sample(double x, double z, double terrainHeight) const;
	/// @brief Channel geometry near a point; fixed for the routed topology, so renderers may cache it.
	struct ChannelCandidate { std::size_t index = 0; double t = 0, distance = 0, halfWidth = 0; };
	/// @brief Append the channels that sample() would consider at this point, in the same order.
	void channelCandidates(double x, double z, std::vector<ChannelCandidate>& out) const;
	/// @brief Same result as sample() with routed channels, using candidates from channelCandidates() at the same point.
	Sample sampleCandidates(double x, double z, double terrainHeight, const ChannelCandidate* first, const ChannelCandidate* last) const;
	/// @brief Test whether a region needs a dynamic inland mesh, independent of camera distance.
	bool hasLandWater(double x, double z, double width, double height) const;
	/// @brief Preserve owned water through terrain edits; routed river topology remains fixed.
	void setBed(int x, int z, double height);
	/// @brief Write an exact versioned snapshot; restore validates before changing the live grid.
	bool write(std::ostream& stream) const;
	bool read(std::istream& stream, bool apply=true);
	std::uint64_t surfaceRevision() const { return m_surfaceRevision; }
	std::uint64_t terrainRevision() const { return m_terrainRevision; }
	bool ocean(int x,int z) const;
	const GridWaterPrototype::GridWater& grid() const { return *m_grid; }
	double elapsed() const { return m_elapsed; }
	double ownedVolume() const { return m_sea->ownedVolume()+channelVolume(); }
	double outflow() const { return m_sea->oceanOutflow()+m_channelOutflow; }
	double inflow() const { return m_sea->oceanInflow(); }
	double balanceError() const { return m_sea->balanceError()+channelVolume()+m_channelOutflow-m_channelInjected-m_channelInitial; }
	std::size_t sourceCount() const { return m_grid->sources.size(); }
	std::size_t rejectedSources() const { return m_rejectedSources; }
	std::size_t oceanCells() const { return m_sea->oceanCellCount(); }
	std::uint64_t revision() const { return m_revision; }
	/// @brief True once a step left every channel volume and flow unchanged (steady routed flow).
	bool channelsSettled() const { return m_channelsSettled; }

private:
	/// @brief Local spatial buckets keep water sampling independent of total river length.
	std::vector<Channel> m_channels;
	std::vector<double> m_channelVolume,m_channelArea,m_channelResidence,m_channelFlow,m_mapDepth;
	std::vector<std::size_t> m_channelOrder;
	std::unordered_map<std::int64_t,std::vector<std::size_t>> m_channelIndex;
	double m_channelInitial=0,m_channelInjected=0,m_channelOutflow=0,m_supplyScale=1;
	double channelVolume() const;
	/// @brief Nearest wet channel found so far while sampling one point.
	struct ChannelHead
	{
		explicit ChannelHead(double terrainHeight) : head(terrainHeight-.05) {}
		double head, nearest=std::numeric_limits<double>::max(), bankClip=-.05;
	};
	ChannelCandidate channelGeometry(std::size_t index, double x, double z) const;
	void applyChannel(const ChannelCandidate& candidate, ChannelHead& state) const;
	/// @brief Linear-reservoir factors for the last timestep; the simulation always uses the same step.
	std::vector<double> m_channelDecay,m_channelRelease;
	double m_decaySeconds=-1;
	/// @brief Set when a full step changed no volume or flow; the outlet terms repeat exactly.
	bool m_channelsSettled=false;
	double m_settledScale=1;
	std::vector<double> m_settledOutflow;
	std::unique_ptr<GridWaterPrototype::GridWater> m_grid;
	std::unique_ptr<GridWaterPrototype::FixedSeaBoundary> m_sea;
	double m_width = 0.0, m_height = 0.0, m_elapsed = 0.0;
	std::uint64_t m_revision = 1, m_terrainRevision = 1,m_surfaceRevision=1;
	std::size_t m_rejectedSources = 0;
};

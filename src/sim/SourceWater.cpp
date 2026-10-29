#include "SourceWater.hpp"
#include <limits>

namespace
{
	constexpr double kWetBankMargin = 8.0;
	constexpr double kDryBankSupport = 32.0;
}

SourceWater::SourceWater(double width, double height, const Height& terrain, const Marine& marine,
	const std::vector<Source>& sources, double spacing)
	: m_width(width), m_height(height)
{
	if (!std::isfinite(width) || !std::isfinite(height) || !std::isfinite(spacing)
		|| width <= 0.0 || height <= 0.0 || spacing <= 0.0 || !terrain || !marine)
	{
		throw std::invalid_argument("Invalid source-water domain");
	}
	const int columns = static_cast<int>(std::ceil(width/spacing)), rows = static_cast<int>(std::ceil(height/spacing));
	m_grid = std::make_unique<GridWaterPrototype::GridWater>(columns,rows,spacing,512.0);
	std::vector<std::uint8_t> mask(m_grid->depth.size());
	for (int z = 0; z < rows; ++z)
	{
		for (int x = 0; x < columns; ++x)
		{
			const double wx = std::min((x+0.5)*spacing,width-0.001), wz = std::min((z+0.5)*spacing,height-0.001);
			const auto i = m_grid->index(x,z); m_grid->bed[i] = terrain(wx,wz);
			if (!std::isfinite(m_grid->bed[i])) { throw std::invalid_argument("Nonfinite source-water terrain"); }
			mask[i] = static_cast<std::uint8_t>(m_grid->bed[i] < 0.0 && marine(wx,wz));
		}
	}
	m_sea = std::make_unique<GridWaterPrototype::FixedSeaBoundary>(*m_grid,std::move(mask),0.0,false);
	for (const auto& source : sources)
	{
		if (!std::isfinite(source.x) || !std::isfinite(source.z) || !std::isfinite(source.rate) || source.rate < 0.0)
		{
			throw std::invalid_argument("Invalid source-water inlet");
		}
		const int x = static_cast<int>(std::floor(source.x/spacing)), z = static_cast<int>(std::floor(source.z/spacing));
		if (x < 0 || z < 0 || x >= columns || z >= rows || ocean(x,z)) { ++m_rejectedSources; continue; }
		m_grid->sources.push_back({m_grid->index(x,z),source.rate});
	}
}

void SourceWater::advance(double seconds)
{
	if (!std::isfinite(seconds) || seconds<0) { throw std::invalid_argument("Invalid river timestep"); }
	if (m_channels.empty()) { m_sea->advance(seconds); }
	else if (seconds>0)
	{
		std::vector<double> incoming(m_channels.size(),0);
		for (const auto index:m_channelOrder)
		{
			const auto& channel=m_channels[index];
			const double supplied=channel.inlet*m_supplyScale*seconds;
			m_channelInjected+=supplied; incoming[index]+=supplied;
			if (channel.downstream==-1) { m_channelVolume[index]+=incoming[index];m_channelFlow[index]=0;continue; }
			// Exact linear-reservoir routing is positive and conservative even for very short reaches.
			const double fraction=seconds/m_channelResidence[index];
			const double retained=m_channelVolume[index]*std::exp(-fraction)+incoming[index]*(-std::expm1(-fraction))/fraction;
			const double transferred=std::max(0.0,m_channelVolume[index]+incoming[index]-retained);
			m_channelVolume[index]+=incoming[index]-transferred; m_channelFlow[index]=transferred/seconds;
			if (channel.downstream==-2) { m_channelOutflow+=transferred; }
			else { incoming[static_cast<std::size_t>(channel.downstream)]+=transferred; }
		}
	}
	if (seconds > 0.0)
	{
		m_elapsed+=seconds;++m_revision;
		bool changed=m_channels.empty() && static_cast<int>(m_elapsed/60)!=static_cast<int>((m_elapsed-seconds)/60);
		for (std::size_t i=0;i<m_channels.size() && !changed;++i) { changed=std::abs(channelDepth(i)-m_mapDepth[i])>.02; }
		if (changed) { ++m_surfaceRevision;for (std::size_t i=0;i<m_channels.size();++i) { m_mapDepth[i]=channelDepth(i); } }
	}
}

namespace
{
	/// @brief Stable signed bucket key, including points just outside the map.
	std::int64_t channelKey(int x,int z) { return static_cast<std::int64_t>(x)*0x100000000LL+static_cast<std::uint32_t>(z); }
}

double SourceWater::channelVolume() const
{
	long double volume=0;for (const double value:m_channelVolume) { volume+=value; }return static_cast<double>(volume);
}

void SourceWater::setSupplyScale(double value)
{
	if (!std::isfinite(value) || value<0) { throw std::invalid_argument("Invalid river supply"); }m_supplyScale=value;
}

void SourceWater::configureChannels(std::vector<Channel> channels)
{
	if (m_elapsed!=0 || !m_channels.empty()) { throw std::logic_error("River configuration requires a new water domain"); }
	const auto count=channels.size(); std::vector<int> incoming(count,0);
	for (const auto& c:channels)
	{
		for (const double v:{c.x0,c.z0,c.x1,c.z1,c.head0,c.head1,c.width0,c.width1,c.depth,c.inlet})
		{ if (!std::isfinite(v)) { throw std::invalid_argument("Nonfinite river geometry"); } }
		if (c.width0<=0 || c.width1<=0 || c.depth<=0 || c.inlet<0 || c.downstream<-2 || c.downstream>=static_cast<int>(count)
			|| std::hypot(c.x1-c.x0,c.z1-c.z0)<0.00001) { throw std::invalid_argument("Invalid river geometry"); }
		if (c.downstream>=0) { ++incoming[static_cast<std::size_t>(c.downstream)]; }
	}
	std::vector<std::size_t> order;
	for (std::size_t i=0;i<count;++i) { if (incoming[i]==0) { order.push_back(i); } }
	for (std::size_t i=0;i<order.size();++i)
	{
		const int next=channels[order[i]].downstream;
		if (next>=0 && --incoming[static_cast<std::size_t>(next)]==0) { order.push_back(static_cast<std::size_t>(next)); }
	}
	if (order.size()!=count) { throw std::invalid_argument("Cyclic river network"); }
	m_channels=std::move(channels);m_channelOrder=std::move(order);
	m_channelVolume.resize(count);m_channelArea.resize(count);m_channelResidence.resize(count);m_channelFlow.assign(count,0);
	std::vector<double> discharge(count,0);
	for (const auto i:m_channelOrder)
	{
		const auto& c=m_channels[i];discharge[i]+=c.inlet;
		if (c.downstream>=0) { discharge[static_cast<std::size_t>(c.downstream)]+=discharge[i]; }
		m_channelArea[i]=std::hypot(c.x1-c.x0,c.z1-c.z0)*(c.width0+c.width1)*.5;
		m_channelVolume[i]=0;
		m_channelResidence[i]=discharge[i]>0 ? m_channelArea[i]*c.depth/discharge[i] : 60;
		m_channelFlow[i]=0;
		const double margin=std::max(c.width0,c.width1)*.5+kDryBankSupport;
		for (int z=static_cast<int>(std::floor((std::min(c.z0,c.z1)-margin)/512));z<=static_cast<int>(std::floor((std::max(c.z0,c.z1)+margin)/512));++z)
		{ for (int x=static_cast<int>(std::floor((std::min(c.x0,c.x1)-margin)/512));x<=static_cast<int>(std::floor((std::max(c.x0,c.x1)+margin)/512));++x)
			{ m_channelIndex[channelKey(x,z)].push_back(i); } }
	}
	m_channelInitial=channelVolume();m_mapDepth.resize(count);for (std::size_t i=0;i<count;++i) { m_mapDepth[i]=channelDepth(i); }++m_revision;++m_surfaceRevision;
}

bool SourceWater::ocean(int x,int z) const
{
	return x >= 0 && z >= 0 && x < m_grid->width && z < m_grid->height && m_sea->isOcean(m_grid->index(x,z));
}

SourceWater::Sample SourceWater::sample(double x,double z,double terrainHeight) const
{
	if (x < 0.0 || z < 0.0 || x > m_width || z > m_height) { return {terrainHeight-0.05,-0.05,false}; }
	const double gx = std::min(x/m_grid->cellSize,m_grid->width-0.00001), gz = std::min(z/m_grid->cellSize,m_grid->height-0.00001);
	if (ocean(static_cast<int>(gx),static_cast<int>(gz))) { return {0.0,-terrainHeight,true}; }
	if (!m_channels.empty())
	{
		const auto found=m_channelIndex.find(channelKey(static_cast<int>(x/512),static_cast<int>(z/512)));
		double nearest=std::numeric_limits<double>::max(),head=terrainHeight-.05, bankClip=-.05;
		if (found!=m_channelIndex.end()) { for (const auto i:found->second)
		{
			const auto& c=m_channels[i];const double dx=c.x1-c.x0,dz=c.z1-c.z0;
			const double t=std::clamp(((x-c.x0)*dx+(z-c.z0)*dz)/(dx*dx+dz*dz),0.0,1.0);
			const double distance=std::hypot(x-c.x0-dx*t,z-c.z0-dz*t),halfWidth=(c.width0+(c.width1-c.width0)*t)*.5;
			if (distance>halfWidth+kDryBankSupport || channelDepth(i)<=.003 || distance-halfWidth>=nearest) { continue; }
			nearest=distance-halfWidth;bankClip=halfWidth+kWetBankMargin-distance;
			const double startOffset=channelDepth(i)-c.depth;
			const double endOffset=c.downstream>=0 ? channelDepth(static_cast<std::size_t>(c.downstream))-m_channels[static_cast<std::size_t>(c.downstream)].depth
				: c.downstream==-2 ? 0 : startOffset;
			head=c.head0+(c.head1-c.head0)*t+startOffset*(1-t)+endOffset*t;
		} }
		// Extrapolate the hydraulic head through the dry bank, rather than lowering dry vertices
		// to ground minus 5 cm. Signed terrain/width clipping keeps the outside bank dry.
		return {head,std::min(head-terrainHeight,bankClip),false};
	}
	const int firstX = static_cast<int>(std::floor(gx-0.5)), firstZ = static_cast<int>(std::floor(gz-0.5));
	double sum = 0.0, weight = 0.0;
	for (int dz = 0; dz < 2; ++dz)
	{
		for (int dx = 0; dx < 2; ++dx)
		{
			const int cx = firstX+dx, cz = firstZ+dz;
			if (cx < 0 || cz < 0 || cx >= m_grid->width || cz >= m_grid->height || ocean(cx,cz)) { continue; }
			const auto i = m_grid->index(cx,cz);
			if (m_grid->depth[i] <= 0.003) { continue; }
			const double w = std::max(0.0,1.0-std::abs(gx-cx-0.5))*std::max(0.0,1.0-std::abs(gz-cz-0.5));
			sum += (m_grid->bed[i]+m_grid->depth[i])*w; weight += w;
		}
	}
	const double head = weight > 0.0 ? sum/weight : terrainHeight-0.05;
	return {head,head-terrainHeight,false};
}

bool SourceWater::hasLandWater(double x,double z,double width,double height) const
{
	if (!m_channels.empty())
	{
		for (int cz=static_cast<int>(std::floor(z/512));cz<=static_cast<int>(std::floor((z+height)/512));++cz)
		{ for (int cx=static_cast<int>(std::floor(x/512));cx<=static_cast<int>(std::floor((x+width)/512));++cx)
			{ if (m_channelIndex.find(channelKey(cx,cz))!=m_channelIndex.end()) { return true; } } }
		return false;
	}
	const int x0 = std::max(0,static_cast<int>(std::floor(x/m_grid->cellSize))-1);
	const int z0 = std::max(0,static_cast<int>(std::floor(z/m_grid->cellSize))-1);
	const int x1 = std::min(m_grid->width-1,static_cast<int>(std::floor((x+width)/m_grid->cellSize))+1);
	const int z1 = std::min(m_grid->height-1,static_cast<int>(std::floor((z+height)/m_grid->cellSize))+1);
	for (int cz = z0; cz <= z1; ++cz)
	{
		for (int cx = x0; cx <= x1; ++cx)
		{
			if (!ocean(cx,cz) && m_grid->depth[m_grid->index(cx,cz)] > 0.003) { return true; }
		}
	}
	return false;
}

void SourceWater::setBed(int x,int z,double height)
{
	if (x < 0 || z < 0 || x >= m_grid->width || z >= m_grid->height || !std::isfinite(height)) { return; }
	const auto i = m_grid->index(x,z); m_grid->bed[i] = height;
	if (ocean(x,z)) { m_grid->depth[i] = std::max(0.0,-height); }
	++m_revision; ++m_terrainRevision;++m_surfaceRevision;
}


namespace
{
	/// @brief Explicit field IO avoids struct padding and preserves double precision.
	template<class T> bool emit(std::ostream& stream,const T& value)
	{
		stream.write(reinterpret_cast<const char*>(&value),sizeof(value)); return static_cast<bool>(stream);
	}
	template<class T> bool take(std::istream& stream,T& value)
	{
		stream.read(reinterpret_cast<char*>(&value),sizeof(value)); return static_cast<bool>(stream);
	}
}

bool SourceWater::write(std::ostream& stream) const
{
	const std::uint32_t magic=0x31575453,version=static_cast<std::uint32_t>(snapshotVersion());
	const auto ledger=m_sea->ledger();
	if (!(emit(stream,magic) && emit(stream,version) && emit(stream,m_grid->width) && emit(stream,m_grid->height)
		&& emit(stream,m_grid->cellSize) && emit(stream,m_elapsed) && emit(stream,m_grid->injected)
		&& emit(stream,m_grid->drained) && emit(stream,ledger.outflow) && emit(stream,ledger.inflow)
		&& emit(stream,ledger.initialOwned) && emit(stream,ledger.initialInjected) && emit(stream,ledger.initialDrained))) { return false; }
	for (std::size_t i=0;i<m_grid->depth.size();++i)
	{
		if (!(emit(stream,m_grid->bed[i]) && emit(stream,m_grid->depth[i]))) { return false; }
	}
	if (!m_channels.empty())
	{
		const auto count=static_cast<std::uint32_t>(m_channels.size());
		if (!(emit(stream,count) && emit(stream,m_channelInitial) && emit(stream,m_channelInjected) && emit(stream,m_channelOutflow) && emit(stream,m_supplyScale))) { return false; }
		for (std::size_t i=0;i<m_channels.size();++i) { if (!(emit(stream,m_channelVolume[i]) && emit(stream,m_channelFlow[i]))) { return false; } }
	}
	return static_cast<bool>(stream);
}

bool SourceWater::read(std::istream& stream,bool apply)
{
	std::uint32_t magic=0,version=0;int width=0,height=0;
	double spacing=0,elapsed=0,injected=0,drained=0;
	GridWaterPrototype::FixedSeaBoundary::Ledger ledger;
	if (!(take(stream,magic) && take(stream,version) && magic==0x31575453 && version==static_cast<std::uint32_t>(snapshotVersion())
		&& take(stream,width) && take(stream,height) && take(stream,spacing) && width==m_grid->width && height==m_grid->height
		&& spacing==m_grid->cellSize && take(stream,elapsed) && take(stream,injected) && take(stream,drained)
		&& take(stream,ledger.outflow) && take(stream,ledger.inflow) && take(stream,ledger.initialOwned)
		&& take(stream,ledger.initialInjected) && take(stream,ledger.initialDrained))) { return false; }
	for (const double value:{elapsed,injected,drained,ledger.outflow,ledger.inflow,ledger.initialOwned,ledger.initialInjected,ledger.initialDrained})
	{
		if (!std::isfinite(value) || value<0.0) { return false; }
	}
	if (ledger.inflow!=0.0) { return false; }
	std::vector<double> bed(m_grid->bed.size()),depth(m_grid->depth.size());
	long double owned=0;
	for (std::size_t i=0;i<depth.size();++i)
	{
		if (!take(stream,bed[i]) || !take(stream,depth[i]) || !std::isfinite(bed[i]) || !std::isfinite(depth[i]) || depth[i]<0.0) { return false; }
		if (m_sea->isOcean(i))
		{
			if (depth[i]!=std::max(0.0,-bed[i])) { return false; }
		}
		else { owned+=depth[i]*spacing*spacing; }
	}
	double channelInitial=0,channelInjected=0,channelOutflow=0,supplyScale=1;
	std::vector<double> channelVolumes(m_channels.size()),channelFlows(m_channels.size());
	if (!m_channels.empty())
	{
		std::uint32_t count=0;
		if (!(take(stream,count) && count==m_channels.size() && take(stream,channelInitial) && take(stream,channelInjected)
			&& take(stream,channelOutflow) && take(stream,supplyScale))) { return false; }
		for (const double v:{channelInitial,channelInjected,channelOutflow,supplyScale}) { if (!std::isfinite(v) || v<0) { return false; } }
		long double channelOwned=0;
		for (std::size_t i=0;i<count;++i)
		{
			if (!(take(stream,channelVolumes[i]) && take(stream,channelFlows[i])) || !std::isfinite(channelVolumes[i]) || channelVolumes[i]<0
				|| !std::isfinite(channelFlows[i]) || channelFlows[i]<0) { return false; }channelOwned+=channelVolumes[i];
		}
		if (std::abs(static_cast<double>(channelOwned)+channelOutflow-channelInjected-channelInitial)>std::max(.001,channelInjected*1e-10)) { return false; }
		if (!apply && (channelVolumes!=m_channelVolume || channelFlows!=m_channelFlow || channelInitial!=m_channelInitial
			|| channelInjected!=m_channelInjected || channelOutflow!=m_channelOutflow || supplyScale!=m_supplyScale)) { return false; }
	}
	if (stream.peek()!= std::char_traits<char>::eof()) { return false; }
	const double balance=static_cast<double>(owned)+ledger.outflow-ledger.inflow+(drained-ledger.initialDrained)-(injected-ledger.initialInjected)-ledger.initialOwned;
	if (std::abs(balance)>std::max(0.001,injected*1e-10)) { return false; }
	if (!apply)
	{
		const auto current=m_sea->ledger();
		return bed==m_grid->bed && depth==m_grid->depth && elapsed==m_elapsed && injected==m_grid->injected && drained==m_grid->drained
			&& ledger.outflow==current.outflow && ledger.inflow==current.inflow && ledger.initialOwned==current.initialOwned
			&& ledger.initialInjected==current.initialInjected && ledger.initialDrained==current.initialDrained;
	}
	m_grid->bed=std::move(bed);m_grid->depth=std::move(depth);m_grid->injected=injected;m_grid->drained=drained;
	m_channelVolume=std::move(channelVolumes);m_channelFlow=std::move(channelFlows);
	m_channelInitial=channelInitial;m_channelInjected=channelInjected;m_channelOutflow=channelOutflow;m_supplyScale=supplyScale;
	m_elapsed=elapsed; m_sea->restoreLedger(ledger); ++m_revision; ++m_terrainRevision;++m_surfaceRevision;
	return true;
}

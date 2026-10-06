#include "WorldRenderer.hpp"
#include "SourceWaterGeometry.hpp"
#include "../debug/DebugLog.hpp"

namespace SourceWaterGeometry
{
	/// @brief Shared world lattice clips the water against the actual uncarved terrain.
	MeshData build(const World& world, double originX, double originZ, int cells, double step, int mode)
	{
		const auto& water = *world.sourceWater();
		if (mode==2 && step>16 && !water.channels().empty())
		{
			MeshData strips;
			for (std::size_t index=0;index<water.channels().size();++index)
			{
				if (water.channelDepth(index)<=.003) { continue; }
				const auto& c=water.channels()[index];
				const double speed=water.channelFlow(index)/Max(.01,(c.width0+c.width1)*.5*water.channelDepth(index));
				const Vec2 delta{c.x1-c.x0,c.z1-c.z0};const Vec2 right{delta.y,-delta.x};const auto normal=right.normalized();
				const uint32 first=static_cast<uint32>(strips.vertices.size());
				for (const bool end:{false,true}) { for (const int side:{-1,1})
				{
					const Vec2 center=end ? Vec2{c.x1,c.z1} : Vec2{c.x0,c.z0};const auto p=center+normal*(end ? c.width1 : c.width0)*.5*side;
					const auto sample=water.sample(p.x,p.y,world.waterBedHeight(p.x,p.y));
					strips.vertices << Vertex3D{Float3{static_cast<float>(p.x),static_cast<float>(sample.head+.01),static_cast<float>(p.y)},Float3{0,1+static_cast<float>(Clamp(speed,0.0,4.0)),0},Float2{static_cast<float>(sample.depth),8+static_cast<float>(std::atan2(delta.y,delta.x))}};
				} }
				strips.indices << TriangleIndex32{first,first+2,first+1} << TriangleIndex32{first+1,first+2,first+3};
			}
			return strips;
		}
		const int corners = cells+1;
		MeshData data;
		data.vertices.resize(static_cast<size_t>(corners)*corners);
		for (int z=0; z<=cells; ++z)
		{
			for (int x=0; x<=cells; ++x)
			{
				const double wx=originX+x*step, wz=originZ+z*step;
				const double bed=world.waterBedHeight(wx,wz);
				const auto sample=mode==1 ? SourceWater::Sample{0.0,-bed,true} : water.sample(wx,wz,bed);
				auto& vertex=data.vertices[static_cast<size_t>(z)*corners+x];
				vertex.pos={static_cast<float>(wx),static_cast<float>(sample.head+0.01),static_cast<float>(wz)};
				vertex.normal={0,1,0}; vertex.tex={static_cast<float>(sample.depth),0};
				if (mode==2 && !water.channels().empty())
				{
					const auto river=world.rivers().nearest({wx,wz});
					if (river.reach>=0) { const auto& reach=world.rivers().reaches[river.reach];vertex.tex.y=8+static_cast<float>(std::atan2(reach.end.z-reach.start.z,reach.end.x-reach.start.x));
						const auto index=static_cast<std::size_t>(river.reach);const auto& channel=water.channels()[index];
						const double speed=water.channelFlow(index)/Max(.01,(channel.width0+channel.width1)*.5*water.channelDepth(index));
						vertex.normal.y=1+static_cast<float>(Clamp(speed,0.0,4.0)); }
				}
			}
		}
		for (int z=0; z<cells; ++z)
		{
			for (int x=0; x<cells; ++x)
			{
				const double wx=originX+(x+0.5)*step, wz=originZ+(z+0.5)*step;
				const bool sea=water.ocean(static_cast<int>(wx/water.grid().cellSize),static_cast<int>(wz/water.grid().cellSize));
				if ((mode==1 && !sea) || (mode==2 && sea)) { continue; }
				const uint32 a=static_cast<uint32>(z*corners+x), b=a+1, c=a+corners, d=c+1;
				if (mode==2 && step!=16.0 && !sea && Max({data.vertices[a].tex.x,data.vertices[b].tex.x,data.vertices[c].tex.x,data.vertices[d].tex.x})<=0.002f) { continue; }
				data.indices << TriangleIndex32{a,c,b} << TriangleIndex32{b,c,d};
			}
		}
		// Finite differences supply gentle flow-aligned highlights without another simulation field.
		for (int z=0; z<=cells; ++z)
		{
			for (int x=0; x<=cells; ++x)
			{
				auto& vertex=data.vertices[static_cast<size_t>(z)*corners+x];
				const auto& left=data.vertices[static_cast<size_t>(z)*corners+Max(0,x-1)];
				const auto& right=data.vertices[static_cast<size_t>(z)*corners+Min(cells,x+1)];
				const auto& back=data.vertices[static_cast<size_t>(Max(0,z-1))*corners+x];
				const auto& front=data.vertices[static_cast<size_t>(Min(cells,z+1))*corners+x];
				if (vertex.tex.y<=4 && vertex.tex.x>0.002f && left.tex.x>0.002f && right.tex.x>0.002f && back.tex.x>0.002f && front.tex.x>0.002f)
				{
					vertex.normal=Float3{-(right.pos.y-left.pos.y)/static_cast<float>(step*2),1,-(front.pos.y-back.pos.y)/static_cast<float>(step*2)}.normalized();
				}
			}
		}
		return data;
	}
}

void WorldRenderer::renderSourceWater(const World& world, bool regional)
{
	const auto* water=world.sourceWater();
	if (!water) { return; }
	if (m_sourceWaterIdentity!=water)
	{
		m_sourceWaterMeshes.clear(); m_sourceSeaRegional=Mesh{};m_sourceLandRegional=SourceWaterMesh{};
		m_sourceWaterIdentity=water; m_sourceWaterCursor=0;
	}
	if (!m_sourceWaterShader)
	{
		const FilePath path=FileSystem::Exists(U"shaders/hlsl/source_water.hlsl") ? U"shaders/hlsl/source_water.hlsl" : U"../../App/shaders/hlsl/source_water.hlsl";
		m_sourceWaterShader=ShaderAsset::pixel(path,U"WaterPS");
		DBG_LOG(U"[SourceWaterRender] shader={} path={}"_fmt(static_cast<bool>(m_sourceWaterShader),path));
		if (!m_sourceWaterShader) { return; }
	}
	m_sourceWaterVisual->parameters={static_cast<float>(water->elapsed()/60.0),0,0,0};
	Graphics3D::SetPSConstantBuffer(5,m_sourceWaterVisual);
	const ScopedCustomShader3D shader{m_sourceWaterShader};
	const ScopedRenderStates3D states{BlendState::NonPremultiplied,RasterizerState::SolidCullNone,DepthStencilState::DepthTest};
	if (regional)
	{
		if (!m_sourceSeaRegional || m_sourceLandRegional.terrainRevision!=water->terrainRevision())
		{
			const Stopwatch timer{StartImmediately::Yes};
			const auto data=SourceWaterGeometry::build(world,0,0,water->grid().width,water->grid().cellSize,1);
			if (!data.indices.isEmpty()) { m_sourceSeaRegional=Mesh{data}; }
			DBG_LOG(U"[SourceWaterRender] regionalSea vertices={} triangles={} buildMs={:.2f}"_fmt(data.vertices.size(),data.indices.size(),timer.msF()));
		}
		if (m_sourceSeaRegional) { m_sourceSeaRegional.draw(ColorF{1}); }
		if (m_sourceLandRegional.revision!=water->revision() && Scene::Time()-m_sourceLandRegional.updated>=5.0)
		{
			const auto data=SourceWaterGeometry::build(world,0,0,water->grid().width,water->grid().cellSize,2);
			m_sourceLandRegional.mesh=data.indices.isEmpty() ? DynamicMesh{} : DynamicMesh{data};
			m_sourceLandRegional.revision=water->revision(); m_sourceLandRegional.updated=Scene::Time();m_sourceLandRegional.terrainRevision=water->terrainRevision();
		}
		if (m_sourceLandRegional.mesh) { m_sourceLandRegional.mesh.draw(ColorF{1}); }
		return;
	}
	const auto& chunks=world.getActiveChunks();
	HashSet<Key> active;
	for (const auto* chunk:chunks) { if (chunk) { active.insert(chunkCoordToKey(chunk->coord)); } }
	for (auto it=m_sourceWaterMeshes.begin();it!=m_sourceWaterMeshes.end();)
	{
		if (!active.contains(it->first)) { it=m_sourceWaterMeshes.erase(it); } else { ++it; }
	}
	int uploaded=0;
	for (size_t visit=0; visit<chunks.size() && uploaded<2; ++visit)
	{
		const auto* chunk=chunks[(m_sourceWaterCursor++)%chunks.size()];
		if (!chunk) { continue; }
		const Key key=chunkCoordToKey(chunk->coord);
		const auto origin=chunk->worldOrigin();
		const bool dynamic=water->hasLandWater(origin.x,origin.z,CHUNK_SIZE,CHUNK_SIZE);
		auto& entry=m_sourceWaterMeshes[key];
		if (entry.revision!=0 && (!dynamic && !entry.dynamic && entry.terrainRevision==water->terrainRevision())) { continue; }
		if ((entry.revision==water->revision() && entry.terrainRevision==water->terrainRevision()) || Scene::Time()-entry.updated<0.1) { continue; }
		if (entry.revision==0 || entry.terrainRevision!=water->terrainRevision())
		{
			const auto sea=SourceWaterGeometry::build(world,origin.x,origin.z,16,64.0,1);
			entry.sea=sea.indices.isEmpty() ? Mesh{} : Mesh{sea};
		}
		const auto data=dynamic ? SourceWaterGeometry::build(world,origin.x,origin.z,HEIGHT_CELLS,16.0,2) : MeshData{};
		if (!dynamic) { entry.mesh=DynamicMesh{}; }
		else if (entry.mesh) { entry.mesh.fill(data.vertices); }
		else if (!data.indices.isEmpty()) { entry.mesh=DynamicMesh{data}; }
		entry.terrainRevision=water->terrainRevision();entry.revision=water->revision();entry.updated=Scene::Time();entry.dynamic=dynamic;
		++uploaded;
	}
	for (const auto& [key,entry]:m_sourceWaterMeshes) { if (entry.sea) { entry.sea.draw(ColorF{1}); } if (entry.mesh) { entry.mesh.draw(ColorF{1}); } }
}

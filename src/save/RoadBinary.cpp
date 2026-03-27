#include "RoadBinary.hpp"

bool RoadBinary::write(const FilePath& path, int32 cx, int32 cy,
                       const Array<RoadNode>& nodes,
                       const Array<RoadEdge>& edges)
{
	// tombstone を除外した有効エントリのみ書き出す
	Array<RoadNode> vn;
	Array<RoadEdge> ve;
	for (const auto& n : nodes) if (n.id >= 0) vn << n;
	for (const auto& e : edges) if (e.id >= 0) ve << e;

	BinaryWriter w{ path };
	if (!w) return false;

	// ---- ヘッダー (30 bytes) ----
	w.write(kMagic);
	w.write(kVersion);
	w.write(cx);
	w.write(cy);
	w.write(static_cast<uint32>(vn.size()));
	w.write(static_cast<uint32>(ve.size()));
	w.write(uint32{ 0 });   // planCount  (未実装)
	w.write(uint32{ 0 });   // lightCount (未実装)

	// ---- RoadNode レコード ----
	for (const auto& n : vn)
	{
		w.write(n.id);
		w.write(static_cast<float>(n.position.x));
		w.write(static_cast<float>(n.position.y));
		w.write(static_cast<float>(n.position.z));
		w.write(static_cast<uint8>(n.type));
		w.write(static_cast<uint32>(n.edgeIds.size()));
		for (int eid : n.edgeIds) w.write(eid);
	}

	// ---- RoadEdge レコード ----
	for (const auto& e : ve)
	{
		w.write(e.id);
		w.write(e.nodeA);
		w.write(e.nodeB);
		w.write(static_cast<float>(e.ctrlA.x));
		w.write(static_cast<float>(e.ctrlA.y));
		w.write(static_cast<float>(e.ctrlA.z));
		w.write(static_cast<float>(e.ctrlB.x));
		w.write(static_cast<float>(e.ctrlB.y));
		w.write(static_cast<float>(e.ctrlB.z));
		w.write(static_cast<uint8>(e.roadType));
		w.write(e.speedLimit);
		w.write(e.length);
		w.write(e.planId);
		w.write(e.cutoffA);
		w.write(e.cutoffB);
		w.write(static_cast<uint8>(e.edgeState));
		w.write(e.borderNodeA);
		w.write(e.borderNodeB);
		w.write(static_cast<uint32>(e.lanes.size()));
		for (const auto& lane : e.lanes)
		{
			w.write(lane.index);
			w.write(static_cast<uint8>(lane.build));
			w.write(static_cast<uint8>(lane.type));
			w.write(lane.width);
			w.write(static_cast<uint8>(lane.dir));
			w.write(static_cast<uint8>(lane.op));
		}
	}

	return true;
}

bool RoadBinary::read(const FilePath& path,
                      Array<RoadNode>& outNodes,
                      Array<RoadEdge>& outEdges)
{
	BinaryReader r{ path };
	if (!r) return false;

	// ---- ヘッダー ----
	uint32 magic;
	if (!r.read(magic) || magic != kMagic) return false;
	uint16 version;
	if (!r.read(version) || version > kVersion) return false;

	int32  cx, cy;
	r.read(cx); r.read(cy);
	uint32 nodeCount, edgeCount, planCount, lightCount;
	r.read(nodeCount); r.read(edgeCount);
	r.read(planCount); r.read(lightCount);

	// ---- RoadNode レコード ----
	for (uint32 i = 0; i < nodeCount; ++i)
	{
		RoadNode n;
		float px, py, pz;
		uint8 type;
		uint32 edgeCnt;

		if (!r.read(n.id)) return false;
		r.read(px); r.read(py); r.read(pz);
		n.position = Vec3{ px, py, pz };
		r.read(type); n.type = static_cast<NodeType>(type);
		r.read(edgeCnt);
		// edgeIds は読み飛ばす（addEdgeRaw で再構築するためクリア）
		for (uint32 j = 0; j < edgeCnt; ++j) { int dummy; r.read(dummy); }
		n.edgeIds.clear();

		outNodes << n;
	}

	// ---- RoadEdge レコード ----
	for (uint32 i = 0; i < edgeCount; ++i)
	{
		RoadEdge e;
		float x, y, z;
		uint8 rt, es;
		uint32 laneCnt;

		if (!r.read(e.id)) return false;
		r.read(e.nodeA); r.read(e.nodeB);
		r.read(x); r.read(y); r.read(z); e.ctrlA = Vec3{ x, y, z };
		r.read(x); r.read(y); r.read(z); e.ctrlB = Vec3{ x, y, z };
		r.read(rt); e.roadType = static_cast<RoadType>(rt);
		r.read(e.speedLimit);
		r.read(e.length);
		r.read(e.planId);
		r.read(e.cutoffA);
		r.read(e.cutoffB);
		r.read(es); e.edgeState = static_cast<EdgeState>(es);
		r.read(e.borderNodeA);
		r.read(e.borderNodeB);
		r.read(laneCnt);
		e.lanes.resize(laneCnt);
		for (uint32 j = 0; j < laneCnt; ++j)
		{
			Lane& lane = e.lanes[j];
			uint8 build, ltype, dir, op;
			r.read(lane.index);
			r.read(build); lane.build = static_cast<BuildState>(build);
			r.read(ltype); lane.type  = static_cast<LaneType>(ltype);
			r.read(lane.width);
			r.read(dir); lane.dir = static_cast<LaneDir>(dir);
			r.read(op);  lane.op  = static_cast<OpState>(op);
		}
		e.laneVehicles = Array<Array<int>>(e.lanes.size());

		outEdges << e;
	}

	return true;
}

bool RoadBinary::writeGlobal(const FilePath& path, const RoadNetwork& network)
{
	return write(path, 0, 0, network.nodes(), network.edges());
}

bool RoadBinary::readGlobal(const FilePath& path, RoadNetwork& network)
{
	Array<RoadNode> nodes;
	Array<RoadEdge> edges;
	if (!read(path, nodes, edges)) return false;

	for (const auto& n : nodes) network.addNodeRaw(n);
	for (const auto& e : edges) network.addEdgeRaw(e);
	return true;
}

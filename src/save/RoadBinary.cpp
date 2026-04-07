#include "RoadBinary.hpp"

namespace
{
	/// @brief 文字列を uint16 長さ + UTF-8 バイト列で書き出す
	void writeString(BinaryWriter& w, const String& s)
	{
		const std::string u8 = s.toUTF8();
		const uint16 len = static_cast<uint16>(u8.size());
		w.write(len);
		if (len > 0) w.write(u8.data(), len);
	}

	/// @brief writeString と対称に文字列を読み込む
	bool readString(BinaryReader& r, String& out)
	{
		uint16 len = 0;
		if (!r.read(len)) return false;
		if (len == 0) { out.clear(); return true; }
		std::string buf(len, '\0');
		if (r.read(buf.data(), len) != static_cast<int64>(len)) return false;
		out = Unicode::FromUTF8(buf);
		return true;
	}

	/// @brief 文字列を読み飛ばす（readString のスキップ版）
	void skipString(BinaryReader& r)
	{
		uint16 len; r.read(len);
		r.skip(len);
	}

	/// @brief SignalPlacement をスキップする
	void skipSignalPlacement(BinaryReader& r)
	{
		uint8 hasSig; r.read(hasSig);
		if (hasSig == 0) return;
		skipString(r);  // signalDefId
		uint32 subCnt; r.read(subCnt);
		for (uint32 j = 0; j < subCnt; ++j) skipString(r);
		r.skip(sizeof(float));  // yawOffset
	}

}

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
		w.write(static_cast<uint32>(n.attachments.size()));
		for (const auto& att : n.attachments)
		{
			w.write(att.edgeId);
			w.write(att.lateralOffset);
			w.write(static_cast<uint8>(att.isThrough ? 1 : 0));
			w.write(static_cast<uint8>(att.control));
		}

		// v4: SignalPlacement
		const bool hasSignal = n.signalPlacement.has_value();
		w.write(static_cast<uint8>(hasSignal ? 1 : 0));
		if (hasSignal)
		{
			const auto& sp = *n.signalPlacement;
			writeString(w, sp.signalDefId);
			w.write(static_cast<uint32>(sp.subLampStates.size()));
			for (const auto& s : sp.subLampStates) writeString(w, s);
			w.write(sp.yawOffset);
		}
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
			w.write(lane.offsetA_L);
			w.write(lane.offsetA_R);
			w.write(lane.offsetB_L);
			w.write(lane.offsetB_R);
			w.write(lane.nominalWidth);
			w.write(static_cast<uint8>(lane.dir));
			w.write(static_cast<uint8>(lane.op));
			w.write(static_cast<uint8>(lane.type));
			w.write(static_cast<uint8>(lane.lineLeft));
			w.write(static_cast<uint8>(lane.lineRight));
			w.write(static_cast<uint8>(lane.canChangeLaneLeft ? 1 : 0));
			w.write(static_cast<uint8>(lane.canChangeLaneRight ? 1 : 0));
		}

		// v2: useElevation
		w.write(static_cast<uint8>(e.useElevation ? 1 : 0));

		// v3: parts（道路部品配列）
		w.write(static_cast<uint32>(e.parts.size()));
		for (const auto& p : e.parts)
		{
			writeString(w, p.defId);
			w.write(p.width);
			w.write(p.offset);
			w.write(static_cast<uint8>(p.build));
			w.write(static_cast<uint8>(p.type));
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
	if (!r.read(version) || version != kVersion)
	{
		Console << U"[RoadBinary] Unsupported save version: " << version << U" (expected " << kVersion << U")";
		return false;
	}

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

		// attachment の全フィールドを読む
		for (uint32 j = 0; j < edgeCnt; ++j)
		{
			EdgeAttachment att;
			r.read(att.edgeId);
			r.read(att.lateralOffset);
			uint8 isThrough, ctrl;
			r.read(isThrough); att.isThrough = (isThrough != 0);
			r.read(ctrl); att.control = static_cast<TrafficControl>(ctrl);
			n.attachments << att;
		}

		// SignalPlacement
		{
			uint8 hasSignal;
			if (r.read(hasSignal) && hasSignal != 0)
			{
				SignalPlacement sp;
				if (!readString(r, sp.signalDefId)) return false;
				uint32 subCnt;
				r.read(subCnt);
				for (uint32 j = 0; j < subCnt; ++j)
				{
					String s;
					if (!readString(r, s)) return false;
					sp.subLampStates << s;
				}
				r.read(sp.yawOffset);
				n.signalPlacement = std::move(sp);
			}
		}

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
			uint8 dir, op, ltype, lineL, lineR, canL, canR;
			r.read(lane.offsetA_L);
			r.read(lane.offsetA_R);
			r.read(lane.offsetB_L);
			r.read(lane.offsetB_R);
			r.read(lane.nominalWidth);
			r.read(dir);   lane.dir   = static_cast<LaneDir>(dir);
			r.read(op);    lane.op    = static_cast<OpState>(op);
			r.read(ltype); lane.type  = static_cast<LaneType>(ltype);
			r.read(lineL); lane.lineLeft  = static_cast<LineType>(lineL);
			r.read(lineR); lane.lineRight = static_cast<LineType>(lineR);
			r.read(canL);  lane.canChangeLaneLeft  = (canL != 0);
			r.read(canR);  lane.canChangeLaneRight = (canR != 0);
		}
		e.laneVehicles = Array<Array<int>>(e.lanes.size());

		// useElevation
		{
			uint8 elev;
			r.read(elev);
			e.useElevation = (elev != 0);
		}

		// parts
		{
			uint32 partCnt;
			r.read(partCnt);
			e.parts.clear();
			e.parts.reserve(partCnt);
			for (uint32 p = 0; p < partCnt; ++p)
			{
				RoadPart part;
				uint8 bs, pt;
				if (!readString(r, part.defId)) return false;
				r.read(part.width);
				r.read(part.offset);
				r.read(bs); part.build = static_cast<BuildState>(bs);
				r.read(pt); part.type  = static_cast<RoadPartType>(pt);
				e.parts << part;
			}
		}

		outEdges << e;
	}

	return true;
}

bool RoadBinary::writeGlobal(const FilePath& path, const RoadNetwork& network)
{
	if (!write(path, 0, 0, network.nodes(), network.edges()))
		return false;

	// RoadObject をファイル末尾に追記
	BinaryWriter w{ path, OpenMode::Append };
	if (!w) return false;

	Array<RoadObject> validObjects;
	for (const auto& obj : network.objects())
		if (obj.id >= 0) validObjects << obj;

	w.write(static_cast<uint32>(validObjects.size()));
	for (const auto& obj : validObjects)
	{
		w.write(obj.id);
		w.write(obj.parentEdgeId);
		w.write(obj.arcPos);
		w.write(obj.lateralOffset);
		w.write(static_cast<uint8>(obj.type));
		w.write(obj.scale);
		w.write(obj.yawOffset);
		w.write(obj.heightOverride);
	}
	return true;
}

bool RoadBinary::readGlobal(const FilePath& path, RoadNetwork& network)
{
	Array<RoadNode> nodes;
	Array<RoadEdge> edges;
	if (!read(path, nodes, edges)) return false;

	// attachment 情報を退避（addEdgeRaw がデフォルト attachment で上書きするため）
	HashTable<int, Array<EdgeAttachment>> savedAttachments;
	for (const auto& n : nodes)
		if (!n.attachments.isEmpty())
			savedAttachments[n.id] = n.attachments;

	for (auto& n : nodes) n.attachments.clear();
	for (const auto& n : nodes) network.addNodeRaw(n);
	for (const auto& e : edges) network.addEdgeRaw(e);

	// 保存した attachment 情報を復元
	for (const auto& [nid, atts] : savedAttachments)
	{
		RoadNode* node = network.getNode(nid);
		if (!node) continue;
		for (const auto& saved : atts)
		{
			if (auto* att = node->getAttachment(saved.edgeId))
			{
				att->lateralOffset = saved.lateralOffset;
				att->isThrough     = saved.isThrough;
				att->control       = saved.control;
			}
		}
	}

	// RoadObject を読み込む
	// read() 後にファイルを再度開いてノード+エッジをスキップする
	BinaryReader r{ path };
	if (!r) return true;

	// ヘッダーをスキップ
	r.setPos(0);
	uint32 magic2; r.read(magic2);
	uint16 ver2;   r.read(ver2);
	int32 cx2, cy2; r.read(cx2); r.read(cy2);
	uint32 nc2, ec2, pc2, lc2;
	r.read(nc2); r.read(ec2); r.read(pc2); r.read(lc2);

	// ノードをスキップ
	for (uint32 i = 0; i < nc2; ++i)
	{
		r.skip(sizeof(int32) + sizeof(float) * 3 + sizeof(uint8));
		uint32 attCnt; r.read(attCnt);
		r.skip(attCnt * (sizeof(int32) + sizeof(float) + sizeof(uint8) * 2));
		skipSignalPlacement(r);
	}

	// エッジをスキップ
	for (uint32 i = 0; i < ec2; ++i)
	{
		r.skip(sizeof(int32) * 3 + sizeof(float) * 6 + sizeof(uint8) +
		       sizeof(float) * 3 + sizeof(float) * 2 +
		       sizeof(uint8) + sizeof(int32) * 2);
		uint32 laneCnt; r.read(laneCnt);
		r.skip(laneCnt * (sizeof(float) * 5 + sizeof(uint8) * 7));
		r.skip(sizeof(uint8));  // useElevation
		uint32 partCnt; r.read(partCnt);
		for (uint32 p = 0; p < partCnt; ++p)
		{
			skipString(r);
			r.skip(sizeof(float) * 2 + sizeof(uint8) * 2);
		}
	}

	// RoadObject を読み込み
	uint32 objCount;
	if (r.read(objCount))
	{
		for (uint32 i = 0; i < objCount; ++i)
		{
			RoadObject obj;
			r.read(obj.id);
			r.read(obj.parentEdgeId);
			r.read(obj.arcPos);
			r.read(obj.lateralOffset);
			uint8 objType; r.read(objType);
			obj.type = static_cast<RoadObjectType>(objType);
			r.read(obj.scale);
			r.read(obj.yawOffset);
			r.read(obj.heightOverride);
			network.addObject(obj);
		}
	}

	return true;
}

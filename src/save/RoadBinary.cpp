#include "RoadBinary.hpp"

namespace
{
	constexpr uint32 kMaxNodeCount              = 2'000'000;
	constexpr uint32 kMaxEdgeCount              = 2'000'000;
	constexpr uint32 kMaxAttachmentPerNode      = 512;
	constexpr uint32 kMaxSignalPhaseCount       = 128;
	constexpr uint32 kMaxConnPerSignalPhase     = 2048;
	constexpr uint32 kMaxLanePerEdge            = 64;
	constexpr uint32 kMaxPartPerEdge            = 256;
	constexpr uint32 kMaxSignPerEdge            = 256;
	constexpr uint32 kMaxRoadObjectCount        = 1'000'000;
	constexpr uint32 kMaxRouteCount             = 200'000;
	constexpr uint32 kMaxRouteEdgeCount         = 200'000;

	bool validateCount(uint32 count, uint32 maxAllowed, StringView label)
	{
		if (count <= maxAllowed)
		{
			return true;
		}

		Console << U"[RoadBinary] Invalid count for {}: {} > {}"_fmt(label, count, maxAllowed);
		return false;
	}

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
	bool skipSignalPlacement(BinaryReader& r)
	{
		uint8 hasSig;
		if (!r.read(hasSig)) return false;
		if (hasSig == 0) return true;
		skipString(r);  // signalDefId
		r.skip(sizeof(float));  // yawOffset
		// phases
		uint32 phaseCnt;
		if (!r.read(phaseCnt)) return false;
		if (!validateCount(phaseCnt, kMaxSignalPhaseCount, U"skip.signal.phaseCnt")) return false;
		for (uint32 pi = 0; pi < phaseCnt; ++pi)
		{
			r.skip(sizeof(float));  // duration
			uint32 gCnt;
			if (!r.read(gCnt)) return false;
			if (!validateCount(gCnt, kMaxConnPerSignalPhase, U"skip.signal.greenCnt")) return false;
			r.skip(gCnt * sizeof(int32));
		}
		return true;
	}

}

bool RoadBinary::write(const FilePath& path, int32 cx, int32 cy,
                       const Array<RoadNode>& nodes,
                       const Array<RoadEdge>& edges)
{
	// tombstone を除いた現役ノード/エッジだけを、チャンク単位の道路スナップショットとして直列化する。
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

		// SignalPlacement
		const bool hasSignal = n.signalPlacement.has_value();
		w.write(static_cast<uint8>(hasSignal ? 1 : 0));
		if (hasSignal)
		{
			const auto& sp = *n.signalPlacement;
			writeString(w, sp.signalDefId);
			w.write(sp.yawOffset);
			// フェーズ定義
			w.write(static_cast<uint32>(sp.phases.size()));
			for (const auto& ph : sp.phases)
			{
				w.write(ph.duration);
				w.write(static_cast<uint32>(ph.greenConnectionIds.size()));
				for (const int cid : ph.greenConnectionIds) w.write(static_cast<int32>(cid));
			}
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
		w.write(e.constructionStartTime);
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
			w.write(p.offsetA_L);
			w.write(p.offsetA_R);
			w.write(p.offsetB_L);
			w.write(p.offsetB_R);
			w.write(static_cast<uint8>(p.build));
			w.write(static_cast<uint8>(p.type));
		}

		// signs（道路標識配置）
		w.write(static_cast<uint32>(e.signs.size()));
		for (const auto& s : e.signs)
		{
			w.write(static_cast<uint8>(s.type));
			w.write(s.nodeEndId);
			w.write(s.arcOffset);
			w.write(s.lateralOffset);
			w.write(s.poleHeight);
			w.write(s.yawOffset);
			w.write(static_cast<int32>(s.auxValue));
			w.write(static_cast<uint8>(s.autoGenerated ? 1 : 0));
		}

		// guideSigns は roads.bin から分離し、
		// global/guide_signs.json + cache PNG に保存する（plan/21_guide_sign_spec.md）
	}

	return true;
}

bool RoadBinary::read(const FilePath& path,
                      Array<RoadNode>& outNodes,
                      Array<RoadEdge>& outEdges)
{
	// 基本道路データを防御的に検証しながら読み戻し、上位で再構築できる形の配列へ復元する。
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
	if (!r.read(cx) || !r.read(cy)) return false;
	uint32 nodeCount, edgeCount, planCount, lightCount;
	if (!r.read(nodeCount) || !r.read(edgeCount) || !r.read(planCount) || !r.read(lightCount)) return false;
	if (!validateCount(nodeCount, kMaxNodeCount, U"nodeCount")) return false;
	if (!validateCount(edgeCount, kMaxEdgeCount, U"edgeCount")) return false;

	// ---- RoadNode レコード ----
	for (uint32 i = 0; i < nodeCount; ++i)
	{
		RoadNode n;
		float px, py, pz;
		uint8 type;
		uint32 edgeCnt;

		if (!r.read(n.id)) return false;
		if (!r.read(px) || !r.read(py) || !r.read(pz)) return false;
		n.position = Vec3{ px, py, pz };
		if (!r.read(type) || !r.read(edgeCnt)) return false;
		if (!validateCount(edgeCnt, kMaxAttachmentPerNode, U"node.edgeCnt")) return false;

		// attachment の全フィールドを読む
		for (uint32 j = 0; j < edgeCnt; ++j)
		{
			EdgeAttachment att;
			if (!r.read(att.edgeId) || !r.read(att.lateralOffset)) return false;
			uint8 isThrough, ctrl;
			if (!r.read(isThrough) || !r.read(ctrl)) return false;
			att.isThrough = (isThrough != 0);
			att.control = static_cast<TrafficControl>(ctrl);
			n.attachments << att;
		}

		// SignalPlacement
		{
			uint8 hasSignal;
			if (!r.read(hasSignal)) return false;
			if (hasSignal != 0)
			{
				SignalPlacement sp;
				if (!readString(r, sp.signalDefId)) return false;
				if (!r.read(sp.yawOffset)) return false;
				// フェーズ定義
				uint32 phaseCnt;
				if (!r.read(phaseCnt)) return false;
				if (!validateCount(phaseCnt, kMaxSignalPhaseCount, U"signal.phaseCnt")) return false;
				for (uint32 pi = 0; pi < phaseCnt; ++pi)
				{
					SignalPhaseDef ph;
					if (!r.read(ph.duration)) return false;
					uint32 gCnt;
					if (!r.read(gCnt)) return false;
					if (!validateCount(gCnt, kMaxConnPerSignalPhase, U"signal.greenCnt")) return false;
					for (uint32 gi = 0; gi < gCnt; ++gi)
					{
						int32 cid;
						if (!r.read(cid)) return false;
						ph.greenConnectionIds << cid;
					}
					sp.phases << std::move(ph);
				}
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
		if (!r.read(e.nodeA) || !r.read(e.nodeB)) return false;
		if (!r.read(x) || !r.read(y) || !r.read(z)) return false;
		e.ctrlA = Vec3{ x, y, z };
		if (!r.read(x) || !r.read(y) || !r.read(z)) return false;
		e.ctrlB = Vec3{ x, y, z };
		if (!r.read(rt)) return false;
		e.roadType = static_cast<RoadType>(rt);
		if (!r.read(e.speedLimit) || !r.read(e.length) || !r.read(e.planId) || !r.read(e.cutoffA) || !r.read(e.cutoffB)) return false;
		if (!r.read(es)) return false;
		e.edgeState = static_cast<EdgeState>(es);
		if (!r.read(e.constructionStartTime)) return false;
		if (!r.read(e.borderNodeA) || !r.read(e.borderNodeB) || !r.read(laneCnt)) return false;
		if (!validateCount(laneCnt, kMaxLanePerEdge, U"edge.laneCnt")) return false;
		e.lanes.resize(laneCnt);
		for (uint32 j = 0; j < laneCnt; ++j)
		{
			Lane& lane = e.lanes[j];
			uint8 dir, op, ltype, lineL, lineR, canL, canR;
			if (!r.read(lane.offsetA_L) || !r.read(lane.offsetA_R) || !r.read(lane.offsetB_L) || !r.read(lane.offsetB_R) || !r.read(lane.nominalWidth)) return false;
			if (!r.read(dir) || !r.read(op) || !r.read(ltype) || !r.read(lineL) || !r.read(lineR) || !r.read(canL) || !r.read(canR)) return false;
			lane.dir   = static_cast<LaneDir>(dir);
			lane.op    = static_cast<OpState>(op);
			lane.type  = static_cast<LaneType>(ltype);
			lane.lineLeft  = static_cast<LineType>(lineL);
			lane.lineRight = static_cast<LineType>(lineR);
			lane.canChangeLaneLeft  = (canL != 0);
			lane.canChangeLaneRight = (canR != 0);
		}
		e.laneVehicles = Array<Array<int>>(e.lanes.size());

		// useElevation
		{
			uint8 elev;
			if (!r.read(elev)) return false;
			e.useElevation = (elev != 0);
		}

		// parts
		{
			uint32 partCnt;
			if (!r.read(partCnt)) return false;
			if (!validateCount(partCnt, kMaxPartPerEdge, U"edge.partCnt")) return false;
			e.parts.clear();
			e.parts.reserve(partCnt);
			for (uint32 p = 0; p < partCnt; ++p)
			{
				RoadPart part;
				uint8 bs, pt;
				if (!readString(r, part.defId)) return false;
				if (!r.read(part.offsetA_L) || !r.read(part.offsetA_R) || !r.read(part.offsetB_L) || !r.read(part.offsetB_R) || !r.read(bs) || !r.read(pt)) return false;
				part.build = static_cast<BuildState>(bs);
				part.type  = static_cast<RoadPartType>(pt);
				e.parts << part;
			}
		}

		// signs（道路標識配置）
		{
			uint32 signCnt;
			if (!r.read(signCnt)) return false;
			if (!validateCount(signCnt, kMaxSignPerEdge, U"edge.signCnt")) return false;
			e.signs.clear();
			e.signs.reserve(signCnt);
			for (uint32 si = 0; si < signCnt; ++si)
			{
				RoadSignPlacement s;
				uint8 stype, autoGen;
				int32 aux;
				if (!r.read(stype) || !r.read(s.nodeEndId) || !r.read(s.arcOffset) || !r.read(s.lateralOffset) || !r.read(s.poleHeight) || !r.read(s.yawOffset) || !r.read(aux) || !r.read(autoGen)) return false;
				s.type = static_cast<RoadSignType>(stype);
				s.auxValue = static_cast<int>(aux);
				s.autoGenerated = (autoGen != 0);
				e.signs << s;
			}
		}

		// v11 以降: guideSigns は roads.bin から分離済み
		{
			// no-op
		}

		outEdges << e;
	}

	return true;
}

bool RoadBinary::writeGlobal(const FilePath& path, const RoadNetwork& network)
{
	// 基本道路スナップショットの後ろに、グローバル管理の道路オブジェクトと路線情報を追記する。
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

	// RoadRoute 配列
	Array<RoadRoute> validRoutes;
	for (const auto& r : network.routes())
		if (r.id >= 0) validRoutes << r;

	w.write(static_cast<uint32>(validRoutes.size()));
	for (const auto& rt : validRoutes)
	{
		w.write(rt.id);
		w.write(static_cast<uint8>(rt.kind));
		writeString(w, rt.name);
		w.write(static_cast<int32>(rt.number));
		w.write(static_cast<uint32>(rt.edgeIds.size()));
		for (const int eid : rt.edgeIds) w.write(static_cast<int32>(eid));
		w.write(static_cast<float>(rt.color.r));
		w.write(static_cast<float>(rt.color.g));
		w.write(static_cast<float>(rt.color.b));
	}

	Array<RoadPlan> validPlans;
	for (const auto& plan : network.plans())
		if (plan.id >= 0) validPlans << plan;

	w.write(static_cast<uint32>(validPlans.size()));
	for (const auto& plan : validPlans)
	{
		w.write(plan.id);
		writeString(w, plan.name);
		writeString(w, plan.routeName);
		w.write(static_cast<int32>(plan.routeId));
		writeString(w, plan.originName);
		writeString(w, plan.destName);
		w.write(static_cast<uint8>(plan.roadType));
		w.write(static_cast<uint8>(plan.state));
		w.write(static_cast<uint32>(plan.edgeIds.size()));
		for (const int eid : plan.edgeIds) w.write(static_cast<int32>(eid));
		w.write(static_cast<uint32>(plan.viaPoints.size()));
		for (const Vec3& point : plan.viaPoints)
		{
			w.write(static_cast<float>(point.x));
			w.write(static_cast<float>(point.y));
			w.write(static_cast<float>(point.z));
		}
		w.write(plan.totalCost);
		w.write(plan.totalLength);
		const bool hasStart = plan.constructionStart.has_value();
		w.write(static_cast<uint8>(hasStart ? 1 : 0));
		if (hasStart) w.write(*plan.constructionStart);
		w.write(plan.constructionDuration);
		const bool hasCompletion = plan.completionDate.has_value();
		w.write(static_cast<uint8>(hasCompletion ? 1 : 0));
		if (hasCompletion) w.write(*plan.completionDate);
	}
	return true;
}

bool RoadBinary::readGlobal(const FilePath& path, RoadNetwork& network)
{
	// 基本道路を復元した後、付帯情報を順に読み足して RoadNetwork 全体を再生する。
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

	// addEdgeRaw 後に、保存時の attachment 補助情報だけをノード側へ上書きで戻す。
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
	uint32 magic2;
	uint16 ver2;
	int32 cx2, cy2;
	if (!r.read(magic2) || !r.read(ver2) || !r.read(cx2) || !r.read(cy2)) return false;
	if (magic2 != kMagic || ver2 != kVersion) return false;
	uint32 nc2, ec2, pc2, lc2;
	if (!r.read(nc2) || !r.read(ec2) || !r.read(pc2) || !r.read(lc2)) return false;
	if (!validateCount(nc2, kMaxNodeCount, U"skip.nodeCount")) return false;
	if (!validateCount(ec2, kMaxEdgeCount, U"skip.edgeCount")) return false;

	// ノードをスキップ
	for (uint32 i = 0; i < nc2; ++i)
	{
		r.skip(sizeof(int32) + sizeof(float) * 3 + sizeof(uint8));
		uint32 attCnt;
		if (!r.read(attCnt)) return false;
		if (!validateCount(attCnt, kMaxAttachmentPerNode, U"skip.node.attCnt")) return false;
		r.skip(attCnt * (sizeof(int32) + sizeof(float) + sizeof(uint8) * 2));
		if (!skipSignalPlacement(r)) return false;
	}

	// エッジをスキップ（現行フォーマット固定）
	for (uint32 i = 0; i < ec2; ++i)
	{
		// id + nodeA + nodeB + ctrlA.xyz + ctrlB.xyz + roadType + speedLimit + length + planId + cutoffA + cutoffB + edgeState
		r.skip(sizeof(int32) * 3 + sizeof(float) * 6 + sizeof(uint8) +
		       sizeof(float) * 3 + sizeof(float) * 2 +
		       sizeof(uint8));
		r.skip(sizeof(double)); // constructionStartTime
		r.skip(sizeof(int32) * 2);  // borderNodeA, borderNodeB
		uint32 laneCnt;
		if (!r.read(laneCnt)) return false;
		if (!validateCount(laneCnt, kMaxLanePerEdge, U"skip.edge.laneCnt")) return false;
		r.skip(laneCnt * (sizeof(float) * 5 + sizeof(uint8) * 7));
		r.skip(sizeof(uint8));  // useElevation
		uint32 partCnt;
		if (!r.read(partCnt)) return false;
		if (!validateCount(partCnt, kMaxPartPerEdge, U"skip.edge.partCnt")) return false;
		for (uint32 p = 0; p < partCnt; ++p)
		{
			skipString(r);
			// offsetA_L + offsetA_R + offsetB_L + offsetB_R + build + type
			r.skip(sizeof(float) * 4 + sizeof(uint8) * 2);
		}
		// signs
		{
			uint32 signCnt;
			if (!r.read(signCnt)) return false;
			if (!validateCount(signCnt, kMaxSignPerEdge, U"skip.edge.signCnt")) return false;
			// 1 sign のサイズ: uint8 + int32 + float*4 + int32 + uint8 = 1+4+16+4+1 = 26
			r.skip(signCnt * (sizeof(uint8) + sizeof(int32) + sizeof(float) * 4 + sizeof(int32) + sizeof(uint8)));
		}

		// guideSigns は別ファイル管理（skip なし）
	}

	// 自動生成 signs を再計算
	network.recomputeAllAutoSigns();

	// 末尾追記領域から道路オブジェクトと路線を読み戻し、最後に逆引きインデックスを張り直す。
	// RoadObject を読み込み
	uint32 objCount;
	if (!r.read(objCount)) return false;
	if (!validateCount(objCount, kMaxRoadObjectCount, U"objCount")) return false;
	for (uint32 i = 0; i < objCount; ++i)
	{
		RoadObject obj;
		if (!r.read(obj.id) ||
			!r.read(obj.parentEdgeId) ||
			!r.read(obj.arcPos) ||
			!r.read(obj.lateralOffset))
		{
			return false;
		}
		uint8 objType;
		if (!r.read(objType) ||
			!r.read(obj.scale) ||
			!r.read(obj.yawOffset) ||
			!r.read(obj.heightOverride))
		{
			return false;
		}
		obj.type = static_cast<RoadObjectType>(objType);
		network.addObject(obj);
	}

	// RoadRoute 配列
	uint32 routeCount;
	if (!r.read(routeCount)) return false;
	if (!validateCount(routeCount, kMaxRouteCount, U"routeCount")) return false;
	for (uint32 i = 0; i < routeCount; ++i)
	{
		int32 rid;
		uint8 kind;
		String name;
		int32 number;
		if (!r.read(rid) || !r.read(kind) || !readString(r, name) || !r.read(number))
		{
			return false;
		}

		uint32 edgeCount;
		if (!r.read(edgeCount)) return false;
		if (!validateCount(edgeCount, kMaxRouteEdgeCount, U"route.edgeCount")) return false;
		Array<int> edgeIds;
		edgeIds.reserve(edgeCount);
		for (uint32 j = 0; j < edgeCount; ++j)
		{
			int32 eid;
			if (!r.read(eid)) return false;
			edgeIds << eid;
		}

		float cr, cg, cb;
		if (!r.read(cr) || !r.read(cg) || !r.read(cb)) return false;

		RoadRoute route;
		route.id = rid;
		route.kind = static_cast<RoadRouteKind>(kind);
		route.name = std::move(name);
		route.number = static_cast<int>(number);
		route.edgeIds = std::move(edgeIds);
		route.color = ColorF{ cr, cg, cb };
		network.addRouteRaw(route);
	}

	uint32 planCount;
	if (!r.read(planCount)) return false;
	if (!validateCount(planCount, kMaxRouteCount, U"planCount")) return false;
	for (uint32 i = 0; i < planCount; ++i)
	{
		RoadPlan plan;
		int32 routeId;
		uint8 roadType;
		uint8 state;
		if (!r.read(plan.id) ||
			!readString(r, plan.name) ||
			!readString(r, plan.routeName) ||
			!r.read(routeId) ||
			!readString(r, plan.originName) ||
			!readString(r, plan.destName) ||
			!r.read(roadType) ||
			!r.read(state))
		{
			return false;
		}
		plan.routeId = routeId;
		plan.roadType = static_cast<RoadType>(roadType);
		plan.state = static_cast<PlanState>(state);

		uint32 edgeCount;
		if (!r.read(edgeCount)) return false;
		if (!validateCount(edgeCount, kMaxRouteEdgeCount, U"plan.edgeCount")) return false;
		plan.edgeIds.reserve(edgeCount);
		for (uint32 j = 0; j < edgeCount; ++j)
		{
			int32 eid;
			if (!r.read(eid)) return false;
			plan.edgeIds << eid;
		}

		uint32 viaCount;
		if (!r.read(viaCount)) return false;
		if (!validateCount(viaCount, kMaxRouteEdgeCount, U"plan.viaCount")) return false;
		plan.viaPoints.reserve(viaCount);
		for (uint32 j = 0; j < viaCount; ++j)
		{
			float x, y, z;
			if (!r.read(x) || !r.read(y) || !r.read(z)) return false;
			plan.viaPoints << Vec3{ x, y, z };
		}

		if (!r.read(plan.totalCost) || !r.read(plan.totalLength)) return false;
		uint8 hasStart;
		if (!r.read(hasStart)) return false;
		if (hasStart)
		{
			GameTime start;
			if (!r.read(start)) return false;
			plan.constructionStart = start;
		}
		if (!r.read(plan.constructionDuration)) return false;
		uint8 hasCompletion;
		if (!r.read(hasCompletion)) return false;
		if (hasCompletion)
		{
			GameTime completion;
			if (!r.read(completion)) return false;
			plan.completionDate = completion;
		}
		network.addPlanRaw(plan);
	}

	network.rebuildEdgeRouteIndex();
	network.rebuildPlanEdgeLinks();
	network.rebuildAllPlanStats();

	return true;
}

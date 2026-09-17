#pragma once
#include "../railway/TrainManager.hpp"
#include "RailFacilities.hpp"
#include "ModelLod.hpp"
#include "MeshLod.hpp"
#include "../railway/TrainNetwork.hpp"
#include "../world/World.hpp"
#include "../gen/ParcelRoadIndex.hpp"

/// @brief 鉄道（線路・列車）の描画クラス
class TrainRenderer
{
public:
	// 線路の静的描画と列車の動的描画を分け、線路側だけ個別キャッシュできる構成にする。
	/// @brief 線路を描画する
	void renderTracks(const TrainNetwork& network,const World& world,Vec3 eye,const RoadNetwork& roads);

	/// @brief 列車を描画する
	void renderTrains(const Array<Train>& trains,const TrainNetwork& network, Optional<Vec3> eye = none);
	/// @brief 前フレームで見えた駅名・車庫名を3D描画前に合成する。
	void prepareFacilityTextures();
	/// @brief 選択中の編成を通常描画と同じ姿勢・LODでマスクへ描く。
	void drawTrainSilhouette(const Train& train,const TrainNetwork& network,Vec3 eye,const ColorF& color);

	/// @brief エッジの線路メッシュキャッシュを無効化する（線路変更時に呼ぶ）
	void invalidateTrackCache(int edgeId);

	/// @brief 道路形状の変更時に橋脚の干渉判定と床版キャッシュを更新する。
	void invalidateRoadClearance() { m_roadClearance.reset(); m_bedMeshCache.clear(); }

private:
	/// @brief エッジの線路メッシュを構築する
	static Mesh buildTrackMesh(const TrackEdge& edge, const CubicBezier& bez, bool distant = false);

	struct FacilityDraw
	{
		std::array<Mesh,RailFacilities::Count> parts, distant;
		Array<std::pair<Mesh,String>> signs;
	};
	FacilityDraw buildFacility(const RailFacilities::Geometry& geometry);
	void drawFacility(const FacilityDraw& draw, bool distant);
	size_t m_facilityEdgeCount = 0;
	HashTable<int,FacilityDraw> m_stations;
	HashTable<int,FacilityDraw> m_depots;
	HashTable<String,RenderTexture> m_facilityTextures;
	HashSet<String> m_pendingFacilityNames;
	Model& ensureModel(const String& stem, int level);
	HashTable<String, ModelLod> m_models;
	Array<Train> m_parkedTrains;
	HashTable<int, Mesh> m_distantTracks;
	std::unique_ptr<ParcelRoadIndex> m_roadClearance;
	size_t m_roadEdgeCount=0;
	HashTable<int, Mesh> m_bedMeshCache;
	HashTable<int, Mesh> m_trackMeshCache;  ///< エッジ ID → 線路メッシュ
};

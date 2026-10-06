#pragma once
#include "../gen/DistrictHierarchy.hpp"
#include "../gen/MapGenerator.hpp"
#include "../ui/Camera.hpp"
#include "../ui/LocalMapView.hpp"
#include "../ui/MapTerrainLayer.hpp"
#include "../railway/TrainNetwork.hpp"
#include "../road/RoadNetwork.hpp"
#include "../asset/AssetRegistrar.hpp"

/// @brief 周辺地図と全画面地図で、同じ道路・鉄道・地名を共有する。
class MinimapRenderer
{
public:
	/// @brief Warm initial cartography and local terrain without consuming player input.
	void preload(const GameCamera& camera,const RoadNetwork& roads,const TrainNetwork& railway,const Array<MapGenerator::Settlement>& settlements);
	void setSmallBounds(Optional<RectF> bounds) { m_smallBounds=bounds;m_smallVisible=bounds.has_value(); }
	void buildTerrainTexture(const World& world);
	void setGeography(const World& world,const DistrictHierarchy& districts) { m_world=&world;m_districts=&districts;m_mapDirty=true; }
	void updateRoadOverlay(const RoadNetwork& roads,const World& world);
	void updateRoadOverlayAround(const Array<int>& nodes,const RoadNetwork& roads);
	Optional<Vec2> update(const GameCamera& camera,const RoadNetwork& roads,const TrainNetwork& railway,const Array<MapGenerator::Settlement>& settlements);
	void openFullScreen(const GameCamera& camera,const RoadNetwork& roads,const TrainNetwork& railway,const Array<MapGenerator::Settlement>& settlements);
	WorldMapView& mapView() { return m_map; }
	LocalMapView& localView() { return m_local; }
	bool fullScreen() const { return m_map.visible; }
	bool consumedInput() const { return m_consumedInput; }
	void drawFullScreen(const GameCamera& camera) const;
	void render(const GameCamera& camera,const Array<MapGenerator::Settlement>& settlements) const;
private:
	mutable WorldMapView m_map;
	LocalMapView m_local;
	const World* m_world=nullptr;
	const DistrictHierarchy* m_districts=nullptr;
	bool m_mapDirty=true,m_consumedInput=false,m_smallVisible=true;
	size_t m_railEdges=0,m_railNodes=0;
	static constexpr int kDisplaySize=200,kMargin=12;
	mutable MapTerrainLayer m_localTerrain,m_fullTerrain;
	mutable uint64 m_waterMapRevision=0;
	mutable double m_waterMapUpdated=0;
	Font m_font=FontAsset(Asset::CJK14);
	Optional<RectF> m_smallBounds;
	RectF smallRect() const;
	float terrainHeight(Vec2 point) const;
	/// @brief Simplified wet centerlines keep narrow rivers legible at country-wide map scales.
	void refreshWaterStrokes() const;
	void refreshMap(const RoadNetwork& roads,const TrainNetwork& railway,const Array<MapGenerator::Settlement>& settlements);
};

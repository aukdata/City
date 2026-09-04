#pragma once
#include "../time/GameClock.hpp"
#include "../road/RoadNetwork.hpp"

/// @brief イベント種別
enum class EventType : uint8
{
	// 季節イベント
	GoldenWeek,        ///< ゴールデンウィーク（5月）
	SummerFestival,    ///< 夏祭り（7-8月）
	OBonRush,          ///< お盆帰省ラッシュ（8月）
	Fireworks,         ///< 花火大会（8月）
	FoliageSeason,     ///< 紅葉シーズン（10-11月）
	NewYearRush,       ///< 年末年始ラッシュ（12-1月）
	HatsuMode,         ///< 初詣渋滞（1月）
	GraduationSeason,  ///< 卒業・入学式（3-4月）
	// ランダムイベント
	Typhoon,           ///< 台風
	HeavyRain,         ///< 大雨・冠水
	Landslide,         ///< 土砂崩れ
	Rockfall,          ///< 落石
	Snowfall,          ///< 積雪
	TrafficAccident,   ///< 交通事故
	LargeVehicleBreakdown, ///< 大型車故障
	WaterPipeWork,     ///< 水道管工事
	GasPipeWork,       ///< ガス管工事
	Ekiden,            ///< 駅伝大会
	Marathon,          ///< マラソン大会
};

/// @brief アクティブなイベント
struct GameEvent
{
	// 進行中イベントの影響量と UI 表示文言をひとまとめに持つ。
	int       id       = -1;
	EventType type;
	GameTime  startAt  = 0.0;
	GameTime  endAt    = 0.0;
	float     speedMultiplier = 1.0f;  ///< 全車速度への係数
	String    title;
	String    description;
	bool      notified = false;        ///< UI 通知済みか
};

/// @brief ゲームイベントシステム
/// @details 季節イベントの自動発生・ランダムイベントのロール・TempOp 適用
class EventSystem
{
public:
	// 毎フレームの寿命管理と、月次ロールによるイベント発生をここで集約して扱う。
	/// @brief 毎フレーム更新する
	/// @param gameNow 現在のゲーム時刻
	void update(GameTime gameNow);

	/// @brief 月初めに季節・ランダムイベントを判定する
	/// @param gameNow           月初めのゲーム時刻
	/// @param month             現在の月 (1-12)
	/// @param elapsedMonthIndex ゲーム開始からの経過月インデックス
	void rollMonthly(GameTime gameNow, uint8 month, int64 elapsedMonthIndex, const RoadNetwork& network);

	/// @brief アクティブなイベント一覧を返す（UI 表示用）
	const Array<GameEvent>& activeEvents() const { return m_active; }

	/// @brief 現在の車速グローバル係数を返す（全イベントの最小値）
	float globalSpeedMultiplier() const;

	/// @brief 現在のイベントによる交通需要係数を返す
	double globalDemandMultiplier() const;

	/// @brief 新しい通知を取り出す（呼んだら削除される）
	Array<GameEvent> popNewNotifications();

private:
	Array<GameEvent> m_active;
	Array<GameEvent> m_pendingNotifications;
	int              m_nextId = 0;
	int64            m_lastRollMonthIndex = -1;

	void addEvent(GameEvent ev);
	void removeExpired(GameTime now);

	/// @brief シーズンイベントを生成する
	/// @return なければ none
	Optional<GameEvent> buildSeasonalEvent(uint8 month, GameTime now) const;

	/// @brief ランダムイベントをロールする（nullptr = 発生せず）
	Optional<GameEvent> rollRandomEvent(uint8 month, GameTime now, const RoadNetwork& network) const;
};

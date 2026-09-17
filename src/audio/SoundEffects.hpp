#pragma once
#include <Siv3D.hpp>

/// @brief 小さな合成音源を共有し、操作の結果と運転状態を音で伝える。
class SoundEffects
{
public:
	enum class Cue : uint8 { Select, Confirm, Reject, Construction, Complete, EnterCar, ExitCar, Contact, Count };
	struct DrivingMix
	{
		double pitch=1, engine=0, road=0;
	};
	SoundEffects();
	~SoundEffects();
	void setVolume(double volume);
	void play(Cue cue,double intensity=1);
	void updateDriving(double dt,bool active,double speed,double throttle,bool blocked);
	void stopDriving();
	[[nodiscard]] double volume() const { return m_volume; }
	[[nodiscard]] bool drivingPlaying() const { return m_engine.isPlaying(); }
	[[nodiscard]] size_t playedCount() const { return m_playedCount; }
	[[nodiscard]] DrivingMix mix() const { return m_mix; }
	/// @brief 実際に再生する素材。Testで境界・音量・出力を検証する。
	[[nodiscard]] static Wave makeCue(Cue cue);
	[[nodiscard]] static Wave makeEngine();
	[[nodiscard]] static Wave makeRoadNoise();
	[[nodiscard]] static DrivingMix drivingMix(double speed,double throttle);
private:
	std::array<Audio,static_cast<size_t>(Cue::Count)> m_cues;
	Audio m_engine,m_road;
	DrivingMix m_mix;
	double m_volume=.6,m_previousSpeed=0,m_contactCooldown=0;
	bool m_wasBlocked=false,m_running=false;
	size_t m_playedCount=0;
};

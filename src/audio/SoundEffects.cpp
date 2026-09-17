#include "SoundEffects.hpp"

namespace
{
	constexpr uint32 kSampleRate=44100;
	constexpr double kTwoPi=Math::TwoPi;
	double tone(double frequency,double time) { return Sin(kTwoPi*frequency*time); }
	/// @brief 先頭末尾をゼロにして単発音のクリックを防ぐ。
	double envelope(double time,double length)
	{
		return Min(1.0,time/.008)*Min(1.0,Max(0.0,length-time)/.035);
	}
}

Wave SoundEffects::makeEngine()
{
	Wave wave{size_t(kSampleRate),Arg::sampleRate=kSampleRate};
	for (size_t i=0;i<wave.size();++i)
	{
		const double time=static_cast<double>(i)/kSampleRate;
		// 4気筒の燃焼周期と低い共鳴。整数周期でループ継ぎ目をなくす。
		double value=.17*tone(28,time)+.07*tone(56,time)+.045*tone(84,time)+.025*tone(112,time);
		value*=1+.07*tone(7,time);
		wave[i]=static_cast<float>(value);
	}
	return wave;
}
Wave SoundEffects::makeRoadNoise()
{
	Wave wave{size_t(kSampleRate),Arg::sampleRate=kSampleRate};
	for (size_t i=0;i<wave.size();++i)
	{
		const double time=static_cast<double>(i)/kSampleRate;
		double value=0;
		for (int band=0;band<48;++band)
		{
			const int frequency=173+band*67+(band*band*19)%53;
			value+=Sin(kTwoPi*frequency*time+band*2.399963)/Sqrt(1.0+band);
		}
		wave[i]=static_cast<float>(value*.022);
	}
	return wave;
}
Wave SoundEffects::makeCue(Cue cue)
{
	const std::array<double,static_cast<size_t>(Cue::Count)> lengths{.08,.28,.24,.42,.65,.32,.22,.23};
	const double length=lengths[static_cast<size_t>(cue)];
	Wave wave{Duration{length},Arg::sampleRate=kSampleRate};
	uint32 random=0x54b39821;
	double noise=0;
	for (size_t i=0;i<wave.size();++i)
	{
		const double time=static_cast<double>(i)/kSampleRate;
		random=random*1664525u+1013904223u;
		noise+=.15*((static_cast<double>(random)/4294967295.0*2-1)-noise);
		double value=0;
		switch (cue)
		{
		case Cue::Select: value=.22*tone(720,time)*Exp(-time*42)+.06*noise;break;
		case Cue::Confirm:
			value=.19*tone(740,time)*Exp(-time*16);
			if (time>.09) { value+=.18*tone(988,time-.09)*Exp(-(time-.09)*15); }
			break;
		case Cue::Reject: value=.14*tone(time<.12 ? 260 : 220,time)*Exp(-time*7);break;
		case Cue::Construction: value=(.27*tone(120,time)+.28*noise)*Exp(-time*13)+.1*tone(440,time)*Exp(-time*8);break;
		case Cue::Complete:
			for (int note=0;note<3;++note)
			{
				const double local=time-note*.12;
				if (local>=0) { value+=.16*tone(660+note*165,local)*Min(1.0,local/.008)*Exp(-local*9); }
			}
			break;
		case Cue::EnterCar: value=(.18*tone(75+time*50,time)+.2*noise)*Exp(-time*13);break;
		case Cue::ExitCar: value=(.24*tone(95,time)+.24*noise)*Exp(-time*21);break;
		case Cue::Contact: value=(.38*tone(68,time)+.32*noise)*Exp(-time*22);break;
		default: break;
		}
		wave[i]=static_cast<float>(value*envelope(time,length));
	}
	return wave;
}

SoundEffects::SoundEffects()
	: m_engine{makeEngine(),Loop::Yes},m_road{makeRoadNoise(),Loop::Yes}
{
	for (size_t index=0;index<m_cues.size();++index) { m_cues[index]=Audio{makeCue(static_cast<Cue>(index))}; }
}
SoundEffects::~SoundEffects()
{
	stopDriving();
	for (const auto& cue : m_cues) { cue.stopAllShots(); }
}
void SoundEffects::setVolume(double volume)
{
	volume=Clamp(volume,0.0,1.0);
	if (m_volume==volume) { return; }
	m_volume=volume;
	if (m_volume==0)
	{
		stopDriving();
		for (const auto& cue : m_cues) { cue.stopAllShots(); }
	}
}
void SoundEffects::play(Cue cue,double intensity)
{
	if (m_volume==0 || cue==Cue::Count) { return; }
	m_cues[static_cast<size_t>(cue)].playOneShot(MixBus1,m_volume*Clamp(intensity,0.0,1.0));
	++m_playedCount;
}
SoundEffects::DrivingMix SoundEffects::drivingMix(double speed,double throttle)
{
	const double velocity=Clamp(Abs(speed),0.0,34.0),load=Clamp(throttle,0.0,1.0);
	// 低速の回転上昇を強め、高速では巡航音に落ち着く。
	return {1+Min(velocity,10.0)*.105+Max(velocity-10,0.0)*.012+load*.3,
		.28+load*.22+Min(velocity/30,.15),Min(velocity/25,1.0)*.8};
}
void SoundEffects::stopDriving()
{
	if (!m_running) { return; }
	m_engine.stop();m_road.stop();m_running=false;m_mix={};
	m_previousSpeed=0;m_wasBlocked=false;
}
void SoundEffects::updateDriving(double dt,bool active,double speed,double throttle,bool blocked)
{
	dt=Clamp(dt,0.0,.1);
	m_contactCooldown=Max(0.0,m_contactCooldown-dt);
	if (!active || m_volume==0) { stopDriving();return; }
	if (blocked && !m_wasBlocked && m_previousSpeed>1.2 && m_contactCooldown==0)
	{
		play(Cue::Contact,Clamp(m_previousSpeed/8,.25,1.0));m_contactCooldown=.5;
	}
	m_previousSpeed=Abs(speed);m_wasBlocked=blocked;
	const auto target=drivingMix(speed,throttle);
	const double blend=1-Exp(-dt*7);
	m_mix.pitch+=(target.pitch-m_mix.pitch)*blend;
	m_mix.engine+=(target.engine-m_mix.engine)*blend;
	m_mix.road+=(target.road-m_mix.road)*blend;
	m_engine.setSpeed(m_mix.pitch).setVolume(m_volume*m_mix.engine);
	m_road.setSpeed(1+Min(Abs(speed)/80,.4)).setVolume(m_volume*m_mix.road);
	if (!m_running) { m_engine.play(MixBus1);m_road.play(MixBus1);m_running=true; }
}

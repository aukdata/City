#include "TestCases.hpp"
#include "TestRunner.hpp"
#include "src/audio/SoundEffects.hpp"

void registerSoundEffectsTests(TestRunner& runner)
{
	runner.add(U"Sound.WaveIntegrity",[](TestContext& context)
	{
		Array<Wave> waves{SoundEffects::makeEngine(),SoundEffects::makeRoadNoise()};
		for (int cue=0;cue<static_cast<int>(SoundEffects::Cue::Count);++cue) { waves << SoundEffects::makeCue(static_cast<SoundEffects::Cue>(cue)); }
		FileSystem::CreateDirectories(U"TestResults/audio");
		JSON report;
		for (size_t index=0;index<waves.size();++index)
		{
			const Wave& wave=waves[index];double peak=0,squares=0,mean=0;
			for (const auto& sample : wave)
			{
				peak=Max(peak,Abs(static_cast<double>(sample.left)));squares+=sample.left*sample.left;mean+=sample.left;
			}
			const double rms=Sqrt(squares/wave.size());
			context.expect(peak<.85 && rms>.005,U"Every generated sound is audible with ample clipping headroom");
			context.expect(Abs(mean/wave.size())<.005,U"Waveform has no significant DC offset");
			if (index<2) { context.expect(Abs(wave.front().left-wave.back().left)<.025,U"Loop boundaries have no large discontinuity"); }
			else { context.expect(Abs(wave.front().left)<.001 && Abs(wave.back().left)<.001,U"One shots begin and end silently"); }
			wave.saveWAVE(U"TestResults/audio/{:02}.wav"_fmt(index));
			report[index][U"peak"]=peak;report[index][U"rms"]=rms;
		}
		report.save(U"TestResults/audio/wave_metrics.json");
	});
	runner.add(U"Sound.PlaybackAndMute",[](TestContext& context)
	{
		SoundEffects sound;
		sound.setVolume(.6);
		double outputPeak=0;
		for (int frame=0;frame<30;++frame)
		{
			sound.updateDriving(1.0/60,true,frame*.4,.8,false);
			if (!System::Update()) { break; }
			for (const float sample : GlobalAudio::BusGetSamples(MixBus1)) { outputPeak=Max(outputPeak,Abs(static_cast<double>(sample))); }
		}
		context.expect(sound.drivingPlaying() && outputPeak>.001,U"The audio backend actually outputs the driving sound");
		const auto playing=sound.mix();
		context.expect(playing.pitch>1.2 && playing.road>0,U"Acceleration raises engine pitch and road noise");
		sound.updateDriving(1.0/60,true,0,0,true);
		const size_t count=sound.playedCount();
		for (int frame=0;frame<60;++frame) { sound.updateDriving(1.0/60,true,0,1,true); }
		context.expect(count==1 && sound.playedCount()==count,U"Contact makes one impact, not an alarm on every blocked frame");
		sound.setVolume(0);sound.play(SoundEffects::Cue::Complete);sound.updateDriving(.1,true,10,1,false);
		context.expect(!sound.drivingPlaying() && sound.playedCount()==count,U"Mute stops loops and prevents new event sounds");
		sound.setVolume(.6);sound.updateDriving(.1,true,10,1,false);sound.updateDriving(.1,false,10,1,false);
		context.expect(!sound.drivingPlaying(),U"Pause and leaving driving mode stop both loops");
		JSON report;report[U"outputPeak"]=outputPeak;report[U"pitch"]=playing.pitch;report[U"contactSounds"]=count;
		report.save(U"TestResults/audio/playback.json");
	});
}

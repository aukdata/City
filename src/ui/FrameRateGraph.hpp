#pragma once
#include <Siv3D.hpp>

/// @brief 直近30秒の実フレーム時間を0.1秒ごとに集計する。非表示中も履歴を保つ。
class FrameRateGraph
{
public:
	bool visible = false;
	/// @brief ゲーム用DeltaTimeの上限に影響されず、長いフレームも実時間で記録する。
	void sampleNow()
	{
		const double seconds=m_frameClock.msF()/1000;
		m_frameClock.restart();
		if (m_clockStarted) { sample(seconds); }
		m_clockStarted=true;
	}
	void sample(double seconds)
	{
		if (!std::isfinite(seconds) || seconds <= 0) { return; }
		m_time += seconds; m_elapsed += seconds; ++m_frames;
		if (m_elapsed + 1e-9 < kInterval) { return; }
		m_history[m_next] = m_frames / m_elapsed;
		m_times[m_next] = m_time;
		m_next = (m_next + 1) % kCapacity;
		m_count = Min(m_count + 1, kCapacity);
		m_elapsed = 0; m_frames = 0;
	}
	[[nodiscard]] size_t count() const { return m_count; }
	[[nodiscard]] double latest() const { return m_count ? m_history[(m_next + kCapacity - 1) % kCapacity] : 0; }
	[[nodiscard]] RectF bounds(Size size) const { return {16, Max(16, size.y - 180), Min(330, size.x - 32), 160}; }
	void draw(const Font& font, Size size) const
	{
		if (!visible) { return; }
		const auto panel = bounds(size);
		panel.draw(ColorF{.025,.04,.06,.92}).drawFrame(1,ColorF{.45,.5,.55,.8});
		font(U"FPS  {:.0f}"_fmt(latest())).draw(16,panel.pos+Vec2{12,8},Palette::White);
		font(U"F3").draw(12,panel.pos+Vec2{panel.w-32,11},ColorF{.7});
		const RectF plot{panel.x+38,panel.y+36,panel.w-50,100};
		double maximum = 120;
		for (size_t i=0;i<m_count;++i) { maximum=Max(maximum,m_history[i]); }
		maximum = Ceil(maximum/30)*30;
		for (const double fps : {0.0,30.0,60.0,maximum})
		{
			const double y=plot.br().y-plot.h*fps/maximum;
			Line{plot.x,y,plot.br().x,y}.draw(1,ColorF{1,fps==60 ? .28 : .1});
			font(U"{:.0f}"_fmt(fps)).draw(10,panel.x+5,y-6,ColorF{.72});
		}
		Array<Vec2> points;
		for (size_t i=0;i<m_count;++i)
		{
			const size_t index=(m_next+kCapacity-m_count+i)%kCapacity;
			const double age=m_time-m_times[index];if (age>30) { continue; }
			points << Vec2{plot.br().x-plot.w*age/30,plot.br().y-plot.h*m_history[index]/maximum};
		}
		for (size_t i=1;i<points.size();++i) { Line{points[i-1],points[i]}.draw(2,ColorF{.3,.95,.65}); }
		font(U"30秒前").draw(10,plot.x,panel.br().y-19,ColorF{.65});
		font(U"現在").draw(10,plot.br().x-22,panel.br().y-19,ColorF{.65});
	}
private:
	static constexpr size_t kCapacity = 300;
	static constexpr double kInterval = .1;
	std::array<double,kCapacity> m_history{}, m_times{};
	size_t m_next = 0, m_count = 0;
	double m_elapsed = 0, m_time = 0;
	int m_frames = 0;
	Stopwatch m_frameClock{StartImmediately::Yes};
	bool m_clockStarted = false;
};

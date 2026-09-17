#include "RoadPlanToolbar.hpp"
#include "ConstructionStatus.hpp"

namespace RoadPlanToolbar
{
	Action draw(const Font& font, const Font& bold, int width, const State& state)
	{
		Action action = Action::None;
		constexpr int kPad = 8;
		const int inner = width-kPad*2;
		const int half = (inner-8)/2;
		const auto button = [&](StringView label, Rect rect, Action value, bool enabled = true, bool selected = false)
		{
			const bool hover = enabled && rect.mouseOver();
			rect.draw(!enabled ? ColorF{0.10,0.12,0.15} : (selected ? ColorF{0.18,0.39,0.53} : (hover ? ColorF{0.24,0.29,0.35} : ColorF{0.16,0.20,0.25})));
			font(label).draw(rect.pos+Point{8,4}, enabled ? ColorF{0.93} : ColorF{0.42});
			if (hover && MouseL.down()) { action = value; }
		};
		bold(U"1  道路を選ぶ").draw(kPad,4,ColorF{0.68,0.86,0.96});
		const String labels[] = {U"生活道路",U"サブ幹線",U"幹線道路",U"一方通行 →"};
		const String details[] = {U"2車線・30 km/h",U"2車線・40 km/h",U"4車線・50 km/h",U"1車線・30 km/h"};
		for (int i = 0; i < 4; ++i)
		{
			const Rect rect{kPad+(i%2)*(half+8),30+(i/2)*56,half,50};
			button(labels[i],rect,static_cast<Action>(static_cast<int>(Action::Local)+i),true,state.preset == i);
			font(details[i]).draw(rect.pos+Point{8,27},ColorF{0.67,0.77,0.82});
		}
		font(U"幅員 {:.1f}m  /  一方通行は描く向きに進行"_fmt(state.width)).draw(kPad,143,ColorF{0.72});
		bold(U"2  始点・終点 → 経路生成").draw(kPad,168,ColorF{0.68,0.86,0.96});
		button(state.generated ? U"経路を再生成" : U"経路生成",{kPad,194,half,28},Action::Generate,state.points >= 2,true);
		button(state.snapping ? U"道路に接続: ON" : U"道路に接続: OFF",{kPad+half+8,194,half,28},Action::Snap,true,state.snapping);
		font(U"点・線をドラッグで調整").draw(kPad,234,ColorF{0.82});
		const int quarter = (half-6)/2;
		button(U"戻す",{kPad+half+8,230,quarter,28},Action::Undo,state.canUndo);
		button(U"やり直す",{kPad+half+8+quarter+6,230,quarter,28},Action::Redo,state.canRedo);
		font(U"右クリック / Ctrl+Z: 戻す   Ctrl+Y: やり直す").draw(kPad,264,ColorF{0.67});
		font(U"PgUp / PgDn: 高さ {:+.0f} m（地下も可）"_fmt(state.elevation)).draw(kPad,283,ColorF{0.67});
		bold(U"3  確定して建設").draw(kPad,308,ColorF{0.68,0.86,0.96});
		font(state.valid ? U"延長 {:.0f}m   概算 {:.2f}億円"_fmt(state.length,state.cost) : U"経路生成後に見積りを表示").draw(kPad,334,ColorF{0.88});
		if (state.valid)
		{
			font(U"開通まで {}（標準速度）"_fmt(ConstructionStatus::durationLabel(state.constructionSeconds))).draw(kPad,355,ColorF{0.75});
		}
		String status = state.message;
		if (status.isEmpty() && state.valid && state.cost > state.funds) { status = U"着工資金が不足しています"; }
		if (status.isEmpty())
		{
			status = state.points == 0 ? U"地面か既存の道路をクリックして開始" : (state.points == 1 ? U"次のクリックで終点を指定" : U"経路生成 → 点や線をドラッグで調整");
		}
		while (font(status).region().w > inner && !status.isEmpty()) { status.pop_back(); }
		font(status).draw(kPad,377,state.error ? ColorF{1.0,0.50,0.40} : ColorF{0.66,0.84,0.70});
		button(U"確定して着工 [Enter]",{kPad,400,half,26},Action::Construct,state.valid && state.cost <= state.funds,true);
		button(U"破棄  [Esc]",{kPad+half+8,400,half,26},Action::Clear,state.points > 0);
		return action;
	}
}

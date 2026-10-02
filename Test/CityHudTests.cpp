#include "TestCases.hpp"
#include "TestRunner.hpp"
#include "src/ui/CityHud.hpp"
#include "src/render/UIRenderer.hpp"
#include "src/asset/AssetRegistrar.hpp"
#include "src/ui/NavigationHeader.hpp"
#include "src/ui/NavigationHelp.hpp"
#include "src/ui/PanelManager.hpp"
#include "src/ui/RoadPlanToolbar.hpp"
#include "src/ui/LocationTooltip.hpp"

void registerCityHudTests(TestRunner& runner)
{
	runner.add(U"CityHud.CompactLayoutAndInteraction", [](TestContext& context)
	{
		CityHud hud;
		hud.updateLayout({1280,768});
		const auto originalMap = hud.minimapBounds();
		context.expect(originalMap && originalMap->y == 44, U"周辺地図は通知の件数と独立した固定位置にある");
		context.expect(hud.activeTab() == CityHud::Tab::None && !hud.helpVisible(), U"詳細と操作説明は最初から展開しない");
		double area = 0;
		for (const auto& rect : hud.bounds()) { area += rect.area(); }
		context.expect(area < 85000, U"通常のHUD専有面積を従来の半分以下へ抑える");
		for (const auto tab : {CityHud::Tab::City,CityHud::Tab::Traffic,CityHud::Tab::Notices})
		{
			hud.interact(hud.tabBounds(tab).center(),true);
			context.expect(hud.activeTab() == tab && hud.minimapBounds() == originalMap, U"詳細タブは一つずつ開き、地図を移動させない");
		}
		hud.interact(hud.tabBounds(CityHud::Tab::Notices).center(),true);
		context.expect(hud.activeTab() == CityHud::Tab::None, U"同じタブを押すと詳細を閉じる");
		hud.interact(hud.helpButtonBounds().center(),true);
		context.expect(hud.helpVisible(), U"必要なときに操作説明を開ける");
		hud.interact(hud.tabBounds(CityHud::Tab::Traffic).center(),true);
		context.expect(!hud.helpVisible(), U"詳細と操作説明を同時に積み重ねない");
		context.expect(hud.interact(hud.pauseBounds().center(),true) == CityHud::Action::TogglePause, U"時間操作はゲームへ明確な操作要求として返す");
		context.expect(hud.interact(hud.speedBounds().center(),true) == CityHud::Action::NextSpeed, U"速度操作をクリックで選べる");
		hud.interact(hud.mapToggleBounds().center(),true);
		context.expect(!hud.minimapBounds() && hud.blocksMouse({1000,400}), U"地図を閉じたクリックを同じフレームの地面へ流さない");
		hud.interact({1000,400},false);
		context.expect(!hud.blocksMouse({1000,400}), U"翌フレームには閉じた場所を操作できる");
		hud.interact(hud.mapToggleBounds().center(),true,false);
		context.expect(!hud.minimapBounds(), U"ポーズ・地図・文字入力で保護されている間は背後のHUDを操作しない");
	});

	runner.add(U"CityHud.ProductionAdapter", [](TestContext& context)
	{
		UIRenderer renderer; CityHud model;
		for (int view = 0; view < 3; ++view)
		{
			const bool walking = view == 1, driving = view == 2;
			renderer.updateLayout(walking,driving); model.updateLayout(Scene::Size(),walking,driving);
			context.expect(renderer.panelBounds() == model.bounds() && renderer.minimapBounds() == model.minimapBounds(),
				U"本体のUIアダプタがTestで検証した領域をそのまま使う");
		}
	});

	runner.add(U"CityHud.NavigationHelpEditorInteraction", [](TestContext& context)
	{
		for (const Size size : {Size{800,600}, Size{1280,768}, Size{1920,1080}})
		{
			for (const bool walking : {false, true})
			{
				CityHud hud;
				hud.updateLayout(size, walking, false, true);
				const auto help = NavigationHelp::bounds(size);
				const RectF editor{size.x - 384.0, 10, 374, size.y - 20.0};
				context.expect(!hud.helpButtonBounds().intersects(editor), U"道路計画を開いてもヘルプボタンを押せる");
				hud.interact(hud.helpButtonBounds().center(), true);
				context.expect(hud.helpVisible(), U"俯瞰・徒歩から操作説明を開ける");
				hud.interact(help.center(), false);
				context.expect(hud.blocksMouse(help.center()), U"新しい位置の説明上では道路を配置しない");
				const auto areas = hud.bounds();
				for (size_t first = 0; first < areas.size(); ++first)
				{
					for (size_t second = first + 1; second < areas.size(); ++second)
					{
						context.expect(!areas[first].intersects(areas[second]), U"操作説明・ボタン・HUDが相互に重ならない");
					}
				}
				hud.interact(hud.helpButtonBounds().center(), true);
				context.expect(!hud.helpVisible() && hud.blocksMouse(help.center()), U"閉じるクリックは地面へ流れない");
				hud.interact(help.center(), false);
				context.expect(!hud.blocksMouse(help.center()), U"翌フレームから閉じた説明の位置を操作できる");
				if (!walking)
				{
					hud.interact(hud.tabBounds(CityHud::Tab::City).center(), true);
					const auto detailAreas = hud.bounds();
					context.expect(!detailAreas.includes(hud.helpButtonBounds()), U"詳細タブの上にヘルプボタンを重ねない");
					hud.interact(hud.helpButtonBounds().center(), true);
					context.expect(!hud.helpVisible(), U"非表示のヘルプボタンは入力を受けない");
					hud.interact(hud.tabBounds(CityHud::Tab::City).center(), true);
					hud.interact(hud.helpButtonBounds().center(), true);
					context.expect(hud.helpVisible(), U"詳細を閉じるとヘルプボタンが復帰する");
				}
				hud.updateLayout(size, false, true, true);
				context.expect(!hud.helpVisible(), U"運転へ移ると既存どおり操作説明を閉じる");
			}
		}
	});

	runner.add(U"CityHud.NavigationHelpEditorPreview", [](TestContext& context)
	{
		RegisterAssets();
		const auto font = FontAsset(Asset::Small16);
		font.preload(NavigationHelp::kOverviewText + NavigationHelp::kWalkingText);
		for (int frame = 0; frame < 3; ++frame) { System::Update(); }
		JSON report;
		for (const Size size : {Size{800,600}, Size{1280,768}, Size{1920,1080}})
		{
			PanelManager panels;
			const Vec2 editorSize{374, size.y - 20.0};
			const Vec2 editorPos{size.x - editorSize.x - 10, 10};
			const RectF editor{editorPos, editorSize};
			panels.registerPanel(U"road_plan", editorSize, true, true);
			panels.show(U"road_plan", U"道路計画  [R]", editorPos);
			const auto help = NavigationHelp::bounds(size);
			const RectF button{help.x, help.y - 34, 84, 28};
			const LocationTooltip::Content location{U"山手町", U"やまてちょう", U"山手通り", 1};
			context.expect(!help.intersects(editor) && !button.intersects(editor),
				U"操作説明と開閉ボタンは最小ウィンドウでも道路計画を避ける");
			context.expect(!help.intersects(LocationTooltip::bounds(size, location)),
				U"操作説明は下部の住所・道路表示を遮らない");
			context.expect(help.x >= 0 && help.y >= 0 && help.br().x <= size.x && help.br().y <= size.y,
				U"操作説明全体が画面内に収まる");
			for (const bool walking : {false, true})
			{
				CityHud hud;
				hud.updateLayout(size, walking, false, true);
				hud.interact(hud.helpButtonBounds().center(), true);
				const Color background{62,81,52};
				const RenderTexture target{size, TextureFormat::R8G8B8A8_Unorm};
				const auto render = [&](bool editorVisible)
				{
					{
						const ScopedRenderTarget2D rt{target.clear(background)};
						NavigationHelp::draw(font, size, walking);
						if (editorVisible)
						{
							panels.drawBackground(U"road_plan");
							if (const auto content = panels.beginContent(U"road_plan"))
							{
								RoadPlanToolbar::draw(FontAsset(Asset::Panel14), FontAsset(Asset::PanelBold14), 364, {});
							}
						}
					}
					Graphics2D::Flush();
					Image image;
					target.readAsImage(image);
					return image;
				};
				const Image before = render(false), after = render(true);
				int coveredPixels = 0, textPixels = 0, outsidePixels = 0;
				for (int y = 0; y < size.y; ++y)
				{
					for (int x = 0; x < size.x; ++x)
					{
						const bool inside = help.stretched(2).contains(Point{x,y});
						coveredPixels += inside && before[y][x] != after[y][x];
						textPixels += inside && after[y][x].r > 180;
						outsidePixels += !inside && before[y][x] != background;
					}
				}
				context.expectEqual(coveredPixels, 0, U"後から描く道路計画が操作説明の画素を隠さない");
				context.expectEqual(outsidePixels, 0, U"狭い画面でも説明文が背景からはみ出さない");
				context.expect(textPixels > 200, U"俯瞰・徒歩の操作説明が読み取れる画素で描かれる");
				const String key = U"{}_{}"_fmt(size.x, walking ? U"walking" : U"overview");
				after.save(U"Screenshot/navigation_help_editor_{}.png"_fmt(key));
				{
					const ScopedRenderTarget2D rt{target.clear(background)};
					hud.draw(GameClock{}, 0, U"道路計画 · 右側のパネルで経路を編集", Economy{}, CityHudStats{}, font);
					panels.drawBackground(U"road_plan");
					if (const auto content = panels.beginContent(U"road_plan"))
					{
						RoadPlanToolbar::draw(FontAsset(Asset::Panel14), FontAsset(Asset::PanelBold14), 364, {});
					}
				}
				Graphics2D::Flush();
				Image integrated;
				target.readAsImage(integrated);
				integrated.save(U"Screenshot/navigation_help_hud_{}.png"_fmt(key));
				report[key] = JSON{{U"coveredPixels", coveredPixels}, {U"outsidePixels", outsidePixels}, {U"textPixels", textPixels}};
			}
		}
		report.save(U"TestResults/navigation_help_editor.json");
	});

	runner.add(U"CityHud.GpuReview", [](TestContext& context)
	{
		RegisterAssets();
		const auto font = FontAsset(Asset::Small16);
		font.preload(U"街の情報交通通知停止再開操作速度人口資金周辺地図混雑平均最大住宅充足率月次収支幸福度道路工事中お祭り同じタブを押すと閉じます0123456789×億円");
		for (int frame = 0; frame < 3; ++frame) { System::Update(); }
		CityHudStats stats; GameClock clock; Economy economy;
		economy.population = 999999999; economy.funds = 123456789;
		stats.housingCapacity = 888888888; stats.averageSpeedKmh = 35; stats.maxCongestion = .81;
		stats.activeEventSummaries = {U"道路工事中：山間部から市街地に向かう道路の長い説明です。道路工事中：山間部から市街地に向かう道路の長い説明です。"};
		stats.notificationSummaries = {U"まちの開発が進んでいます",U"駅前の道路が混雑しています"};
		JSON report; int index = 0;
		for (const Size size : {Size{800,600},Size{1280,768},Size{1920,1080}})
		{
			for (int variant = 0; variant < 9; ++variant)
			{
				const bool walking = variant == 5 || variant == 8, driving = variant == 6;
				CityHud hud; hud.updateLayout(size,walking,driving,variant >= 7);
				if (variant >= 1 && variant <= 3) { hud.interact(hud.tabBounds(static_cast<CityHud::Tab>(variant)).center(),true); }
				if (variant == 4) { hud.interact(hud.helpButtonBounds().center(),true); }
				const auto areas = hud.bounds();
				for (const auto& rect : areas)
				{
					context.expect(rect.x >= 0 && rect.y >= 0 && rect.br().x <= size.x && rect.br().y <= size.y,
						U"HUDの全領域がウィンドウ内に収まる");
				}
				if (walking || driving)
				{
					context.expect(!hud.summaryBounds().intersects(NavigationHeader::placeBounds(size)), U"徒歩・運転では上部の地名表示を遮らない");
				}
				const Color background{62,81,52};
				const RenderTexture target{size,TextureFormat::R8G8B8A8_Unorm};
				{
					const ScopedRenderTarget2D rt{target.clear(background)};
					hud.draw(clock,1234,variant == 8 ? U"近くに走れる道路がありません" : variant == 7 ? U"道路計画 · 右側のパネルで経路を編集" : U"",economy,stats,font);
				}
				Graphics2D::Flush(); Image image; target.readAsImage(image);
				int outside = 0, changed = 0, bright = 0;
				for (int y = 0; y < size.y; ++y)
				{
					for (int x = 0; x < size.x; ++x)
					{
						const auto pixel = image[y][x];
						if (Abs(static_cast<int>(pixel.r)-background.r)+Abs(static_cast<int>(pixel.g)-background.g)+Abs(static_cast<int>(pixel.b)-background.b) < 8) { continue; }
						++changed; bright += pixel.r > 160 && pixel.g > 160 && pixel.b > 160;
						bool contained = false;
						for (const auto& rect : areas) { contained |= rect.stretched(2).contains(Point{x,y}); }
						outside += !contained;
					}
				}
				context.expect(outside == 0, U"文字・背景・長い通知がHUD領域の外へはみ出さない: {}×{} variant={} outside={}"_fmt(size.x,size.y,variant,outside));
				context.expect(changed > 1000 && bright > 100, U"背景だけでなく読み取り可能な文字がGPUに描画される");
				image.save(U"Screenshot/city_hud_{}_{}.png"_fmt(size.x,variant));
				report[U"case_{}"_fmt(index++)] = JSON{{U"width",size.x},{U"variant",variant},{U"outsidePixels",outside},{U"drawnPixels",changed},{U"textPixels",bright}};
			}
		}
		report.save(U"TestResults/city_hud_review.json");
	});
}

#pragma once
#include "TestRunner.hpp"
#include "src/ui/RoadInspectorPanels.hpp"
#include "src/ui/PanelWidget.hpp"

/// @brief Exercise the production road-inspector visibility and input boundary before scene integration.
inline void registerRoadInspectorPanelTests(TestRunner& runner)
{
	runner.add(U"RoadInspectorPanels.ExclusiveSwitchAndInput", [](TestContext& context)
	{
		RegisterAssets();
		PanelManager panels;
		RoadInspectorPanels::registerPanels(panels, 768);
		panels.registerPanel(U"unrelated", {160, 100}, true);
		panels.show(U"unrelated", U"Other", {10, 10});
		constexpr Vec2 kEdgePos{896, 10}, kRoutePos{958, 10};
		panels.show(U"edge_info", U"道路エッジ #616", kEdgePos);
		context.expect(panels.isVisible(U"edge_info") && !panels.isVisible(U"route_info"),
			U"An edge opens without a stale route inspector");
		panels.handleInput(kEdgePos + Vec2{80, 180}, true, true, 0);
		panels.show(U"route_info", U"路線 #4", kRoutePos);
		context.expect(!panels.isVisible(U"edge_info") && panels.isVisible(U"route_info"),
			U"Opening the selected edge's route hides the edge inspector");
		context.expect(panels.isVisible(U"unrelated") && panels.sortedPanelIds().size() == 2,
			U"Exclusive road inspectors do not hide an unrelated panel");
		context.expect(panels.consumedInput(), U"The transition keeps ownership of its original click");
		panels.handleInput(kRoutePos + Vec2{90, 90}, true, true, 1);
		int hiddenControls = 0;
		if (const auto area = panels.beginContent(U"edge_info")) { ++hiddenControls; }
		context.expectEqual(hiddenControls, 0, U"Covered edge controls cannot run, draw, or receive the route click");
		context.expect(!panels.isMouseOnAnyPanel(kEdgePos + Vec2{20, 200}),
			U"The former edge-only strip no longer intercepts world input");

		panels.show(U"edge_info", U"道路エッジ #617", kEdgePos);
		context.expect(panels.isVisible(U"edge_info") && !panels.isVisible(U"route_info"),
			U"Choosing a member edge hides its route inspector");
		context.expect(!panels.beginContent(U"route_info"), U"The hidden route field cannot consume text input");
		panels.show(U"route_info", U"路線 #4", kRoutePos);
		context.expect(!panels.isVisible(U"edge_info") && panels.isVisible(U"route_info"),
			U"Reopening the route does not accumulate inspectors");
		panels.handleInput(kRoutePos + Vec2{300, 12}, true, true, 0);
		context.expect(!panels.isVisible(U"route_info") && !panels.isVisible(U"edge_info")
			&& panels.consumedInput(), U"Closing the route consumes the click without reviving the previous edge");
		context.expect(!panels.handleInput(kRoutePos + Vec2{300, 12}, false, true, 0),
			U"The following frame no longer belongs to the dismissed inspector");

		panels.show(U"edge_info", U"道路エッジ #616", kEdgePos);
		panels.handleInput(kEdgePos + Vec2{80, 12}, true, true, 0);
		panels.show(U"route_info", U"路線 #4", kRoutePos);
		context.expect(!panels.handleInput({500, 500}, false, true, 0),
			U"A replaced inspector cannot keep a hidden title-bar drag alive");
	});

	runner.add(U"RoadInspectorPanels.HiddenRouteNameFocus", [](TestContext& context)
	{
		RegisterAssets();
		GameInput::buffer = KeyboardActionBuffer{};
		GameInput::textInput = nullptr;
		GameInput::textOwnedFrame = false;
		PanelManager panels;
		RoadInspectorPanels::registerPanels(panels, 768);
		TextEditState routeName, unrelatedName;
		routeName.text = U"国道224号";
		panels.show(U"route_info", U"路線 #4", {958, 10});
		routeName.active = true;
		GameInput::textInput = &routeName;
		RoadInspectorPanels::releaseHiddenRouteNameFocus(panels, routeName);
		context.expect(routeName.active && GameInput::textInput == &routeName,
			U"A visible route keeps its existing text focus");
		panels.show(U"edge_info", U"道路エッジ #616", {896, 10});
		RoadInspectorPanels::releaseHiddenRouteNameFocus(panels, routeName);
		context.expect(!routeName.active && GameInput::textInput == nullptr,
			U"A member-edge transition releases only the now-hidden route name");
		GameInput::buffer.update({{0, 0, KeyR.code(), true, false}}, true);
		context.expect(GameInput::keyboardBlocked() && !GameInput::down(KeyR),
			U"Text keys cannot replay as game shortcuts in the closing frame");
		GameInput::textOwnedFrame = false;
		GameInput::buffer.update({{0, 1, KeyR.code(), true, false}}, true);
		context.expect(GameInput::down(KeyR), U"A fresh later shortcut is no longer blocked by an invisible field");

		panels.show(U"route_info", U"路線 #4", {958, 10});
		routeName.active = true;
		GameInput::textInput = &routeName;
		panels.handleInput({1258, 22}, true, true, 0);
		RoadInspectorPanels::releaseHiddenRouteNameFocus(panels, routeName);
		context.expect(!panels.isVisible(U"route_info") && !routeName.active && GameInput::textInput == nullptr,
			U"The actual close-button path also releases the hidden route name");
		context.expect(panels.consumedInput() && GameInput::keyboardBlocked(),
			U"Closing a focused route retains mouse and keyboard ownership for that frame");
		unrelatedName.active = true;
		GameInput::textInput = &unrelatedName;
		RoadInspectorPanels::releaseHiddenRouteNameFocus(panels, routeName);
		context.expect(unrelatedName.active && GameInput::textInput == &unrelatedName,
			U"Dismissing a route never steals an unrelated field's focus");
		context.expect(routeName.text == U"国道224号", U"Inspector dismissal does not alter the edited route name");
		GameInput::releaseTextFocus();
		GameInput::textOwnedFrame = false;
		GameInput::buffer = KeyboardActionBuffer{};
	});

	runner.add(U"RoadInspectorPanels.ExclusivePreview", [](TestContext& context)
	{
		RegisterAssets();
		const Font font = FontAsset(Asset::Panel14);
		const Font titleFont = FontAsset(Asset::PanelBold14);
		font.preload(U"国道224号edge content sentinel");
		titleFont.preload(U"道路エッジ #616路線 #4");
		for (int frame = 0; frame < 3; ++frame) { System::Update(); }
		JSON report;
		for (const Size size : {Size{800, 600}, Size{1280, 768}, Size{1920, 1080}})
		{
			PanelManager panels;
			RoadInspectorPanels::registerPanels(panels, size.y);
			constexpr double kMargin = 10;
			const Vec2 edgePos{size.x - panels.getSize(U"edge_info").x - kMargin, kMargin};
			const Vec2 routePos{size.x - panels.getSize(U"route_info").x - kMargin, kMargin};
			const RectF routeBounds{routePos, panels.getSize(U"route_info")};
			const Color background{62, 81, 52};
			const RenderTexture target{size, TextureFormat::R8G8B8A8_Unorm};
			TextEditState routeName;
			routeName.text = U"国道224号";
			// Diagnostic edge pixels expose translucent overlap; this is not a copied scene layout.
			const auto render = [&](bool omitEdge)
			{
				{
					const ScopedRenderTarget2D rt{target.clear(background)};
					for (const auto& id : panels.sortedPanelIds())
					{
						if (omitEdge && id == U"edge_info") { continue; }
						panels.drawBackground(id);
						if (const auto area = panels.beginContent(id))
						{
							if (id == U"edge_info")
							{
								RectF{6, 6, 350, 64}.draw(Palette::Magenta);
								font(U"edge content sentinel").draw(Vec2{6, 80}, Palette::White);
							}
							else
							{
								PanelWidget::textInput(font, routeName, 6, 6, 240, 20, 64);
							}
						}
					}
				}
				Graphics2D::Flush();
				Image image;
				target.readAsImage(image);
				return image;
			};
			panels.show(U"edge_info", U"道路エッジ #616", edgePos);
			panels.show(U"route_info", U"路線 #4", routePos);
			const Image expected = render(true), actual = render(false);
			int stalePixels = 0, outsidePixels = 0, textPixels = 0;
			for (int y = 0; y < size.y; ++y)
			{
				for (int x = 0; x < size.x; ++x)
				{
					stalePixels += actual[y][x] != expected[y][x];
					outsidePixels += !routeBounds.contains(Point{x, y}) && actual[y][x] != background;
					textPixels += routeBounds.contains(Point{x, y}) && actual[y][x].r > 170;
				}
			}
			context.expectEqual(stalePixels, 0, U"Hidden edge content contributes no pixels through the route inspector");
			context.expectEqual(outsidePixels, 0, U"The previous wider edge inspector leaves no stray panel or controls");
			context.expect(textPixels > 50, U"The production title and editable route name remain readable");
			actual.save(U"Screenshot/road_inspector_route_{}.png"_fmt(size.x));
			panels.show(U"edge_info", U"道路エッジ #616", edgePos);
			render(false).save(U"Screenshot/road_inspector_edge_{}.png"_fmt(size.x));
			report[U"{}"_fmt(size.x)] = JSON{{U"stalePixels", stalePixels}, {U"outsidePixels", outsidePixels}, {U"textPixels", textPixels}};
		}
		report.save(U"TestResults/road_inspector_panels.json");
	});
}

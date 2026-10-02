#pragma once
#include <Siv3D.hpp>
#include "PlainLabel.hpp"

/// @brief 通常プレイとポーズ中に保存結果を一件だけ表示する。ゲーム時刻には依存しない。
class SaveStatusNotice
{
public:
	struct Layout
	{
		RectF bounds;
		Array<String> lines;
		bool truncated = false;
	};
	static constexpr double kSuccessSeconds = 5.0;
	static constexpr double kErrorSeconds = 18.0;
	static constexpr double kBodySize = 14.0;
	static constexpr double kLineHeight = 18.0;

	/// @brief 新しい結果で置き換える。連続操作で通知が積み重なることはない。
	void show(StringView title, StringView message, bool success, double nowSeconds = currentSeconds())
	{
		m_title = title;
		m_message = message;
		m_success = success;
		m_expiresAt = nowSeconds + (success ? kSuccessSeconds : kErrorSeconds);
	}
	[[nodiscard]] bool visible(double nowSeconds = currentSeconds()) const
	{
		return !m_title.isEmpty() && nowSeconds < m_expiresAt;
	}
	[[nodiscard]] bool succeeded() const { return m_success; }
	[[nodiscard]] const String& title() const { return m_title; }
	[[nodiscard]] const String& message() const { return m_message; }

	/// @brief 通常時は編集パネル・住所表示を避け、ポーズ時はメニューのボタンより下へ置く。
	[[nodiscard]] Layout layout(const Font& font, Size size, bool paused = false) const
	{
		constexpr double kMargin = 12, kWidth = 390, kPauseWidth = 740;
		constexpr double kTextInset = 12, kTitleArea = 36, kLocationInset = 62;
		constexpr size_t kMaxLines = 6, kPauseMaxLines = 4;
		const double width = Max(0.0, Min(paused ? kPauseWidth : kWidth, size.x - kMargin * 2));
		const double textWidth = Max(0.0, width - kTextInset * 2);
		const size_t maxLines = paused ? kPauseMaxLines : kMaxLines;
		Layout result;
		String line;
		for (size_t index = 0; index < m_message.size(); ++index)
		{
			const char32 character = m_message[index];
			if (character == U'\r') { continue; }
			const String candidate = line + character;
			if (character == U'\n' || (!line.isEmpty() && font(candidate).region(kBodySize).w > textWidth))
			{
				result.lines << line;
				line.clear();
				if (result.lines.size() == maxLines)
				{
					result.truncated = index < m_message.size() - 1 || character != U'\n';
					break;
				}
				if (character == U'\n') { continue; }
			}
			line += character;
		}
		if (!line.isEmpty() && result.lines.size() < maxLines) { result.lines << line; }
		if (result.truncated)
		{
			auto& last = result.lines.back();
			while (!last.isEmpty() && font(last + U"…").region(kBodySize).w > textWidth) { last.pop_back(); }
			last += U"…";
		}
		const double height = kTitleArea + result.lines.size() * kLineHeight;
		const double left = paused ? (size.x - width) * .5 : kMargin;
		const double bottomInset = paused ? kMargin : kLocationInset;
		result.bounds = {left, Max(kMargin, size.y - bottomInset - height), width, height};
		return result;
	}

	/// @brief 不透明に近い背景と固定14pxの折返しで、失敗理由と対処方法を読める時間だけ残す。
	void draw(const Font& font, Size size, bool paused = false, double nowSeconds = currentSeconds()) const
	{
		if (!visible(nowSeconds)) { return; }
		const auto content = layout(font, size, paused);
		const auto area = content.bounds;
		const ColorF accent = m_success ? ColorF{.48, .92, .65} : ColorF{1.0, .69, .42};
		area.rounded(5).draw(ColorF{.055, .075, .09, .97}).drawFrame(1, accent);
		PlainLabel::fitted(font, m_title, 16, {area.x + 12, area.y + 7, area.w - 24, 22}, accent, ColorF{0, .3});
		double y = area.y + 30;
		for (const auto& line : content.lines)
		{
			font(line).draw(kBodySize, area.x + 12, y, ColorF{.94, .96, .97});
			y += kLineHeight;
		}
	}

private:
	static double currentSeconds() { return Time::GetMillisec() / 1000.0; }
	String m_title;
	String m_message;
	bool m_success = true;
	double m_expiresAt = 0;
};

#pragma once
#include <Siv3D.hpp>
#include <functional>

/// @brief 1テスト内の検証結果を収集するコンテキスト
class TestContext
{
public:
	void expect(bool condition, StringView message);
	void expectEqual(int64 actual, int64 expected, StringView message);
	void expectNear(double actual, double expected, double tolerance, StringView message);
	[[nodiscard]] const Array<String>& failures() const;

private:
	Array<String> m_failures;
};

/// @brief Testプロジェクトの常設テストランナー
class TestRunner
{
public:
	using TestFunction = std::function<void(TestContext& context)>;

	void add(StringView name, TestFunction testFunction);
	/// @return 全件成功なら0、それ以外は1
	int run(FilePathView outputDirectory = U"TestResults");

private:
	struct TestCase
	{
		String name;
		TestFunction function;
	};

	struct TestCaseResult
	{
		String name;
		bool passed = false;
		double durationMilliseconds = 0.0;
		Array<String> failures;
	};

	bool writeJson(FilePathView path, const Array<TestCaseResult>& results) const;
	bool writeXml(FilePathView path, const Array<TestCaseResult>& results) const;
	Array<TestCase> m_tests;
};

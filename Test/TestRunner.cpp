#include "TestRunner.hpp"
#include <chrono>
#include <exception>

namespace
{
	String escapeJson(StringView value)
	{
		String result;
		for (const char32 character : value)
		{
			switch (character)
			{
			case U'\\': result += U"\\\\"; break;
			case U'\"': result += U"\\\""; break;
			case U'\n': result += U"\\n"; break;
			case U'\r': result += U"\\r"; break;
			case U'\t': result += U"\\t"; break;
			default: result += character; break;
			}
		}
		return result;
	}

	String escapeXml(StringView value)
	{
		String result;
		for (const char32 character : value)
		{
			switch (character)
			{
			case U'&': result += U"&amp;"; break;
			case U'<': result += U"&lt;"; break;
			case U'>': result += U"&gt;"; break;
			case U'\"': result += U"&quot;"; break;
			default: result += character; break;
			}
		}
		return result;
	}
}

void TestContext::expect(bool condition, StringView message)
{
	if (!condition)
	{
		m_failures << String{ message };
	}
}

void TestContext::expectEqual(int64 actual, int64 expected, StringView message)
{
	if (actual != expected)
	{
		m_failures << U"{} (actual={}, expected={})"_fmt(message, actual, expected);
	}
}

void TestContext::expectNear(double actual, double expected, double tolerance, StringView message)
{
	if (Abs(actual - expected) > tolerance)
	{
		m_failures << U"{} (actual={}, expected={}, tolerance={})"_fmt(
			message, actual, expected, tolerance);
	}
}

const Array<String>& TestContext::failures() const
{
	return m_failures;
}

void TestRunner::add(StringView name, TestFunction testFunction)
{
	m_tests << TestCase{ String{ name }, std::move(testFunction) };
}

int TestRunner::run(FilePathView outputDirectory)
{
	Array<TestCaseResult> results;
	results.reserve(m_tests.size());
	int testFailedCount = 0;
	for (const TestCase& test : m_tests)
	{
		FileSystem::CreateDirectories(outputDirectory);
		{ TextWriter progress{FilePath{outputDirectory}+U"/running.txt"}; progress << test.name; }
		TestContext context;
		const auto startedAt = std::chrono::steady_clock::now();
		try
		{
			test.function(context);
		}
		catch (const std::exception& exception)
		{
			context.expect(false, U"Unhandled exception: {}"_fmt(Unicode::FromUTF8(exception.what())));
		}
		catch (...)
		{
			context.expect(false, U"Unhandled non-standard exception");
		}

		const auto endedAt = std::chrono::steady_clock::now();
		const double duration = std::chrono::duration<double, std::milli>(endedAt - startedAt).count();
		const bool passed = context.failures().isEmpty();
		results << TestCaseResult{ test.name, passed, duration, context.failures() };
		if (!passed)
		{
			++testFailedCount;
		}

		Logger << U"[{}] {} ({:.3f} ms)"_fmt(passed ? U"PASS" : U"FAIL", test.name, duration);
		for (const String& failure : context.failures())
		{
			Logger << U"  " << failure;
		}
	}

	FileSystem::CreateDirectories(outputDirectory);
	const FilePath directory{ outputDirectory };
	const bool jsonWritten = writeJson(directory + U"/results.json", results);
	const bool xmlWritten = writeXml(directory + U"/results.xml", results);
	const bool artifactsWritten = jsonWritten && xmlWritten;
	if (!artifactsWritten)
	{
		Logger << U"[FAIL] テスト結果ファイルを書き込めませんでした";
	}

	Logger << U"[SUMMARY] total={}, passed={}, failed={}"_fmt(
		results.size(), results.size() - testFailedCount, testFailedCount);
	return testFailedCount == 0 && artifactsWritten ? 0 : 1;
}

bool TestRunner::writeJson(FilePathView path, const Array<TestCaseResult>& results) const
{
	TextWriter writer{ path };
	if (!writer)
	{
		return false;
	}
	const size_t passedCount = results.count_if([](const TestCaseResult& result)
	{
		return result.passed;
	});
	writer.writeln(U"{");
	writer.writeln(U"  \"total\": {},"_fmt(results.size()));
	writer.writeln(U"  \"passed\": {},"_fmt(passedCount));
	writer.writeln(U"  \"failed\": {},"_fmt(results.size() - passedCount));
	writer.writeln(U"  \"tests\": [");
	for (size_t index = 0; index < results.size(); ++index)
	{
		const TestCaseResult& result = results[index];
		writer.writeln(U"    {");
		writer.writeln(U"      \"name\": \"{}\","_fmt(escapeJson(result.name)));
		writer.writeln(U"      \"status\": \"{}\","_fmt(result.passed ? U"passed" : U"failed"));
		writer.writeln(U"      \"durationMs\": {:.3f},"_fmt(result.durationMilliseconds));
		writer.writeln(U"      \"failures\": [");
		for (size_t failureIndex = 0; failureIndex < result.failures.size(); ++failureIndex)
		{
			const String comma = failureIndex + 1 < result.failures.size() ? U"," : U"";
			writer.writeln(U"        \"{}\"{}"_fmt(escapeJson(result.failures[failureIndex]), comma));
		}
		writer.writeln(U"      ]");
		writer.writeln(index + 1 < results.size() ? U"    }," : U"    }");
	}
	writer.writeln(U"  ]");
	writer.writeln(U"}");
	return true;
}

bool TestRunner::writeXml(FilePathView path, const Array<TestCaseResult>& results) const
{
	TextWriter writer{ path };
	if (!writer)
	{
		return false;
	}
	const size_t failedCount = results.count_if([](const TestCaseResult& result)
	{
		return !result.passed;
	});
	writer.writeln(U"<?xml version=\"1.0\" encoding=\"UTF-8\"?>");
	writer.writeln(U"<testsuite name=\"City.Test\" tests=\"{}\" failures=\"{}\">"_fmt(
		results.size(), failedCount));
	for (const TestCaseResult& result : results)
	{
		writer.writeln(U"  <testcase name=\"{}\" time=\"{:.6f}\">"_fmt(
			escapeXml(result.name), result.durationMilliseconds / 1000.0));
		for (const String& failure : result.failures)
		{
			writer.writeln(U"    <failure message=\"{}\"/>"_fmt(escapeXml(failure)));
		}
		writer.writeln(U"  </testcase>");
	}
	writer.writeln(U"</testsuite>");
	return true;
}

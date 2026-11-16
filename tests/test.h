#pragma once

// A minimal test runner: TEST registers a case, CHECK / CHECK_EQ record a
// failure and keep going. No framework to fetch.

#include <functional>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

namespace cluau_test {

	struct Case
	{
		const char* Name;
		std::function<void()> Fn;
	};

	inline std::vector<Case>& Cases()
	{
		static std::vector<Case> cases;
		return cases;
	}

	inline int& Failures()
	{
		static int failures = 0;
		return failures;
	}

	struct Register
	{
		Register(const char* name, std::function<void()> fn) { Cases().push_back({ name, std::move(fn) }); }
	};

	inline void Fail(const char* file, int line, const std::string& what)
	{
		Failures()++;
		std::cerr << file << ":" << line << ": FAILED: " << what << "\n";
	}

} // namespace cluau_test

#define CLUAU_CAT2(a, b) a##b
#define CLUAU_CAT(a, b) CLUAU_CAT2(a, b)
#define TEST(name)                                                                                  \
	static void CLUAU_CAT(test_fn_, __LINE__)();                                                    \
	static cluau_test::Register CLUAU_CAT(test_reg_, __LINE__)(name, CLUAU_CAT(test_fn_, __LINE__)); \
	static void CLUAU_CAT(test_fn_, __LINE__)()

#define CHECK(cond)                                           \
	do                                                        \
	{                                                         \
		if (!(cond))                                          \
			cluau_test::Fail(__FILE__, __LINE__, #cond);      \
	} while (0)

#define CHECK_EQ(a, b)                                                                     \
	do                                                                                     \
	{                                                                                      \
		const auto& va = (a);                                                              \
		const auto& vb = (b);                                                              \
		if (!(va == vb))                                                                   \
		{                                                                                  \
			std::ostringstream os;                                                         \
			os << #a << " == " << #b << "\n    got:      " << va << "\n    expected: " << vb; \
			cluau_test::Fail(__FILE__, __LINE__, os.str());                                \
		}                                                                                  \
	} while (0)

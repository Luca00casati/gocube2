#pragma once
// The tests' own small test framework (no third-party code): test cases and
// the checks they make.
//
//   TEST_CASE("what it tests") { CHECK(a == b); REQUIRE(ok); }
//
// CHECK records a failure and goes on; REQUIRE ends the test case. A failed
// comparison prints both sides (when they can be printed). Also CHECK_FALSE,
// REQUIRE_FALSE, REQUIRE_MESSAGE(cond, why), CHECK_THROWS(expr),
// CHECK_THROWS_AS(expr, type), FAIL(msg), MESSAGE(msg << ...), CAPTURE(var)
// (printed with any failure in its scope) and check::Approx(value).epsilon(e)
// (equal within a relative margin). A test case that throws fails.
//
// Running: every test case, in file and line order; `-tc=PATTERN` runs those
// whose name matches (* for anything; several, comma-separated), and
// `-tce=PATTERN` all but those. The slowest test cases are listed at the end.
// The exit code is 1 if anything failed.

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <functional>
#include <limits>
#include <ostream>
#include <sstream>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace check {

struct TestCase {
	const char *name;
	const char *file;
	int line;
	void (*fn)();
};

inline std::vector<TestCase> &test_cases() {
	static std::vector<TestCase> all;
	return all;
}

struct Register {
	Register(const char *name, const char *file, int line, void (*fn)()) { test_cases().push_back({name, file, line, fn}); }
};

// Thrown by a failed REQUIRE (or FAIL): ends the test case.
struct Abort {};

struct State {
	const TestCase *current = nullptr;
	long long checks = 0, failed_checks = 0;
	bool case_failed = false;
	std::vector<std::pair<const char *, std::function<std::string()>>> captures;
};

inline State &state() {
	static State s;
	return s;
}

// --- Printing values -----------------------------------------------------------------

template <class T, class = void>
struct printable : std::false_type {};
template <class T>
struct printable<T, std::void_t<decltype(std::declval<std::ostream &>() << std::declval<const T &>())>>
		: std::true_type {};

template <class T>
std::string show(const T &v) {
	if constexpr (std::is_same_v<T, bool>) {
		return v ? "true" : "false";
	} else if constexpr (std::is_same_v<T, std::string> || std::is_same_v<T, std::string_view>) {
		return "\"" + std::string(v) + "\"";
	} else if constexpr (std::is_same_v<std::decay_t<T>, const char *> || std::is_same_v<std::decay_t<T>, char *>) {
		return v ? "\"" + std::string(v) + "\"" : "nullptr";
	} else if constexpr (std::is_same_v<T, signed char> || std::is_same_v<T, unsigned char>) {
		return std::to_string(int(v));
	} else if constexpr (std::is_enum_v<T>) {
		return std::to_string(static_cast<long long>(v));
	} else if constexpr (std::is_same_v<T, std::nullptr_t>) {
		return "nullptr";
	} else if constexpr (printable<T>::value) {
		std::ostringstream os;
		os << v;
		return os.str();
	} else {
		return "{?}";
	}
}

// --- Comparing within a margin -------------------------------------------------------

class Approx {
public:
	explicit Approx(double value) : value_(value) {}
	Approx &epsilon(double e) {
		epsilon_ = e;
		return *this;
	}
	// |a - value| < epsilon x (1 + the larger of |a| and |value|)
	friend bool operator==(double a, const Approx &b) {
		return std::fabs(a - b.value_) < b.epsilon_ * (1.0 + std::max(std::fabs(a), std::fabs(b.value_)));
	}
	friend bool operator==(const Approx &b, double a) { return a == b; }
	friend bool operator!=(double a, const Approx &b) { return !(a == b); }
	friend bool operator!=(const Approx &b, double a) { return !(a == b); }
	friend std::ostream &operator<<(std::ostream &os, const Approx &a) { return os << "Approx( " << a.value_ << " )"; }

private:
	double value_;
	double epsilon_ = double(std::numeric_limits<float>::epsilon()) * 100;
};

// --- Taking a comparison apart -------------------------------------------------------

struct Result {
	bool pass;
	std::string values; // "" if there's nothing more to show than the expression
};

#pragma warning(push)
#pragma warning(disable : 4018 4389 4805 4800) // (signed/unsigned and bool comparisons, as the test wrote them)

template <class L>
struct Lhs {
	const L &l;
	template <class R>
	Result operator==(const R &r) const { return {bool(l == r), show(l) + " == " + show(r)}; }
	template <class R>
	Result operator!=(const R &r) const { return {bool(l != r), show(l) + " != " + show(r)}; }
	template <class R>
	Result operator<(const R &r) const { return {bool(l < r), show(l) + " < " + show(r)}; }
	template <class R>
	Result operator<=(const R &r) const { return {bool(l <= r), show(l) + " <= " + show(r)}; }
	template <class R>
	Result operator>(const R &r) const { return {bool(l > r), show(l) + " > " + show(r)}; }
	template <class R>
	Result operator>=(const R &r) const { return {bool(l >= r), show(l) + " >= " + show(r)}; }
	operator Result() const { return {static_cast<bool>(l), show(l)}; }
};

#pragma warning(pop)

// `Decomposer() <= a == b` is `(Decomposer() <= a) == b`: <= binds tighter than ==.
struct Decomposer {
	template <class L>
	Lhs<L> operator<=(const L &l) const { return {l}; }
};

// --- Reporting -----------------------------------------------------------------------

inline void print_failure(const char *file, int line, const char *macro, const char *expr, const std::string &values,
		const std::string &message) {
	State &s = state();
	std::fprintf(stderr, "\n%s(%d): FAILED in \"%s\":\n  %s( %s )\n", file, line, s.current ? s.current->name : "?", macro,
			expr);
	if (!values.empty()) {
		std::fprintf(stderr, "  values: %s( %s )\n", macro, values.c_str());
	}
	if (!message.empty()) {
		std::fprintf(stderr, "  %s\n", message.c_str());
	}
	for (const auto &[name, value] : s.captures) {
		std::fprintf(stderr, "  with %s := %s\n", name, value().c_str());
	}
	std::fflush(stderr);
}

inline void report(const char *file, int line, const char *macro, const char *expr, const Result &r, bool fatal,
		const std::string &message = "") {
	State &s = state();
	s.checks++;
	if (r.pass) {
		return;
	}
	s.failed_checks++;
	s.case_failed = true;
	print_failure(file, line, macro, expr, r.values, message);
	if (fatal) {
		throw Abort{};
	}
}

// CAPTURE: printed with failures while in scope.
struct Capture {
	Capture(const char *name, std::function<std::string()> value) { state().captures.push_back({name, std::move(value)}); }
	~Capture() { state().captures.pop_back(); }
	Capture(const Capture &) = delete;
	Capture &operator=(const Capture &) = delete;
};

// --- Running -------------------------------------------------------------------------

// `pattern` with * for anything.
inline bool matches(const char *pattern, const char *text) {
	if (*pattern == '\0') {
		return *text == '\0';
	}
	if (*pattern == '*') {
		return matches(pattern + 1, text) || (*text != '\0' && matches(pattern, text + 1));
	}
	return *text != '\0' && *pattern == *text && matches(pattern + 1, text + 1);
}

inline int run(int argc, char **argv) {
	// The patterns of -tc= (run) and -tce= (don't).
	std::vector<std::string> filters, excludes;
	for (int i = 1; i < argc; i++) {
		const std::string_view a = argv[i];
		for (auto [prefix, into] : {std::pair{"-tc=", &filters}, std::pair{"--test-case=", &filters},
				 std::pair{"-tce=", &excludes}, std::pair{"--test-case-exclude=", &excludes}}) {
			if (a.rfind(prefix, 0) == 0) {
				std::string list(a.substr(strlen(prefix)));
				size_t from = 0;
				while (from <= list.size()) {
					size_t comma = list.find(',', from);
					comma = comma == std::string::npos ? list.size() : comma;
					into->push_back(list.substr(from, comma - from));
					from = comma + 1;
				}
			}
		}
	}
	std::vector<TestCase> cases = test_cases();
	std::sort(cases.begin(), cases.end(), [](const TestCase &a, const TestCase &b) {
		const int c = std::strcmp(a.file, b.file);
		return c != 0 ? c < 0 : a.line < b.line;
	});
	State &s = state();
	int ran = 0, failed = 0, skipped = 0;
	std::vector<std::pair<double, const TestCase *>> times;
	const auto any = [&](const std::vector<std::string> &patterns, const char *name) {
		return std::any_of(patterns.begin(), patterns.end(), [&](const std::string &f) { return matches(f.c_str(), name); });
	};
	for (const TestCase &tc : cases) {
		if ((!filters.empty() && !any(filters, tc.name)) || any(excludes, tc.name)) {
			skipped++;
			continue;
		}
		s.current = &tc;
		s.case_failed = false;
		s.captures.clear();
		const auto start = std::chrono::steady_clock::now();
		try {
			tc.fn();
		} catch (const Abort &) {
			// (already reported)
		} catch (const std::exception &e) {
			s.case_failed = true;
			print_failure(tc.file, tc.line, "TEST_CASE", tc.name, "", std::string("threw an exception: ") + e.what());
		} catch (...) {
			s.case_failed = true;
			print_failure(tc.file, tc.line, "TEST_CASE", tc.name, "", "threw an unknown exception");
		}
		times.push_back({std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count(), &tc});
		ran++;
		failed += s.case_failed ? 1 : 0;
		s.current = nullptr;
	}
	std::sort(times.begin(), times.end(), [](const auto &a, const auto &b) { return a.first > b.first; });
	std::printf("\n[tests] slowest:\n");
	for (size_t i = 0; i < times.size() && i < 10; i++) {
		std::printf("  %6.1f s  %s\n", times[i].first, times[i].second->name);
	}
	std::printf("\n[tests] test cases: %d | %d passed | %d failed | %d skipped\n", ran, ran - failed, failed, skipped);
	std::printf("[tests] checks: %lld | %lld passed | %lld failed\n", s.checks, s.checks - s.failed_checks, s.failed_checks);
	std::printf("[tests] %s\n", failed == 0 ? "SUCCESS" : "FAILURE");
	return failed == 0 ? 0 : 1;
}

} // namespace check

#define CHECK_CAT2(a, b) a##b
#define CHECK_CAT(a, b) CHECK_CAT2(a, b)

#define CHECK_TEST_CASE(name, fn)                                                                       \
	static void fn();                                                                                   \
	static const ::check::Register CHECK_CAT(fn, _registered)(name, __FILE__, __LINE__, &fn);         \
	static void fn()
#define TEST_CASE(name) CHECK_TEST_CASE(name, CHECK_CAT(check_test_case_, __COUNTER__))

#define CHECK_ASSERT(macro, fatal, ...)                                                                  \
	::check::report(__FILE__, __LINE__, macro, #__VA_ARGS__, ::check::Result(::check::Decomposer() <= __VA_ARGS__), fatal)
#define CHECK_ASSERT_FALSE(macro, fatal, ...)                                                            \
	::check::report(__FILE__, __LINE__, macro, #__VA_ARGS__, ::check::Result{!static_cast<bool>(__VA_ARGS__), ""}, fatal)

#define CHECK(...) CHECK_ASSERT("CHECK", false, __VA_ARGS__)
#define REQUIRE(...) CHECK_ASSERT("REQUIRE", true, __VA_ARGS__)
#define CHECK_FALSE(...) CHECK_ASSERT_FALSE("CHECK_FALSE", false, __VA_ARGS__)
#define REQUIRE_FALSE(...) CHECK_ASSERT_FALSE("REQUIRE_FALSE", true, __VA_ARGS__)

#define REQUIRE_MESSAGE(cond, msg)                                                                       \
	do {                                                                                                 \
		std::ostringstream check_message_;                                                               \
		const bool check_pass_ = static_cast<bool>(cond);                                                \
		if (!check_pass_) {                                                                              \
			check_message_ << msg;                                                                       \
		}                                                                                                \
		::check::report(__FILE__, __LINE__, "REQUIRE", #cond, ::check::Result{check_pass_, ""}, true, check_message_.str()); \
	} while (false)

#define CHECK_THROWS(...)                                                                                \
	do {                                                                                                 \
		bool check_threw_ = false;                                                                       \
		try {                                                                                            \
			static_cast<void>(__VA_ARGS__);                                                              \
		} catch (...) {                                                                                  \
			check_threw_ = true;                                                                         \
		}                                                                                                \
		::check::report(__FILE__, __LINE__, "CHECK_THROWS", #__VA_ARGS__, ::check::Result{check_threw_, ""}, false); \
	} while (false)

#define CHECK_THROWS_AS(expr, type)                                                                      \
	do {                                                                                                 \
		bool check_threw_ = false;                                                                       \
		try {                                                                                            \
			static_cast<void>(expr);                                                                     \
		} catch (const type &) {                                                                         \
			check_threw_ = true;                                                                         \
		} catch (...) {                                                                                  \
		}                                                                                                \
		::check::report(__FILE__, __LINE__, "CHECK_THROWS_AS", #expr ", " #type, ::check::Result{check_threw_, ""}, false); \
	} while (false)

#define FAIL(msg)                                                                                        \
	do {                                                                                                 \
		std::ostringstream check_message_;                                                               \
		check_message_ << msg;                                                                           \
		::check::report(__FILE__, __LINE__, "FAIL", "", ::check::Result{false, ""}, true, check_message_.str()); \
	} while (false)

#define MESSAGE(msg)                                                                                     \
	do {                                                                                                 \
		std::ostringstream check_message_;                                                               \
		check_message_ << msg;                                                                           \
		std::printf("%s(%d): MESSAGE: %s\n", __FILE__, __LINE__, check_message_.str().c_str());          \
	} while (false)

#define CAPTURE(x) const ::check::Capture CHECK_CAT(check_capture_, __COUNTER__)(#x, [&] { return ::check::show(x); })

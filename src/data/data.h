#pragma once
// The game's dynamic data value: what a JSON file (data/json.h) holds,
// what mods' rules take and return, what goes over the network. Null, a
// boolean, an integer, a float, a string, a list or an object (string keys,
// kept in the order they were added: that order can reach the world generator,
// so it must not depend on anything but the input).
//
// Brace lists build values the way they read: {{"a", 1}, {"b", 2}} is an
// object (every element a pair with a string first), anything else a list;
// Data::array({...}) and Data::object({...}) say which.

#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <iterator>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace gc {

class Data {
public:
	enum class Type : uint8_t { Null, Boolean, Integer, Float, String, Array, Object };
	using Member = std::pair<std::string, Data>;
	using Members = std::vector<Member>;
	using List = std::vector<Data>;

	// --- Making values ---------------------------------------------------------------

	Data() = default;
	// Copying, moving and destroying are in data.cpp: written here, the
	// variant's code for them would be compiled again in every file that
	// copies a Data (most of the game), which is most of the build's time.
	Data(const Data &other);
	Data(Data &&other) noexcept;
	Data &operator=(const Data &other);
	Data &operator=(Data &&other) noexcept;
	~Data();
	Data(std::nullptr_t) {}
	Data(bool b) : v_(b) {}
	template <class T, std::enable_if_t<std::is_integral_v<T> && !std::is_same_v<T, bool>, int> = 0>
	Data(T i) : v_(int64_t(i)) {}
	template <class T, std::enable_if_t<std::is_floating_point_v<T>, int> = 0>
	Data(T d) : v_(double(d)) {}
	Data(const char *s) : v_(std::string(s)) {}
	Data(std::string s) : v_(std::move(s)) {}
	Data(std::string_view s) : v_(std::string(s)) {}
	Data(std::initializer_list<Data> init);
	// A list of anything a Data can be made from.
	template <class T, std::enable_if_t<!std::is_same_v<T, Data> && std::is_constructible_v<Data, const T &>, int> = 0>
	Data(const std::vector<T> &list) : v_(List(list.begin(), list.end())) {}

	static Data array(std::initializer_list<Data> init = {});
	static Data object(std::initializer_list<Data> init = {}); // elements: {"key", value}
	static Data array(List list);

	// --- What it is ------------------------------------------------------------------

	Type type() const { return Type(v_.index()); }
	bool is_null() const { return type() == Type::Null; }
	bool is_boolean() const { return type() == Type::Boolean; }
	bool is_number() const { return type() == Type::Integer || type() == Type::Float; }
	bool is_number_integer() const { return type() == Type::Integer; }
	bool is_number_float() const { return type() == Type::Float; }
	bool is_string() const { return type() == Type::String; }
	bool is_array() const { return type() == Type::Array; }
	bool is_object() const { return type() == Type::Object; }
	bool is_structured() const { return is_array() || is_object(); }

	// --- Reading it ------------------------------------------------------------------

	// The value as T (bool, a number type, std::string). Numbers convert into
	// each other; anything else of the wrong type throws std::invalid_argument.
	template <class T>
	T get() const;
	const std::string &str() const; // throws if it isn't a string

	// An object's field, or `fallback` if this isn't an object, the field is
	// missing or it can't be a T (a string where a number was wanted).
	template <class T>
	T value(std::string_view key, const T &fallback) const;
	std::string value(std::string_view key, const char *fallback) const { return value<std::string>(key, fallback); }

	bool contains(std::string_view key) const { return find_member(key) != nullptr; }
	// Null if this isn't an object or the field is missing / out of range.
	const Data &operator[](std::string_view key) const;
	const Data &operator[](const char *key) const { return (*this)[std::string_view(key)]; }
	const Data &operator[](size_t i) const;
	template <class I, std::enable_if_t<std::is_integral_v<I>, int> = 0>
	const Data &operator[](I i) const { return (*this)[size_t(i)]; }
	// The same, but missing fields and indexes throw std::out_of_range.
	const Data &at(std::string_view key) const;
	const Data &at(size_t i) const;

	size_t size() const; // a list's or object's entries; 0 for null; 1 for anything else
	bool empty() const { return size() == 0; }
	const Data &front() const { return at(size_t(0)); }
	const Data &back() const { return at(size() - 1); }

	// --- Changing it -----------------------------------------------------------------

	// An object's field, added (as null) if missing; null becomes an empty object first.
	Data &operator[](std::string_view key);
	Data &operator[](const char *key) { return (*this)[std::string_view(key)]; }
	// A list's element, the list growing (with nulls) to reach it; null becomes an empty list first.
	Data &operator[](size_t i);
	template <class I, std::enable_if_t<std::is_integral_v<I>, int> = 0>
	Data &operator[](I i) { return (*this)[size_t(i)]; }
	Data &at(std::string_view key);
	Data &at(size_t i);
	Data &front() { return at(size_t(0)); }
	Data &back() { return at(size() - 1); }

	void push_back(Data v); // null becomes an empty list first
	template <class... A>
	Data &emplace_back(A &&...a) {
		push_back(Data(std::forward<A>(a)...));
		return std::get<List>(v_).back();
	}
	// Adds a field without looking for one with the same key (the caller knows
	// it's new): for building large objects.
	void add_member(std::string key, Data v);
	size_t erase(std::string_view key); // how many were removed (0 or 1)
	void erase(size_t index);
	void clear(); // keeps the type, empties it
	// Sets every field `other` (an object) has.
	void update(const Data &other);

	// --- Going through it ------------------------------------------------------------

	// An object's fields in order (empty for anything else).
	const Members &items() const;
	Members &items();
	// A list's elements in order (empty for anything else).
	const List &list() const;

	// Iterating goes through a list's elements or an object's values.
	template <bool Const>
	class Iter {
	public:
		using iterator_category = std::bidirectional_iterator_tag;
		using value_type = Data;
		using difference_type = std::ptrdiff_t;
		using pointer = std::conditional_t<Const, const Data *, Data *>;
		using reference = std::conditional_t<Const, const Data &, Data &>;
		using Owner = std::conditional_t<Const, const Data, Data>;

		Iter() = default;
		Iter(Owner *d, size_t i) : d_(d), i_(i) {}
		operator Iter<true>() const { return Iter<true>(d_, i_); }

		reference operator*() const {
			if (auto *m = std::get_if<Members>(&d_->v_)) {
				return (*m)[i_].second;
			}
			return std::get<List>(d_->v_)[i_];
		}
		pointer operator->() const { return &**this; }
		const std::string &key() const { return std::get<Members>(d_->v_).at(i_).first; }
		reference value() const { return **this; }
		Iter &operator++() { ++i_; return *this; }
		Iter operator++(int) { Iter t = *this; ++i_; return t; }
		Iter &operator--() { --i_; return *this; }
		Iter operator--(int) { Iter t = *this; --i_; return t; }
		bool operator==(const Iter &o) const { return d_ == o.d_ && i_ == o.i_; }
		bool operator!=(const Iter &o) const { return !(*this == o); }

	private:
		Owner *d_ = nullptr;
		size_t i_ = 0;
	};
	using iterator = Iter<false>;
	using const_iterator = Iter<true>;

	iterator begin() { return iterator(this, 0); }
	iterator end() { return iterator(this, is_structured() ? size() : 0); }
	const_iterator begin() const { return const_iterator(this, 0); }
	const_iterator end() const { return const_iterator(this, is_structured() ? size() : 0); }
	// An object's field, or end().
	iterator find(std::string_view key);
	const_iterator find(std::string_view key) const;

	// Deep equality; an object's field order doesn't matter, and 1 == 1.0.
	friend bool operator==(const Data &a, const Data &b);
	friend bool operator!=(const Data &a, const Data &b) { return !(a == b); }

private:
	const Data *find_member(std::string_view key) const;
	size_t member_index(std::string_view key) const; // members_.size() if missing

	// (The alternatives' order is Type's.)
	std::variant<std::monostate, bool, int64_t, double, std::string, List, Members> v_;
};

// --- Templates -------------------------------------------------------------------

template <class T>
T Data::get() const {
	if constexpr (std::is_same_v<T, Data>) {
		return *this;
	} else if constexpr (std::is_same_v<T, bool>) {
		if (type() != Type::Boolean) {
			throw std::invalid_argument("data: not a boolean");
		}
		return std::get<bool>(v_);
	} else if constexpr (std::is_arithmetic_v<T>) {
		if (const int64_t *i = std::get_if<int64_t>(&v_)) {
			return T(*i);
		}
		if (const double *d = std::get_if<double>(&v_)) {
			return T(*d);
		}
		throw std::invalid_argument("data: not a number");
	} else if constexpr (std::is_same_v<T, std::string>) {
		return str();
	} else {
		static_assert(sizeof(T) == 0, "Data::get: not a type data holds");
	}
}

template <class T>
T Data::value(std::string_view key, const T &fallback) const {
	const Data *v = find_member(key);
	if (!v) {
		return fallback;
	}
	if constexpr (std::is_same_v<T, Data>) {
		return *v;
	} else if constexpr (std::is_same_v<T, bool>) {
		return v->is_boolean() ? v->get<bool>() : fallback;
	} else if constexpr (std::is_arithmetic_v<T>) {
		return v->is_number() ? v->get<T>() : fallback;
	} else if constexpr (std::is_same_v<T, std::string>) {
		return v->is_string() ? v->str() : fallback;
	} else {
		static_assert(sizeof(T) == 0, "Data::value: not a type data holds");
	}
}

} // namespace gc

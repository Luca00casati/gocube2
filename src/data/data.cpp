#include "data/data.h"

#include <algorithm>

namespace gc {

namespace {

const Data NONE;
const Data::Members NO_MEMBERS;
const Data::List NO_ELEMENTS;

bool is_pair(const Data &d) {
	return d.is_array() && d.size() == 2 && d[size_t(0)].is_string();
}

} // namespace

Data::Data(const Data &other) = default;
Data::Data(Data &&other) noexcept = default;
Data &Data::operator=(const Data &other) = default;
Data &Data::operator=(Data &&other) noexcept = default;
Data::~Data() = default;

Data::Data(std::initializer_list<Data> init) {
	const bool object = std::all_of(init.begin(), init.end(), is_pair);
	if (object && init.size() > 0) {
		*this = Data::object(init);
	} else {
		v_ = List(init.begin(), init.end());
	}
}

Data Data::array(std::initializer_list<Data> init) {
	Data d;
	d.v_ = List(init.begin(), init.end());
	return d;
}

Data Data::array(List list) {
	Data d;
	d.v_ = std::move(list);
	return d;
}

Data Data::object(std::initializer_list<Data> init) {
	Data d;
	d.v_ = Members();
	for (const Data &e : init) {
		if (!is_pair(e)) {
			throw std::invalid_argument("data: an object's entries are {\"key\", value} pairs");
		}
		d[e[size_t(0)].str()] = e[size_t(1)];
	}
	return d;
}

const std::string &Data::str() const {
	if (const std::string *s = std::get_if<std::string>(&v_)) {
		return *s;
	}
	throw std::invalid_argument("data: not a string");
}

size_t Data::member_index(std::string_view key) const {
	const Members *m = std::get_if<Members>(&v_);
	if (!m) {
		return 0;
	}
	for (size_t i = 0; i < m->size(); i++) {
		if ((*m)[i].first == key) {
			return i;
		}
	}
	return m->size();
}

const Data *Data::find_member(std::string_view key) const {
	const Members *m = std::get_if<Members>(&v_);
	if (!m) {
		return nullptr;
	}
	const size_t i = member_index(key);
	return i < m->size() ? &(*m)[i].second : nullptr;
}

const Data &Data::operator[](std::string_view key) const {
	const Data *v = find_member(key);
	return v ? *v : NONE;
}

const Data &Data::operator[](size_t i) const {
	const List *l = std::get_if<List>(&v_);
	return l && i < l->size() ? (*l)[i] : NONE;
}

const Data &Data::at(std::string_view key) const {
	const Data *v = find_member(key);
	if (!v) {
		throw std::out_of_range("data: no field \"" + std::string(key) + "\"");
	}
	return *v;
}

const Data &Data::at(size_t i) const {
	const List *l = std::get_if<List>(&v_);
	if (!l || i >= l->size()) {
		throw std::out_of_range("data: no element " + std::to_string(i));
	}
	return (*l)[i];
}

Data &Data::at(std::string_view key) {
	return const_cast<Data &>(static_cast<const Data &>(*this).at(key));
}

Data &Data::at(size_t i) {
	return const_cast<Data &>(static_cast<const Data &>(*this).at(i));
}

size_t Data::size() const {
	if (const List *l = std::get_if<List>(&v_)) {
		return l->size();
	}
	if (const Members *m = std::get_if<Members>(&v_)) {
		return m->size();
	}
	return is_null() ? 0 : 1;
}

Data &Data::operator[](std::string_view key) {
	if (is_null()) {
		v_ = Members();
	}
	Members *m = std::get_if<Members>(&v_);
	if (!m) {
		throw std::invalid_argument("data: a field of something that isn't an object");
	}
	const size_t i = member_index(key);
	if (i < m->size()) {
		return (*m)[i].second;
	}
	m->emplace_back(std::string(key), Data());
	return m->back().second;
}

Data &Data::operator[](size_t i) {
	if (is_null()) {
		v_ = List();
	}
	List *l = std::get_if<List>(&v_);
	if (!l) {
		throw std::invalid_argument("data: an element of something that isn't a list");
	}
	if (i >= l->size()) {
		l->resize(i + 1);
	}
	return (*l)[i];
}

void Data::push_back(Data v) {
	if (is_null()) {
		v_ = List();
	}
	List *l = std::get_if<List>(&v_);
	if (!l) {
		throw std::invalid_argument("data: push_back on something that isn't a list");
	}
	l->push_back(std::move(v));
}

void Data::add_member(std::string key, Data v) {
	if (is_null()) {
		v_ = Members();
	}
	Members *m = std::get_if<Members>(&v_);
	if (!m) {
		throw std::invalid_argument("data: a field of something that isn't an object");
	}
	m->emplace_back(std::move(key), std::move(v));
}

size_t Data::erase(std::string_view key) {
	Members *m = std::get_if<Members>(&v_);
	if (!m) {
		return 0;
	}
	const size_t i = member_index(key);
	if (i >= m->size()) {
		return 0;
	}
	m->erase(m->begin() + std::ptrdiff_t(i));
	return 1;
}

void Data::erase(size_t index) {
	List *l = std::get_if<List>(&v_);
	if (!l || index >= l->size()) {
		throw std::out_of_range("data: no element " + std::to_string(index));
	}
	l->erase(l->begin() + std::ptrdiff_t(index));
}

void Data::clear() {
	switch (type()) {
		case Type::Null: break;
		case Type::Boolean: v_ = false; break;
		case Type::Integer: v_ = int64_t(0); break;
		case Type::Float: v_ = 0.0; break;
		case Type::String: v_ = std::string(); break;
		case Type::Array: v_ = List(); break;
		case Type::Object: v_ = Members(); break;
	}
}

void Data::update(const Data &other) {
	for (const auto &[k, v] : other.items()) {
		(*this)[k] = v;
	}
}

const Data::Members &Data::items() const {
	const Members *m = std::get_if<Members>(&v_);
	return m ? *m : NO_MEMBERS;
}

Data::Members &Data::items() {
	if (is_null()) {
		v_ = Members();
	}
	Members *m = std::get_if<Members>(&v_);
	if (!m) {
		throw std::invalid_argument("data: fields of something that isn't an object");
	}
	return *m;
}

const Data::List &Data::list() const {
	const List *l = std::get_if<List>(&v_);
	return l ? *l : NO_ELEMENTS;
}

Data::iterator Data::find(std::string_view key) {
	return is_object() ? iterator(this, member_index(key)) : end();
}

Data::const_iterator Data::find(std::string_view key) const {
	return is_object() ? const_iterator(this, member_index(key)) : end();
}

bool operator==(const Data &a, const Data &b) {
	if (a.is_number() && b.is_number()) {
		if (a.is_number_integer() && b.is_number_integer()) {
			return a.get<int64_t>() == b.get<int64_t>();
		}
		return a.get<double>() == b.get<double>();
	}
	if (a.type() != b.type()) {
		return false;
	}
	switch (a.type()) {
		case Data::Type::Null: return true;
		case Data::Type::Boolean: return a.get<bool>() == b.get<bool>();
		case Data::Type::String: return a.str() == b.str();
		case Data::Type::Array: return a.list() == b.list();
		case Data::Type::Object: {
			if (a.size() != b.size()) {
				return false;
			}
			for (const auto &[k, v] : a.items()) {
				const Data *w = b.find_member(k);
				if (!w || !(v == *w)) {
					return false;
				}
			}
			return true;
		}
		default: return false; // (numbers: above)
	}
}

} // namespace gc

#pragma once
// Minimal, dependency-free JSON for the dog harness.
//
// Minimal JSON value type (dog::json) covering just the subset the code uses:
// objects/arrays/scalars, operator[], value(), contains(), get<T>(), is_*(),
// push_back(), size/empty, range-for, rbegin/rend, parse(), dump(), the
// object-literal initializer-list construction, and implicit scalar conversion.
// Header-only; no external deps.

#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <map>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

namespace dog {

namespace detail {
// Whitelist of types the implicit conversion operator may produce. Constraining
// operator T() to this set (a whitelist, mirroring the get<> SFINAE) is what
// keeps `std::string s = j;` unambiguous: it rules out T=const char*, T=char,
// etc., which otherwise make std::string::operator= ambiguous.
template <typename T> struct is_convertible : std::false_type {};
#define DOG_JSON_CONV(T) \
  template <> \
  struct is_convertible<T> : std::true_type {};
DOG_JSON_CONV(bool)
DOG_JSON_CONV(int)
DOG_JSON_CONV(long)
DOG_JSON_CONV(long long)
DOG_JSON_CONV(unsigned)
DOG_JSON_CONV(unsigned long)
DOG_JSON_CONV(unsigned long long)
DOG_JSON_CONV(double)
DOG_JSON_CONV(float)
DOG_JSON_CONV(std::string)
#undef DOG_JSON_CONV
}  // namespace detail

class json {
public:
  enum class Type { Null, Bool, Int, Double, String, Array, Object };

  // ---------------- constructors ----------------
  json() : type_(Type::Null) {}
  json(std::nullptr_t) : type_(Type::Null) {}
  json(bool b) : type_(Type::Bool), b_(b) {}
  json(int v) : type_(Type::Int), i_(v) {}
  json(long v) : type_(Type::Int), i_(v) {}
  json(long long v) : type_(Type::Int), i_(v) {}
  json(unsigned v) : type_(Type::Int), i_(v) {}
  json(unsigned long v) : type_(Type::Int), i_(v) {}
  json(unsigned long long v) : type_(Type::Int), i_(v) {}
  json(double v) : type_(Type::Double), d_(v) {}
  json(float v) : type_(Type::Double), d_(v) {}
  json(const char* s) : type_(Type::String), s_(s ? s : "") {}
  json(const std::string& s) : type_(Type::String), s_(s) {}
  json(std::string&& s) : type_(Type::String), s_(std::move(s)) {}

  // Object/array from initializer lists. Rule: the list is an object iff every
  // element is a size-2 array whose first item is a string
  // (i.e. {{"k",v},{...}}); an empty list is null; anything else is an array.
  json(std::initializer_list<json> init, bool type_deduction = true,
       Type manual = Type::Array)
      : type_(Type::Null) {
    (void)type_deduction;
    bool is_obj = (init.size() != 0);
    if (is_obj)
      for (const auto& e : init)
        if (!(e.type_ == Type::Array && e.arr_.size() == 2 && e.arr_[0].type_ == Type::String)) {
          is_obj = false;
          break;
        }
    if (is_obj) {
      type_ = Type::Object;
      for (auto& e : init) obj_.emplace(std::move(e.arr_[0].s_), std::move(e.arr_[1]));
    } else if (init.size() != 0) {
      type_ = Type::Array;
      arr_ = std::vector<json>(init.begin(), init.end());
    } else {
      type_ = (manual == Type::Object) ? Type::Object : Type::Null;
      if (type_ == Type::Object) obj_;  // empty map
      if (type_ == Type::Array) arr_;   // empty vector
    }
  }

  // ---------------- static factories ----------------
  static json object() {
    json j;
    j.type_ = Type::Object;
    return j;
  }
  static json array() {
    json j;
    j.type_ = Type::Array;
    return j;
  }

  // ---------------- type predicates ----------------
  bool is_null() const { return type_ == Type::Null; }
  bool is_boolean() const { return type_ == Type::Bool; }
  bool is_number() const { return type_ == Type::Int || type_ == Type::Double; }
  bool is_number_integer() const { return type_ == Type::Int; }
  bool is_number_unsigned() const { return type_ == Type::Int && i_ >= 0; }
  bool is_string() const { return type_ == Type::String; }
  bool is_array() const { return type_ == Type::Array; }
  bool is_object() const { return type_ == Type::Object; }
  Type type() const { return type_; }

  // ---------------- element access ----------------
  // Non-const: creates a null entry on miss (objects); extends arrays with nulls.
  json& operator[](const std::string& key) {
    if (type_ == Type::Null) type_ = Type::Object;
    if (type_ != Type::Object) throw std::runtime_error("json [] string on non-object");
    return obj_[key];
  }
  // const: returns the value, or a shared null ref on miss (never throws).
  const json& operator[](const std::string& key) const {
    if (type_ == Type::Object) {
      auto it = obj_.find(key);
      if (it != obj_.end()) return it->second;
    }
    return null_ref();
  }
  json& operator[](std::size_t idx) {
    if (type_ == Type::Null) type_ = Type::Array;
    if (type_ != Type::Array) throw std::runtime_error("json [] int on non-array");
    if (idx >= arr_.size()) arr_.resize(idx + 1);
    return arr_[idx];
  }
  const json& operator[](std::size_t idx) const {
    if (type_ == Type::Array && idx < arr_.size()) return arr_[idx];
    return null_ref();
  }

  // value(key, default): lenient; returns default when not an object or key absent.
  std::string value(const std::string& key, const char* def) const {
    const json* v = find(key);
    return v ? v->to_string_lenient() : std::string(def);
  }
  std::string value(const std::string& key, const std::string& def) const {
    const json* v = find(key);
    return v ? v->to_string_lenient() : def;
  }
  long value(const std::string& key, long def) const {
    const json* v = find(key);
    return v ? v->as_int() : def;
  }
  int value(const std::string& key, int def) const {
    const json* v = find(key);
    return v ? (int)v->as_int() : def;
  }
  unsigned value(const std::string& key, unsigned def) const {
    const json* v = find(key);
    return v ? (unsigned)v->as_int() : def;
  }
  bool value(const std::string& key, bool def) const {
    const json* v = find(key);
    return v ? v->as_bool() : def;
  }
  double value(const std::string& key, double def) const {
    const json* v = find(key);
    return v ? v->as_double() : def;
  }
  json value(const std::string& key, const json& def) const {
    const json* v = find(key);
    return v ? *v : def;
  }

  bool contains(const std::string& key) const {
    return type_ == Type::Object && obj_.count(key) > 0;
  }

  template <typename T> T get() const;  // defined below

  // ---------------- structural ----------------
  void push_back(const json& v) {
    if (type_ == Type::Null) type_ = Type::Array;
    if (type_ != Type::Array) throw std::runtime_error("json push_back on non-array");
    arr_.push_back(v);
  }
  void push_back(json&& v) {
    if (type_ == Type::Null) type_ = Type::Array;
    if (type_ != Type::Array) throw std::runtime_error("json push_back on non-array");
    arr_.push_back(std::move(v));
  }

  std::size_t size() const {
    if (type_ == Type::Array) return arr_.size();
    if (type_ == Type::Object) return obj_.size();
    return 0;
  }
  bool empty() const {
    if (type_ == Type::Array) return arr_.empty();
    if (type_ == Type::Object) return obj_.empty();
    return true;  // null/leaf
  }
  void clear() { *this = json(); }

  // Iterators (arrays). Not used on objects by the code.
  std::vector<json>::iterator begin() { return arr_.begin(); }
  std::vector<json>::iterator end() { return arr_.end(); }
  std::vector<json>::const_iterator begin() const { return arr_.begin(); }
  std::vector<json>::const_iterator end() const { return arr_.end(); }
  std::vector<json>::reverse_iterator rbegin() { return arr_.rbegin(); }
  std::vector<json>::reverse_iterator rend() { return arr_.rend(); }
  std::vector<json>::const_reverse_iterator rbegin() const { return arr_.rbegin(); }
  std::vector<json>::const_reverse_iterator rend() const { return arr_.rend(); }

  // ---------------- serialization ----------------
  std::string dump() const {
    std::string out;
    dump_into(out);
    return out;
  }

  // parse: throws std::runtime_error on invalid input (callers use catch...).
  static json parse(const std::string& s);

  // ---------------- assignment ----------------
  json(const json&) = default;
  json(json&&) = default;
  json& operator=(const json&) = default;
  json& operator=(json&&) = default;
  json& operator=(std::nullptr_t) {
    *this = json();
    return *this;
  }
  json& operator=(bool v) {
    *this = json(v);
    return *this;
  }
  json& operator=(int v) {
    *this = json(v);
    return *this;
  }
  json& operator=(long v) {
    *this = json(v);
    return *this;
  }
  json& operator=(long long v) {
    *this = json(v);
    return *this;
  }
  json& operator=(unsigned v) {
    *this = json(v);
    return *this;
  }
  json& operator=(unsigned long v) {
    *this = json(v);
    return *this;
  }
  json& operator=(double v) {
    *this = json(v);
    return *this;
  }
  json& operator=(const char* v) {
    *this = json(std::string(v ? v : ""));
    return *this;
  }
  json& operator=(const std::string& v) {
    *this = json(v);
    return *this;
  }

  // ---------------- equality ----------------
  bool operator==(const json& o) const { return equals(o); }
  bool operator!=(const json& o) const { return !equals(o); }

  // ---------------- implicit conversion (single template operator) ----------------
  // A single templated operator is required: separate fixed `operator long` +
  // `operator std::string` make o["a"] ambiguous with the built-in
  // operator[](long, const char*). operator T() -> get<T>().
  template <typename T,
            typename std::enable_if<!std::is_same<T, json>::value &&
                                           detail::is_convertible<T>::value,
                                      int>::type = 0>
  operator T() const {
    return get<T>();
  }

private:
  Type type_;
  bool b_ = false;
  long long i_ = 0;
  double d_ = 0.0;
  std::string s_;
  std::vector<json> arr_;
  std::map<std::string, json> obj_;

  static const json& null_ref() {
    static const json n;  // null
    return n;
  }

  const json* find(const std::string& key) const {
    if (type_ != Type::Object) return nullptr;
    auto it = obj_.find(key);
    return it == obj_.end() ? nullptr : &it->second;
  }

  long long as_int() const {
    switch (type_) {
      case Type::Int: return i_;
      case Type::Double: return (long long)d_;
      case Type::Bool: return b_ ? 1 : 0;
      case Type::String: {
        char* end = nullptr;
        long long v = std::strtoll(s_.c_str(), &end, 10);
        if (end && *end == 0) return v;
        throw std::runtime_error("json get<int> from non-numeric string");
      }
      case Type::Null: return 0;
      default: throw std::runtime_error("json get<int> from container");
    }
  }
  double as_double() const {
    switch (type_) {
      case Type::Double: return d_;
      case Type::Int: return (double)i_;
      case Type::Bool: return b_ ? 1.0 : 0.0;
      case Type::String: {
        char* end = nullptr;
        double v = std::strtod(s_.c_str(), &end);
        if (end && *end == 0) return v;
        throw std::runtime_error("json get<double> from non-numeric string");
      }
      case Type::Null: return 0.0;
      default: throw std::runtime_error("json get<double> from container");
    }
  }
  bool as_bool() const {
    switch (type_) {
      case Type::Bool: return b_;
      case Type::Int: return i_ != 0;
      case Type::Double: return d_ != 0.0;
      case Type::String: return s_ == "true";
      case Type::Null: return false;
      default: throw std::runtime_error("json get<bool> from container");
    }
  }
  std::string to_string_lenient() const {
    switch (type_) {
      case Type::String: return s_;
      case Type::Int: return std::to_string(i_);
      case Type::Double: return format_double(d_);
      case Type::Bool: return b_ ? "true" : "false";
      case Type::Null: return "";
      default: throw std::runtime_error("json get<string> from container");
    }
  }

  static std::string format_double(double v) {
    char buf[64];
    std::snprintf(buf, sizeof buf, "%g", v);
    return std::string(buf);
  }

  static void escape_str(const std::string& s, std::string& out) {
    out.push_back('"');
    for (unsigned char c : s) {
      switch (c) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\t': out += "\\t"; break;
        case '\r': out += "\\r"; break;
        case '\b': out += "\\b"; break;
        case '\f': out += "\\f"; break;
        default:
          if (c < 0x20) {
            char buf[8];
            std::snprintf(buf, sizeof buf, "\\u%04x", c);
            out += buf;
          } else
            out.push_back((char)c);
      }
    }
    out.push_back('"');
  }

  void dump_into(std::string& out) const {
    switch (type_) {
      case Type::Null:
        out += "null";
        break;
      case Type::Bool:
        out += (b_ ? "true" : "false");
        break;
      case Type::Int:
        out += std::to_string(i_);
        break;
      case Type::Double:
        out += format_double(d_);
        break;
      case Type::String:
        escape_str(s_, out);
        break;
      case Type::Array:
        out.push_back('[');
        for (std::size_t k = 0; k < arr_.size(); ++k) {
          if (k) out.push_back(',');
          arr_[k].dump_into(out);
        }
        out.push_back(']');
        break;
      case Type::Object: {
        out.push_back('{');
        bool first = true;
        for (const auto& kv : obj_) {
          if (!first) out.push_back(',');
          first = false;
          escape_str(kv.first, out);
          out.push_back(':');
          kv.second.dump_into(out);
        }
        out.push_back('}');
        break;
      }
    }
  }

  bool equals(const json& o) const {
    if (type_ != o.type_) {
      if (is_number() && o.is_number()) return as_double() == o.as_double();
      return false;
    }
    switch (type_) {
      case Type::Null:
        return true;
      case Type::Bool:
        return b_ == o.b_;
      case Type::Int:
        return i_ == o.i_;
      case Type::Double:
        return d_ == o.d_;
      case Type::String:
        return s_ == o.s_;
      case Type::Array:
        if (arr_.size() != o.arr_.size()) return false;
        for (std::size_t k = 0; k < arr_.size(); ++k)
          if (!arr_[k].equals(o.arr_[k])) return false;
        return true;
      case Type::Object:
        if (obj_.size() != o.obj_.size()) return false;
        for (const auto& kv : obj_) {
          auto it = o.obj_.find(kv.first);
          if (it == o.obj_.end() || !kv.second.equals(it->second)) return false;
        }
        return true;
    }
    return false;
  }
};

// ---- recursive-descent parser (builds json via public API) ----
struct JsonParser {
  const std::string& s;
  std::size_t i = 0;
  explicit JsonParser(const std::string& str) : s(str) {}

  void skip_ws() {
    while (i < s.size() && (s[i] == ' ' || s[i] == '\t' || s[i] == '\n' || s[i] == '\r'))
      ++i;
  }
  char peek() {
    return i < s.size() ? s[i] : '\0';
  }
  char getc() {
    if (i >= s.size()) throw std::runtime_error("json unexpected EOF");
    return s[i++];
  }
  void expect(char c) {
    if (i >= s.size() || s[i] != c)
      throw std::runtime_error(std::string("json expected ") + c);
    ++i;
  }
  void literal(const char* lit) {
    std::size_t n = std::string(lit).size();
    if (s.compare(i, n, lit) != 0) throw std::runtime_error(std::string("json bad literal ") + lit);
    i += n;
  }

  json parse_value() {
    skip_ws();
    char c = peek();
    if (c == '{') return parse_object();
    if (c == '[') return parse_array();
    if (c == '"') return json(parse_string_raw());
    if (c == 't') {
      literal("true");
      return json(true);
    }
    if (c == 'f') {
      literal("false");
      return json(false);
    }
    if (c == 'n') {
      literal("null");
      return json();
    }
    return parse_number();
  }

  json parse_object() {
    json j = json::object();
    expect('{');
    skip_ws();
    if (peek() == '}') {
      ++i;
      return j;
    }
    for (;;) {
      skip_ws();
      if (peek() != '"') throw std::runtime_error("json expected string key");
      std::string key = parse_string_raw();
      skip_ws();
      expect(':');
      json v = parse_value();
      j[key] = std::move(v);
      skip_ws();
      char c = getc();
      if (c == '}') return j;
      if (c != ',') throw std::runtime_error("json bad object");
    }
  }

  json parse_array() {
    json j = json::array();
    expect('[');
    skip_ws();
    if (peek() == ']') {
      ++i;
      return j;
    }
    for (;;) {
      json v = parse_value();
      j.push_back(std::move(v));
      skip_ws();
      char c = getc();
      if (c == ']') return j;
      if (c != ',') throw std::runtime_error("json bad array");
    }
  }

  std::string parse_string_raw() {
    expect('"');
    std::string out;
    for (;;) {
      if (i >= s.size()) throw std::runtime_error("json unterminated string");
      char c = getc();
      if (c == '"') return out;
      if (c == '\\') {
        char e = getc();
        switch (e) {
          case '"': out.push_back('"'); break;
          case '\\': out.push_back('\\'); break;
          case '/': out.push_back('/'); break;
          case 'n': out.push_back('\n'); break;
          case 't': out.push_back('\t'); break;
          case 'r': out.push_back('\r'); break;
          case 'b': out.push_back('\b'); break;
          case 'f': out.push_back('\f'); break;
          case 'u': {
            int cp = hex4();
            if (cp >= 0xD800 && cp <= 0xDBFF && i + 1 < s.size() && s[i] == '\\' && s[i + 1] == 'u') {
              i += 2;
              int lo = hex4();
              cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
            }
            append_utf8(out, cp);
            break;
          }
          default: throw std::runtime_error("json bad escape");
        }
      } else {
        out.push_back(c);
      }
    }
  }

  int hex4() {
    int v = 0;
    for (int k = 0; k < 4; ++k) {
      char c = getc();
      v <<= 4;
      if (c >= '0' && c <= '9')
        v |= c - '0';
      else if (c >= 'a' && c <= 'f')
        v |= c - 'a' + 10;
      else if (c >= 'A' && c <= 'F')
        v |= c - 'A' + 10;
      else
        throw std::runtime_error("json bad unicode escape");
    }
    return v;
  }

  static void append_utf8(std::string& out, int cp) {
    if (cp < 0x80) {
      out.push_back((char)cp);
    } else if (cp < 0x800) {
      out.push_back((char)(0xC0 | (cp >> 6)));
      out.push_back((char)(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
      out.push_back((char)(0xE0 | (cp >> 12)));
      out.push_back((char)(0x80 | ((cp >> 6) & 0x3F)));
      out.push_back((char)(0x80 | (cp & 0x3F)));
    } else {
      out.push_back((char)(0xF0 | (cp >> 18)));
      out.push_back((char)(0x80 | ((cp >> 12) & 0x3F)));
      out.push_back((char)(0x80 | ((cp >> 6) & 0x3F)));
      out.push_back((char)(0x80 | (cp & 0x3F)));
    }
  }

  json parse_number() {
    std::size_t start = i;
    bool is_float = false;
    if (peek() == '-') {
      if (i >= s.size()) throw std::runtime_error("json bad number");
      ++i;
    }
    while (i < s.size()) {
      char c = s[i];
      if (c >= '0' && c <= '9') {
        ++i;
      } else if (c == '.' || c == 'e' || c == 'E' || c == '+' || c == '-') {
        is_float = (c == '.' || c == 'e' || c == 'E');
        ++i;
      } else
        break;
    }
    std::string tok = s.substr(start, i - start);
    if (tok.empty() || tok == "-") throw std::runtime_error("json bad number");
    char* end = nullptr;
    if (is_float) {
      double v = std::strtod(tok.c_str(), &end);
      if (end && *end == 0) return json(v);
    } else {
      long long v = std::strtoll(tok.c_str(), &end, 10);
      if (end && *end == 0) return json(v);
    }
    throw std::runtime_error("json bad number");
  }
};  // struct JsonParser

// make json::parse delegate to the parser above
inline json json::parse(const std::string& s) {
  JsonParser p(s);
  json v = p.parse_value();
  p.skip_ws();
  if (p.i != s.size()) throw std::runtime_error("json trailing data");
  return v;
}

// stream input: read the whole stream and parse.
inline std::istream& operator>>(std::istream& is, json& out) {
  std::string s((std::istreambuf_iterator<char>(is)), std::istreambuf_iterator<char>());
  out = json::parse(s);
  return is;
}

// ---- get<T> specializations ----
template <> inline std::string json::get<std::string>() const { return to_string_lenient(); }
template <> inline bool json::get<bool>() const { return as_bool(); }
template <> inline int json::get<int>() const { return (int)as_int(); }
template <> inline long json::get<long>() const { return as_int(); }
template <> inline long long json::get<long long>() const { return as_int(); }
template <> inline unsigned json::get<unsigned>() const { return (unsigned)as_int(); }
template <> inline unsigned long json::get<unsigned long>() const { return (unsigned long)as_int(); }
template <> inline double json::get<double>() const { return as_double(); }
template <> inline float json::get<float>() const { return (float)as_double(); }
template <> inline json json::get<json>() const { return *this; }

}  // namespace dog

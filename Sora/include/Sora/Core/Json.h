/**
 * @file Json.h
 * @brief Owned JSON value, parser bridge, and compact writer backed by simdjson parsing.
 * @ingroup Core
 */
#pragma once

#include <simdjson.h>

#include <algorithm>
#include <charconv>
#include <cstdint>
#include <initializer_list>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace Sora {

    /** @brief Exception thrown by Sora's owned JSON value layer. */
    class JsonException : public std::runtime_error {
    public:
        explicit JsonException(std::string message) : std::runtime_error(std::move(message)) {}
    };

    /** @brief Owned mutable JSON value used by Sora serialization APIs. */
    class Json {
    public:
        using Array = std::vector<Json>;
        using Object = std::vector<std::pair<std::string, Json>>;

        Json() = default;
        Json(std::nullptr_t) noexcept : value_(std::monostate{}) {}
        Json(bool value) noexcept : value_(value) {}
        Json(const char* value) : value_(std::string(value == nullptr ? "" : value)) {}
        Json(std::string value) : value_(std::move(value)) {}
        Json(std::string_view value) : value_(std::string(value)) {}
        Json(Array value) : value_(std::move(value)) {}
        Json(Object value) : value_(std::move(value)) {}

        template<typename T>
            requires(std::is_integral_v<std::remove_cvref_t<T>> && !std::same_as<std::remove_cvref_t<T>, bool> &&
                     !std::same_as<std::remove_cvref_t<T>, char> && !std::same_as<std::remove_cvref_t<T>, char8_t> &&
                     !std::same_as<std::remove_cvref_t<T>, char16_t> &&
                     !std::same_as<std::remove_cvref_t<T>, char32_t> && !std::same_as<std::remove_cvref_t<T>, wchar_t>)
        Json(T value) noexcept {
            if constexpr (std::is_signed_v<std::remove_cvref_t<T>>) {
                value_ = static_cast<int64_t>(value);
            } else {
                value_ = static_cast<uint64_t>(value);
            }
        }

        template<typename T>
            requires std::is_floating_point_v<std::remove_cvref_t<T>>
        Json(T value) noexcept : value_(static_cast<double>(value)) {}

        Json(std::initializer_list<Json> values) {
            const bool objectLike = std::ranges::all_of(values, [](const Json& entry) {
                return entry.is_array() && entry.size() == 2 && entry.at(size_t{0}).is_string();
            });
            if (objectLike) {
                Object object;
                object.reserve(values.size());
                for (const Json& entry : values) {
                    object.emplace_back(entry.at(size_t{0}).get<std::string>(), entry.at(size_t{1}));
                }
                value_ = std::move(object);
            } else {
                value_ = Array(values.begin(), values.end());
            }
        }

        /** @brief Return a new empty JSON object. */
        [[nodiscard]] static Json object() { return Json(Object{}); }

        /** @brief Return a new object initialized from key/value brace pairs. */
        [[nodiscard]] static Json object(std::initializer_list<Json> values) { return Json(values); }

        /** @brief Return a new empty JSON array. */
        [[nodiscard]] static Json array() { return Json(Array{}); }

        /** @brief Parse UTF-8 JSON text into an owned value using simdjson. */
        [[nodiscard]] static Json parse(std::string_view text) {
            std::string owned{text};
            simdjson::dom::parser parser;
            simdjson::dom::element element{};
            if (auto error = parser.parse(owned).get(element); error != simdjson::SUCCESS) {
                throw JsonException(std::string("invalid JSON: ") + simdjson::error_message(error));
            }
            return FromSimdJson(element);
        }

        [[nodiscard]] bool is_null() const noexcept { return std::holds_alternative<std::monostate>(value_); }
        [[nodiscard]] bool is_boolean() const noexcept { return std::holds_alternative<bool>(value_); }
        [[nodiscard]] bool is_string() const noexcept { return std::holds_alternative<std::string>(value_); }
        [[nodiscard]] bool is_array() const noexcept { return std::holds_alternative<Array>(value_); }
        [[nodiscard]] bool is_object() const noexcept { return std::holds_alternative<Object>(value_); }
        [[nodiscard]] bool is_number_integer() const noexcept {
            return std::holds_alternative<int64_t>(value_) || std::holds_alternative<uint64_t>(value_);
        }
        [[nodiscard]] bool is_number() const noexcept {
            return is_number_integer() || std::holds_alternative<double>(value_);
        }

        [[nodiscard]] size_t size() const noexcept {
            if (const auto* array = std::get_if<Array>(&value_)) {
                return array->size();
            }
            if (const auto* object = std::get_if<Object>(&value_)) {
                return object->size();
            }
            if (is_null()) {
                return 0;
            }
            return 1;
        }

        void clear() {
            if (auto* array = std::get_if<Array>(&value_)) {
                array->clear();
            } else if (auto* object = std::get_if<Object>(&value_)) {
                object->clear();
            } else {
                value_ = std::monostate{};
            }
        }

        template<bool Const>
        class BasicIterator {
        public:
            using Owner = std::conditional_t<Const, const Json, Json>;
            using Reference = std::conditional_t<Const, const Json&, Json&>;

            BasicIterator() = default;
            BasicIterator(Owner* owner, bool object, size_t index) noexcept
                : owner_(owner), object_(object), index_(index) {}

            Reference operator*() const { return value(); }
            auto operator->() const { return &value(); }
            BasicIterator& operator++() noexcept {
                ++index_;
                return *this;
            }
            BasicIterator operator++(int) noexcept {
                BasicIterator old = *this;
                ++*this;
                return old;
            }
            friend bool operator==(const BasicIterator& a, const BasicIterator& b) noexcept {
                return a.owner_ == b.owner_ && a.object_ == b.object_ && a.index_ == b.index_;
            }
            friend bool operator!=(const BasicIterator& a, const BasicIterator& b) noexcept { return !(a == b); }

            [[nodiscard]] std::string_view key() const {
                if (!object_) {
                    throw JsonException("JSON iterator does not reference an object member");
                }
                return object_storage()[index_].first;
            }

            [[nodiscard]] Reference value() const {
                if (object_) {
                    return object_storage()[index_].second;
                }
                return array_storage()[index_];
            }


        private:
            using ArrayRef = std::conditional_t<Const, const Array&, Array&>;
            using ObjectRef = std::conditional_t<Const, const Object&, Object&>;

            [[nodiscard]] ArrayRef array_storage() const {
                return std::get<Array>(owner_->value_);
            }
            [[nodiscard]] ObjectRef object_storage() const {
                return std::get<Object>(owner_->value_);
            }

            Owner* owner_ = nullptr;
            bool object_ = false;
            size_t index_ = 0;
        };

        using iterator = BasicIterator<false>;
        using const_iterator = BasicIterator<true>;

        [[nodiscard]] iterator begin() {
            if (is_object()) {
                return iterator(this, true, 0);
            }
            if (is_array()) {
                return iterator(this, false, 0);
            }
            throw JsonException("JSON value is not iterable");
        }
        [[nodiscard]] iterator end() {
            if (is_object()) {
                return iterator(this, true, std::get<Object>(value_).size());
            }
            if (is_array()) {
                return iterator(this, false, std::get<Array>(value_).size());
            }
            throw JsonException("JSON value is not iterable");
        }
        [[nodiscard]] const_iterator begin() const {
            if (is_object()) {
                return const_iterator(this, true, 0);
            }
            if (is_array()) {
                return const_iterator(this, false, 0);
            }
            throw JsonException("JSON value is not iterable");
        }
        [[nodiscard]] const_iterator end() const {
            if (is_object()) {
                return const_iterator(this, true, std::get<Object>(value_).size());
            }
            if (is_array()) {
                return const_iterator(this, false, std::get<Array>(value_).size());
            }
            throw JsonException("JSON value is not iterable");
        }

        [[nodiscard]] iterator find(std::string_view key) {
            Object& object = require_object();
            for (size_t i = 0; i < object.size(); ++i) {
                if (object[i].first == key) {
                    return iterator(this, true, i);
                }
            }
            return end();
        }
        [[nodiscard]] const_iterator find(std::string_view key) const {
            const Object& object = require_object();
            for (size_t i = 0; i < object.size(); ++i) {
                if (object[i].first == key) {
                    return const_iterator(this, true, i);
                }
            }
            return end();
        }

        [[nodiscard]] Json& operator[](std::string_view key) {
            if (is_null()) {
                value_ = Object{};
            }
            Object& object = require_object();
            for (auto& [name, value] : object) {
                if (name == key) {
                    return value;
                }
            }
            object.emplace_back(std::string(key), Json{});
            return object.back().second;
        }
        [[nodiscard]] const Json& operator[](std::string_view key) const { return at(key); }
        [[nodiscard]] Json& operator[](const std::string& key) { return (*this)[std::string_view(key)]; }
        [[nodiscard]] const Json& operator[](const std::string& key) const { return at(key); }
        [[nodiscard]] Json& operator[](const char* key) { return (*this)[std::string_view(key)]; }
        [[nodiscard]] const Json& operator[](const char* key) const { return at(std::string_view(key)); }

        [[nodiscard]] Json& operator[](size_t index) { return require_array().at(index); }
        [[nodiscard]] const Json& operator[](size_t index) const { return require_array().at(index); }

        [[nodiscard]] Json& at(std::string_view key) {
            Object& object = require_object();
            for (auto& [name, value] : object) {
                if (name == key) {
                    return value;
                }
            }
            throw JsonException("missing JSON object key: " + std::string(key));
        }
        [[nodiscard]] const Json& at(std::string_view key) const {
            const Object& object = require_object();
            for (const auto& [name, value] : object) {
                if (name == key) {
                    return value;
                }
            }
            throw JsonException("missing JSON object key: " + std::string(key));
        }
        [[nodiscard]] Json& at(const std::string& key) { return at(std::string_view(key)); }
        [[nodiscard]] const Json& at(const std::string& key) const { return at(std::string_view(key)); }
        [[nodiscard]] Json& at(const char* key) { return at(std::string_view(key)); }
        [[nodiscard]] const Json& at(const char* key) const { return at(std::string_view(key)); }
        [[nodiscard]] Json& at(size_t index) { return require_array().at(index); }
        [[nodiscard]] const Json& at(size_t index) const { return require_array().at(index); }

        void push_back(Json value) { require_array().push_back(std::move(value)); }

        template<typename T>
        [[nodiscard]] T get() const {
            if constexpr (std::same_as<T, Json>) {
                return *this;
            } else if constexpr (std::same_as<T, std::string>) {
                return require_string();
            } else if constexpr (std::same_as<T, bool>) {
                return require_bool();
            } else if constexpr (std::is_integral_v<T> && !std::same_as<T, bool>) {
                return checked_integer<T>();
            } else if constexpr (std::is_floating_point_v<T>) {
                return static_cast<T>(number_as_double());
            } else {
                static_assert(false, "Unsupported Json::get<T>() target type");
            }
        }

        template<typename T>
        T& get_to(T& output) const {
            output = get<T>();
            return output;
        }

        template<typename T>
        [[nodiscard]] const T& get_ref() const {
            if constexpr (std::same_as<T, const std::string&>) {
                return require_string();
            } else {
                static_assert(false, "Unsupported Json::get_ref<T>() target type");
            }
        }

        [[nodiscard]] std::string dump() const {
            std::string out;
            append_dump(out);
            return out;
        }

        friend bool operator==(const Json& a, const Json& b) noexcept { return a.value_ == b.value_; }
        friend bool operator!=(const Json& a, const Json& b) noexcept { return !(a == b); }

    private:
        using Storage = std::variant<std::monostate, bool, int64_t, uint64_t, double, std::string, Array, Object>;

        [[nodiscard]] static Json FromSimdJson(simdjson::dom::element element) {
            switch (element.type()) {
                case simdjson::dom::element_type::NULL_VALUE:
                    return Json(nullptr);
                case simdjson::dom::element_type::BOOL: {
                    bool value = false;
                    Require(element.get(value));
                    return Json(value);
                }
                case simdjson::dom::element_type::INT64: {
                    int64_t value = 0;
                    Require(element.get(value));
                    return Json(value);
                }
                case simdjson::dom::element_type::UINT64: {
                    uint64_t value = 0;
                    Require(element.get(value));
                    return Json(value);
                }
                case simdjson::dom::element_type::DOUBLE: {
                    double value = 0.0;
                    Require(element.get(value));
                    return Json(value);
                }
                case simdjson::dom::element_type::STRING: {
                    std::string_view value;
                    Require(element.get(value));
                    return Json(value);
                }
                case simdjson::dom::element_type::ARRAY: {
                    simdjson::dom::array input;
                    Require(element.get(input));
                    Array output;
                    for (simdjson::dom::element child : input) {
                        output.push_back(FromSimdJson(child));
                    }
                    return Json(std::move(output));
                }
                case simdjson::dom::element_type::BIGINT:
                    throw JsonException("JSON bigint is outside Sora::Json numeric range");
                case simdjson::dom::element_type::OBJECT: {
                    simdjson::dom::object input;
                    Require(element.get(input));
                    Object output;
                    for (auto field : input) {
                        output.emplace_back(std::string(field.key), FromSimdJson(field.value));
                    }
                    return Json(std::move(output));
                }
            }
            throw JsonException("unsupported simdjson element type");
        }

        static void Require(simdjson::error_code error) {
            if (error != simdjson::SUCCESS) {
                throw JsonException(std::string("JSON parse error: ") + simdjson::error_message(error));
            }
        }

        [[nodiscard]] Array& require_array() {
            auto* array = std::get_if<Array>(&value_);
            if (array == nullptr) {
                throw JsonException("JSON value is not an array");
            }
            return *array;
        }
        [[nodiscard]] const Array& require_array() const {
            const auto* array = std::get_if<Array>(&value_);
            if (array == nullptr) {
                throw JsonException("JSON value is not an array");
            }
            return *array;
        }
        [[nodiscard]] Object& require_object() {
            auto* object = std::get_if<Object>(&value_);
            if (object == nullptr) {
                throw JsonException("JSON value is not an object");
            }
            return *object;
        }
        [[nodiscard]] const Object& require_object() const {
            const auto* object = std::get_if<Object>(&value_);
            if (object == nullptr) {
                throw JsonException("JSON value is not an object");
            }
            return *object;
        }
        [[nodiscard]] const std::string& require_string() const {
            const auto* text = std::get_if<std::string>(&value_);
            if (text == nullptr) {
                throw JsonException("JSON value is not a string");
            }
            return *text;
        }
        [[nodiscard]] bool require_bool() const {
            const auto* value = std::get_if<bool>(&value_);
            if (value == nullptr) {
                throw JsonException("JSON value is not a boolean");
            }
            return *value;
        }

        template<typename T>
        [[nodiscard]] T checked_integer() const {
            if (const auto* value = std::get_if<int64_t>(&value_)) {
                if constexpr (std::is_unsigned_v<T>) {
                    if (*value < 0) {
                        throw JsonException("negative JSON integer cannot be converted to unsigned target");
                    }
                }
                if (*value < static_cast<int64_t>(std::numeric_limits<T>::min()) ||
                    *value > static_cast<int64_t>(std::numeric_limits<T>::max())) {
                    throw JsonException("JSON integer is out of target range");
                }
                return static_cast<T>(*value);
            }
            if (const auto* value = std::get_if<uint64_t>(&value_)) {
                if (*value > static_cast<uint64_t>(std::numeric_limits<T>::max())) {
                    throw JsonException("JSON unsigned integer is out of target range");
                }
                return static_cast<T>(*value);
            }
            throw JsonException("JSON value is not an integer");
        }

        [[nodiscard]] double number_as_double() const {
            if (const auto* value = std::get_if<double>(&value_)) {
                return *value;
            }
            if (const auto* value = std::get_if<int64_t>(&value_)) {
                return static_cast<double>(*value);
            }
            if (const auto* value = std::get_if<uint64_t>(&value_)) {
                return static_cast<double>(*value);
            }
            throw JsonException("JSON value is not a number");
        }

        void append_dump(std::string& out) const {
            if (is_null()) {
                out += "null";
            } else if (const auto* value = std::get_if<bool>(&value_)) {
                out += *value ? "true" : "false";
            } else if (const auto* value = std::get_if<int64_t>(&value_)) {
                append_number(out, *value);
            } else if (const auto* value = std::get_if<uint64_t>(&value_)) {
                append_number(out, *value);
            } else if (const auto* value = std::get_if<double>(&value_)) {
                append_number(out, *value);
            } else if (const auto* value = std::get_if<std::string>(&value_)) {
                append_string(out, *value);
            } else if (const auto* array = std::get_if<Array>(&value_)) {
                out.push_back('[');
                for (size_t i = 0; i < array->size(); ++i) {
                    if (i != 0) {
                        out.push_back(',');
                    }
                    (*array)[i].append_dump(out);
                }
                out.push_back(']');
            } else if (const auto* object = std::get_if<Object>(&value_)) {
                out.push_back('{');
                for (size_t i = 0; i < object->size(); ++i) {
                    if (i != 0) {
                        out.push_back(',');
                    }
                    append_string(out, (*object)[i].first);
                    out.push_back(':');
                    (*object)[i].second.append_dump(out);
                }
                out.push_back('}');
            }
        }

        template<typename T>
        static void append_number(std::string& out, T value) {
            char buffer[64]{};
            auto [ptr, error] = std::to_chars(buffer, buffer + sizeof(buffer), value);
            if (error != std::errc{}) {
                throw JsonException("failed to format JSON number");
            }
            out.append(buffer, ptr);
        }

        static void append_string(std::string& out, std::string_view text) {
            out.push_back('"');
            for (unsigned char c : text) {
                switch (c) {
                    case '"':
                        out += "\\\"";
                        break;
                    case '\\':
                        out += "\\\\";
                        break;
                    case '\b':
                        out += "\\b";
                        break;
                    case '\f':
                        out += "\\f";
                        break;
                    case '\n':
                        out += "\\n";
                        break;
                    case '\r':
                        out += "\\r";
                        break;
                    case '\t':
                        out += "\\t";
                        break;
                    default:
                        if (c < 0x20) {
                            constexpr char hex[] = "0123456789abcdef";
                            out += "\\u00";
                            out.push_back(hex[c >> 4]);
                            out.push_back(hex[c & 0x0F]);
                        } else {
                            out.push_back(static_cast<char>(c));
                        }
                        break;
                }
            }
            out.push_back('"');
        }

        Storage value_{};
    };

    /** @brief Non-owning simdjson DOM view used for zero-materialization JSON-to-struct binding. */
    class JsonView {
    public:
        JsonView() = default;
        explicit JsonView(simdjson::dom::element element) noexcept : element_(element) {}

        /** @brief Iterator over an array element or object field in a DOM-backed view. */
        class Iterator {
        public:
            Iterator() = default;
            Iterator(simdjson::dom::array::iterator iterator, bool object) noexcept
                : array_(iterator), object_(object) {}
            Iterator(simdjson::dom::object::iterator iterator, bool object) noexcept
                : objectIterator_(iterator), object_(object) {}

            [[nodiscard]] JsonView operator*() const noexcept { return value(); }
            [[nodiscard]] JsonView value() const noexcept {
                return object_ ? JsonView{objectIterator_.value()} : JsonView{*array_};
            }
            [[nodiscard]] std::string_view key() const {
                if (!object_) {
                    throw JsonException("JSON view iterator does not reference an object member");
                }
                return objectIterator_.key();
            }
            Iterator& operator++() noexcept {
                if (object_) {
                    ++objectIterator_;
                } else {
                    ++array_;
                }
                return *this;
            }
            Iterator operator++(int) noexcept {
                Iterator old = *this;
                ++*this;
                return old;
            }
            friend bool operator==(const Iterator& a, const Iterator& b) noexcept {
                return a.object_ == b.object_ &&
                       (a.object_ ? a.objectIterator_ == b.objectIterator_ : a.array_ == b.array_);
            }
            friend bool operator!=(const Iterator& a, const Iterator& b) noexcept { return !(a == b); }

        private:
            simdjson::dom::array::iterator array_{};
            simdjson::dom::object::iterator objectIterator_{};
            bool object_ = false;
        };

        [[nodiscard]] simdjson::dom::element Native() const noexcept { return element_; }

        [[nodiscard]] bool is_null() const noexcept {
            return element_.type() == simdjson::dom::element_type::NULL_VALUE;
        }
        [[nodiscard]] bool is_boolean() const noexcept { return element_.type() == simdjson::dom::element_type::BOOL; }
        [[nodiscard]] bool is_string() const noexcept { return element_.type() == simdjson::dom::element_type::STRING; }
        [[nodiscard]] bool is_array() const noexcept { return element_.type() == simdjson::dom::element_type::ARRAY; }
        [[nodiscard]] bool is_object() const noexcept { return element_.type() == simdjson::dom::element_type::OBJECT; }
        [[nodiscard]] bool is_number_integer() const noexcept {
            return element_.type() == simdjson::dom::element_type::INT64 ||
                   element_.type() == simdjson::dom::element_type::UINT64;
        }
        [[nodiscard]] bool is_number() const noexcept {
            return is_number_integer() || element_.type() == simdjson::dom::element_type::DOUBLE;
        }

        [[nodiscard]] size_t size() const {
            if (is_array()) {
                return array_storage().size();
            }
            if (is_object()) {
                return object_storage().size();
            }
            if (is_null()) {
                return 0;
            }
            return 1;
        }

        [[nodiscard]] Iterator begin() const {
            if (is_array()) {
                return Iterator{array_storage().begin(), false};
            }
            if (is_object()) {
                return Iterator{object_storage().begin(), true};
            }
            throw JsonException("JSON view is not iterable");
        }
        [[nodiscard]] Iterator end() const {
            if (is_array()) {
                return Iterator{array_storage().end(), false};
            }
            if (is_object()) {
                return Iterator{object_storage().end(), true};
            }
            throw JsonException("JSON view is not iterable");
        }

        [[nodiscard]] Iterator find(std::string_view key) const {
            const auto object = object_storage();
            for (auto it = object.begin(); it != object.end(); ++it) {
                if (it.key() == key) {
                    return Iterator{it, true};
                }
            }
            return Iterator{object.end(), true};
        }

        [[nodiscard]] JsonView at(std::string_view key) const {
            simdjson::dom::element child{};
            Require(object_storage().at_key(key).get(child));
            return JsonView{child};
        }
        [[nodiscard]] JsonView at(const std::string& key) const { return at(std::string_view(key)); }
        [[nodiscard]] JsonView at(const char* key) const { return at(std::string_view(key)); }
        [[nodiscard]] JsonView at(size_t index) const {
            simdjson::dom::element child{};
            Require(array_storage().at(index).get(child));
            return JsonView{child};
        }
        [[nodiscard]] JsonView operator[](std::string_view key) const { return at(key); }
        [[nodiscard]] JsonView operator[](const std::string& key) const { return at(key); }
        [[nodiscard]] JsonView operator[](const char* key) const { return at(key); }
        [[nodiscard]] JsonView operator[](size_t index) const { return at(index); }

        template<typename T>
        [[nodiscard]] T get() const {
            if constexpr (std::same_as<T, JsonView>) {
                return *this;
            } else if constexpr (std::same_as<T, std::string>) {
                std::string_view value{};
                Require(element_.get(value));
                return std::string(value);
            } else if constexpr (std::same_as<T, bool>) {
                bool value = false;
                Require(element_.get(value));
                return value;
            } else if constexpr (std::is_integral_v<T> && !std::same_as<T, bool>) {
                return checked_integer<T>();
            } else if constexpr (std::is_floating_point_v<T>) {
                double value = 0.0;
                if (element_.type() == simdjson::dom::element_type::DOUBLE) {
                    Require(element_.get(value));
                } else if (element_.type() == simdjson::dom::element_type::INT64) {
                    int64_t integer = 0;
                    Require(element_.get(integer));
                    value = static_cast<double>(integer);
                } else if (element_.type() == simdjson::dom::element_type::UINT64) {
                    uint64_t integer = 0;
                    Require(element_.get(integer));
                    value = static_cast<double>(integer);
                } else {
                    throw JsonException("JSON view value is not a number");
                }
                return static_cast<T>(value);
            } else {
                static_assert(false, "Unsupported JsonView::get<T>() target type");
            }
        }

    private:
        [[nodiscard]] simdjson::dom::array array_storage() const {
            simdjson::dom::array array{};
            Require(element_.get(array));
            return array;
        }
        [[nodiscard]] simdjson::dom::object object_storage() const {
            simdjson::dom::object object{};
            Require(element_.get(object));
            return object;
        }

        template<typename T>
        [[nodiscard]] T checked_integer() const {
            if (element_.type() == simdjson::dom::element_type::INT64) {
                int64_t value = 0;
                Require(element_.get(value));
                if constexpr (std::is_unsigned_v<T>) {
                    if (value < 0) {
                        throw JsonException("negative JSON integer cannot be converted to unsigned target");
                    }
                }
                if (value < static_cast<int64_t>(std::numeric_limits<T>::min()) ||
                    value > static_cast<int64_t>(std::numeric_limits<T>::max())) {
                    throw JsonException("JSON integer is out of target range");
                }
                return static_cast<T>(value);
            }
            if (element_.type() == simdjson::dom::element_type::UINT64) {
                uint64_t value = 0;
                Require(element_.get(value));
                if (value > static_cast<uint64_t>(std::numeric_limits<T>::max())) {
                    throw JsonException("JSON unsigned integer is out of target range");
                }
                return static_cast<T>(value);
            }
            throw JsonException("JSON view value is not an integer");
        }

        static void Require(simdjson::error_code error) {
            if (error != simdjson::SUCCESS) {
                throw JsonException(std::string("JSON view error: ") + simdjson::error_message(error));
            }
        }

        simdjson::dom::element element_{};
    };

    /** @brief Owning simdjson DOM parser whose root can be passed to @ref FromJson without materializing Sora::Json. */
    class JsonParser {
    public:
        JsonParser() = default;
        JsonParser(const JsonParser&) = delete;
        JsonParser& operator=(const JsonParser&) = delete;

        [[nodiscard]] JsonView Parse(std::string_view text) {
            storage_.assign(text);
            if (auto error = parser_.parse(storage_).get(root_); error != simdjson::SUCCESS) {
                throw JsonException(std::string("invalid JSON: ") + simdjson::error_message(error));
            }
            return JsonView{root_};
        }

        [[nodiscard]] JsonView Root() const noexcept { return JsonView{root_}; }

    private:
        std::string storage_{};
        simdjson::dom::parser parser_{};
        simdjson::dom::element root_{};
    };
    /** @brief Ordered JSON uses Sora's insertion-order-preserving object storage. */
    using OrderedJson = Json;

} // namespace Sora
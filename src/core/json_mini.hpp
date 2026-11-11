#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace dirlocker {

class JsonValue {
public:
    enum class Type {
        Null,
        Bool,
        Int,
        String,
        Array,
        Object
    };

    using Array = std::vector<JsonValue>;
    using Object = std::map<std::string, JsonValue>;

    JsonValue() = default;
    JsonValue(std::nullptr_t) : type_(Type::Null) {}
    JsonValue(bool b) : type_(Type::Bool), bool_val_(b) {}
    JsonValue(int i) : type_(Type::Int), int_val_(i) {}
    JsonValue(int64_t i) : type_(Type::Int), int_val_(i) {}
    JsonValue(uint32_t i) : type_(Type::Int), int_val_(static_cast<int64_t>(i)) {}
    JsonValue(const char* s) : type_(Type::String), str_val_(s ? s : "") {}
    JsonValue(std::string s) : type_(Type::String), str_val_(std::move(s)) {}
    JsonValue(Array arr) : type_(Type::Array), arr_val_(std::move(arr)) {}
    JsonValue(Object obj) : type_(Type::Object), obj_val_(std::move(obj)) {}

    static JsonValue object() { return JsonValue(Object{}); }
    static JsonValue array() { return JsonValue(Array{}); }

    Type type() const { return type_; }
    bool is_null() const { return type_ == Type::Null; }
    bool is_bool() const { return type_ == Type::Bool; }
    bool is_int() const { return type_ == Type::Int; }
    bool is_string() const { return type_ == Type::String; }
    bool is_array() const { return type_ == Type::Array; }
    bool is_object() const { return type_ == Type::Object; }

    bool as_bool(bool def = false) const { return is_bool() ? bool_val_ : def; }
    int64_t as_int(int64_t def = 0) const { return is_int() ? int_val_ : def; }
    const std::string& as_string() const {
        static const std::string empty;
        return is_string() ? str_val_ : empty;
    }
    std::string as_string_or(const std::string& def) const {
        return is_string() ? str_val_ : def;
    }
    const Array& as_array() const {
        static const Array empty;
        return is_array() ? arr_val_ : empty;
    }
    Array& as_array_mut() { return arr_val_; }
    const Object& as_object() const {
        static const Object empty;
        return is_object() ? obj_val_ : empty;
    }
    Object& as_object_mut() { return obj_val_; }

    bool has(const std::string& key) const {
        return is_object() && obj_val_.find(key) != obj_val_.end();
    }

    const JsonValue& get(const std::string& key) const {
        static const JsonValue null_val;
        if (!is_object()) return null_val;
        auto it = obj_val_.find(key);
        return it != obj_val_.end() ? it->second : null_val;
    }

    void set(const std::string& key, JsonValue val) {
        if (type_ != Type::Object) {
            type_ = Type::Object;
            obj_val_.clear();
        }
        obj_val_[key] = std::move(val);
    }

    void push_back(JsonValue val) {
        if (type_ != Type::Array) {
            type_ = Type::Array;
            arr_val_.clear();
        }
        arr_val_.push_back(std::move(val));
    }

    std::string dump(int indent = -1) const;
    static bool parse(const std::string& input, JsonValue& out, std::string* err = nullptr);

private:
    Type type_ = Type::Null;
    bool bool_val_ = false;
    int64_t int_val_ = 0;
    std::string str_val_;
    Array arr_val_;
    Object obj_val_;
};

}  // namespace dirlocker


#include "vm/tuple.hpp"

#include <cstdint>
#include <cstring>
#include <stdexcept>

namespace db::vm {

namespace {

void appendU32(std::string& out, std::uint32_t v) {
    char buf[4];
    std::memcpy(buf, &v, sizeof(v));
    out.append(buf, sizeof(buf));
}

void appendI64(std::string& out, std::int64_t v) {
    char buf[8];
    std::memcpy(buf, &v, sizeof(v));
    out.append(buf, sizeof(buf));
}

void appendF64(std::string& out, double v) {
    char buf[8];
    std::memcpy(buf, &v, sizeof(v));
    out.append(buf, sizeof(buf));
}

std::uint32_t readU32(const std::string& in, std::size_t& pos) {
    if (pos + 4 > in.size()) throw std::runtime_error("tuple: truncated u32");
    std::uint32_t v;
    std::memcpy(&v, in.data() + pos, sizeof(v));
    pos += 4;
    return v;
}

std::int64_t readI64(const std::string& in, std::size_t& pos) {
    if (pos + 8 > in.size()) throw std::runtime_error("tuple: truncated i64");
    std::int64_t v;
    std::memcpy(&v, in.data() + pos, sizeof(v));
    pos += 8;
    return v;
}

double readF64(const std::string& in, std::size_t& pos) {
    if (pos + 8 > in.size()) throw std::runtime_error("tuple: truncated f64");
    double v;
    std::memcpy(&v, in.data() + pos, sizeof(v));
    pos += 8;
    return v;
}

}

std::string Tuple::serialize(const Schema& schema) const {
    const std::size_t n = schema.size();
    std::string out;

    const std::size_t bitmapBytes = (n + 7) / 8;
    std::string bitmap(bitmapBytes, '\0');
    for (std::size_t i = 0; i < n; ++i) {
        if (i < values_.size() && values_[i].isNull()) {
            bitmap[i / 8] |= static_cast<char>(1u << (i % 8));
        }
    }
    out.append(bitmap);

    for (std::size_t i = 0; i < n; ++i) {
        const Value& v = (i < values_.size()) ? values_[i] : Value::null();
        if (v.isNull()) {
            continue;
        }
        switch (schema[i]) {
            case parser::DataType::Int:
                appendI64(out, v.intValue);
                break;
            case parser::DataType::Float:
                appendF64(out, v.type == ValueType::Double
                                   ? v.doubleValue
                                   : static_cast<double>(v.intValue));
                break;
            case parser::DataType::Bool:
                out.push_back(v.boolValue ? '\1' : '\0');
                break;
            case parser::DataType::Text:
            case parser::DataType::Varchar:
            case parser::DataType::Date:
            case parser::DataType::Timestamp:
                appendU32(out, static_cast<std::uint32_t>(v.textValue.size()));
                out.append(v.textValue);
                break;
        }
    }
    return out;
}

Tuple Tuple::deserialize(const std::string& bytes, const Schema& schema) {
    const std::size_t n = schema.size();
    const std::size_t bitmapBytes = (n + 7) / 8;
    if (bytes.size() < bitmapBytes) {
        throw std::runtime_error("tuple: truncated null bitmap");
    }

    std::vector<Value> values;
    values.reserve(n);
    std::size_t pos = bitmapBytes;

    for (std::size_t i = 0; i < n; ++i) {
        bool isNull = (static_cast<unsigned char>(bytes[i / 8]) >> (i % 8)) & 1u;
        if (isNull) {
            values.push_back(Value::null());
            continue;
        }
        switch (schema[i]) {
            case parser::DataType::Int:
                values.push_back(Value::makeInt(readI64(bytes, pos)));
                break;
            case parser::DataType::Float:
                values.push_back(Value::makeDouble(readF64(bytes, pos)));
                break;
            case parser::DataType::Bool: {
                if (pos + 1 > bytes.size()) throw std::runtime_error("tuple: truncated bool");
                values.push_back(Value::makeBool(bytes[pos] != '\0'));
                pos += 1;
                break;
            }
            case parser::DataType::Text:
            case parser::DataType::Varchar:
            case parser::DataType::Date:
            case parser::DataType::Timestamp: {
                std::uint32_t len = readU32(bytes, pos);
                if (pos + len > bytes.size()) throw std::runtime_error("tuple: truncated text");
                values.push_back(Value::makeText(bytes.substr(pos, len)));
                pos += len;
                break;
            }
        }
    }
    return Tuple(std::move(values));
}

}

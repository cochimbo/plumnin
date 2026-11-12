#pragma once

#include <array>
#include <cstdint>
#include <cstring>
#include <string>
#include <type_traits>
#include <vector>

namespace plumnin {

template <class T>
struct IsByteArray : std::false_type {};
template <size_t N>
struct IsByteArray<std::array<uint8_t, N>> : std::true_type {};

// Every component describes its state once, in a single function used both to save and
// to load, so the two can never drift apart:
//
//   template <class Ar> void state(Ar& ar) { ar(pc, sr, ram); }
//
// Ar is StateWriter or StateReader. Values are stored little-endian with fixed sizes;
// nothing is memcpy'd as a struct, so the format does not depend on the compiler.
class StateWriter {
public:
    static constexpr bool kLoading = false;

    template <class... T>
    void operator()(T&... values) {
        (put(values), ...);
    }

    std::vector<uint8_t>& data() { return data_; }
    const std::vector<uint8_t>& data() const { return data_; }

    void raw(const void* p, size_t n) {
        const auto* b = static_cast<const uint8_t*>(p);
        data_.insert(data_.end(), b, b + n);
    }

private:
    template <class T>
    void put(T& v) {
        if constexpr (std::is_same_v<T, bool>) {
            data_.push_back(v ? 1 : 0);
        } else if constexpr (std::is_enum_v<T>) {
            auto u = static_cast<std::underlying_type_t<T>>(v);
            put(u);
        } else if constexpr (std::is_integral_v<T>) {
            using U = std::make_unsigned_t<T>;
            U u = static_cast<U>(v);
            for (size_t i = 0; i < sizeof(T); ++i) data_.push_back(static_cast<uint8_t>(u >> (8 * i)));
        } else if constexpr (std::is_same_v<T, double>) {
            uint64_t u;
            std::memcpy(&u, &v, sizeof u);
            put(u);
        } else if constexpr (std::is_same_v<T, std::vector<uint8_t>>) {
            uint32_t n = static_cast<uint32_t>(v.size());
            put(n);
            raw(v.data(), v.size());
        } else if constexpr (std::is_same_v<T, std::vector<int16_t>>) {
            uint32_t n = static_cast<uint32_t>(v.size());
            put(n);
            for (auto& e : v) put(e);
        } else if constexpr (IsByteArray<T>::value) {
            raw(v.data(), v.size());  // memory: no need to go byte by byte
        } else {
            for (auto& e : v) put(e);  // std::array and C arrays
        }
    }

    std::vector<uint8_t> data_;
};

class StateReader {
public:
    static constexpr bool kLoading = true;

    StateReader(const uint8_t* data, size_t size) : p_(data), end_(data + size) {}

    template <class... T>
    void operator()(T&... values) {
        (get(values), ...);
    }

    // False once a read ran past the end: the block was truncated or of another version.
    bool ok() const { return ok_; }
    bool atEnd() const { return p_ == end_; }
    size_t remaining() const { return static_cast<size_t>(end_ - p_); }

    bool raw(void* out, size_t n) {
        if (static_cast<size_t>(end_ - p_) < n) {
            ok_ = false;
            std::memset(out, 0, n);
            p_ = end_;
            return false;
        }
        std::memcpy(out, p_, n);
        p_ += n;
        return true;
    }

private:
    template <class T>
    void get(T& v) {
        if constexpr (std::is_same_v<T, bool>) {
            uint8_t b = 0;
            raw(&b, 1);
            v = b != 0;
        } else if constexpr (std::is_enum_v<T>) {
            std::underlying_type_t<T> u{};
            get(u);
            v = static_cast<T>(u);
        } else if constexpr (std::is_integral_v<T>) {
            uint8_t b[sizeof(T)];
            raw(b, sizeof b);
            std::make_unsigned_t<T> u = 0;
            for (size_t i = 0; i < sizeof(T); ++i) u |= static_cast<std::make_unsigned_t<T>>(b[i]) << (8 * i);
            v = static_cast<T>(u);
        } else if constexpr (std::is_same_v<T, double>) {
            uint64_t u = 0;
            get(u);
            std::memcpy(&v, &u, sizeof v);
        } else if constexpr (std::is_same_v<T, std::vector<uint8_t>>) {
            uint32_t n = 0;
            get(n);
            if (n > static_cast<size_t>(end_ - p_)) {
                ok_ = false;
                p_ = end_;
                return;
            }
            v.assign(p_, p_ + n);
            p_ += n;
        } else if constexpr (std::is_same_v<T, std::vector<int16_t>>) {
            uint32_t n = 0;
            get(n);
            if (n > static_cast<size_t>(end_ - p_) / 2) {
                ok_ = false;
                p_ = end_;
                return;
            }
            v.resize(n);
            for (auto& e : v) get(e);
        } else if constexpr (IsByteArray<T>::value) {
            raw(v.data(), v.size());
        } else {
            for (auto& e : v) get(e);
        }
    }

    const uint8_t* p_;
    const uint8_t* end_;
    bool ok_ = true;
};

}  // namespace plumnin

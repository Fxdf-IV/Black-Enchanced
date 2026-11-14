#pragma once
// Leitura e escrita little-endian sobre buffers de tamanho fixo.
// Nunca lê nem escreve fora dos limites: após o primeiro estouro, ok() fica falso para sempre.

#include <bit>
#include <cstddef>
#include <cstdint>
#include <span>

namespace netcore {

class ByteWriter {
public:
    explicit ByteWriter(std::span<uint8_t> out) : out_(out) {}

    void u8(uint8_t v) { put(&v, 1); }
    void u16(uint16_t v) {
        const uint8_t b[2] = {uint8_t(v), uint8_t(v >> 8)};
        put(b, 2);
    }
    void u32(uint32_t v) {
        const uint8_t b[4] = {uint8_t(v), uint8_t(v >> 8), uint8_t(v >> 16), uint8_t(v >> 24)};
        put(b, 4);
    }
    void i16(int16_t v) { u16(static_cast<uint16_t>(v)); }
    void f32(float v) { u32(std::bit_cast<uint32_t>(v)); }
    void bytes(std::span<const uint8_t> v) { put(v.data(), v.size()); }

    bool ok() const { return ok_; }
    std::size_t size() const { return pos_; }

private:
    void put(const uint8_t* p, std::size_t n) {
        if (!ok_ || n > out_.size() - pos_) {
            ok_ = false;
            return;
        }
        for (std::size_t i = 0; i < n; ++i) out_[pos_ + i] = p[i];
        pos_ += n;
    }

    std::span<uint8_t> out_;
    std::size_t pos_ = 0;
    bool ok_ = true;
};

class ByteReader {
public:
    explicit ByteReader(std::span<const uint8_t> in) : in_(in) {}

    uint8_t u8() {
        uint8_t b[1] = {};
        get(b, 1);
        return b[0];
    }
    uint16_t u16() {
        uint8_t b[2] = {};
        get(b, 2);
        return uint16_t(uint16_t(b[0]) | uint16_t(uint16_t(b[1]) << 8));
    }
    uint32_t u32() {
        uint8_t b[4] = {};
        get(b, 4);
        return uint32_t(b[0]) | (uint32_t(b[1]) << 8) | (uint32_t(b[2]) << 16) | (uint32_t(b[3]) << 24);
    }
    int16_t i16() { return static_cast<int16_t>(u16()); }
    float f32() { return std::bit_cast<float>(u32()); }
    std::span<const uint8_t> bytes(std::size_t n) {
        if (!ok_ || n > remaining()) {
            ok_ = false;
            return {};
        }
        auto s = in_.subspan(pos_, n);
        pos_ += n;
        return s;
    }

    bool ok() const { return ok_; }
    std::size_t remaining() const { return in_.size() - pos_; }
    bool at_end() const { return pos_ == in_.size(); }

private:
    void get(uint8_t* p, std::size_t n) {
        if (!ok_ || n > remaining()) {
            ok_ = false;
            return;
        }
        for (std::size_t i = 0; i < n; ++i) p[i] = in_[pos_ + i];
        pos_ += n;
    }

    std::span<const uint8_t> in_;
    std::size_t pos_ = 0;
    bool ok_ = true;
};

}  // namespace netcore

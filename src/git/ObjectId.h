#pragma once

#include <array>
#include <cstdint>
#include <string>

struct git_oid;

namespace gitbolt::git {

class ObjectId {
public:
    static constexpr size_t RAW_SIZE = 20;
    static constexpr size_t HEX_SIZE = 40;

    ObjectId();
    explicit ObjectId(const git_oid* oid);
    static ObjectId fromHex(const std::string& hex);

    bool isZero() const;
    std::string toHex() const;
    std::string toShortHex(size_t length = 7) const;

    const std::array<uint8_t, RAW_SIZE>& raw() const { return raw_; }

    bool operator==(const ObjectId& other) const;
    bool operator!=(const ObjectId& other) const;
    bool operator<(const ObjectId& other) const;

    struct Hash {
        size_t operator()(const ObjectId& id) const;
    };

private:
    std::array<uint8_t, RAW_SIZE> raw_{};
};

} // namespace gitbolt::git

#include "gitbolt/core/object_id.h"
#include <git2.h>
#include <algorithm>
#include <cstring>

namespace gitbolt::core {

ObjectId::ObjectId() {
    raw_.fill(0);
}

ObjectId::ObjectId(const git_oid* oid) {
    if (oid) {
        std::memcpy(raw_.data(), oid->id, RAW_SIZE);
    } else {
        raw_.fill(0);
    }
}

ObjectId ObjectId::fromHex(const std::string& hex) {
    ObjectId result;
    git_oid oid;
    if (git_oid_fromstr(&oid, hex.c_str()) == 0) {
        std::memcpy(result.raw_.data(), oid.id, RAW_SIZE);
    }
    return result;
}

bool ObjectId::isZero() const {
    return std::all_of(raw_.begin(), raw_.end(), [](uint8_t b) { return b == 0; });
}

std::string ObjectId::toHex() const {
    char buf[HEX_SIZE + 1];
    git_oid oid;
    std::memcpy(oid.id, raw_.data(), RAW_SIZE);
    git_oid_tostr(buf, sizeof(buf), &oid);
    return std::string(buf);
}

std::string ObjectId::toShortHex(size_t length) const {
    return toHex().substr(0, length);
}

bool ObjectId::operator==(const ObjectId& other) const {
    return raw_ == other.raw_;
}

bool ObjectId::operator!=(const ObjectId& other) const {
    return raw_ != other.raw_;
}

bool ObjectId::operator<(const ObjectId& other) const {
    return raw_ < other.raw_;
}

size_t ObjectId::Hash::operator()(const ObjectId& id) const {
    size_t hash = 0;
    for (size_t i = 0; i < sizeof(size_t) && i < RAW_SIZE; ++i) {
        hash |= static_cast<size_t>(id.raw_[i]) << (i * 8);
    }
    return hash;
}

} // namespace gitbolt::core

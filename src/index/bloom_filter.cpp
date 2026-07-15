#include "index/bloom_filter.hpp"

#include <cstring>

namespace db::index {

namespace {

std::uint64_t fnv1a(const void* data, std::size_t len, std::uint64_t seed) {
    std::uint64_t h = seed;
    const unsigned char* p = static_cast<const unsigned char*>(data);
    for (std::size_t i = 0; i < len; ++i) {
        h ^= p[i];
        h *= 1099511628211ULL;
    }
    return h;
}

constexpr std::uint64_t kSeed1 = 1469598103934665603ULL;
constexpr std::uint64_t kSeed2 = 1099511628211ULL;

}

BloomFilter::BloomFilter(std::size_t expectedItems, std::size_t numHashes)
    : numHashes_(numHashes == 0 ? 1 : numHashes) {
    std::size_t bits = (expectedItems == 0 ? 1 : expectedItems) * numHashes_ * 2;
    if (bits < 64) bits = 64;
    std::size_t wordCount = (bits + 63) / 64;
    numBits_ = wordCount * 64;
    words_ = std::vector<std::atomic<std::uint64_t>>(wordCount);
    for (auto& w : words_) {
        w.store(0, std::memory_order_relaxed);
    }
}

void BloomFilter::locate(const std::string& key, std::size_t i, std::size_t& word,
                         std::uint64_t& mask) const {
    std::uint64_t h1 = fnv1a(key.data(), key.size(), kSeed1);
    std::uint64_t h2 = fnv1a(key.data(), key.size(), kSeed2) | 1ULL;
    std::uint64_t bit = (h1 + i * h2) % numBits_;
    word = static_cast<std::size_t>(bit / 64);
    mask = 1ULL << (bit % 64);
}

void BloomFilter::add(const std::string& key) {
    for (std::size_t i = 0; i < numHashes_; ++i) {
        std::size_t word;
        std::uint64_t mask;
        locate(key, i, word, mask);
        words_[word].fetch_or(mask, std::memory_order_relaxed);
    }
}

bool BloomFilter::mightContain(const std::string& key) const {
    for (std::size_t i = 0; i < numHashes_; ++i) {
        std::size_t word;
        std::uint64_t mask;
        locate(key, i, word, mask);
        if ((words_[word].load(std::memory_order_relaxed) & mask) == 0) {
            return false;
        }
    }
    return true;
}

void BloomFilter::addInt(std::int64_t value) {
    add(std::string(reinterpret_cast<const char*>(&value), sizeof(value)));
}

bool BloomFilter::mightContainInt(std::int64_t value) const {
    return mightContain(std::string(reinterpret_cast<const char*>(&value), sizeof(value)));
}

}

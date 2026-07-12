#pragma once

#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

namespace db::index {

// Probabilistic set membership backed by a lock-free atomic bit array with k
// hash functions (double hashing). add() and mightContain() are thread-safe.
// mightContain() may return false positives but never false negatives, so it is
// used to skip work when a key is definitely absent.
class BloomFilter {
public:
    BloomFilter(std::size_t expectedItems, std::size_t numHashes = 4);

    void add(const std::string& key);
    bool mightContain(const std::string& key) const;

    void addInt(std::int64_t value);
    bool mightContainInt(std::int64_t value) const;

    std::size_t bitCount() const { return numBits_; }

private:
    void locate(const std::string& key, std::size_t i, std::size_t& word,
                std::uint64_t& mask) const;

    std::size_t numBits_;
    std::size_t numHashes_;
    std::vector<std::atomic<std::uint64_t>> words_;
};

}  // namespace db::index

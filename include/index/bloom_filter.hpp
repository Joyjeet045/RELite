#pragma once

#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

namespace db::index {

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

}

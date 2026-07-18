#include "vertex_set.hpp"

#include <algorithm>
#include <bit>
#include <stdexcept>

namespace cutwidth {

VertexSet::VertexSet(std::size_t vertex_count, bool full)
    : vertex_count_(vertex_count), words_((vertex_count + 63U) / 64U, full ? ~Word{0} : Word{0}) {
    if (full && !words_.empty() && vertex_count % 64U != 0)
        words_.back() = (Word{1} << (vertex_count % 64U)) - 1U;
}

bool VertexSet::contains(std::size_t vertex) const {
    if (vertex >= vertex_count_) throw std::out_of_range("vertex out of range");
    return (words_[vertex / 64U] & (Word{1} << (vertex % 64U))) != 0;
}

bool VertexSet::empty() const noexcept {
    return std::all_of(words_.begin(), words_.end(), [](Word word) { return word == 0; });
}

std::size_t VertexSet::count() const noexcept {
    std::size_t result = 0;
    for (const Word word : words_) result += static_cast<std::size_t>(std::popcount(word));
    return result;
}

void VertexSet::insert(std::size_t vertex) {
    if (vertex >= vertex_count_) throw std::out_of_range("vertex out of range");
    words_[vertex / 64U] |= Word{1} << (vertex % 64U);
}

void VertexSet::erase(std::size_t vertex) {
    if (vertex >= vertex_count_) throw std::out_of_range("vertex out of range");
    words_[vertex / 64U] &= ~(Word{1} << (vertex % 64U));
}

void VertexSet::clear() noexcept { std::fill(words_.begin(), words_.end(), Word{0}); }

std::uint64_t hash_words(std::span<const std::uint64_t> words) noexcept {
    // wyhash-style integer mixing. Equality is always checked by cache users,
    // so this is not relied on as a fingerprint.
    std::uint64_t hash = 0xa0761d6478bd642fULL ^ static_cast<std::uint64_t>(words.size());
    for (std::uint64_t word : words) {
        word ^= word >> 32U;
        word *= 0xe7037ed1a0b428dbULL;
        word ^= word >> 29U;
        hash ^= word + 0x9e3779b97f4a7c15ULL + (hash << 6U) + (hash >> 2U);
    }
    hash ^= hash >> 30U;
    hash *= 0xbf58476d1ce4e5b9ULL;
    return hash ^ (hash >> 27U);
}

} // namespace cutwidth

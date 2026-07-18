#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace cutwidth {

// A compact owning vertex subset used by the dynamic (>63 vertex) engine.
// The number of vertices is fixed at construction; unused high bits are zero.
class VertexSet {
public:
    using Word = std::uint64_t;

    explicit VertexSet(std::size_t vertex_count = 0, bool full = false);

    [[nodiscard]] std::size_t vertex_count() const noexcept { return vertex_count_; }
    [[nodiscard]] std::size_t word_count() const noexcept { return words_.size(); }
    [[nodiscard]] std::span<const Word> words() const noexcept { return words_; }
    [[nodiscard]] std::span<Word> words() noexcept { return words_; }
    [[nodiscard]] bool contains(std::size_t vertex) const;
    [[nodiscard]] bool empty() const noexcept;
    [[nodiscard]] std::size_t count() const noexcept;

    void insert(std::size_t vertex);
    void erase(std::size_t vertex);
    void clear() noexcept;

    [[nodiscard]] bool operator==(const VertexSet&) const noexcept = default;

private:
    std::size_t vertex_count_ = 0;
    std::vector<Word> words_;
};

[[nodiscard]] std::uint64_t hash_words(std::span<const std::uint64_t> words) noexcept;

} // namespace cutwidth

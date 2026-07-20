#pragma once

#include "graph.hpp"
#include "pb_encoding.hpp"

#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>
#include <cmath>
#include <limits>

#ifdef CUTWIDTH_HAVE_CADICAL
#ifdef _MSC_VER
#include "msvc_compat/cadical_msvc.hpp"
#endif
#include <cadical.hpp>
#include <tracer.hpp>
#endif

namespace cutwidth::pb {

#ifdef CUTWIDTH_HAVE_CADICAL
class InMemoryDratTracer : public CaDiCaL::Tracer {
    std::vector<std::uint8_t> &buffer;
    bool binary;
    std::size_t max_bytes;
    bool overflow = false;

    void put_byte(std::uint8_t byte) {
        if (overflow) return;
        if (buffer.size() >= max_bytes) {
            overflow = true;
            return;
        }
        buffer.push_back(byte);
    }

    void put_binary_lit(int lit) {
        unsigned idx = std::abs(lit);
        unsigned x = 2u * idx + (lit < 0);
        while (x & ~0x7f) {
            std::uint8_t ch = (x & 0x7f) | 0x80;
            put_byte(ch);
            x >>= 7;
        }
        put_byte(static_cast<std::uint8_t>(x));
    }

    void put_string(const std::string &s) {
        if (overflow) return;
        if (buffer.size() > max_bytes || s.size() > max_bytes - buffer.size()) {
            overflow = true;
            return;
        }
        buffer.insert(buffer.end(), s.begin(), s.end());
    }

    void drat_add_clause(const std::vector<int> &clause) {
        if (binary) {
            put_byte('a');
            for (int lit : clause) {
                put_binary_lit(lit);
            }
            put_byte(0);
        } else {
            for (int lit : clause) {
                put_string(std::to_string(lit) + " ");
            }
            put_string("0\n");
        }
    }

    void drat_delete_clause(const std::vector<int> &clause) {
        if (binary) {
            put_byte('d');
            for (int lit : clause) {
                put_binary_lit(lit);
            }
            put_byte(0);
        } else {
            put_string("d ");
            for (int lit : clause) {
                put_string(std::to_string(lit) + " ");
            }
            put_string("0\n");
        }
    }

public:
    InMemoryDratTracer(
        std::vector<std::uint8_t> &buf, bool bin = true,
        std::size_t limit = std::size_t{1} << 30U)
        : buffer(buf), binary(bin), max_bytes(limit) {}

    [[nodiscard]] bool overflowed() const noexcept { return overflow; }

    void add_derived_clause(uint64_t, bool, const std::vector<int> &clause, const std::vector<uint64_t> &) override {
        drat_add_clause(clause);
    }

    void delete_clause(uint64_t, bool, const std::vector<int> &clause) override {
        drat_delete_clause(clause);
    }
};
#endif

enum class IncrementalStatus { sat, unsat_exploratory, timed_out, unavailable, error };

struct IncrementalResult {
    IncrementalStatus status = IncrementalStatus::unavailable;
    std::vector<std::int8_t> assignment;
    std::string diagnostic;
    double runtime_seconds = 0.0;
    std::string proof_path;
    std::vector<std::int32_t> added_unit_clauses;
    std::string proof_backend;
    std::string proof_provenance;
    std::vector<std::uint8_t> proof_bytes;
    std::string proof_hash;
};

// A learned-clause-preserving CaDiCaL session. Thresholds may only tighten.
// UNSAT is intentionally exploratory: callers must independently check the
// returned in-memory DRAT proof before strengthening a lower bound.
class IncrementalCadicalSession {
public:
    IncrementalCadicalSession(
        const CutwidthCnf& encoding, std::uint32_t initial_threshold,
        const std::vector<Graph::Vertex>& phase_ordering = {},
        bool trace_proof = false, bool keep_temporary_files = false);
    ~IncrementalCadicalSession();
    IncrementalCadicalSession(IncrementalCadicalSession&&) = delete;
    IncrementalCadicalSession& operator=(IncrementalCadicalSession&&) = delete;
    IncrementalCadicalSession(const IncrementalCadicalSession&) = delete;
    IncrementalCadicalSession& operator=(const IncrementalCadicalSession&) = delete;

    [[nodiscard]] bool available() const noexcept;
    [[nodiscard]] IncrementalResult solve(
        std::uint32_t threshold, std::chrono::milliseconds time_limit);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace cutwidth::pb

#include "checkpoint.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cerrno>
#include <charconv>
#include <cstring>
#include <cstdio>
#include <fstream>
#include <iomanip>
#include <limits>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <type_traits>
#include <system_error>

#ifdef _WIN32
#include <io.h>
#define NOMINMAX
#include <windows.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

namespace cutwidth {
namespace {

constexpr std::string_view magic = "CWCP2";

class Sha256 {
public:
    void update(std::string_view input) {
        for (const unsigned char byte : input) {
            buffer_[buffer_size_++] = byte;
            bit_count_ += 8;
            if (buffer_size_ == 64) { transform(); buffer_size_ = 0; }
        }
    }
    std::string finish() {
        const auto original_bits = bit_count_;
        buffer_[buffer_size_++] = 0x80;
        if (buffer_size_ > 56) {
            while (buffer_size_ < 64) buffer_[buffer_size_++] = 0;
            transform(); buffer_size_ = 0;
        }
        while (buffer_size_ < 56) buffer_[buffer_size_++] = 0;
        for (int shift = 56; shift >= 0; shift -= 8)
            buffer_[buffer_size_++] = static_cast<std::uint8_t>(original_bits >> shift);
        transform();
        std::ostringstream out;
        out << std::hex << std::setfill('0');
        for (const auto word : state_) out << std::setw(8) << word;
        return out.str();
    }
private:
    static constexpr std::array<std::uint32_t, 64> k_{
        0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
        0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
        0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
        0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
        0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
        0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
        0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
        0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2};
    void transform() {
        std::array<std::uint32_t, 64> w{};
        for (std::size_t i = 0; i < 16; ++i)
            w[i] = (std::uint32_t(buffer_[4*i]) << 24) | (std::uint32_t(buffer_[4*i+1]) << 16) |
                   (std::uint32_t(buffer_[4*i+2]) << 8) | buffer_[4*i+3];
        for (std::size_t i = 16; i < 64; ++i) {
            const auto s0 = std::rotr(w[i-15],7) ^ std::rotr(w[i-15],18) ^ (w[i-15] >> 3);
            const auto s1 = std::rotr(w[i-2],17) ^ std::rotr(w[i-2],19) ^ (w[i-2] >> 10);
            w[i] = w[i-16] + s0 + w[i-7] + s1;
        }
        auto [a,b,c,d,e,f,g,h] = state_;
        for (std::size_t i = 0; i < 64; ++i) {
            const auto s1 = std::rotr(e,6) ^ std::rotr(e,11) ^ std::rotr(e,25);
            const auto ch = (e & f) ^ (~e & g);
            const auto t1 = h + s1 + ch + k_[i] + w[i];
            const auto s0 = std::rotr(a,2) ^ std::rotr(a,13) ^ std::rotr(a,22);
            const auto maj = (a & b) ^ (a & c) ^ (b & c);
            const auto t2 = s0 + maj;
            h=g; g=f; f=e; e=d+t1; d=c; c=b; b=a; a=t1+t2;
        }
        state_[0]+=a; state_[1]+=b; state_[2]+=c; state_[3]+=d;
        state_[4]+=e; state_[5]+=f; state_[6]+=g; state_[7]+=h;
    }
    std::array<std::uint32_t,8> state_{0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,
                                       0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19};
    std::array<std::uint8_t,64> buffer_{};
    std::size_t buffer_size_ = 0;
    std::uint64_t bit_count_ = 0;
};

template <typename T> T number(std::string_view text, std::string_view name) {
    if (text.empty() || (text.size() > 1 && text.front() == '0'))
        throw std::invalid_argument("invalid CWCP2 " + std::string(name));
    T result{};
    const auto [end, ec] = std::from_chars(text.data(), text.data()+text.size(), result);
    if (ec != std::errc{} || end != text.data()+text.size())
        throw std::invalid_argument("invalid CWCP2 " + std::string(name));
    return result;
}

double parse_double(std::string_view text, std::string_view name) {
    if (text.empty())
        throw std::invalid_argument("invalid CWCP2 double " + std::string(name));
    try {
        std::size_t processed = 0;
        double result = std::stod(std::string(text), &processed);
        if (processed != text.size())
            throw std::invalid_argument("invalid CWCP2 double " + std::string(name));
        return result;
    } catch (...) {
        throw std::invalid_argument("invalid CWCP2 double " + std::string(name));
    }
}

std::string prefixed(const std::string& text) { return std::to_string(text.size()) + ":" + text; }
std::string unprefix(std::string_view text, std::string_view name) {
    const auto colon = text.find(':');
    if (colon == text.npos) throw std::invalid_argument("invalid CWCP2 " + std::string(name));
    const auto length = number<std::size_t>(text.substr(0,colon), name);
    text.remove_prefix(colon+1);
    if (text.size() != length || text.empty())
        throw std::invalid_argument("invalid CWCP2 " + std::string(name));
    return std::string(text);
}

template <typename Range> std::string csv(const Range& values) {
    std::ostringstream out; bool first=true;
    for (const auto value : values) { if (!first) out << ','; first=false; out << value; }
    return out.str();
}
std::vector<std::uint32_t> parse_vertices(std::string_view text, std::uint32_t n,
                                         std::size_t max_count) {
    std::vector<std::uint32_t> out;
    if (text.empty()) return out;
    while (true) {
        const auto comma=text.find(',');
        const auto v=number<std::uint32_t>(text.substr(0,comma),"vertex");
        if (v>=n || out.size()>=max_count) throw std::invalid_argument("CWCP2 vertex list exceeds graph");
        out.push_back(v);
        if (comma==text.npos) break;
        text.remove_prefix(comma+1);
        if (text.empty()) throw std::invalid_argument("invalid CWCP2 vertex list");
    }
    return out;
}

std::string status_code(SessionStatus status) {
    switch(status) { case SessionStatus::unresolved:return "U"; case SessionStatus::feasible:return "F";
        case SessionStatus::infeasible:return "I"; case SessionStatus::cancelled:return "C"; }
    throw std::logic_error("unknown session status");
}
SessionStatus parse_status(std::string_view value) {
    if(value=="U")return SessionStatus::unresolved; if(value=="F")return SessionStatus::feasible;
    if(value=="I")return SessionStatus::infeasible; if(value=="C")return SessionStatus::cancelled;
    throw std::invalid_argument("invalid CWCP2 session status");
}

void emit(FILE* file, Sha256& digest, const std::string& line) {
    const auto encoded=line+"\n"; digest.update(encoded);
    if (std::fwrite(encoded.data(),1,encoded.size(),file)!=encoded.size())
        throw std::system_error(errno,std::generic_category(),"cannot write CWCP2");
}

std::string field(std::string_view line, std::string_view key) {
    if (!line.starts_with(key) || line.size()<=key.size() || line[key.size()]!='=')
        throw std::invalid_argument("expected CWCP2 field " + std::string(key));
    return std::string(line.substr(key.size()+1));
}

// Warm controller state is deliberately a length-prefixed binary image encoded
// as hexadecimal.  It keeps the line-oriented checkpoint framing strict while
// preserving cache capacities, RNG bits, and floating statistic bits exactly.
class BlobWriter {
public:
    template <class T> void pod(T value) {
        static_assert(std::is_trivially_copyable_v<T>);
        const auto* bytes = reinterpret_cast<const unsigned char*>(&value);
        data_.append(reinterpret_cast<const char*>(bytes), sizeof(T));
    }
    template <class T> void vector(const std::vector<T>& values) {
        pod<std::uint64_t>(values.size());
        if (!values.empty()) data_.append(reinterpret_cast<const char*>(values.data()),
                                          values.size() * sizeof(T));
    }
    void string(const std::string& value) {
        pod<std::uint64_t>(value.size());
        data_.append(value);
    }
    [[nodiscard]] std::string hex() const {
        static constexpr char digits[] = "0123456789abcdef";
        std::string out; out.reserve(data_.size() * 2);
        for (const unsigned char byte : data_) { out.push_back(digits[byte >> 4]); out.push_back(digits[byte & 15]); }
        return out;
    }
private: std::string data_;
};
class BlobReader {
public:
    BlobReader(std::string_view hex, std::string_view name) : name_(name) {
        if (hex.size() % 2) throw std::invalid_argument("invalid CWCP2 " + std::string(name));
        data_.reserve(hex.size() / 2);
        auto nibble = [&](char c) -> unsigned char { if (c >= '0' && c <= '9') return c - '0'; if (c >= 'a' && c <= 'f') return c - 'a' + 10; throw std::invalid_argument("invalid CWCP2 " + std::string(name)); };
        for (std::size_t i=0;i<hex.size();i+=2) data_.push_back(static_cast<char>((nibble(hex[i])<<4)|nibble(hex[i+1])));
    }
    template <class T> T pod() {
        static_assert(std::is_trivially_copyable_v<T>);
        if (data_.size() - cursor_ < sizeof(T)) throw std::invalid_argument("truncated CWCP2 " + std::string(name_));
        T value; std::memcpy(&value, data_.data()+cursor_, sizeof(T)); cursor_ += sizeof(T); return value;
    }
    template <class T> std::vector<T> vector(std::size_t max_count) {
        const auto n = pod<std::uint64_t>();
        if (n > max_count || n > (data_.size() - cursor_) / sizeof(T)) throw std::invalid_argument("CWCP2 blob exceeds projection");
        std::vector<T> out(static_cast<std::size_t>(n));
        if (n) std::memcpy(out.data(), data_.data()+cursor_, out.size()*sizeof(T));
        cursor_ += out.size()*sizeof(T); return out;
    }
    std::string string(std::size_t max_size) {
        const auto n = pod<std::uint64_t>();
        if (n > max_size || n > data_.size() - cursor_)
            throw std::invalid_argument("CWCP2 blob string exceeds projection");
        std::string out(data_.data() + cursor_, static_cast<std::size_t>(n));
        cursor_ += static_cast<std::size_t>(n);
        return out;
    }
    void finish() const { if (cursor_ != data_.size()) throw std::invalid_argument("trailing CWCP2 blob data"); }
private: std::string data_; std::size_t cursor_ = 0; std::string_view name_;
};

constexpr std::uint64_t cache_blob_magic = 0x4357434143484531ULL;
constexpr std::uint32_t cache_blob_version = 1;

// Physical layout used before replacement telemetry was added. Cache blobs
// were raw-POD images, so retain this decoder to keep existing checkpoints
// resumable instead of silently reinterpreting the enlarged current struct.
struct LegacyDecisionCacheStats {
    std::uint64_t queries = 0;
    std::uint64_t hits = 0;
    std::uint64_t inserts = 0;
    std::uint64_t strengthenings = 0;
    std::uint64_t rejected_capacity = 0;
    std::uint64_t collisions = 0;
    std::uint64_t rehashes = 0;
    std::uint64_t segment_growths = 0;
    std::uint64_t lookup_probes = 0;
    std::uint64_t insertion_probes = 0;
    std::uint64_t probes_avoided_after_saturation = 0;
    std::size_t entries = 0;
    std::size_t capacity = 0;
    std::size_t memory_bytes = 0;
    double bytes_per_state = 0.0;
    bool saturated = false;
};

DecisionCacheStats upgrade_legacy_cache_stats(
    const LegacyDecisionCacheStats& old) {
    DecisionCacheStats result;
    result.queries = old.queries;
    result.hits = old.hits;
    result.inserts = old.inserts;
    result.strengthenings = old.strengthenings;
    result.rejected_capacity = old.rejected_capacity;
    result.collisions = old.collisions;
    result.rehashes = old.rehashes;
    result.segment_growths = old.segment_growths;
    result.lookup_probes = old.lookup_probes;
    result.insertion_probes = old.insertion_probes;
    result.probes_avoided_after_saturation = old.probes_avoided_after_saturation;
    result.entries = old.entries;
    result.capacity = old.capacity;
    result.memory_bytes = old.memory_bytes;
    result.bytes_per_state = old.bytes_per_state;
    result.saturated = old.saturated;
    return result;
}

void write_cache_blob(BlobWriter& out, const ShardedFixedThresholdDynamicCacheSnapshot& cache) {
    out.pod(cache_blob_magic); out.pod(cache_blob_version);
    out.pod<std::uint64_t>(cache.word_count); out.pod(cache.threshold); out.pod<std::uint64_t>(cache.shard_count);
    out.pod<std::uint64_t>(cache.shards.size());
    for (const auto& shard : cache.shards) {
        out.pod<std::uint64_t>(shard.word_count); out.pod(shard.threshold);
        out.pod<std::uint64_t>(shard.options.max_entries); out.pod<std::uint64_t>(shard.options.max_memory_bytes);
        out.pod<std::uint8_t>(static_cast<std::uint8_t>(shard.options.replacement));
        out.pod<std::uint64_t>(shard.options.replacement_page_capacity);
        out.pod(shard.stats); out.pod<std::uint64_t>(shard.size);
        out.pod<std::uint64_t>(shard.clock_hand);
        out.pod<std::uint64_t>(shard.segments.size());
        for (const auto& segment : shard.segments) {
            out.vector(segment.control); out.vector(segment.dense_index); out.vector(segment.keys); out.vector(segment.bloom);
            out.pod<std::uint64_t>(segment.size); out.pod<std::uint64_t>(segment.control_capacity);
            out.pod<std::uint64_t>(segment.dense_index_capacity); out.pod<std::uint64_t>(segment.keys_capacity); out.pod<std::uint64_t>(segment.bloom_capacity);
            out.pod(segment.depth_sum); out.pod(segment.maximum_depth);
            out.pod<std::uint8_t>(segment.referenced ? 1 : 0);
        }
    }
}
ShardedFixedThresholdDynamicCacheSnapshot read_cache_blob(BlobReader& in, std::size_t max_bytes) {
    ShardedFixedThresholdDynamicCacheSnapshot cache;
    const auto first=in.pod<std::uint64_t>();
    const bool current=first==cache_blob_magic;
    if(current){const auto version=in.pod<std::uint32_t>();if(version!=cache_blob_version)throw std::invalid_argument("unsupported CWCP2 cache blob version");cache.word_count=static_cast<std::size_t>(in.pod<std::uint64_t>());}
    else cache.word_count=static_cast<std::size_t>(first);
    cache.threshold=in.pod<std::uint32_t>(); cache.shard_count=static_cast<std::size_t>(in.pod<std::uint64_t>());
    const auto shards=in.pod<std::uint64_t>(); if (!cache.word_count || !cache.shard_count || shards != cache.shard_count || shards > max_bytes) throw std::invalid_argument("invalid CWCP2 cache shard count");
    cache.shards.reserve(static_cast<std::size_t>(shards));
    for (std::uint64_t s=0;s<shards;++s) { FixedThresholdDynamicCacheSnapshot shard;
        shard.word_count=static_cast<std::size_t>(in.pod<std::uint64_t>()); shard.threshold=in.pod<std::uint32_t>();
        shard.options.max_entries=static_cast<std::size_t>(in.pod<std::uint64_t>()); shard.options.max_memory_bytes=static_cast<std::size_t>(in.pod<std::uint64_t>());
        if(current){const auto replacement=in.pod<std::uint8_t>();
            if (replacement > static_cast<std::uint8_t>(CacheReplacementPolicy::generational_clock)) throw std::invalid_argument("invalid CWCP2 cache replacement policy");
            shard.options.replacement=static_cast<CacheReplacementPolicy>(replacement);
            shard.options.replacement_page_capacity=static_cast<std::size_t>(in.pod<std::uint64_t>());
            shard.stats=in.pod<DecisionCacheStats>();}
        else shard.stats=upgrade_legacy_cache_stats(in.pod<LegacyDecisionCacheStats>());
        shard.size=static_cast<std::size_t>(in.pod<std::uint64_t>());
        if(current) shard.clock_hand=static_cast<std::size_t>(in.pod<std::uint64_t>());
        const auto segments=in.pod<std::uint64_t>();
        if (shard.word_count != cache.word_count || shard.threshold != cache.threshold || segments > max_bytes) throw std::invalid_argument("invalid CWCP2 cache shard identity");
        shard.segments.reserve(static_cast<std::size_t>(segments));
        for (std::uint64_t i=0;i<segments;++i) { FixedThresholdDynamicCacheSegmentSnapshot seg;
            seg.control=in.vector<std::uint8_t>(max_bytes); seg.dense_index=in.vector<std::uint32_t>(max_bytes/sizeof(std::uint32_t)); seg.keys=in.vector<std::uint64_t>(max_bytes/sizeof(std::uint64_t)); seg.bloom=in.vector<std::uint64_t>(max_bytes/sizeof(std::uint64_t));
            seg.size=static_cast<std::size_t>(in.pod<std::uint64_t>()); seg.control_capacity=static_cast<std::size_t>(in.pod<std::uint64_t>()); seg.dense_index_capacity=static_cast<std::size_t>(in.pod<std::uint64_t>()); seg.keys_capacity=static_cast<std::size_t>(in.pod<std::uint64_t>()); seg.bloom_capacity=static_cast<std::size_t>(in.pod<std::uint64_t>());
            if(current){seg.depth_sum=in.pod<std::uint64_t>(); seg.maximum_depth=in.pod<std::uint32_t>();
                const auto referenced=in.pod<std::uint8_t>(); if(referenced>1) throw std::invalid_argument("invalid CWCP2 cache reference bit"); seg.referenced=referenced!=0;}
            else {for(std::size_t dense=0;dense<seg.size;++dense){std::uint32_t depth=0;const auto offset=dense*shard.word_count;for(std::size_t word=0;word<shard.word_count;++word)depth+=std::popcount(seg.keys[offset+word]);seg.depth_sum+=depth;seg.maximum_depth=std::max(seg.maximum_depth,depth);}}
            shard.segments.push_back(std::move(seg)); }
        cache.shards.push_back(std::move(shard)); }
    return cache;
}

void write_incumbent_blob(BlobWriter& out, const IncumbentSnapshot& snapshot) {
    out.vector(snapshot.current_ordering); out.vector(snapshot.best_ordering); out.pod(snapshot.current_width); out.pod(snapshot.best_width); out.pod(snapshot.rng_state); out.pod<std::uint64_t>(snapshot.operator_cursor); out.pod<std::uint64_t>(snapshot.destroy_scale); out.pod(snapshot.stats); out.pod<std::uint8_t>(snapshot.repair ? 1 : 0);
    if (snapshot.repair) { const auto& r=*snapshot.repair; out.vector(r.kept); out.vector(r.pending); out.pod(r.vertex); out.pod<std::uint64_t>(r.next_position); out.vector(r.best); out.vector(r.best_profile); out.pod(r.best_width); }
}
IncumbentSnapshot read_incumbent_blob(BlobReader& in, std::size_t n) {
    IncumbentSnapshot out; out.current_ordering=in.vector<Graph::Vertex>(n); out.best_ordering=in.vector<Graph::Vertex>(n); out.current_width=in.pod<std::uint32_t>(); out.best_width=in.pod<std::uint32_t>(); out.rng_state=in.pod<std::uint64_t>(); out.operator_cursor=static_cast<std::size_t>(in.pod<std::uint64_t>()); out.destroy_scale=static_cast<std::size_t>(in.pod<std::uint64_t>()); out.stats=in.pod<IncumbentSessionStats>(); const auto has=in.pod<std::uint8_t>(); if (has>1) throw std::invalid_argument("invalid CWCP2 incumbent marker"); if(has) { IncumbentRepairSnapshot r; r.kept=in.vector<Graph::Vertex>(n); r.pending=in.vector<Graph::Vertex>(n); r.vertex=in.pod<Graph::Vertex>(); r.next_position=static_cast<std::size_t>(in.pod<std::uint64_t>()); r.best=in.vector<Graph::Vertex>(n); r.best_profile=in.vector<std::uint32_t>(n); r.best_width=in.pod<std::uint32_t>(); out.repair=std::move(r); } return out;
}

template <class Writer>
void write_live_generations(Writer& out,
                            const std::map<std::uint32_t, std::uint64_t>& live) {
    out.template pod<std::uint64_t>(live.size());
    for (const auto& [threshold, generation] : live) {
        out.pod(threshold); out.pod(generation);
    }
}
std::map<std::uint32_t, std::uint64_t> read_live_generations(BlobReader& in,
                                                              std::size_t maximum) {
    std::map<std::uint32_t, std::uint64_t> out;
    const auto count = in.pod<std::uint64_t>();
    if (count > maximum) throw std::invalid_argument("CWCP2 too many live arm generations");
    for (std::uint64_t i = 0; i < count; ++i) {
        const auto threshold = in.pod<std::uint32_t>();
        const auto generation = in.pod<std::uint64_t>();
        if (generation == 0 || !out.emplace(threshold, generation).second)
            throw std::invalid_argument("CWCP2 invalid live arm generation");
    }
    return out;
}
void write_sdp_blob(BlobWriter& out, const sdp::ProgressiveSdpSnapshot& snapshot) {
    write_live_generations(out, snapshot.live_generations);
    out.pod<std::uint64_t>(snapshot.tasks.size());
    for (const auto& task : snapshot.tasks) {
        out.pod(task.id.threshold); out.pod(task.id.generation);
        out.vector(task.id.prefix); out.pod<std::uint64_t>(task.id.cardinality);
        out.pod(task.accumulated_subtree_nodes); out.pod(task.existing_certified_bound);
        out.pod<std::uint8_t>(task.root ? 1 : 0);
    }
    out.pod<std::uint64_t>(snapshot.cursor);
    out.pod<std::uint64_t>(snapshot.committed_records.size());
    for (const auto& record : snapshot.committed_records) {
        out.pod(record.id.threshold); out.pod(record.id.generation);
        out.vector(record.id.prefix); out.pod<std::uint64_t>(record.id.cardinality);
        out.pod(record.certified_lower_bound); out.string(record.proof_kind);
        out.string(record.graph_hash); out.string(record.model_hash); out.string(record.backend_hash);
    }
    out.pod<std::uint8_t>(snapshot.certified_lower_bound ? 1 : 0);
    if (snapshot.certified_lower_bound) out.pod(*snapshot.certified_lower_bound);
}
sdp::ProgressiveSdpSnapshot read_sdp_blob(BlobReader& in, std::size_t n,
                                           std::size_t max_items) {
    sdp::ProgressiveSdpSnapshot out;
    out.live_generations = read_live_generations(in, max_items);
    const auto tasks = in.pod<std::uint64_t>();
    if (tasks > max_items) throw std::invalid_argument("CWCP2 too many SDP tasks");
    out.tasks.reserve(static_cast<std::size_t>(tasks));
    for (std::uint64_t i=0; i<tasks; ++i) {
        sdp::ProgressiveSdpTask task;
        task.id.threshold=in.pod<std::uint32_t>(); task.id.generation=in.pod<std::uint64_t>();
        task.id.prefix=in.vector<Graph::Mask>(n); task.id.cardinality=static_cast<std::size_t>(in.pod<std::uint64_t>());
        task.accumulated_subtree_nodes=in.pod<std::uint64_t>(); task.existing_certified_bound=in.pod<std::uint32_t>();
        const auto root=in.pod<std::uint8_t>(); if(root>1) throw std::invalid_argument("CWCP2 invalid SDP root marker"); task.root=root;
        if (task.id.generation == 0 || task.id.prefix.size() > n || task.id.cardinality > n)
            throw std::invalid_argument("CWCP2 invalid SDP task identity");
        out.tasks.push_back(std::move(task));
    }
    out.cursor=static_cast<std::size_t>(in.pod<std::uint64_t>());
    const auto records=in.pod<std::uint64_t>();
    if(records>max_items) throw std::invalid_argument("CWCP2 too many SDP records");
    out.committed_records.reserve(static_cast<std::size_t>(records));
    for(std::uint64_t i=0;i<records;++i) { sdp::ProgressiveSdpRecord record;
        record.id.threshold=in.pod<std::uint32_t>(); record.id.generation=in.pod<std::uint64_t>();
        record.id.prefix=in.vector<Graph::Mask>(n); record.id.cardinality=static_cast<std::size_t>(in.pod<std::uint64_t>());
        record.certified_lower_bound=in.pod<std::uint32_t>(); record.proof_kind=in.string(4096);
        record.graph_hash=in.string(4096); record.model_hash=in.string(4096); record.backend_hash=in.string(4096);
        if(record.id.generation==0 || record.id.prefix.size()>n || record.id.cardinality>n || record.proof_kind.empty())
            throw std::invalid_argument("CWCP2 invalid SDP record identity");
        out.committed_records.push_back(std::move(record)); }
    const auto has=in.pod<std::uint8_t>(); if(has>1) throw std::invalid_argument("CWCP2 invalid SDP lower marker");
    if(has) out.certified_lower_bound=in.pod<std::uint32_t>();
    if(out.cursor>out.tasks.size()) throw std::invalid_argument("CWCP2 invalid SDP cursor");
    return out;
}
void write_cheap_blob(BlobWriter& out, const ProgressiveCheapBoundSnapshot& snapshot) {
    write_live_generations(out, snapshot.live_generations);
    out.pod<std::uint64_t>(snapshot.tasks.size());
    for (const auto& task : snapshot.tasks) {
        out.pod(task.id.threshold); out.pod(task.id.generation); out.pod(task.id.region_id);
        out.vector(task.id.prefix);
    }
    out.pod<std::uint64_t>(snapshot.cursor);
}
ProgressiveCheapBoundSnapshot read_cheap_blob(BlobReader& in, std::size_t n,
                                              std::size_t max_items) {
    ProgressiveCheapBoundSnapshot out;
    out.live_generations=read_live_generations(in,max_items);
    const auto tasks=in.pod<std::uint64_t>(); if(tasks>max_items) throw std::invalid_argument("CWCP2 too many cheap-bound tasks");
    out.tasks.reserve(static_cast<std::size_t>(tasks));
    for(std::uint64_t i=0;i<tasks;++i) { ProgressiveCheapBoundTask task;
        task.id.threshold=in.pod<std::uint32_t>(); task.id.generation=in.pod<std::uint64_t>(); task.id.region_id=in.pod<std::uint64_t>();
        task.id.prefix=in.vector<Graph::Vertex>(n);
        std::set<Graph::Vertex> unique(task.id.prefix.begin(),task.id.prefix.end());
        if(task.id.generation==0 || task.id.region_id==0 || unique.size()!=task.id.prefix.size() || (!unique.empty() && *unique.rbegin()>=n))
            throw std::invalid_argument("CWCP2 invalid cheap-bound task identity");
        out.tasks.push_back(std::move(task)); }
    out.cursor=static_cast<std::size_t>(in.pod<std::uint64_t>());
    if(out.cursor>out.tasks.size()) throw std::invalid_argument("CWCP2 invalid cheap-bound cursor");
    return out;
}

void write_residual_dp_blob(BlobWriter& out, const ResidualDpSnapshot& snapshot) {
    out.vector(snapshot.initial_prefix);
    out.vector(snapshot.remaining);
    out.pod<std::uint8_t>(snapshot.projection ? 1 : 0);
    if (snapshot.projection) {
        const auto& projection = *snapshot.projection;
        out.pod<std::uint64_t>(projection.residual_vertices);
        out.pod<std::uint64_t>(projection.states);
        out.pod<std::uint64_t>(projection.table_bytes);
        out.pod<std::uint64_t>(projection.workspace_bytes);
        out.pod<std::uint64_t>(projection.peak_bytes);
    }
    out.vector(snapshot.table);
    out.pod<std::uint64_t>(snapshot.next_state);
    out.pod<std::uint8_t>(snapshot.applicable ? 1 : 0);
    out.pod<std::uint8_t>(snapshot.complete ? 1 : 0);
}

ResidualDpSnapshot read_residual_dp_blob(BlobReader& in, std::size_t n,
                                         std::size_t max_bytes) {
    ResidualDpSnapshot out;
    const auto word_count = (n + 63U) / 64U;
    out.initial_prefix = in.vector<Graph::Mask>(word_count);
    out.remaining = in.vector<Graph::Vertex>(n);
    const auto has_projection = in.pod<std::uint8_t>();
    if (has_projection > 1)
        throw std::invalid_argument("CWCP2 invalid residual-DP projection marker");
    if (has_projection) {
        ResidualDpProjection projection;
        projection.residual_vertices = static_cast<std::size_t>(in.pod<std::uint64_t>());
        projection.states = static_cast<std::size_t>(in.pod<std::uint64_t>());
        projection.table_bytes = static_cast<std::size_t>(in.pod<std::uint64_t>());
        projection.workspace_bytes = static_cast<std::size_t>(in.pod<std::uint64_t>());
        projection.peak_bytes = static_cast<std::size_t>(in.pod<std::uint64_t>());
        out.projection = projection;
    }
    out.table = in.vector<std::uint32_t>(max_bytes / sizeof(std::uint32_t));
    out.next_state = static_cast<std::size_t>(in.pod<std::uint64_t>());
    const auto applicable = in.pod<std::uint8_t>();
    const auto complete = in.pod<std::uint8_t>();
    if (applicable > 1 || complete > 1)
        throw std::invalid_argument("CWCP2 invalid residual-DP state marker");
    out.applicable = applicable;
    out.complete = complete;
    return out;
}

} // namespace

std::string sha256_hex(std::string_view bytes) {
    Sha256 digest;
    digest.update(bytes);
    return digest.finish();
}

void validate_adaptive_checkpoint(const AdaptiveCheckpoint& cp) {
    for (const auto* hash : {&cp.graph_hash,&cp.solver_semantic_hash,
                             &cp.proof_policy_hash,&cp.candidate_order_hash})
        if (hash->empty() || hash->size()>4096)
            throw std::invalid_argument("CWCP2 compatibility hash missing or too long");
    if (cp.lower_bound>cp.upper_bound) throw std::invalid_argument("CWCP2 interval inverted");
    if (cp.value_aware_epoch) {
        const auto& epoch = *cp.value_aware_epoch;
        if (epoch.lower_bound >= epoch.upper_bound ||
            epoch.lower_bound > cp.lower_bound ||
            epoch.upper_bound != cp.upper_bound ||
            epoch.candidates.empty())
            throw std::invalid_argument("CWCP2 value-aware epoch bounds are invalid");
        std::set<std::uint32_t> epoch_candidates;
        for (const auto candidate : epoch.candidates) {
            if (candidate < epoch.lower_bound || candidate >= epoch.upper_bound ||
                !epoch_candidates.insert(candidate).second)
                throw std::invalid_argument("CWCP2 value-aware epoch candidate is invalid");
        }
    }
    if (cp.ordering.size()!=cp.vertex_count)
        throw std::invalid_argument("CWCP2 incumbent is not complete");
    std::set<std::uint32_t> seen(cp.ordering.begin(),cp.ordering.end());
    if (seen.size()!=cp.ordering.size() || (!seen.empty() && *seen.rbegin()>=cp.vertex_count))
        throw std::invalid_argument("CWCP2 incumbent is invalid");
    std::set<std::uint32_t> thresholds;
    std::optional<std::uint32_t> previous_completed;
    for (const auto& completed : cp.completed_thresholds) {
        if (previous_completed && completed.threshold <= *previous_completed)
            throw std::invalid_argument("CWCP2 completed thresholds are not sorted");
        previous_completed = completed.threshold;
        if (completed.result == CompletedThresholdResult::feasible &&
            cp.upper_bound > completed.threshold)
            throw std::invalid_argument("CWCP2 feasible proof contradicts interval");
        if (completed.result == CompletedThresholdResult::infeasible &&
            (completed.threshold == std::numeric_limits<std::uint32_t>::max() ||
             cp.lower_bound < completed.threshold + 1))
            throw std::invalid_argument("CWCP2 infeasible proof contradicts interval");
    }
    for (const auto& session : cp.sessions) {
        if (session.controller_quantum == 0)
            throw std::invalid_argument("CWCP2 controller quantum is zero");
        if (!thresholds.insert(session.threshold).second)
            throw std::invalid_argument("CWCP2 duplicate threshold session");
        if (session.status==SessionStatus::cancelled)
            throw std::invalid_argument("CWCP2 contains cancelled session");
        if (session.status==SessionStatus::unresolved &&
            (session.threshold<cp.lower_bound || session.threshold>=cp.upper_bound))
            throw std::invalid_argument("CWCP2 unresolved session outside interval");
        const auto active = session.status == SessionStatus::unresolved &&
            !session.frames.empty() ? 1U : 0U;
        if (session.external_regions != 0 ||
            session.unfinished_regions != session.pending.size() + active)
            throw std::invalid_argument("CWCP2 session region accounting is inconsistent");
        if (session.status == SessionStatus::unresolved && session.frames.empty())
            throw std::invalid_argument("CWCP2 unresolved session has no frames");
        auto validate_path = [&](const std::vector<Graph::Vertex>& path) {
            std::set<Graph::Vertex> vertices;
            for (const auto vertex : path)
                if (vertex >= cp.vertex_count || !vertices.insert(vertex).second)
                    throw std::invalid_argument("CWCP2 continuation path is invalid");
        };
        validate_path(session.path);
        for (const auto& pending : session.pending) validate_path(pending.path);
        for (const auto& frame : session.frames) {
            if (frame.next_candidate > frame.candidates.size())
                throw std::invalid_argument("CWCP2 frame cursor exceeds candidates");
            if (frame.has_incoming && frame.incoming >= cp.vertex_count)
                throw std::invalid_argument("CWCP2 incoming vertex is invalid");
            for (const auto candidate : frame.candidates)
                if (candidate.vertex >= cp.vertex_count)
                    throw std::invalid_argument("CWCP2 candidate vertex is invalid");
        }
    }
    for (const auto& parallel : cp.parallel_sessions) {
        if (parallel.controller_quantum == 0)
            throw std::invalid_argument("CWCP2 controller quantum is zero");
        if (!thresholds.insert(parallel.threshold).second)
            throw std::invalid_argument("CWCP2 duplicate threshold session");
        if (parallel.status != SessionStatus::unresolved || parallel.regions.empty())
            throw std::invalid_argument("CWCP2 parallel session is not resumable");
        std::set<std::uint64_t> ids;
        std::map<std::uint64_t, std::uint64_t> child_counts;
        std::size_t roots = 0;
        for (const auto& region : parallel.regions) {
            if (region.region_id == 0 || !ids.insert(region.region_id).second ||
                region.session.threshold != parallel.threshold ||
                region.session.status != SessionStatus::unresolved)
                throw std::invalid_argument("CWCP2 invalid parallel region");
            if (region.parent_region_id == 0) ++roots;
            else ++child_counts[region.parent_region_id];
            std::set<Graph::Vertex> path_vertices;
            for (const auto vertex : region.session.path)
                if (vertex >= cp.vertex_count || !path_vertices.insert(vertex).second)
                    throw std::invalid_argument("CWCP2 parallel path is invalid");
            for (const auto& frame : region.session.frames) {
                if (frame.next_candidate > frame.candidates.size())
                    throw std::invalid_argument("CWCP2 parallel frame cursor is invalid");
                for (const auto candidate : frame.candidates)
                    if (candidate.vertex >= cp.vertex_count)
                        throw std::invalid_argument("CWCP2 parallel candidate is invalid");
            }
        }
        if (roots != 1) throw std::invalid_argument("CWCP2 parallel forest has invalid root count");
        for (const auto& region : parallel.regions) {
            if (region.parent_region_id != 0 && !ids.contains(region.parent_region_id))
                throw std::invalid_argument("CWCP2 parallel region parent is missing");
            const auto active = !region.session.frames.empty() ? 1U : 0U;
            if (region.session.external_regions != child_counts[region.region_id] ||
                region.session.unfinished_regions != region.session.pending.size() +
                    active + region.session.external_regions)
                throw std::invalid_argument("CWCP2 parallel region accounting is inconsistent");
        }
        std::map<std::uint64_t, std::uint64_t> parent_of;
        for (const auto& region : parallel.regions)
            parent_of.emplace(region.region_id, region.parent_region_id);
        for (const auto& [id, unused] : parent_of) {
            (void)unused;
            auto cursor = id;
            for (std::size_t hops = 0; cursor != 0; ++hops) {
                if (hops >= parent_of.size())
                    throw std::invalid_argument("CWCP2 parallel region cycle");
                cursor = parent_of.at(cursor);
            }
        }
    }
    std::size_t projected=0;
    auto add = [&](std::size_t bytes) {
        if (bytes > std::numeric_limits<std::size_t>::max() - projected)
            throw std::invalid_argument("CWCP2 size overflow");
        projected += bytes;
    };
    for(const auto& s:cp.sessions){ add(s.path.size()*sizeof(Graph::Vertex));
        for(const auto& f:s.frames)add(f.candidates.size()*sizeof(SessionCandidateSnapshot));
        for(const auto& p:s.pending)add(p.path.size()*sizeof(Graph::Vertex)); }
    for (const auto& parallel : cp.parallel_sessions)
        for (const auto& region : parallel.regions) {
            const auto& s = region.session;
            add(s.path.size() * sizeof(Graph::Vertex));
            for (const auto& f : s.frames)
                add(f.candidates.size() * sizeof(SessionCandidateSnapshot));
            for (const auto& p : s.pending)
                add(p.path.size() * sizeof(Graph::Vertex));
        }
    if (cp.residual_dp) {
        const auto& residual = *cp.residual_dp;
        const auto word_count = (static_cast<std::size_t>(cp.vertex_count) + 63U) / 64U;
        if (residual.initial_prefix.size() != word_count)
            throw std::invalid_argument("CWCP2 residual-DP prefix has invalid word count");
        if (cp.vertex_count % 64U != 0 && !residual.initial_prefix.empty()) {
            const auto used = cp.vertex_count % 64U;
            const auto allowed = (Graph::Mask{1} << used) - 1U;
            if ((residual.initial_prefix.back() & ~allowed) != 0)
                throw std::invalid_argument("CWCP2 residual-DP prefix has invalid high bits");
        }
        std::vector<Graph::Vertex> expected_remaining;
        expected_remaining.reserve(cp.vertex_count);
        for (Graph::Vertex vertex = 0; vertex < cp.vertex_count; ++vertex)
            if ((residual.initial_prefix[vertex / 64U] &
                 (Graph::Mask{1} << (vertex % 64U))) == 0)
                expected_remaining.push_back(vertex);
        if (residual.remaining != expected_remaining)
            throw std::invalid_argument("CWCP2 residual-DP remaining set is invalid");
        if (residual.projection) {
            const auto expected = project_residual_dp(residual.remaining.size(), word_count);
            const auto& actual = *residual.projection;
            if (!expected || actual.residual_vertices != expected->residual_vertices ||
                actual.states != expected->states || actual.table_bytes != expected->table_bytes ||
                actual.workspace_bytes != expected->workspace_bytes ||
                actual.peak_bytes != expected->peak_bytes)
                throw std::invalid_argument("CWCP2 residual-DP projection is invalid");
        }
        if (!residual.applicable) {
            if (!residual.table.empty() || residual.next_state != 1 || residual.complete)
                throw std::invalid_argument("CWCP2 inapplicable residual-DP has live state");
        } else if (!residual.projection || residual.table.size() != residual.projection->states ||
                   residual.table.empty() || residual.next_state == 0 ||
                   residual.next_state > residual.table.size() ||
                   residual.complete != (residual.next_state == residual.table.size())) {
            throw std::invalid_argument("CWCP2 residual-DP table shape is invalid");
        }
        add(residual.initial_prefix.size() * sizeof(Graph::Mask));
        add(residual.remaining.size() * sizeof(Graph::Vertex));
        add(residual.table.size() * sizeof(std::uint32_t));
    }
    if(cp.declared_memory_bytes!=0 && projected>cp.declared_memory_bytes)
        throw std::invalid_argument("CWCP2 continuations exceed declared memory");
}

void validate_checkpoint_compatibility(const AdaptiveCheckpoint& cp,
                                       const CheckpointCompatibility& expected) {
    if(cp.graph_hash!=expected.graph_hash || cp.solver_semantic_hash!=expected.solver_semantic_hash ||
       cp.proof_policy_hash!=expected.proof_policy_hash || cp.candidate_order_hash!=expected.candidate_order_hash)
        throw std::invalid_argument("CWCP2 compatibility hash mismatch");
}

void write_adaptive_checkpoint_atomic(const std::filesystem::path& path,
                                      const AdaptiveCheckpoint& cp) {
    validate_adaptive_checkpoint(cp);
    if(path.empty()||path.filename().empty())throw std::invalid_argument("checkpoint path must name a file");
    const auto parent=path.has_parent_path()?path.parent_path():std::filesystem::path{"."};
    static std::atomic<std::uint64_t> seq{0};
#ifdef _WIN32
    const auto pid=static_cast<unsigned long>(GetCurrentProcessId());
#else
    const auto pid=static_cast<unsigned long>(::getpid());
#endif
    const auto temp=parent/(path.filename().string()+".tmp."+std::to_string(pid)+"."+std::to_string(seq++));
    FILE* file=std::fopen(temp.string().c_str(),"wb");
    if(!file)throw std::system_error(errno,std::generic_category(),"cannot create CWCP2 temporary");
    bool closed=false; std::error_code cleanup;
    try {
        Sha256 sha;
        emit(file,sha,std::string(magic));
        emit(file,sha,"graph_hash="+prefixed(cp.graph_hash));
        emit(file,sha,"solver_hash="+prefixed(cp.solver_semantic_hash));
        emit(file,sha,"proof_hash="+prefixed(cp.proof_policy_hash));
        emit(file,sha,"order_hash="+prefixed(cp.candidate_order_hash));
        emit(file,sha,"vertex_count="+std::to_string(cp.vertex_count));
        emit(file,sha,"memory_bytes="+std::to_string(cp.declared_memory_bytes));
        emit(file,sha,"lower_bound="+std::to_string(cp.lower_bound));
        emit(file,sha,"upper_bound="+std::to_string(cp.upper_bound));
        emit(file,sha,"elapsed_ms="+std::to_string(cp.elapsed_milliseconds));
        emit(file,sha,"ordering="+csv(cp.ordering));
        std::ostringstream completed;
        for(std::size_t i=0;i<cp.completed_thresholds.size();++i){if(i)completed<<',';
            completed<<cp.completed_thresholds[i].threshold<<':'<<
                (cp.completed_thresholds[i].result==CompletedThresholdResult::feasible?'F':'I');}
        emit(file,sha,"completed="+completed.str());
        if (cp.value_aware_epoch) {
            const auto& epoch = *cp.value_aware_epoch;
            emit(file, sha, "scheduler_epoch=" +
                std::to_string(epoch.lower_bound) + ":" +
                std::to_string(epoch.upper_bound) + ":" +
                csv(epoch.candidates));
        } else {
            emit(file, sha, "scheduler_epoch=-");
        }
        if (cp.incumbent) {
            BlobWriter blob;
            write_incumbent_blob(blob, *cp.incumbent);
            emit(file,sha,"incumbent="+blob.hex());
        } else {
            emit(file,sha,"incumbent=-");
        }
        emit(file,sha,"session_count="+std::to_string(cp.sessions.size()));
        for(const auto& s:cp.sessions){
            std::string line = "session="+std::to_string(s.threshold)+":"+status_code(s.status)+":"+
                std::to_string(s.unfinished_regions)+":"+std::to_string(s.external_regions)+":"+
                (s.continuation_partitioned?"1":"0")+":"+
                std::to_string(s.frames.size())+":"+std::to_string(s.pending.size())+":"+
                std::to_string(s.controller_quantum)+":"+
                std::to_string(s.controller_services)+":"+
                std::to_string(s.session_generation);
            auto it = cp.session_telemetry.find(s.threshold);
            if (it != cp.session_telemetry.end() && it->second.has_telemetry) {
                line += ":" + std::to_string(it->second.nodes) + ":" +
                        std::to_string(it->second.busy_seconds) + ":" +
                        std::to_string(it->second.allocated_seconds);
            }
            emit(file,sha,line);
            emit(file,sha,"path="+csv(s.path)); emit(file,sha,"witness="+csv(s.ordering));
            for(const auto& f:s.frames){std::ostringstream fline; fline<<"frame="<<f.cut<<':'
                <<(f.has_incoming?std::to_string(f.incoming):"-")<<':'<<(f.entered?1:0)<<':'
                <<f.next_candidate<<':'<<f.candidates.size()<<':';
                for(std::size_t i=0;i<f.candidates.size();++i){if(i)fline<<',';
                    fline<<f.candidates[i].vertex<<':'<<f.candidates[i].cut;} emit(file,sha,fline.str());}
            for(const auto& p:s.pending)emit(file,sha,"pending="+std::to_string(p.cut)+":"+csv(p.path));
        }
        emit(file,sha,"parallel_count="+std::to_string(cp.parallel_sessions.size()));
        for (const auto& parallel : cp.parallel_sessions) {
            std::string line = "parallel="+std::to_string(parallel.threshold)+":"+
                status_code(parallel.status)+":"+std::to_string(parallel.regions.size())+":"+
                std::to_string(parallel.controller_quantum)+":"+
                std::to_string(parallel.controller_services)+":"+
                std::to_string(parallel.session_generation);
            auto it = cp.session_telemetry.find(parallel.threshold);
            if (it != cp.session_telemetry.end() && it->second.has_telemetry) {
                line += ":" + std::to_string(it->second.nodes) + ":" +
                        std::to_string(it->second.busy_seconds) + ":" +
                        std::to_string(it->second.allocated_seconds);
            }
            emit(file,sha,line);
            emit(file,sha,"parallel_witness="+csv(parallel.ordering));
            if (parallel.fixed_cache) {
                BlobWriter blob;
                write_cache_blob(blob, *parallel.fixed_cache);
                emit(file,sha,"parallel_cache="+blob.hex());
            } else {
                emit(file,sha,"parallel_cache=-");
            }
            for (const auto& region : parallel.regions) {
                emit(file,sha,"region="+std::to_string(region.region_id)+":"+
                    std::to_string(region.parent_region_id));
                const auto& s=region.session;
                emit(file,sha,"region_session="+std::to_string(s.threshold)+":"+
                    status_code(s.status)+":"+std::to_string(s.unfinished_regions)+":"+
                    std::to_string(s.external_regions)+":"+
                    (s.continuation_partitioned?"1":"0")+":"+
                    std::to_string(s.frames.size())+":"+std::to_string(s.pending.size())+":"+
                    std::to_string(s.controller_quantum)+":"+
                    std::to_string(s.controller_services)+":"+
                    std::to_string(s.session_generation));
                emit(file,sha,"path="+csv(s.path)); emit(file,sha,"witness="+csv(s.ordering));
                for(const auto& f:s.frames){std::ostringstream line; line<<"frame="<<f.cut<<':'
                    <<(f.has_incoming?std::to_string(f.incoming):"-")<<':'<<(f.entered?1:0)<<':'
                    <<f.next_candidate<<':'<<f.candidates.size()<<':';
                    for(std::size_t i=0;i<f.candidates.size();++i){if(i)line<<',';
                        line<<f.candidates[i].vertex<<':'<<f.candidates[i].cut;} emit(file,sha,line.str());}
                for(const auto& p:s.pending)
                    emit(file,sha,"pending="+std::to_string(p.cut)+":"+csv(p.path));
            }
        }
        if (cp.progressive_sdp) {
            BlobWriter blob;
            write_sdp_blob(blob, *cp.progressive_sdp);
            emit(file, sha, "progressive_sdp=" + blob.hex());
        } else {
            emit(file, sha, "progressive_sdp=-");
        }
        if (cp.progressive_cheap_bounds) {
            BlobWriter blob;
            write_cheap_blob(blob, *cp.progressive_cheap_bounds);
            emit(file, sha, "progressive_cheap_bounds=" + blob.hex());
        } else {
            emit(file, sha, "progressive_cheap_bounds=-");
        }
        if (cp.residual_dp) {
            BlobWriter blob;
            write_residual_dp_blob(blob, *cp.residual_dp);
            emit(file, sha, "residual_dp=" + blob.hex());
        } else {
            emit(file, sha, "residual_dp=-");
        }
        const auto digest="digest="+sha.finish()+"\n";
        if(std::fwrite(digest.data(),1,digest.size(),file)!=digest.size()||std::fflush(file)!=0)
            throw std::system_error(errno,std::generic_category(),"cannot flush CWCP2");
#ifdef _WIN32
        if(_commit(_fileno(file))!=0)
#else
        if(::fsync(fileno(file))!=0)
#endif
            throw std::system_error(errno,std::generic_category(),"cannot sync CWCP2");
        if(std::fclose(file)!=0){closed=true;throw std::system_error(errno,std::generic_category(),"cannot close CWCP2");}
        closed=true;
#ifdef _WIN32
        if(!MoveFileExW(temp.c_str(),path.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH))
            throw std::system_error(static_cast<int>(GetLastError()),std::system_category(),"cannot replace CWCP2");
#else
        std::filesystem::rename(temp,path);
        const int directory=::open(parent.string().c_str(),O_RDONLY);
        if(directory>=0){(void)::fsync(directory);(void)::close(directory);}
#endif
    } catch(...){if(!closed)std::fclose(file);std::filesystem::remove(temp,cleanup);throw;}
}

AdaptiveCheckpoint read_adaptive_checkpoint(const std::filesystem::path& path) {
    std::ifstream in(path,std::ios::binary); if(!in)throw std::runtime_error("cannot open CWCP2");
    Sha256 sha; std::string line;
    auto next = [&](bool hash = true) {
        if (!std::getline(in, line)) throw std::invalid_argument("truncated CWCP2");
        if (hash) sha.update(line + "\n");
        return line;
    };
    if(next()!=magic)throw std::invalid_argument("unsupported adaptive checkpoint format");
    AdaptiveCheckpoint cp;
    cp.graph_hash=unprefix(field(next(),"graph_hash"),"graph_hash");
    cp.solver_semantic_hash=unprefix(field(next(),"solver_hash"),"solver_hash");
    cp.proof_policy_hash=unprefix(field(next(),"proof_hash"),"proof_hash");
    cp.candidate_order_hash=unprefix(field(next(),"order_hash"),"order_hash");
    cp.vertex_count=number<std::uint32_t>(field(next(),"vertex_count"),"vertex_count");
    cp.declared_memory_bytes=number<std::size_t>(field(next(),"memory_bytes"),"memory_bytes");
    cp.lower_bound=number<std::uint32_t>(field(next(),"lower_bound"),"lower_bound");
    cp.upper_bound=number<std::uint32_t>(field(next(),"upper_bound"),"upper_bound");
    cp.elapsed_milliseconds=number<std::uint64_t>(field(next(),"elapsed_ms"),"elapsed_ms");
    cp.ordering=parse_vertices(field(next(),"ordering"),cp.vertex_count,cp.vertex_count);
    const auto completed=field(next(),"completed");
    if(!completed.empty()){std::string_view rest=completed;while(true){const auto comma=rest.find(',');
        const auto item=rest.substr(0,comma);const auto colon=item.find(':');
        if(colon==item.npos||colon+2!=item.size())throw std::invalid_argument("invalid CWCP2 completed proof");
        cp.completed_thresholds.push_back({number<std::uint32_t>(item.substr(0,colon),"completed"),
            item.back()=='F'?CompletedThresholdResult::feasible:CompletedThresholdResult::infeasible});
        if(item.back()!='F'&&item.back()!='I')throw std::invalid_argument("invalid CWCP2 proof code");
        if (comma == rest.npos) break;
        rest.remove_prefix(comma + 1);
    }}
    // scheduler_epoch was added after the original CWCP2 format. Accept an
    // older checkpoint whose next line is incumbent= and reconstruct the
    // epoch later from its certified interval and retained sessions.
    auto epoch_or_incumbent = next();
    if (epoch_or_incumbent.starts_with("scheduler_epoch=")) {
        const auto encoded = field(epoch_or_incumbent, "scheduler_epoch");
        if (encoded != "-") {
            const auto first = encoded.find(':');
            const auto second = first == std::string::npos
                ? std::string::npos : encoded.find(':', first + 1U);
            if (first == std::string::npos || second == std::string::npos)
                throw std::invalid_argument("invalid CWCP2 value-aware epoch");
            ValueAwareEpochCheckpoint epoch;
            epoch.lower_bound = number<std::uint32_t>(
                std::string_view(encoded).substr(0, first), "epoch lower");
            epoch.upper_bound = number<std::uint32_t>(
                std::string_view(encoded).substr(first + 1U, second - first - 1U),
                "epoch upper");
            epoch.candidates = parse_vertices(
                std::string_view(encoded).substr(second + 1U),
                std::numeric_limits<std::uint32_t>::max(),
                std::numeric_limits<std::size_t>::max());
            cp.value_aware_epoch = std::move(epoch);
        }
        epoch_or_incumbent = next();
    }
    const auto incumbent_blob=field(epoch_or_incumbent,"incumbent");
    if (incumbent_blob != "-") {
        BlobReader blob(incumbent_blob, "incumbent");
        cp.incumbent=read_incumbent_blob(blob, cp.vertex_count);
        blob.finish();
    }
    const auto session_count=number<std::size_t>(field(next(),"session_count"),"session_count");
    const auto minimum_session_bytes=sizeof(SessionSnapshot);
    if(cp.declared_memory_bytes!=0 && session_count>cp.declared_memory_bytes/std::max<std::size_t>(1,minimum_session_bytes))
        throw std::invalid_argument("CWCP2 session count exceeds memory projection");
    auto parse_session = [&](std::string_view header_name) {
        const auto header=field(next(),header_name);
        std::size_t colons = 0;
        for (char c : header) if (c == ':') ++colons;
        std::size_t num_parts = (colons >= 12) ? 13 : 10;
        std::vector<std::string_view> parts(num_parts);
        std::string_view rest=header;
        for(std::size_t i=0;i<num_parts;++i){
            const auto colon=rest.find(':');
            if(i+1<num_parts&&colon==rest.npos)throw std::invalid_argument("invalid CWCP2 session header");
            parts[i]=rest.substr(0,colon);
            if(i+1<num_parts)rest.remove_prefix(colon+1);
        }
        SessionSnapshot s; s.threshold=number<std::uint32_t>(parts[0],"threshold");
        s.status=parse_status(parts[1]);s.unfinished_regions=number<std::uint64_t>(parts[2],"regions");
        s.external_regions=number<std::uint64_t>(parts[3],"external regions");
        if(parts[4]!="0"&&parts[4]!="1")throw std::invalid_argument("invalid partition flag");
        s.continuation_partitioned=parts[4]=="1";const auto frames=number<std::size_t>(parts[5],"frames");
        const auto pending=number<std::size_t>(parts[6],"pending");
        s.controller_quantum=number<std::uint64_t>(parts[7],"controller quantum");
        s.controller_services=number<std::uint64_t>(parts[8],"controller services");
        s.session_generation=number<std::uint64_t>(parts[9],"session generation");
        if (num_parts == 13) {
            SessionTelemetry t;
            t.nodes = number<std::uint64_t>(parts[10], "session nodes");
            t.busy_seconds = parse_double(parts[11], "session busy");
            t.allocated_seconds = parse_double(parts[12], "session allocated");
            t.has_telemetry = true;
            cp.session_telemetry[s.threshold] = t;
        }
        if (s.controller_quantum == 0)
            throw std::invalid_argument("CWCP2 controller quantum is zero");
        if(frames>cp.vertex_count+1 || (cp.declared_memory_bytes!=0 &&
           pending>cp.declared_memory_bytes/std::max<std::size_t>(1,sizeof(SessionPendingSnapshot))))
            throw std::invalid_argument("CWCP2 continuation count exceeds projection");
        s.path=parse_vertices(field(next(),"path"),cp.vertex_count,cp.vertex_count);
        s.ordering=parse_vertices(field(next(),"witness"),cp.vertex_count,cp.vertex_count);
        for(std::size_t fi=0;fi<frames;++fi){const auto value=field(next(),"frame");
            std::array<std::string_view,6> fp{};std::string_view rr=value;
            for(std::size_t i=0;i<6;++i){
                if(i==5){fp[i]=rr;break;}
                const auto colon=rr.find(':');if(colon==rr.npos)throw std::invalid_argument("invalid CWCP2 frame");
                fp[i]=rr.substr(0,colon);rr.remove_prefix(colon+1);}
            SessionFrameSnapshot f;f.cut=number<std::uint32_t>(fp[0],"frame cut");f.has_incoming=fp[1]!="-";
            if(f.has_incoming)f.incoming=number<Graph::Vertex>(fp[1],"incoming");
            if (fp[2] != "0" && fp[2] != "1")
                throw std::invalid_argument("invalid entered flag");
            f.entered = fp[2] == "1";
            f.next_candidate=number<std::size_t>(fp[3],"cursor");const auto count=number<std::size_t>(fp[4],"candidate count");
            std::string_view candidates=fp[5];if(count>cp.vertex_count)throw std::invalid_argument("too many CWCP2 candidates");
            for(std::size_t ci=0;ci<count;++ci){const auto comma=candidates.find(',');const auto item=candidates.substr(0,comma);
                const auto colon=item.find(':');if(colon==item.npos)throw std::invalid_argument("invalid CWCP2 candidate");
                f.candidates.push_back({number<Graph::Vertex>(item.substr(0,colon),"candidate"),number<std::uint32_t>(item.substr(colon+1),"cut")});
                if(ci+1<count){if(comma==candidates.npos)throw std::invalid_argument("missing CWCP2 candidate");candidates.remove_prefix(comma+1);}}
            if((count==0&&!candidates.empty())||(count!=0&&candidates.find(',')!=candidates.npos))throw std::invalid_argument("extra CWCP2 candidates");
            s.frames.push_back(std::move(f));}
        for(std::size_t pi=0;pi<pending;++pi){const auto value=field(next(),"pending");const auto colon=value.find(':');
            if (colon == value.npos)
                throw std::invalid_argument("invalid CWCP2 pending");
            SessionPendingSnapshot p;
            p.cut=number<std::uint32_t>(std::string_view(value).substr(0,colon),"pending cut");
            p.path=parse_vertices(std::string_view(value).substr(colon+1),cp.vertex_count,cp.vertex_count);s.pending.push_back(std::move(p));}
        return s;
    };
    cp.sessions.reserve(session_count);
    for(std::size_t si=0;si<session_count;++si)
        cp.sessions.push_back(parse_session("session"));
    const auto parallel_count=number<std::size_t>(
        field(next(),"parallel_count"),"parallel_count");
    if(cp.declared_memory_bytes!=0 && parallel_count>
       cp.declared_memory_bytes/std::max<std::size_t>(1,sizeof(ParallelDecisionSnapshot)))
        throw std::invalid_argument("CWCP2 parallel session count exceeds memory projection");
    cp.parallel_sessions.reserve(parallel_count);
    for (std::size_t pi=0; pi<parallel_count; ++pi) {
        const auto header=field(next(),"parallel");
        std::size_t colons = 0;
        for (char c : header) if (c == ':') ++colons;
        std::size_t num_parts = (colons >= 8) ? 9 : 6;
        std::vector<std::string_view> parts(num_parts);
        std::string_view header_rest=header;
        for (std::size_t i=0;i<num_parts;++i) {
            const auto colon=header_rest.find(':');
            if (i+1<num_parts && colon==header_rest.npos)
                throw std::invalid_argument("invalid CWCP2 parallel header");
            parts[i]=header_rest.substr(0,colon);
            if (i+1<num_parts) header_rest.remove_prefix(colon+1);
        }
        ParallelDecisionSnapshot parallel;
        parallel.threshold=number<std::uint32_t>(parts[0],"parallel threshold");
        parallel.status=parse_status(parts[1]);
        const auto regions=number<std::size_t>(parts[2],"region count");
        parallel.controller_quantum=number<std::uint64_t>(parts[3],"controller quantum");
        parallel.controller_services=number<std::uint64_t>(parts[4],"controller services");
        parallel.session_generation=number<std::uint64_t>(parts[5],"session generation");
        if (num_parts == 9) {
            SessionTelemetry t;
            t.nodes = number<std::uint64_t>(parts[6], "parallel nodes");
            t.busy_seconds = parse_double(parts[7], "parallel busy");
            t.allocated_seconds = parse_double(parts[8], "parallel allocated");
            t.has_telemetry = true;
            cp.session_telemetry[parallel.threshold] = t;
        }
        if (parallel.controller_quantum == 0)
            throw std::invalid_argument("CWCP2 controller quantum is zero");
        if(cp.declared_memory_bytes!=0 && regions>
           cp.declared_memory_bytes/std::max<std::size_t>(1,sizeof(ParallelRegionSnapshot)))
            throw std::invalid_argument("CWCP2 region count exceeds memory projection");
        parallel.ordering=parse_vertices(field(next(),"parallel_witness"),cp.vertex_count,cp.vertex_count);
        const auto cache_blob=field(next(),"parallel_cache");
        if (cache_blob != "-") {
            BlobReader blob(cache_blob, "parallel cache");
            parallel.fixed_cache=read_cache_blob(blob, std::max<std::size_t>(1, cp.declared_memory_bytes));
            blob.finish();
        }
        parallel.regions.reserve(regions);
        for (std::size_t ri=0; ri<regions; ++ri) {
            const auto identity=field(next(),"region");const auto colon=identity.find(':');
            if(colon==identity.npos)throw std::invalid_argument("invalid CWCP2 region identity");
            ParallelRegionSnapshot region;
            region.region_id=number<std::uint64_t>(std::string_view(identity).substr(0,colon),"region id");
            region.parent_region_id=number<std::uint64_t>(std::string_view(identity).substr(colon+1),"parent region id");
            region.session=parse_session("region_session");
            parallel.regions.push_back(std::move(region));
        }
        cp.parallel_sessions.push_back(std::move(parallel));
    }
    const auto sdp_blob = field(next(), "progressive_sdp");
    if (sdp_blob != "-") {
        BlobReader blob(sdp_blob, "progressive SDP");
        cp.progressive_sdp = read_sdp_blob(blob, cp.vertex_count,
            std::max<std::size_t>(1, cp.declared_memory_bytes));
        blob.finish();
    }
    const auto cheap_blob = field(next(), "progressive_cheap_bounds");
    if (cheap_blob != "-") {
        BlobReader blob(cheap_blob, "progressive cheap bounds");
        cp.progressive_cheap_bounds = read_cheap_blob(blob, cp.vertex_count,
            std::max<std::size_t>(1, cp.declared_memory_bytes));
        blob.finish();
    }
    const auto residual_blob = field(next(), "residual_dp");
    if (residual_blob != "-") {
        BlobReader blob(residual_blob, "residual DP");
        cp.residual_dp = read_residual_dp_blob(blob, cp.vertex_count,
            std::max<std::size_t>(1, cp.declared_memory_bytes));
        blob.finish();
    }
    const auto digest_line=next(false);if(!digest_line.starts_with("digest=")||digest_line.substr(7)!=sha.finish())
        throw std::invalid_argument("CWCP2 digest mismatch");
    if(std::getline(in,line))throw std::invalid_argument("CWCP2 trailing data");
    validate_adaptive_checkpoint(cp); return cp;
}

} // namespace cutwidth

#include "memory_governor.hpp"

#include <limits>
#include <stdexcept>
#include <string>

namespace {
void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}
}

int main() {
    cutwidth::MemoryGovernor governor(1000, 100);
    auto first = governor.try_acquire("primary", 400);
    require(first.has_value() && first->bytes() == 400, "valid lease rejected");
    require(governor.committed_for("primary") == 400, "consumer accounting wrong");
    auto second = governor.try_acquire("secondary", 501);
    require(!second, "over-budget lease accepted");
    auto third = governor.try_acquire("secondary", 500);
    require(third.has_value(), "boundary lease rejected");
    require(governor.stats().committed_lease_bytes == 900, "global accounting wrong");
    third->reset();
    require(governor.stats().committed_lease_bytes == 400, "lease release failed");
    first.reset();
    require(governor.stats().committed_lease_bytes == 0, "optional lease destruction failed");

    auto overflow = governor.try_acquire("overflow", std::numeric_limits<std::size_t>::max());
    require(!overflow && governor.stats().leases_rejected == 2,
            "overflow was not a hard allocation rejection");

    cutwidth::MemoryGovernor unlimited(0, 0);
    auto lease = unlimited.try_acquire("cache", 1024);
    require(lease.has_value(), "unlimited compatibility budget rejected lease");
}

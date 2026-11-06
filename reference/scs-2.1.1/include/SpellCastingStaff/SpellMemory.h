#pragma once
#include <cstdint>
#include <mutex>
#include <unordered_map>
#include <utility>
#include <vector>

namespace SpellCastingStaff {
// Base FormIDs, not inventory instances. The registry can include addon staffs.
// Never hold the lock across engine calls, UI calls or SKSE serialization calls.
class SpellMemory {
public:
    using ID = std::uint32_t;
    ID Get(ID staff) const {
        std::scoped_lock lock(mutex_);
        const auto it = spells_.find(staff);
        return it == spells_.end() ? 0 : it->second;
    }
    void Set(ID staff, ID spell) {
        std::scoped_lock lock(mutex_);
        spells_[staff] = spell;
    }
    void InsertIfMissing(ID staff, ID spell) {
        std::scoped_lock lock(mutex_);
        spells_.try_emplace(staff, spell);
    }
    void Erase(ID staff) {
        std::scoped_lock lock(mutex_);
        spells_.erase(staff);
    }
    void Clear() {
        std::scoped_lock lock(mutex_);
        spells_.clear();
    }
    std::vector<std::pair<ID, ID>> Snapshot() const {
        std::scoped_lock lock(mutex_);
        return {spells_.begin(), spells_.end()};
    }
private:
    mutable std::mutex mutex_;
    std::unordered_map<ID, ID> spells_;
};
}

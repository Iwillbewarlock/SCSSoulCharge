#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <unordered_map>
#include <vector>

// Which charge-spell slot belongs to which staff enchantment.
// Slots are never reused inside one save: SCS remembers staff -> SPEL in its own co-save,
// so handing a slot to another enchantment would make that memory point at the wrong spell.

namespace scssc::core
{
	using FormID = std::uint32_t;

	struct PoolEntry
	{
		FormID        enchantment{ 0 };  // 0 = slot reserved by an enchantment that no longer exists
		std::uint32_t slot{ 0 };
	};

	class PoolTable
	{
	public:
		static constexpr std::uint32_t kFormatVersion = 1;

		explicit PoolTable(std::uint32_t a_capacity);

		// Existing slot for the enchantment, or the next free one. Empty when the pool is full.
		[[nodiscard]] std::optional<std::uint32_t> Assign(FormID a_enchantment);
		[[nodiscard]] std::optional<std::uint32_t> Find(FormID a_enchantment) const;
		[[nodiscard]] std::optional<FormID>        EnchantmentAt(std::uint32_t a_slot) const;

		[[nodiscard]] std::uint32_t                Capacity() const { return _capacity; }
		[[nodiscard]] std::uint32_t                Used() const { return static_cast<std::uint32_t>(_slots.size()); }
		[[nodiscard]] const std::vector<PoolEntry>& Entries() const { return _slots; }

		void Clear();

		// Co-save payload: version, count, then (enchantment, slot) pairs, little-endian.
		[[nodiscard]] std::vector<std::uint8_t> Serialize() const;

		// Rebuilds the table. a_resolve maps a saved FormID to the current one (load order may
		// have changed) or returns empty when that form is gone; such slots stay reserved.
		// Returns false and leaves the table empty when the data is malformed.
		[[nodiscard]] bool Deserialize(std::span<const std::uint8_t>                 a_data,
			const std::function<std::optional<FormID>(FormID)>& a_resolve);

	private:
		std::uint32_t                              _capacity;
		std::vector<PoolEntry>                     _slots;  // index == slot number
		std::unordered_map<FormID, std::uint32_t>  _byEnchantment;
	};
}

#include "core/pool_table.h"


namespace scssc::core
{
	namespace
	{
		void PutU32(std::vector<std::uint8_t>& a_out, std::uint32_t a_value)
		{
			for (int shift = 0; shift < 32; shift += 8) {
				a_out.push_back(static_cast<std::uint8_t>(a_value >> shift));
			}
		}

		bool GetU32(std::span<const std::uint8_t> a_data, std::size_t& a_offset, std::uint32_t& a_value)
		{
			if (a_offset + 4 > a_data.size()) {
				return false;
			}
			a_value = 0;
			for (int i = 0; i < 4; ++i) {
				a_value |= static_cast<std::uint32_t>(a_data[a_offset + i]) << (8 * i);
			}
			a_offset += 4;
			return true;
		}
	}

	PoolTable::PoolTable(std::uint32_t a_capacity) :
		_capacity(a_capacity)
	{}

	std::optional<std::uint32_t> PoolTable::Assign(FormID a_enchantment)
	{
		if (a_enchantment == 0) {
			return std::nullopt;
		}
		if (auto found = Find(a_enchantment)) {
			return found;
		}
		if (_slots.size() >= _capacity) {
			return std::nullopt;
		}
		const auto slot = static_cast<std::uint32_t>(_slots.size());
		_slots.push_back({ a_enchantment, slot });
		_byEnchantment.emplace(a_enchantment, slot);
		return slot;
	}

	std::optional<std::uint32_t> PoolTable::Find(FormID a_enchantment) const
	{
		const auto it = _byEnchantment.find(a_enchantment);
		if (it == _byEnchantment.end()) {
			return std::nullopt;
		}
		return it->second;
	}

	std::optional<FormID> PoolTable::EnchantmentAt(std::uint32_t a_slot) const
	{
		if (a_slot >= _slots.size() || _slots[a_slot].enchantment == 0) {
			return std::nullopt;
		}
		return _slots[a_slot].enchantment;
	}

	void PoolTable::Clear()
	{
		_slots.clear();
		_byEnchantment.clear();
	}

	std::vector<std::uint8_t> PoolTable::Serialize() const
	{
		std::vector<std::uint8_t> out;
		out.reserve(8 + _slots.size() * 8);
		PutU32(out, kFormatVersion);
		PutU32(out, static_cast<std::uint32_t>(_slots.size()));
		for (const auto& entry : _slots) {
			PutU32(out, entry.enchantment);
			PutU32(out, entry.slot);
		}
		return out;
	}

	bool PoolTable::Deserialize(std::span<const std::uint8_t> a_data,
		const std::function<std::optional<FormID>(FormID)>& a_resolve)
	{
		Clear();
		std::size_t   offset = 0;
		std::uint32_t version = 0;
		std::uint32_t count = 0;
		if (!GetU32(a_data, offset, version) || version != kFormatVersion ||
			!GetU32(a_data, offset, count) || count > _capacity ||
			a_data.size() != 8 + static_cast<std::size_t>(count) * 8) {
			return false;
		}

		std::vector<PoolEntry>                    slots(count);
		std::vector<bool>                         seen(count, false);
		std::unordered_map<FormID, std::uint32_t> byEnchantment;
		for (std::uint32_t i = 0; i < count; ++i) {
			std::uint32_t saved = 0;
			std::uint32_t slot = 0;
			if (!GetU32(a_data, offset, saved) || !GetU32(a_data, offset, slot) || slot >= count || seen[slot]) {
				return false;
			}
			seen[slot] = true;
			const auto current = saved != 0 && a_resolve ? a_resolve(saved) : std::nullopt;
			const FormID enchantment = current.value_or(0);
			if (enchantment != 0 && !byEnchantment.emplace(enchantment, slot).second) {
				return false;  // two slots for one enchantment
			}
			slots[slot] = { enchantment, slot };
		}
		_slots = std::move(slots);
		_byEnchantment = std::move(byEnchantment);
		return true;
	}
}

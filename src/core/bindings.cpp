#include "core/bindings.h"

#include <algorithm>

namespace scssc::core
{
	namespace
	{
		void PutU16(std::vector<std::uint8_t>& a_out, std::uint16_t a_value)
		{
			a_out.push_back(static_cast<std::uint8_t>(a_value));
			a_out.push_back(static_cast<std::uint8_t>(a_value >> 8));
		}

		void PutU32(std::vector<std::uint8_t>& a_out, std::uint32_t a_value)
		{
			for (int shift = 0; shift < 32; shift += 8) {
				a_out.push_back(static_cast<std::uint8_t>(a_value >> shift));
			}
		}

		void PutString(std::vector<std::uint8_t>& a_out, const std::string& a_text)
		{
			const auto size = std::min(a_text.size(), BindingTable::kMaxNameBytes);
			PutU16(a_out, static_cast<std::uint16_t>(size));
			a_out.insert(a_out.end(), a_text.begin(), a_text.begin() + static_cast<std::ptrdiff_t>(size));
		}

		class Reader
		{
		public:
			explicit Reader(std::span<const std::uint8_t> a_data) :
				_data(a_data) {}

			bool U16(std::uint16_t& a_out)
			{
				if (_data.size() - _pos < 2) {
					return false;
				}
				a_out = static_cast<std::uint16_t>(_data[_pos] | (_data[_pos + 1] << 8));
				_pos += 2;
				return true;
			}

			bool U32(std::uint32_t& a_out)
			{
				if (_data.size() - _pos < 4) {
					return false;
				}
				a_out = 0;
				for (int i = 0; i < 4; ++i) {
					a_out |= static_cast<std::uint32_t>(_data[_pos + i]) << (8 * i);
				}
				_pos += 4;
				return true;
			}

			bool String(std::string& a_out)
			{
				std::uint16_t size = 0;
				if (!U16(size) || size > BindingTable::kMaxNameBytes || _data.size() - _pos < size) {
					return false;
				}
				a_out.assign(reinterpret_cast<const char*>(_data.data() + _pos), size);
				_pos += size;
				return true;
			}

			[[nodiscard]] bool AtEnd() const { return _pos == _data.size(); }

		private:
			std::span<const std::uint8_t> _data;
			std::size_t                   _pos{ 0 };
		};
	}

	ToggleResult BindingTable::Toggle(const StaffKey& a_key, FormID a_spell, const NameState& a_nameIfNew)
	{
		const auto it = _bindings.find(a_key);
		if (it == _bindings.end()) {
			_bindings.emplace(a_key, Binding{ a_spell, a_nameIfNew });
			return ToggleResult::kAssigned;
		}
		if (it->second.spell == a_spell) {
			_bindings.erase(it);
			return ToggleResult::kCleared;
		}
		it->second.spell = a_spell;
		return ToggleResult::kChanged;
	}

	const Binding* BindingTable::Find(const StaffKey& a_key) const
	{
		const auto it = _bindings.find(a_key);
		return it == _bindings.end() ? nullptr : &it->second;
	}

	Binding* BindingTable::Find(const StaffKey& a_key)
	{
		const auto it = _bindings.find(a_key);
		return it == _bindings.end() ? nullptr : &it->second;
	}

	bool BindingTable::Remove(const StaffKey& a_key)
	{
		return _bindings.erase(a_key) > 0;
	}

	bool BindingTable::Move(const StaffKey& a_from, const StaffKey& a_to)
	{
		const auto it = _bindings.find(a_from);
		if (it == _bindings.end()) {
			return false;
		}
		if (a_from == a_to) {
			return true;
		}
		auto binding = std::move(it->second);
		_bindings.erase(it);
		_bindings.insert_or_assign(a_to, std::move(binding));
		return true;
	}

	std::vector<std::uint8_t> BindingTable::Serialize() const
	{
		std::vector<std::uint8_t> out;
		PutU32(out, kFormatVersion);
		PutU32(out, static_cast<std::uint32_t>(_bindings.size()));
		for (const auto& [key, binding] : _bindings) {
			PutU32(out, key.staff);
			PutU32(out, key.uidBase);
			PutU16(out, key.uid);
			PutU32(out, binding.spell);
			PutU32(out, binding.name.hadOverride ? 1u : 0u);
			PutString(out, binding.name.original);
			PutString(out, binding.name.written);
		}
		return out;
	}

	bool BindingTable::Deserialize(std::span<const std::uint8_t> a_data,
		const std::function<std::optional<FormID>(FormID)>&      a_resolve,
		std::size_t*                                             a_dropped,
		std::size_t*                                             a_orphaned)
	{
		_bindings.clear();
		if (a_dropped) {
			*a_dropped = 0;
		}
		if (a_orphaned) {
			*a_orphaned = 0;
		}
		Reader        reader(a_data);
		std::uint32_t version = 0;
		std::uint32_t count = 0;
		if (!reader.U32(version) || version != kFormatVersion || !reader.U32(count)) {
			return false;
		}
		std::map<StaffKey, Binding> loaded;
		std::size_t                 dropped = 0;
		std::size_t                 orphaned = 0;
		for (std::uint32_t i = 0; i < count; ++i) {
			StaffKey      key;
			Binding       binding;
			std::uint32_t flags = 0;
			if (!reader.U32(key.staff) || !reader.U32(key.uidBase) || !reader.U16(key.uid) || !reader.U32(binding.spell) ||
				!reader.U32(flags) || (flags & ~1u) != 0 || !reader.String(binding.name.original) || !reader.String(binding.name.written)) {
				return false;
			}
			binding.name.hadOverride = (flags & 1u) != 0;
			const auto staff = a_resolve(key.staff);
			const auto base = a_resolve(key.uidBase);
			const auto spell = binding.spell != 0 ? a_resolve(binding.spell) : std::optional<FormID>{};
			if (!staff || !base) {
				++dropped;
				continue;
			}
			if (!spell) {
				++orphaned;
			}
			key.staff = *staff;
			key.uidBase = *base;
			binding.spell = spell.value_or(0);
			loaded.insert_or_assign(key, std::move(binding));
		}
		if (!reader.AtEnd()) {
			return false;
		}
		_bindings = std::move(loaded);
		if (a_dropped) {
			*a_dropped = dropped;
		}
		if (a_orphaned) {
			*a_orphaned = orphaned;
		}
		return true;
	}

	std::string ComposeName(std::string_view a_original, std::string_view a_spell)
	{
		std::string name;
		name.reserve(a_original.size() + a_spell.size() + 3);
		name.append(a_original).append(" [").append(a_spell).append("]");
		return name;
	}

	std::string_view SpellNameForStaff(std::string_view a_spellName)
	{
		if (a_spellName.empty() || a_spellName.back() != ')') {
			return a_spellName;
		}
		const auto open = a_spellName.rfind('(');
		if (open == std::string_view::npos || open == 0) {
			return a_spellName;
		}
		const auto note = a_spellName.substr(open);
		const bool hasDigit = note.find_first_of("0123456789") != std::string_view::npos;
		if (!hasDigit || note.find('%') == std::string_view::npos) {
			return a_spellName;
		}
		auto end = open;
		while (end > 0 && a_spellName[end - 1] == ' ') {
			--end;
		}
		return end == 0 ? a_spellName : a_spellName.substr(0, end);
	}

	std::string_view WithoutMarker(std::string_view a_name, std::string_view a_marker, bool a_ours)
	{
		if (!a_ours || a_marker.empty() || !a_name.ends_with(a_marker)) {
			return a_name;
		}
		return a_name.substr(0, a_name.size() - a_marker.size());
	}

	NameRestore DecideRestore(const NameState& a_state, std::string_view a_current)
	{
		if (a_state.written.empty() || a_current != a_state.written) {
			return NameRestore::kLeave;
		}
		return a_state.hadOverride ? NameRestore::kRestoreOriginal : NameRestore::kRemoveOverride;
	}

	NameState Rebase(const NameState& a_state, std::string_view a_current)
	{
		if (a_state.written.empty() || a_current == a_state.written) {
			return a_state;
		}
		return NameState{ true, std::string(a_current), {} };
	}
}

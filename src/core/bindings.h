#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// Which spell each individual staff carries, and what its name was before we renamed it.
// A staff is one inventory item: its record plus the engine's own ExtraUniqueID (baseID, uniqueID).

namespace scssc::core
{
	using FormID = std::uint32_t;

	struct StaffKey
	{
		FormID        staff{ 0 };   // the weapon record
		FormID        uidBase{ 0 };  // ExtraUniqueID::baseID
		std::uint16_t uid{ 0 };      // ExtraUniqueID::uniqueID

		friend auto operator<=>(const StaffKey&, const StaffKey&) = default;
	};

	// What the item's name looked like before we wrote "<original> [<spell>]", and what we wrote.
	struct NameState
	{
		bool        hadOverride{ false };  // the item already had its own name (ExtraTextDisplayData) before us
		std::string original;              // the name to give back
		std::string written;               // the last name we wrote; empty when we wrote none
	};

	struct Binding
	{
		FormID    spell{ 0 };  // 0 = orphan: the spell's plugin is gone; only the name state is kept
		NameState name;
	};

	enum class ToggleResult
	{
		kAssigned,  // the staff had no spell; now it has this one
		kChanged,   // the staff had another spell; now it has this one (name state kept)
		kCleared,   // the staff had this same spell; the binding is gone
	};

	// What to do with the item's name when a binding ends.
	enum class NameRestore
	{
		kLeave,           // someone renamed it after us (or we wrote nothing): keep their name
		kRestoreOriginal, // put the original override name back
		kRemoveOverride,  // the item had no own name before us: drop our override
	};

	class BindingTable
	{
	public:
		static constexpr std::uint32_t kFormatVersion = 1;
		static constexpr std::size_t   kMaxNameBytes = 512;  // longer saved names are treated as malformed data
		// New bindings stop here, so a table the writer saves always fits what the reader accepts (Astra 2026-11-19).
		static constexpr std::size_t kMaxBindings = 2048;
		static constexpr std::size_t kMaxRowBytes = 4 + 4 + 2 + 4 + 4 + (2 + kMaxNameBytes) * 2;
		static constexpr std::size_t kMaxSerializedBytes = 8 + kMaxBindings * kMaxRowBytes;

		// Same spell again clears; another spell replaces; no binding yet assigns with a_nameIfNew.
		ToggleResult Toggle(const StaffKey& a_key, FormID a_spell, const NameState& a_nameIfNew);

		[[nodiscard]] const Binding* Find(const StaffKey& a_key) const;
		[[nodiscard]] Binding*       Find(const StaffKey& a_key);
		bool                         Remove(const StaffKey& a_key);

		// The engine gave the item a new unique id (TESUniqueIDChangeEvent). An existing binding at
		// a_to is replaced. Returns false when nothing was bound at a_from.
		bool Move(const StaffKey& a_from, const StaffKey& a_to);

		[[nodiscard]] std::size_t                         Size() const { return _bindings.size(); }
		[[nodiscard]] bool                                Full() const { return _bindings.size() >= kMaxBindings; }
		[[nodiscard]] const std::map<StaffKey, Binding>& All() const { return _bindings; }
		void                                              Clear() { _bindings.clear(); }

		// Co-save payload, little-endian: version, count, then per binding: staff, uidBase, uid (u16),
		// spell, flags (bit 0 = hadOverride), original (u16 length + bytes), written (u16 length + bytes).
		[[nodiscard]] std::vector<std::uint8_t> Serialize() const;

		// a_resolve maps a saved FormID to the current one or returns empty when that form is gone.
		// A binding whose staff or uidBase is gone is dropped (counted in a_dropped). A binding whose spell
		// is gone stays with spell 0 (counted in a_orphaned): it binds nothing, but keeps the name state so
		// the suffix can still be taken off the item when it is next seen.
		// Returns false and leaves the table empty when the data is malformed.
		[[nodiscard]] bool Deserialize(std::span<const std::uint8_t> a_data,
			const std::function<std::optional<FormID>(FormID)>&      a_resolve,
			std::size_t*                                             a_dropped = nullptr,
			std::size_t*                                             a_orphaned = nullptr);

	private:
		std::map<StaffKey, Binding> _bindings;
	};

	// "<original> [<spell>]"
	[[nodiscard]] std::string ComposeName(std::string_view a_original, std::string_view a_spell);

	// The spell's name without a trailing progress note in parentheses that holds a number and '%', such as
	// Heart of Magic's " (Learning - 35%)" (it writes that into SpellItem::fullName while a spell is weakened, and puts
	// the plain name back later). Without this the staff's name would keep the percentage.
	[[nodiscard]] std::string_view SpellNameForStaff(std::string_view a_spellName);

	// a_name without the staff marker, taken off only when we added it (a_ours): a spell named "Firebolt II" with the
	// marker "II", or one another mod already ended with " ★", keeps its name (Astra 2026-11-19).
	[[nodiscard]] std::string_view WithoutMarker(std::string_view a_name, std::string_view a_marker, bool a_ours);

	// When the binding ends and the item's name is a_current now.
	[[nodiscard]] NameRestore DecideRestore(const NameState& a_state, std::string_view a_current);

	// Before writing a new name over a_state: if someone renamed the item after our last write,
	// their name becomes the new original (design 7-1).
	[[nodiscard]] NameState Rebase(const NameState& a_state, std::string_view a_current);
}

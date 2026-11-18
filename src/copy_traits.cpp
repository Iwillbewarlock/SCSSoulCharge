#include "copy_traits.h"

#include "spell_copy.h"

#include <atomic>

namespace scssc::copy_traits
{
	namespace
	{
		// MagicItem virtual slots (MagicItem.h:87-93).
		constexpr std::size_t kFixedCastDurationSlot = 0x59;
		constexpr std::size_t kRangeSlot = 0x5A;
		constexpr std::size_t kIgnoresResistanceSlot = 0x5B;
		constexpr std::size_t kIgnoreLosSlot = 0x5C;
		constexpr std::size_t kNoAbsorbSlot = 0x5E;
		constexpr std::size_t kNoDualCastModsSlot = 0x5F;
		constexpr std::size_t kSkillUsageSlot = 0x60;  // GetSkillUsageData: which skill a cast trains, and by how much

		// MagicItem answers false here (MagicItem.h:94), so a staff copy trained nothing (2026-11-17: Restoration and
		// Destruction +0.000 after staff casts). The spell's own answer makes the engine train the spell's school exactly
		// as a hand cast does (Codex 2026-11-17 pointed at SpellItem.h:78).
		struct SkillUsageHook
		{
			static bool Thunk(const RE::MagicItem* a_this, RE::MagicItem::SkillUsageData& a_data)
			{
				if (const auto* spell = spell_copy::OriginalOf(a_this)) {
					return spell->GetSkillUsageData(a_data);
				}
				return original(a_this, a_data);
			}
			static inline REL::Relocation<bool (*)(const RE::MagicItem*, RE::MagicItem::SkillUsageData&)> original;
		};

		std::atomic<std::uint32_t> g_rangeAsks{ 0 };  // a counter only: no lock on the engine's path

		template <std::size_t Slot, class R>
		struct Hook
		{
			static R Thunk(const RE::MagicItem* a_this)
			{
				if (const auto* spell = spell_copy::OriginalOf(a_this)) {
					return Ask(a_this, spell);
				}
				return original(a_this);
			}
			static R Ask(const RE::MagicItem* a_copy, const RE::SpellItem* a_spell);
			static inline REL::Relocation<R (*)(const RE::MagicItem*)> original;
		};

		template <>
		float Hook<kFixedCastDurationSlot, float>::Ask(const RE::MagicItem*, const RE::SpellItem* a_spell)
		{
			return a_spell->GetFixedCastDuration();
		}

		template <>
		float Hook<kRangeSlot, float>::Ask(const RE::MagicItem*, const RE::SpellItem* a_spell)
		{
			const float range = a_spell->GetRange();
			if (range != 0.0f) {
				g_rangeAsks.fetch_add(1, std::memory_order_relaxed);
			}
			return range;
		}

		template <>
		bool Hook<kIgnoresResistanceSlot, bool>::Ask(const RE::MagicItem*, const RE::SpellItem* a_spell)
		{
			return a_spell->IgnoresResistance();
		}

		template <>
		bool Hook<kIgnoreLosSlot, bool>::Ask(const RE::MagicItem*, const RE::SpellItem* a_spell)
		{
			return a_spell->IgnoreLOS();
		}

		template <>
		bool Hook<kNoAbsorbSlot, bool>::Ask(const RE::MagicItem*, const RE::SpellItem* a_spell)
		{
			return a_spell->GetNoAbsorb();
		}

		template <>
		bool Hook<kNoDualCastModsSlot, bool>::Ask(const RE::MagicItem*, const RE::SpellItem* a_spell)
		{
			return a_spell->GetNoDualCastModifications();
		}

		template <std::size_t Slot, class R>
		void Write(REL::Relocation<std::uintptr_t>& a_vtable)
		{
			Hook<Slot, R>::original = a_vtable.write_vfunc(Slot, Hook<Slot, R>::Thunk);
		}
	}

	bool Install()
	{
		REL::Relocation<std::uintptr_t> vtable{ RE::VTABLE_EnchantmentItem[0] };
		Write<kFixedCastDurationSlot, float>(vtable);
		Write<kRangeSlot, float>(vtable);
		Write<kIgnoresResistanceSlot, bool>(vtable);
		Write<kIgnoreLosSlot, bool>(vtable);
		Write<kNoAbsorbSlot, bool>(vtable);
		Write<kNoDualCastModsSlot, bool>(vtable);
		SkillUsageHook::original = vtable.write_vfunc(kSkillUsageSlot, SkillUsageHook::Thunk);
		logger::info("Hook installed: EnchantmentItem range, fixed cast duration, flag and skill usage questions (staff copies answer as their spell)");
		return true;
	}

	std::uint32_t RangeAsks()
	{
		return g_rangeAsks.load(std::memory_order_relaxed);
	}
}

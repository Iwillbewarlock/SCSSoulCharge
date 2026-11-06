#include "PCH.h"

#include "pool.h"

#include <unordered_map>

namespace scssc::pool
{
	namespace
	{
		constexpr auto       kOwnPlugin = "SCSSoulCharge.esp"sv;
		constexpr auto       kScsPlugin = "SpellCastingStaff.esp"sv;
		constexpr RE::FormID kFirstPoolSpell = 0x800;
		constexpr RE::FormID kSharedEnchantment = 0x80B;
		constexpr auto       kEmptyName = "SCSSC Empty Charge Spell"sv;

		struct State
		{
			bool                                           ready{ false };
			std::vector<RE::SpellItem*>                    spells;
			RE::EnchantmentItem*                           shared{ nullptr };
			core::PoolTable                                table{ kPoolSize };
			std::unordered_map<const RE::MagicItem*, std::uint32_t> slotOfSpell;
		};

		State& GetState()
		{
			static State state;
			return state;
		}

		void Empty(RE::SpellItem* a_spell)
		{
			a_spell->effects.clear();  // pointers only; the effects belong to their enchantments
			a_spell->hostileCount = 0;
			a_spell->avEffectSetting = nullptr;
			a_spell->fullName = kEmptyName;
		}

		// Effects are shared with the enchantment (never freed while the game runs), so active effects
		// that keep an Effect* stay valid. Slots are never reassigned within a save.
		void Fill(RE::SpellItem* a_spell, const RE::EnchantmentItem* a_enchantment)
		{
			a_spell->effects = a_enchantment->effects;
			a_spell->hostileCount = a_enchantment->hostileCount;
			a_spell->avEffectSetting = a_enchantment->avEffectSetting;
			a_spell->fullName = a_enchantment->GetFullName();
			auto& data = a_spell->data;
			data.costOverride = 0;
			data.flags.set(RE::SpellItem::SpellFlag::kCostOverride);  // never auto-priced: SCS must charge 0 Magicka
			data.spellType = RE::MagicSystem::SpellType::kSpell;
			data.castingType = a_enchantment->data.castingType;
			data.delivery = a_enchantment->data.delivery;
			data.chargeTime = a_enchantment->data.chargeTime;
		}
	}

	bool Resolve()
	{
		auto& state = GetState();
		auto* data = RE::TESDataHandler::GetSingleton();
		if (!data) {
			return false;
		}
		state.spells.clear();
		state.slotOfSpell.clear();
		for (RE::FormID i = 0; i < kPoolSize; ++i) {
			auto* spell = data->LookupForm<RE::SpellItem>(kFirstPoolSpell + i, kOwnPlugin);
			if (!spell) {
				logger::critical("Charge spell slot {} (local {:03X}) missing from {}; charge spells disabled", i, kFirstPoolSpell + i, kOwnPlugin);
				state.spells.clear();
				state.slotOfSpell.clear();
				return false;
			}
			state.spells.push_back(spell);
			state.slotOfSpell.emplace(spell, i);
		}
		state.shared = data->LookupForm<RE::EnchantmentItem>(kSharedEnchantment, kScsPlugin);
		if (!state.shared) {
			logger::critical("SCS shared staff enchantment {:03X} not found in {}; charge spells disabled", kSharedEnchantment, kScsPlugin);
			return false;
		}
		state.ready = true;
		logger::info("Charge spell pool ready: {} slots, shared ENCH {:08X}", kPoolSize, state.shared->GetFormID());
		return true;
	}

	bool IsReady() { return GetState().ready; }

	RE::SpellItem* SpellFor(RE::EnchantmentItem* a_enchantment)
	{
		auto& state = GetState();
		if (!state.ready || !a_enchantment) {
			return nullptr;
		}
		const auto existing = state.table.Find(a_enchantment->GetFormID());
		const auto slot = existing ? existing : state.table.Assign(a_enchantment->GetFormID());
		if (!slot) {
			logger::error("Charge spell pool full ({} slots); '{}' {:08X} not converted",
				kPoolSize, a_enchantment->GetName(), a_enchantment->GetFormID());
			RE::SendHUDMessage::ShowHUDMessage("SCSSoulCharge: charge spell pool is full");
			return nullptr;
		}
		auto* spell = state.spells[*slot];
		if (!existing) {
			Fill(spell, a_enchantment);
			logger::info("POOL slot {} <- '{}' {:08X} (spell {:08X}, cast={} delivery={} charge={:.2f}s)", *slot,
				a_enchantment->GetName(), a_enchantment->GetFormID(), spell->GetFormID(),
				static_cast<int>(spell->data.castingType), static_cast<int>(spell->data.delivery), spell->data.chargeTime);
		}
		return spell;
	}

	RE::SpellItem* FindSpell(const RE::EnchantmentItem* a_enchantment)
	{
		auto& state = GetState();
		if (!state.ready || !a_enchantment) {
			return nullptr;
		}
		const auto slot = state.table.Find(a_enchantment->GetFormID());
		return slot ? state.spells[*slot] : nullptr;
	}

	RE::EnchantmentItem* EnchantmentOf(const RE::MagicItem* a_spell)
	{
		auto&      state = GetState();
		const auto it = state.slotOfSpell.find(a_spell);
		if (it == state.slotOfSpell.end()) {
			return nullptr;
		}
		const auto id = state.table.EnchantmentAt(it->second);
		return id ? RE::TESForm::LookupByID<RE::EnchantmentItem>(*id) : nullptr;
	}

	bool IsPoolSpell(const RE::MagicItem* a_spell)
	{
		return a_spell && GetState().slotOfSpell.contains(a_spell);
	}

	bool PlayerKnowsAnyPoolSpell()
	{
		auto* player = RE::PlayerCharacter::GetSingleton();
		if (!player) {
			return false;
		}
		return std::ranges::any_of(GetState().spells, [&](RE::SpellItem* a_spell) { return player->HasSpell(a_spell); });
	}

	RE::EnchantmentItem* SharedEnchantment() { return GetState().shared; }

	core::PoolTable& Table() { return GetState().table; }

	void Clear()
	{
		auto& state = GetState();
		state.table.Clear();
		for (auto* spell : state.spells) {
			Empty(spell);
		}
	}

	void RefillFromTable()
	{
		auto& state = GetState();
		std::uint32_t filled = 0;
		for (const auto& entry : state.table.Entries()) {
			auto* spell = state.spells[entry.slot];
			auto* enchantment = entry.enchantment ? RE::TESForm::LookupByID<RE::EnchantmentItem>(entry.enchantment) : nullptr;
			if (enchantment) {
				Fill(spell, enchantment);
				++filled;
			} else {
				Empty(spell);
			}
		}
		logger::info("POOL refilled {} of {} saved slots", filled, state.table.Used());
	}
}

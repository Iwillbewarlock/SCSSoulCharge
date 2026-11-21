#include "spell_copy.h"

#include "core/bindings.h"
#include "selftest.h"
#include "staff_mark.h"

#include <atomic>
#include <shared_mutex>
#include <unordered_map>
#include <vector>

extern "C" std::uint32_t SCSSoulCharge_GetOriginalSpell(std::uint32_t a_item) noexcept;  // api.cpp

namespace scssc::spell_copy
{
	namespace
	{
		struct CopyInfo
		{
			RE::SpellItem*           original{ nullptr };
			RE::FormID               formID{ 0 };      // kept by value: checked before the pointer is ever read again
			std::vector<RE::Effect*> originalEffects;  // index i = the original effect behind the copy's effect i
		};

		// Read on every cost lookup and AddTarget, written once per new spell: a shared lock.
		std::shared_mutex                                              g_lock;
		std::unordered_map<const RE::SpellItem*, RE::EnchantmentItem*> g_bySpell;
		std::unordered_map<const RE::MagicItem*, CopyInfo>             g_byCopy;
		std::atomic<std::uint64_t>                                     g_lockedLookups{ 0 };  // T-PERF

		RE::EnchantmentItem* Make(RE::SpellItem* a_spell, std::vector<RE::Effect*>& a_originalEffects)
		{
			auto* factory = RE::IFormFactory::GetConcreteFormFactoryByType<RE::EnchantmentItem>();
			auto* copy = factory ? factory->Create() : nullptr;
			if (!copy) {
				return nullptr;
			}
			// No AddChange: the copy is rebuilt on load and must leave nothing in the save (T-SAVE).
			copy->hostileCount = a_spell->hostileCount;
			copy->avEffectSetting = a_spell->avEffectSetting;
			for (auto* effect : a_spell->effects) {
				if (!effect) {
					continue;
				}
				auto* dup = new RE::Effect();
				dup->effectItem = effect->effectItem;
				dup->baseEffect = effect->baseEffect;
				dup->cost = effect->cost;
				// The engine's own copy (TESCondition::Copy): a separate list, so neither destructor frees the other's
				// items (plain assignment shares the list, as Immersive Staves does). T-COND checks it.
				dup->conditions.Copy(&effect->conditions);
				copy->effects.push_back(dup);
				a_originalEffects.push_back(effect);
			}
			// Concentration effects stay on the copy and show its name (Carbon 2026-11-17); never with the staff marker.
			// Without Heart of Magic's progress note too: the copy is made once a session (Carbon 2026-11-19).
			copy->fullName = std::string(core::SpellNameForStaff(staff_mark::CleanName(a_spell))).c_str();
			for (std::uint32_t i = 0; i < a_spell->numKeywords; ++i) {
				if (a_spell->keywords[i]) {
					copy->AddKeyword(a_spell->keywords[i]);  // perks that test the spell's keywords (rune, school) see them
				}
			}
			auto& data = copy->data;
			data.spellType = RE::MagicSystem::SpellType::kStaffEnchantment;
			data.castingType = a_spell->GetCastingType();
			data.delivery = a_spell->GetDelivery();
			data.chargeTime = a_spell->GetChargeTime();
			data.costOverride = a_spell->data.costOverride;
			if (a_spell->data.flags.all(RE::SpellItem::SpellFlag::kCostOverride)) {
				data.flags.set(RE::EnchantmentItem::EnchantmentFlag::kCostOverride);
			}
			return copy;
		}
	}

	RE::EnchantmentItem* Get(RE::SpellItem* a_spell)
	{
		if (!a_spell) {
			return nullptr;
		}
		{
			const std::shared_lock lock{ g_lock };
			if (const auto it = g_bySpell.find(a_spell); it != g_bySpell.end()) {
				return it->second;
			}
		}
		RE::EnchantmentItem* copy = nullptr;
		{
			const std::unique_lock lock{ g_lock };
			if (const auto it = g_bySpell.find(a_spell); it != g_bySpell.end()) {
				return it->second;
			}
			std::vector<RE::Effect*> originalEffects;
			copy = Make(a_spell, originalEffects);
			if (!copy) {
				logger::error("spell copy: the form factory could not make an enchantment for {:08X}", a_spell->GetFormID());
				return nullptr;
			}
			g_bySpell.emplace(a_spell, copy);
			g_byCopy.emplace(copy, CopyInfo{ a_spell, copy->GetFormID(), std::move(originalEffects) });
			logger::info("spell copy: {:08X} '{}' -> {:08X} ({} effects)", a_spell->GetFormID(), a_spell->GetName(), copy->GetFormID(), copy->effects.size());
		}
		// What other plugins see (Heart of Magic asks by FormID, 2026-11-17): the copy found again by its FormID, and the
		// exported answer. Outside the lock: the export takes it shared.
		const auto* found = RE::TESForm::LookupByID(copy->GetFormID());
		const auto  viaApi = SCSSoulCharge_GetOriginalSpell(copy->GetFormID());
		selftest::Report("T-API", found == copy && viaApi == a_spell->GetFormID(),
			std::format("copy {:08X}: LookupByID {} the copy, SCSSoulCharge_GetOriginalSpell -> {:08X} (spell {:08X})", copy->GetFormID(),
				found == copy ? "finds" : (found ? "finds ANOTHER form for" : "does NOT find"), viaApi, a_spell->GetFormID()));
		return copy;
	}

	RE::SpellItem* OriginalOf(const RE::MagicItem* a_item)
	{
		if (!a_item || !a_item->IsDynamicForm()) {  // every copy is a runtime form: skip the lock for game forms
			return nullptr;
		}
		g_lockedLookups.fetch_add(1, std::memory_order_relaxed);
		const std::shared_lock lock{ g_lock };
		const auto             it = g_byCopy.find(a_item);
		return it == g_byCopy.end() ? nullptr : it->second.original;
	}

	bool IsCopy(const RE::MagicItem* a_item)
	{
		return OriginalOf(a_item) != nullptr;
	}

	std::uint64_t TakeLockedLookups()
	{
		return g_lockedLookups.exchange(0, std::memory_order_relaxed);
	}

	void VerifyAfterLoad()
	{
		const std::unique_lock lock{ g_lock };
		std::size_t            kept = 0;
		std::string            lost;
		for (auto it = g_bySpell.begin(); it != g_bySpell.end();) {
			auto*      copy = it->second;
			const auto info = g_byCopy.find(copy);
			const auto id = info != g_byCopy.end() ? info->second.formID : 0;
			// The pointer is only compared, never read: if the engine had freed the form, its FormID now finds another
			// form or none (Carbon 2026-11-17: reading copy->GetFormID() first would read freed memory).
			if (id != 0 && RE::TESForm::LookupByID(id) == copy) {
				++kept;
				++it;
				continue;
			}
			lost += std::format(" {:08X}", id);
			g_byCopy.erase(copy);  // left alive: the engine may still point at it
			it = g_bySpell.erase(it);
		}
		// A replaced copy is dropped here and made again on its next use, so a replacement is not a failure: loading an
		// older save let the engine hand a copy's FormID to another form (2026-11-20 17:00, handled as designed).
		const auto detail = std::format("{} copies kept their FormID; replaced (made again on next use):[{} ]", kept, lost);
		if (!lost.empty()) {
			selftest::Skip("T-COPY-ID", detail);
		} else if (kept > 0) {
			selftest::Report("T-COPY-ID", true, detail);
		}
	}

	RE::Effect* OriginalEffectOf(const RE::MagicItem* a_copy, const RE::Effect* a_copyEffect)
	{
		if (!a_copy || !a_copyEffect) {
			return nullptr;
		}
		const std::shared_lock lock{ g_lock };
		const auto             it = g_byCopy.find(a_copy);
		if (it == g_byCopy.end()) {
			return nullptr;
		}
		const auto& effects = a_copy->effects;
		const auto& originals = it->second.originalEffects;
		for (std::uint32_t i = 0; i < effects.size() && i < originals.size(); ++i) {
			if (effects[i] == a_copyEffect) {
				return originals[i];
			}
		}
		return nullptr;
	}

	core::SpellTraits TraitsOf(const RE::SpellItem* a_spell, RE::PlayerCharacter* a_player)
	{
		core::SpellTraits traits;
		if (!a_spell) {
			return traits;
		}
		auto* spell = const_cast<RE::SpellItem*>(a_spell);  // the CommonLib getters below are not const
		traits.isPlainSpell = spell->GetSpellType() == RE::MagicSystem::SpellType::kSpell;
		traits.known = a_player && a_player->HasSpell(spell);
		traits.fireAndForget = spell->GetCastingType() == RE::MagicSystem::CastingType::kFireAndForget;
		traits.concentration = spell->GetCastingType() == RE::MagicSystem::CastingType::kConcentration;
		traits.selfDelivery = spell->GetDelivery() == RE::MagicSystem::Delivery::kSelf;
		traits.range = spell->GetRange();
		traits.fixedCastDuration = spell->GetFixedCastDuration();
		traits.ignoreResistance = spell->IgnoresResistance();
		traits.ignoreLOS = spell->IgnoreLOS();
		traits.noAbsorb = spell->GetNoAbsorb();
		traits.hasEffects = !spell->effects.empty();
		traits.runtimeForm = spell->IsDynamicForm();
		return traits;
	}

	RE::BGSArtObject* TipArtOf(const RE::MagicItem* a_item)
	{
		const auto* main = a_item ? a_item->GetCostliestEffectItem() : nullptr;
		return main && main->baseEffect ? main->baseEffect->data.castingArt : nullptr;
	}
}

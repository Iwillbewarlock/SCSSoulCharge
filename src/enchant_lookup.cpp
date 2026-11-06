#include "PCH.h"

#include "enchant_lookup.h"

#include "detour_helper.h"
#include "keywords.h"
#include "pool.h"

namespace scssc::enchant_lookup
{
	namespace
	{
		using GetEnchantmentFn = RE::EnchantmentItem* (*)(const RE::InventoryEntryData*);
		GetEnchantmentFn g_original = nullptr;

		// The shared enchantment when this entry holds a worn copy we converted, else nullptr.
		RE::EnchantmentItem* ConvertedWornEnchantment(const RE::InventoryEntryData* a_entry)
		{
			auto* shared = pool::SharedEnchantment();
			const auto* object = a_entry ? a_entry->object : nullptr;
			if (!shared || !object || !a_entry->extraLists || !keywords::IsConverted(object->GetFormID())) {
				return nullptr;
			}
			for (auto* list : *a_entry->extraLists) {
				if (!list || !(list->HasType(RE::ExtraDataType::kWorn) || list->HasType(RE::ExtraDataType::kWornLeft))) {
					continue;
				}
				const auto* extra = list->GetByType<RE::ExtraEnchantment>();
				if (extra && extra->enchantment == shared) {
					return shared;
				}
			}
			return nullptr;
		}

		RE::EnchantmentItem* Thunk(const RE::InventoryEntryData* a_entry)
		{
			if (auto* shared = ConvertedWornEnchantment(a_entry)) {
				return shared;
			}
			return g_original(a_entry);
		}
	}

	bool Install()
	{
		const REL::Relocation<std::uintptr_t> target{ RELOCATION_ID(15788, 16026) };
		g_original = reinterpret_cast<GetEnchantmentFn>(target.address());
		const auto err = detour::Attach(reinterpret_cast<void**>(&g_original), reinterpret_cast<void*>(&Thunk));
		if (err != 0) {
			logger::critical("Could not hook InventoryEntryData::GetEnchantment at {:X} (Detours error {})", target.address(), err);
			return false;
		}
		logger::info("Hook installed: InventoryEntryData::GetEnchantment (Detours, {:X})", target.address());
		return true;
	}
}

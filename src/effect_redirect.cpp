#include "effect_redirect.h"

#include "selftest.h"
#include "spell_copy.h"

#include <atomic>
#include <mutex>
#include <unordered_set>

namespace scssc::effect_redirect
{
	namespace
	{
		// One PASS line per copy per game session, so the log shows each spell once without flooding.
		std::mutex                               g_reportLock;
		std::unordered_set<const RE::MagicItem*> g_reported;
		std::atomic<std::uint32_t>               g_copyApplications{ 0 };

		void ReportOnce(const RE::MagicItem* a_copy, bool a_ok, const std::string& a_detail)
		{
			{
				const std::scoped_lock lock{ g_reportLock };
				if (!g_reported.insert(a_copy).second && a_ok) {
					return;
				}
			}
			selftest::Report("T-REDIRECT", a_ok, a_detail);
		}

		// Points a_data at the original spell's effect. Returns false when a_data was left as it was.
		bool Redirect(RE::MagicTarget::AddTargetData& a_data)
		{
			auto* original = spell_copy::OriginalOf(a_data.magicItem);
			if (!original || original->GetCastingType() == RE::MagicSystem::CastingType::kConcentration) {
				return false;
			}
			auto* effect = spell_copy::OriginalEffectOf(a_data.magicItem, a_data.effect);
			if (!effect) {
				ReportOnce(a_data.magicItem, false,
					std::format("copy {:08X} applied an effect that is not in its own list; left on the copy", a_data.magicItem->GetFormID()));
				return false;
			}
			a_data.magicItem = original;
			a_data.effect = effect;
			return true;
		}

		template <std::size_t Id>
		struct AddTargetHook
		{
			static bool Thunk(RE::MagicTarget* a_this, RE::MagicTarget::AddTargetData& a_data)
			{
				// Put back afterwards in case the engine reuses the struct for the next effect.
				auto* const item = a_data.magicItem;
				auto* const effect = a_data.effect;
				const bool redirected = Redirect(a_data);
				const bool result = original(a_this, a_data);
				// Effects on the player only (a summon's, a self spell's): a projectile of the last cast landing on
				// someone else while the next one charges must not count as that one's release.
				auto* player = RE::PlayerCharacter::GetSingleton();
				if (result && player && a_this == player->AsMagicTarget() && (redirected || spell_copy::IsCopy(item))) {
					g_copyApplications.fetch_add(1);
				}
				if (redirected && result) {  // judged once the engine took the effect, not before
					ReportOnce(item, true,
						std::format("copy {:08X} -> spell {:08X} '{}' mgef={:08X}", item->GetFormID(), a_data.magicItem->GetFormID(), a_data.magicItem->GetName(),
							a_data.effect && a_data.effect->baseEffect ? a_data.effect->baseEffect->GetFormID() : 0u));
				}
				a_data.magicItem = item;
				a_data.effect = effect;
				return result;
			}
			static inline REL::Relocation<decltype(&Thunk)> original;
		};
	}

	// MagicTarget::AddTarget is vfunc 1 of the MagicTarget base, the 5th vtable of Character and PlayerCharacter
	// (MagicTarget.h:115, Actor.h:145; the same slot TACTICDataCollection hooks).
	void Install()
	{
		REL::Relocation<std::uintptr_t> character{ RE::VTABLE_Character[4] };
		AddTargetHook<0>::original = character.write_vfunc(0x1, AddTargetHook<0>::Thunk);
		REL::Relocation<std::uintptr_t> player{ RE::VTABLE_PlayerCharacter[4] };
		AddTargetHook<1>::original = player.write_vfunc(0x1, AddTargetHook<1>::Thunk);
		logger::info("Hook installed: MagicTarget::AddTarget (Character, PlayerCharacter)");
	}

	std::uint32_t CopyApplications()
	{
		return g_copyApplications.load();
	}
}

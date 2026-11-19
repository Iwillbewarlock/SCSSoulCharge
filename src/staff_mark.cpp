#include "staff_mark.h"

#include "core/bindings.h"
#include "selftest.h"
#include "settings.h"

#include <atomic>

namespace scssc::staff_mark
{
	namespace
	{
		RE::SpellItem*          g_marked = nullptr;  // main thread
		bool                    g_ours = false;      // main thread: the marker on g_marked's name is the one we added
		std::atomic<RE::FormID> g_heldId{ 0 };

		bool EndsWithMarker(std::string_view a_name)
		{
			const auto marker = Marker();
			return !marker.empty() && a_name.ends_with(marker);
		}

		void Unmark()
		{
			if (g_marked && g_ours) {
				const std::string_view name = g_marked->fullName.c_str();
				if (EndsWithMarker(name)) {
					g_marked->fullName = std::string(core::WithoutMarker(name, Marker(), true)).c_str();
				}
			}
			g_marked = nullptr;
			g_ours = false;
		}

		// Adds the marker when the name lacks it. Heart of Magic rewrites a weakened spell's name ("(Learning - 35%)"),
		// so the check runs every frame and the marker comes back after such a rewrite. A name that already ends with
		// the marker text is left alone and not counted as ours.
		void Mark(RE::SpellItem* a_spell)
		{
			const std::string_view name = a_spell->fullName.c_str();
			if (EndsWithMarker(name)) {
				return;
			}
			const std::string marked = std::string(name) + std::string(Marker());
			a_spell->fullName = marked.c_str();
			g_ours = true;
			selftest::Report("T-MARK", EndsWithMarker(a_spell->GetName()), std::format("spell {:08X} now shows '{}'", a_spell->GetFormID(), a_spell->GetName()));
		}
	}

	std::string_view Marker()
	{
		return Settings::Get().staffMarker;
	}

	std::string_view CleanName(const RE::SpellItem* a_spell)
	{
		if (!a_spell) {
			return {};
		}
		return core::WithoutMarker(a_spell->fullName.c_str(), Marker(), a_spell == g_marked && g_ours);
	}

	void OnFrame(RE::SpellItem* a_spell)
	{
		g_heldId = a_spell ? a_spell->GetFormID() : 0;  // before the marker check: the API answers with the marker off too (Astra 2026-11-19)
		if (Marker().empty()) {
			return;  // turned off in the INI
		}
		if (a_spell != g_marked) {
			Unmark();
			g_marked = a_spell;
		}
		if (g_marked) {
			Mark(g_marked);
		}
	}

	void Reset()
	{
		Unmark();
		g_heldId = 0;
	}

	RE::FormID HeldSpell()
	{
		return g_heldId.load();
	}
}

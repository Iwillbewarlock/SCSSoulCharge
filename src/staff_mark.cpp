#include "staff_mark.h"

#include "selftest.h"
#include "settings.h"

#include <atomic>

namespace scssc::staff_mark
{
	namespace
	{
		RE::SpellItem*          g_marked = nullptr;  // main thread
		std::atomic<RE::FormID> g_markedId{ 0 };

		bool EndsWithMarker(std::string_view a_name)
		{
			const auto marker = Marker();
			return !marker.empty() && a_name.size() >= marker.size() && a_name.substr(a_name.size() - marker.size()) == marker;
		}

		void Unmark()
		{
			if (!g_marked) {
				return;
			}
			const std::string_view name = g_marked->fullName.c_str();
			if (EndsWithMarker(name)) {
				g_marked->fullName = std::string(Strip(name)).c_str();
			}
			g_marked = nullptr;
			g_markedId = 0;
		}

		// Adds the marker when the name lacks it. Heart of Magic rewrites a weakened spell's name ("(Learning - 35%)"),
		// so the check runs every frame and the marker comes back after such a rewrite.
		void Mark(RE::SpellItem* a_spell)
		{
			const std::string_view name = a_spell->fullName.c_str();
			if (EndsWithMarker(name)) {
				return;
			}
			const std::string marked = std::string(name) + std::string(Marker());
			a_spell->fullName = marked.c_str();
			selftest::Report("T-MARK", EndsWithMarker(a_spell->GetName()), std::format("spell {:08X} now shows '{}'", a_spell->GetFormID(), a_spell->GetName()));
		}
	}

	std::string_view Marker()
	{
		return Settings::Get().staffMarker;
	}

	std::string_view Strip(std::string_view a_name)
	{
		return EndsWithMarker(a_name) ? a_name.substr(0, a_name.size() - Marker().size()) : a_name;
	}

	void OnFrame(RE::SpellItem* a_spell)
	{
		if (Marker().empty()) {
			return;  // turned off in the INI
		}
		if (a_spell != g_marked) {
			Unmark();
			g_marked = a_spell;
			g_markedId = a_spell ? a_spell->GetFormID() : 0;
		}
		if (g_marked) {
			Mark(g_marked);
		}
	}

	void Reset()
	{
		Unmark();
	}

	RE::FormID MarkedSpell()
	{
		return g_markedId.load();
	}
}

#include "staff_mark.h"

#include "core/bindings.h"
#include "selftest.h"
#include "settings.h"

#include <array>
#include <atomic>

namespace scssc::staff_mark
{
	namespace
	{
		// What a spell's name should end with: none, right, left, or both (right then left).
		enum class Which : std::uint8_t
		{
			kNone,
			kRight,
			kLeft,
			kBoth,
		};

		struct Mark
		{
			RE::SpellItem* spell{ nullptr };
			Which          which{ Which::kNone };
			bool           ours{ false };  // the suffix on the name is the one we added
		};

		std::array<Mark, 2>                     g_marks{};   // main thread: at most two spells carry a marker
		std::array<std::atomic<RE::FormID>, 2>  g_heldId{};  // per hand
		// Per g_marks slot, for MarkerOn from any thread: (Which << 32) | FormID in one value, so a reader never pairs one
		// spell with another's suffix (Astra 2026-11-20).
		std::array<std::atomic<std::uint64_t>, 2> g_published{};

		const std::string& Suffix(Which a_which)
		{
			static const std::string none;
			const auto& s = Settings::Get();
			switch (a_which) {
			case Which::kRight:
				return s.staffMarkerRight;
			case Which::kLeft:
				return s.staffMarkerLeft;
			case Which::kBoth:
				return s.staffMarkerBoth;
			default:
				return none;
			}
		}

		bool EndsWith(const RE::SpellItem* a_spell, const std::string& a_suffix)
		{
			return !a_suffix.empty() && std::string_view(a_spell->fullName.c_str()).ends_with(a_suffix);
		}

		void Unmark(Mark& a_mark)
		{
			if (a_mark.spell && a_mark.ours) {
				const auto& suffix = Suffix(a_mark.which);
				if (EndsWith(a_mark.spell, suffix)) {
					a_mark.spell->fullName = std::string(core::WithoutMarker(a_mark.spell->fullName.c_str(), suffix, true)).c_str();
				}
			}
			a_mark = {};
		}

		// Adds the suffix when the name lacks it. Heart of Magic rewrites a weakened spell's name ("(Learning - 35%)"),
		// so the check runs every frame and the marker comes back after such a rewrite. A name that already ends with
		// the suffix text is left alone and not counted as ours.
		void Apply(Mark& a_mark)
		{
			const auto& suffix = Suffix(a_mark.which);
			if (suffix.empty() || EndsWith(a_mark.spell, suffix)) {
				return;
			}
			const std::string marked = std::string(a_mark.spell->fullName.c_str()) + suffix;
			a_mark.spell->fullName = marked.c_str();
			a_mark.ours = true;
			selftest::Report("T-MARK", EndsWith(a_mark.spell, suffix),
				std::format("spell {:08X} now shows '{}'", a_mark.spell->GetFormID(), a_mark.spell->GetName()));
		}

		void Publish()
		{
			for (std::size_t i = 0; i < g_marks.size(); ++i) {
				const bool shown = g_marks[i].spell && g_marks[i].ours;
				g_published[i] = shown ? (static_cast<std::uint64_t>(g_marks[i].which) << 32) | g_marks[i].spell->GetFormID() : 0;
			}
		}
	}

	void OnFrame(RE::SpellItem* a_right, RE::SpellItem* a_left)
	{
		// Before the marker check: the API answers with the marker off too (Astra 2026-11-19).
		g_heldId[Index(Hand::kRight)] = a_right ? a_right->GetFormID() : 0;
		g_heldId[Index(Hand::kLeft)] = a_left ? a_left->GetFormID() : 0;
		if (!Settings::Get().staffMarkerOn) {
			return;
		}
		std::array<Mark, 2> wanted{};
		if (a_right && a_right == a_left) {
			wanted[0] = { a_right, Which::kBoth };
		} else {
			wanted[0] = { a_right, a_right ? Which::kRight : Which::kNone };
			wanted[1] = { a_left, a_left ? Which::kLeft : Which::kNone };
		}
		// A mark whose spell or suffix changed comes off first; one that stays keeps its "ours".
		for (auto& mark : g_marks) {
			const auto keep = std::ranges::find_if(wanted, [&](const Mark& w) { return w.spell && w.spell == mark.spell && w.which == mark.which; });
			if (mark.spell && keep == wanted.end()) {
				Unmark(mark);
			}
		}
		for (const auto& w : wanted) {
			if (!w.spell) {
				continue;
			}
			auto it = std::ranges::find_if(g_marks, [&](const Mark& m) { return m.spell == w.spell && m.which == w.which; });
			if (it == g_marks.end()) {
				it = std::ranges::find_if(g_marks, [](const Mark& m) { return !m.spell; });
				*it = w;  // two wanted at most, and every other mark came off above
			}
			Apply(*it);
		}
		Publish();
	}

	void Reset()
	{
		for (auto& mark : g_marks) {
			Unmark(mark);
		}
		for (auto& id : g_heldId) {
			id = 0;
		}
		Publish();
	}

	const char* MarkerOn(RE::FormID a_spell)
	{
		for (const auto& published : g_published) {
			const auto value = published.load();
			if (a_spell != 0 && static_cast<RE::FormID>(value) == a_spell) {
				return Suffix(static_cast<Which>(value >> 32)).c_str();
			}
		}
		return "";
	}

	std::string_view CleanName(const RE::SpellItem* a_spell)
	{
		if (!a_spell) {
			return {};
		}
		for (const auto& mark : g_marks) {
			if (mark.spell == a_spell) {
				return core::WithoutMarker(a_spell->fullName.c_str(), Suffix(mark.which), mark.ours);
			}
		}
		return a_spell->fullName.c_str();
	}

	RE::FormID HeldSpell(Hand a_hand)
	{
		return g_heldId[Index(a_hand)].load();
	}
}

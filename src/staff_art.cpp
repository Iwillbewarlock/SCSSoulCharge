#include "staff_art.h"

#include "hand_slot.h"
#include "selftest.h"
#include "spell_copy.h"

namespace scssc::staff_art
{
	namespace
	{
		using DetachArtFn = void (*)(RE::ActorMagicCaster*, bool);
		using SetCastingArtFn = void (*)(RE::ActorMagicCaster*, RE::BGSArtObject*);
		DetachArtFn     g_detach = nullptr;
		SetCastingArtFn g_setArt = nullptr;

		constexpr float kCheckSecs = 3.0f;      // the 2026-11-17 refreshes attached within 0.13 s
		constexpr float kDrawGraceSecs = 0.5f;  // after a draw, how long the engine gets to attach the art by itself

		// Why a refresh is (or was) waiting, so each case gets its own self-test id.
		enum class Wait
		{
			kNone,
			kSheathed,  // T-ART-SHEATHED: the slot changed while the staff was put away
			kCasting,   // T-ART-MIDCAST: the slot changed during a cast
		};

		struct State
		{
			const RE::MagicItem* shownFor{ nullptr };  // the slot item whose art we last attached; null = unknown
			bool                 wasDrawn{ false };
			bool                 drawPending{ false };  // drawn since the last refresh while a copy is in the slot
			std::uint32_t        slotWrites{ 0 };       // hand_slot::Writes() at the last refresh
			float                drawAt{ 0.0f };
			std::uint32_t        drawWrites{ 0 };  // hand_slot::Writes() at the draw: the engine's art is for that slot item
			bool                 fresh{ true };    // first frame after a reset: take the weapon state as it is
			Wait                 wait{ Wait::kNone };
			float                clock{ 0.0f };
			// self-test window after a refresh
			const char*          checkId{ nullptr };
			float                checkStart{ 0.0f };
			RE::FormID           checkArt{ 0 };
		};
		State g_state;

		RE::ActorMagicCaster* RightCaster(RE::PlayerCharacter* a_player)
		{
			auto* caster = a_player->GetMagicCaster(RE::MagicSystem::CastingSource::kRightHand);
			return caster ? static_cast<RE::ActorMagicCaster*>(caster) : nullptr;
		}

		// Node names inside known hand-art models (houseCARL, 2026-11-16), the same check the test build passed 9/9 with:
		// the engine renames the attached root 'MagicRight', so the model is told apart by a child node.
		constexpr std::array<std::pair<std::string_view, std::string_view>, 4> kMarkers{ {
			{ "fireball01handeffects", "pFireballCore04" },
			{ "frostbolthandfx01", "pSmallSnow03" },
			{ "frosthandfx01", "pSmallSnow03" },
			{ "shieldspellhand01", "BigCube01" },
		} };
		constexpr int kMarkerDepth = 4;

		std::string_view MarkerFor(const RE::BGSArtObject* a_art)
		{
			const char* model = a_art ? a_art->GetModel() : nullptr;
			if (!model) {
				return {};
			}
			std::string stem{ model };
			if (const auto slash = stem.find_last_of("\\/"); slash != std::string::npos) {
				stem.erase(0, slash + 1);
			}
			if (const auto dot = stem.rfind('.'); dot != std::string::npos) {
				stem.erase(dot);
			}
			std::ranges::transform(stem, stem.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
			for (const auto& [name, marker] : kMarkers) {
				if (stem == name) {
					return marker;
				}
			}
			return {};
		}

		bool HasNamedNode(RE::NiAVObject* a_object, std::string_view a_name, int a_depth)
		{
			if (!a_object) {
				return false;
			}
			if (_stricmp(a_object->name.c_str(), std::string(a_name).c_str()) == 0) {
				return true;
			}
			auto* node = a_object->AsNode();
			if (!node || a_depth <= 0) {
				return false;
			}
			for (const auto& child : node->GetChildren()) {
				if (HasNamedNode(child.get(), a_name, a_depth - 1)) {
					return true;
				}
			}
			return false;
		}

		// Whether a_art's model (by its marker node) is what is attached now; empty when the model is not in the table.
		std::optional<bool> ShowsArt(const RE::ActorMagicCaster* a_caster, const RE::BGSArtObject* a_art)
		{
			const auto marker = MarkerFor(a_art);
			if (marker.empty()) {
				return std::nullopt;
			}
			return a_caster->flags.all(RE::ActorMagicCaster::Flags::kCastingArtAttached) &&
			       HasNamedNode(a_caster->castingArtData.attachedArt.get(), marker, kMarkerDepth);
		}

		bool Drawn(RE::PlayerCharacter* a_player)
		{
			const auto* state = a_player->AsActorState();
			return state && state->GetWeaponState() == RE::WEAPON_STATE::kDrawn;
		}

		void Judge(const RE::ActorMagicCaster* a_caster, bool a_drawn)
		{
			auto& s = g_state;
			if (!s.checkId) {
				return;
			}
			const auto elapsedMs = (s.clock - s.checkStart) * 1000.0f;
			const auto detail = std::format("art {:08X} after {:.0f} ms", s.checkArt, elapsedMs);
			// Attached after our detach, with the art we set taken up (castingArt is cleared when it attaches): the
			// attached one is ours. Which model it is was proven by node names in the test build (9/9).
			const bool attachedOurs = a_caster->flags.all(RE::ActorMagicCaster::Flags::kCastingArtAttached) && a_caster->castingArtData.attachedArt &&
			                          !a_caster->castingArt;
			const auto model = ShowsArt(a_caster, RE::TESForm::LookupByID<RE::BGSArtObject>(s.checkArt));  // empty: not in the table
			if (a_caster->state.underlying() != 0) {
				selftest::Skip(s.checkId, detail + " (a cast started first)");
			} else if (!a_drawn) {
				selftest::Skip(s.checkId, detail + " (the staff was put away first)");
			} else if (attachedOurs && model.value_or(true)) {
				selftest::Report(s.checkId, true, detail + (model ? ": attached, model checked by its marker node" : ": attached (model not in the marker table)"));
			} else if (s.clock - s.checkStart >= kCheckSecs) {
				selftest::Report(s.checkId, false, detail + (attachedOurs ? ": another model is attached" : ": nothing attached"));
			} else {
				return;
			}
			s.checkId = nullptr;
		}

		void Refresh(RE::ActorMagicCaster* a_caster, const RE::MagicItem* a_item, const char* a_id)
		{
			auto* art = spell_copy::TipArtOf(a_item);
			g_detach(a_caster, true);
			const bool stillAttached = a_caster->flags.all(RE::ActorMagicCaster::Flags::kCastingArtAttached);
			if (art) {
				g_setArt(a_caster, art);
			}
			logger::debug("staff art: {} for {:08X} -> art {:08X}", a_id, a_item ? a_item->GetFormID() : 0u, art ? art->GetFormID() : 0u);
			auto& s = g_state;
			s.shownFor = a_item;
			s.wait = Wait::kNone;
			s.drawPending = false;
			s.slotWrites = hand_slot::Writes();
			if (stillAttached) {
				selftest::Report(a_id, false, std::format("art {:08X}: the old art was still attached right after DetachCastingArt", art ? art->GetFormID() : 0u));
				s.checkId = nullptr;
			} else if (art) {
				s.checkId = a_id;
				s.checkStart = s.clock;
				s.checkArt = art->GetFormID();
			} else {
				s.checkId = nullptr;  // an item without art: only the old one comes off, nothing to judge
			}
		}
	}

	bool Install()
	{
		if (REL::Module::get().version() != REL::Version{ 1, 6, 1170, 0 }) {
			logger::critical("staff art: 34150/34152 are known for 1.6.1170 only; the mod stays off");
			return false;
		}
		g_detach = reinterpret_cast<DetachArtFn>(REL::ID(34152).address());
		g_setArt = reinterpret_cast<SetCastingArtFn>(REL::ID(34150).address());
		logger::info("staff art: DetachCastingArt 34152 at {:X}, SetCastingArt 34150 at {:X}", reinterpret_cast<std::uintptr_t>(g_detach),
			reinterpret_cast<std::uintptr_t>(g_setArt));
		return true;
	}

	void Reset()
	{
		g_state = {};
	}

	void OnFrame(RE::PlayerCharacter* a_player, float a_delta, bool a_staffHeld)
	{
		auto& s = g_state;
		if (std::isfinite(a_delta) && a_delta > 0.0f) {
			s.clock += a_delta;
		}
		auto* caster = RightCaster(a_player);
		if (!caster || !g_detach) {
			return;
		}
		const bool drawn = Drawn(a_player);
		if (!a_staffHeld && s.checkId) {
			selftest::Skip(s.checkId, std::format("art {:08X} (the staff left the hand first)", s.checkArt));
			s.checkId = nullptr;
		}
		Judge(caster, drawn);
		if (!a_staffHeld) {
			// Another weapon or a spell: the engine owns that hand's art. Start over when a staff comes back.
			s.shownFor = nullptr;
			s.wait = Wait::kNone;
			s.drawPending = false;
			s.wasDrawn = drawn;
			return;
		}
		const auto* slot = a_player->GetActorRuntimeData().selectedSpells[RE::Actor::SlotTypes::kRightHand];
		const bool  slotIsCopy = spell_copy::IsCopy(slot);
		if (s.fresh) {
			s.fresh = false;
			s.wasDrawn = drawn;  // loaded with the staff out: not a draw (the slot change after the load refreshes instead)
		}
		if (drawn && !s.wasDrawn) {
			s.drawPending = slotIsCopy;  // the engine attaches on draw by itself; ours makes sure it is the copy's art
			s.drawAt = s.clock;
			s.drawWrites = hand_slot::Writes();
		}
		s.wasDrawn = drawn;
		if (s.drawPending && slotIsCopy) {
			// Right after a draw the engine attaches the slot item's art by itself (the copy is preloaded). Checked before
			// any other trigger, so a change made while sheathed does not detach an art the engine just put on (Codex r2).
			// Only when the copy was already in the slot at the draw, so the engine attached art for the copy and not
			// for an item we replaced since (two frost models share one marker node). A model not in the marker table is
			// trusted when our art is attached and taken up, instead of detaching it after the grace (Carbon 2026-11-17).
			const bool sameSlot = hand_slot::Writes() == s.drawWrites;
			const bool attachedTakenUp = caster->flags.all(RE::ActorMagicCaster::Flags::kCastingArtAttached) && caster->castingArtData.attachedArt && !caster->castingArt;
			if (sameSlot && ShowsArt(caster, spell_copy::TipArtOf(slot)).value_or(attachedTakenUp)) {
				if (s.wait == Wait::kSheathed) {
					selftest::Report("T-ART-SHEATHED", true, std::format("copy {:08X}: the engine attached its art on draw, model checked", slot->GetFormID()));
				}
				logger::debug("staff art: on draw the engine attached the copy's art itself");
				s.shownFor = slot;
				s.slotWrites = hand_slot::Writes();
				s.wait = Wait::kNone;
				s.drawPending = false;
				return;
			}
			if (s.clock - s.drawAt < kDrawGraceSecs) {
				return;  // give the engine's own attach a moment first
			}
		}
		const char* id = nullptr;
		if (slot != s.shownFor && (slotIsCopy || spell_copy::IsCopy(s.shownFor))) {
			id = slotIsCopy ? "T-ART-SWAP" : "T-ART-CONTROL";  // CONTROL: back to the staff's own enchantment
		} else if (slotIsCopy && hand_slot::Writes() != s.slotWrites) {
			id = "T-ART-SWAP";  // written again (the engine had put another item in between)
		} else if (s.drawPending && slotIsCopy) {
			id = "T-ART-DRAW";
		}
		if (!id) {
			return;
		}
		const bool idle = caster->state.underlying() == 0;
		if (!drawn || !idle || !a_player->Get3D()) {
			if (s.wait == Wait::kNone) {
				s.wait = !drawn ? Wait::kSheathed : !idle ? Wait::kCasting : Wait::kNone;
			}
			return;
		}
		if (s.wait == Wait::kSheathed) {
			id = "T-ART-SHEATHED";
		} else if (s.wait == Wait::kCasting) {
			id = "T-ART-MIDCAST";
		}
		Refresh(caster, slot, id);
	}
}

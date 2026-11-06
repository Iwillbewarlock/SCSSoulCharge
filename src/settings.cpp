#include "PCH.h"

#include "settings.h"

#include <Windows.h>

namespace scssc
{
	namespace
	{
		constexpr auto kIniPath = L"Data\\SKSE\\Plugins\\SCSSoulCharge.ini";
		constexpr int  kMaxBasicTier = 5;
		constexpr DWORD kListBufferSize = 8192;

		bool ReadBool(const wchar_t* a_section, const wchar_t* a_key, bool a_default)
		{
			return ::GetPrivateProfileIntW(a_section, a_key, a_default ? 1 : 0, kIniPath) != 0;
		}

		std::vector<std::string> ReadList(const wchar_t* a_section, const wchar_t* a_key)
		{
			std::wstring buffer(kListBufferSize, L'\0');
			const auto   length = ::GetPrivateProfileStringW(a_section, a_key, L"", buffer.data(), kListBufferSize, kIniPath);
			buffer.resize(length);
			std::vector<std::string> out;
			std::size_t              start = 0;
			while (start <= buffer.size()) {
				const auto end = buffer.find(L',', start);
				auto       item = buffer.substr(start, end == std::wstring::npos ? std::wstring::npos : end - start);
				std::erase_if(item, [](wchar_t c) { return c == L' ' || c == L'\t'; });
				if (!item.empty()) {
					out.push_back(SKSE::stl::utf16_to_utf8(item).value_or(""));
				}
				if (end == std::wstring::npos) {
					break;
				}
				start = end + 1;
			}
			return out;
		}

		std::optional<RE::FormID> ResolveSpec(const std::string& a_spec)
		{
			const auto colon = a_spec.find(':');
			if (colon == std::string::npos || colon == 0 || colon + 1 >= a_spec.size()) {
				return std::nullopt;
			}
			RE::FormID local = 0;
			try {
				local = static_cast<RE::FormID>(std::stoul(a_spec.substr(0, colon), nullptr, 16));
			} catch (const std::exception&) {
				return std::nullopt;
			}
			auto* data = RE::TESDataHandler::GetSingleton();
			if (!data) {
				return std::nullopt;
			}
			const auto id = data->LookupFormID(local, a_spec.substr(colon + 1));
			return id ? std::optional<RE::FormID>(id) : std::nullopt;
		}
	}

	Settings& Settings::Get()
	{
		static Settings instance;
		return instance;
	}

	void Settings::Load()
	{
		basicTier = static_cast<int>(::GetPrivateProfileIntW(L"General", L"iBasicTier", 0, kIniPath));
		if (basicTier < 0 || basicTier > kMaxBasicTier) {
			logger::warn("iBasicTier={} is outside 0..{}; using 0", basicTier, kMaxBasicTier);
			basicTier = 0;
		}
		keepStaffNames = ReadBool(L"General", L"bKeepStaffNames", true);
		includeScripted = ReadBool(L"Staves", L"bIncludeScriptedStaves", false);
		onlyListed = ReadBool(L"Staves", L"bOnlyListedStaves", true);
		includeSpecs = ReadList(L"Staves", L"sInclude");
		excludeSpecs = ReadList(L"Staves", L"sExclude");
		selfTest = ReadBool(L"SelfTest", L"bEnabled", false);
		logger::info("Settings: iBasicTier={} bKeepStaffNames={} bIncludeScriptedStaves={} bOnlyListedStaves={} include={} exclude={} selfTest={}",
			basicTier, keepStaffNames, includeScripted, onlyListed, includeSpecs.size(), excludeSpecs.size(), selfTest);
	}

	core::StaffFilterSettings ResolveFilterSettings(const Settings& a_settings)
	{
		core::StaffFilterSettings out;
		out.includeScripted = a_settings.includeScripted;
		out.onlyListed = a_settings.onlyListed;
		const auto resolveInto = [](const std::vector<std::string>& a_specs, std::unordered_set<std::uint32_t>& a_set, std::string_view a_label) {
			for (const auto& spec : a_specs) {
				if (const auto id = ResolveSpec(spec)) {
					a_set.insert(*id);
				} else {
					logger::warn("{} entry '{}' not found (expected LocalFormID:Plugin.esp); ignored", a_label, spec);
				}
			}
		};
		resolveInto(a_settings.includeSpecs, out.include, "sInclude");
		resolveInto(a_settings.excludeSpecs, out.exclude, "sExclude");
		return out;
	}
}

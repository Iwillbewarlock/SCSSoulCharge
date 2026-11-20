#include "PCH.h"

#include "settings.h"

#include <Windows.h>

namespace scssc
{
	namespace
	{
		constexpr auto  kIniPath = L"Data\\SKSE\\Plugins\\SCSSoulCharge.ini";
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
				// trim the ends only: plugin names may contain spaces
				const auto first = item.find_first_not_of(L" \t");
				const auto last = item.find_last_not_of(L" \t");
				item = first == std::wstring::npos ? std::wstring{} : item.substr(first, last - first + 1);
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
			} catch (const std::invalid_argument&) {
				return std::nullopt;
			} catch (const std::out_of_range&) {
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
		selfTest = ReadBool(L"SelfTest", L"bEnabled", false);
		staffMarkerOn = ReadBool(L"Display", L"bStaffMarker", true);
		if (!staffMarkerOn) {
			staffMarkerRight.clear();
			staffMarkerLeft.clear();
		} else {
			// The INI cannot hold a leading space: one is put back.
			if (const auto custom = ReadList(L"Display", L"sStaffMarkerRight"); !custom.empty()) {
				staffMarkerRight = " " + custom.front();
			}
			if (const auto custom = ReadList(L"Display", L"sStaffMarkerLeft"); !custom.empty()) {
				staffMarkerLeft = " " + custom.front();
			}
		}
		staffMarkerBoth = staffMarkerRight + staffMarkerLeft;
		excludeSpecs = ReadList(L"Staves", L"sExclude");
		restoreNames = ReadBool(L"Uninstall", L"bRestoreNames", false);
		logger::info("Settings: selfTest={} exclude={} staffMarker='{}'/'{}' restoreNames={}", selfTest, excludeSpecs.size(), staffMarkerRight, staffMarkerLeft,
			restoreNames);
	}

	std::unordered_set<RE::FormID> ResolveExcludedStaves(const Settings& a_settings)
	{
		std::unordered_set<RE::FormID> out;
		for (const auto& spec : a_settings.excludeSpecs) {
			if (const auto id = ResolveSpec(spec)) {
				out.insert(*id);
			} else {
				logger::warn("sExclude entry '{}' not found (expected LocalFormID:Plugin.esp, e.g. 4DED8:Skyrim.esm); ignored", spec);
			}
		}
		return out;
	}
}

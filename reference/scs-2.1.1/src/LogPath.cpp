#include "SpellCastingStaff/LogPath.h"

#include <ShlObj.h> // SHGetKnownFolderPath(FOLDERID_Documents)
#include <combaseapi.h> // CoTaskMemFree
#include <memory>
#include <system_error>

namespace SpellCastingStaff::LogPath {

std::optional<std::filesystem::path> ResolveDirectory() {
    PWSTR rawDocuments = nullptr;
    const HRESULT result = ::SHGetKnownFolderPath(::FOLDERID_Documents, KF_FLAG_DEFAULT,
                                                   nullptr, &rawDocuments);
    std::unique_ptr<wchar_t, decltype(&::CoTaskMemFree)> documents(rawDocuments, &::CoTaskMemFree);
    if (FAILED(result) || !documents || !*documents) return std::nullopt;

    const auto directory = FromDocuments(std::filesystem::path(documents.get()));
    std::error_code ec;
    std::filesystem::create_directories(directory, ec);
    if (ec) return std::nullopt;
    if (!std::filesystem::is_directory(directory, ec) || ec) return std::nullopt;
    return directory;
}

}  // namespace SpellCastingStaff::LogPath

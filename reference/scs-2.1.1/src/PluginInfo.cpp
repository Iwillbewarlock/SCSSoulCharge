#include <SKSE/SKSE.h>

// Explicit version list: no claim of support for other AE, GOG or VR builds.
SKSEPluginInfo(
    .Version = REL::Version{2, 1, 1, 0},
    .Name = "SpellCastingStaff"sv,
    .Author = ""sv,
    .SupportEmail = ""sv,
    .StructCompatibility = SKSE::StructCompatibility::Dependent,
    .RuntimeCompatibility = {REL::Version{1, 5, 97, 0}, REL::Version{1, 6, 1170, 0}, REL::Version{1, 7, 104, 0}},
    .MinimumSKSEVersion = REL::Version{0, 0, 0, 0}
)

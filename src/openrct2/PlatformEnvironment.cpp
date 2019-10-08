/*****************************************************************************
 * Copyright (c) 2014-2019 OpenRCT2 developers
 *
 * For a complete list of all authors, please refer to contributors.md
 * Interested in contributing? Visit https://github.com/OpenRCT2/OpenRCT2
 *
 * OpenRCT2 is licensed under the GNU General Public License version 3.
 *****************************************************************************/

#include "PlatformEnvironment.h"

#include "OpenRCT2.h"
#include "config/Config.h"
#include "core/Path.hpp"
#include "core/String.hpp"
#include "platform/Platform2.h"
#include "platform/platform.h"

using namespace OpenRCT2;

class PlatformEnvironment final : public IPlatformEnvironment
{
private:
    std::string _basePath[DIRBASE_COUNT];

public:
    explicit PlatformEnvironment(DIRBASE_VALUES basePaths)
    {
        for (int32_t i = 0; i < DIRBASE_COUNT; i++)
        {
            _basePath[i] = basePaths[i];
        }
    }

    std::string GetDirectoryPath(DIRBASE base) const override
    {
        return _basePath[(size_t)base];
    }

    std::string GetDirectoryPath(DIRBASE base, DIRID did) const override
    {
        auto basePath = GetDirectoryPath(base);
        const utf8* directoryName;
        switch (base)
        {
            default:
            case DIRBASE::RCT1:
            case DIRBASE::RCT2:
                directoryName = DirectoryNamesRCT2[(size_t)did];
                break;
            case DIRBASE::OPENRCT2:
            case DIRBASE::USER:
            case DIRBASE::CONFIG:
                directoryName = DirectoryNamesOpenRCT2[(size_t)did];
                break;
        }

        return Path::Combine(basePath, directoryName);
    }

    std::string GetFilePath(PATHID pathid) const override
    {
        auto dirbase = GetDefaultBaseDirectory(pathid);
        auto basePath = GetDirectoryPath(dirbase);
        auto fileName = FileNames[(size_t)pathid];
        return Path::Combine(basePath, fileName);
    }

    void SetBasePath(DIRBASE base, const std::string& path) override
    {
        _basePath[(size_t)base] = path;
    }

private:
    static const char* DirectoryNamesRCT2[];
    static const char* DirectoryNamesOpenRCT2[];
    static const char* FileNames[];

    static DIRBASE GetDefaultBaseDirectory(PATHID pathid)
    {
        switch (pathid)
        {
            case PATHID::CONFIG:
            case PATHID::CONFIG_KEYBOARD:
                return DIRBASE::CONFIG;
            case PATHID::CACHE_OBJECTS:
            case PATHID::CACHE_TRACKS:
            case PATHID::CACHE_SCENARIOS:
                return DIRBASE::CACHE;
            case PATHID::MP_DAT:
                return DIRBASE::RCT1;
            case PATHID::SCORES_RCT2:
                return DIRBASE::RCT2;
            case PATHID::CHANGELOG:
                return DIRBASE::DOCUMENTATION;
            case PATHID::NETWORK_GROUPS:
            case PATHID::NETWORK_SERVERS:
            case PATHID::NETWORK_USERS:
            case PATHID::SCORES:
            case PATHID::SCORES_LEGACY:
            default:
                return DIRBASE::USER;
        }
    }
};

std::unique_ptr<IPlatformEnvironment> OpenRCT2::CreatePlatformEnvironment(DIRBASE_VALUES basePaths)
{
    return std::make_unique<PlatformEnvironment>(basePaths);
}

static std::string GetOpenRCT2DirectoryName()
{
#if defined(__ANDROID__)
    return "openrct2-user";
#else
    return "OpenRCT2";
#endif
}

std::unique_ptr<IPlatformEnvironment> OpenRCT2::CreatePlatformEnvironment()
{
    auto subDirectory = GetOpenRCT2DirectoryName();

    // Set default paths
    std::string basePaths[DIRBASE_COUNT];
    basePaths[(size_t)DIRBASE::OPENRCT2] = Platform::GetInstallPath();
    basePaths[(size_t)DIRBASE::USER] = Path::Combine(Platform::GetFolderPath(SPECIAL_FOLDER::USER_DATA), subDirectory);
    basePaths[(size_t)DIRBASE::CONFIG] = Path::Combine(Platform::GetFolderPath(SPECIAL_FOLDER::USER_CONFIG), subDirectory);
    basePaths[(size_t)DIRBASE::CACHE] = Path::Combine(Platform::GetFolderPath(SPECIAL_FOLDER::USER_CACHE), subDirectory);
    basePaths[(size_t)DIRBASE::DOCUMENTATION] = Platform::GetDocsPath();

    // Override paths that have been specified via the command line
    if (!String::IsNullOrEmpty(gCustomRCT1DataPath))
    {
        basePaths[(size_t)DIRBASE::RCT1] = gCustomRCT1DataPath;
    }
    if (!String::IsNullOrEmpty(gCustomRCT2DataPath))
    {
        basePaths[(size_t)DIRBASE::RCT2] = gCustomRCT2DataPath;
    }
    if (!String::IsNullOrEmpty(gCustomOpenrctDataPath))
    {
        basePaths[(size_t)DIRBASE::OPENRCT2] = gCustomOpenrctDataPath;
    }
    if (!String::IsNullOrEmpty(gCustomUserDataPath))
    {
        basePaths[(size_t)DIRBASE::USER] = gCustomUserDataPath;
        basePaths[(size_t)DIRBASE::CONFIG] = gCustomUserDataPath;
        basePaths[(size_t)DIRBASE::CACHE] = gCustomUserDataPath;
    }

    if (basePaths[(size_t)DIRBASE::DOCUMENTATION].empty())
    {
        basePaths[(size_t)DIRBASE::DOCUMENTATION] = basePaths[(size_t)DIRBASE::OPENRCT2];
    }

    auto env = OpenRCT2::CreatePlatformEnvironment(basePaths);

    // Now load the config so we can get the RCT1 and RCT2 paths
    auto configPath = env->GetFilePath(PATHID::CONFIG);
    config_set_defaults();
    if (!config_open(configPath.c_str()))
    {
        config_save(configPath.c_str());
    }
    if (String::IsNullOrEmpty(gCustomRCT1DataPath))
    {
        env->SetBasePath(DIRBASE::RCT1, String::ToStd(gConfigGeneral.rct1_path));
    }
    if (String::IsNullOrEmpty(gCustomRCT2DataPath))
    {
        env->SetBasePath(DIRBASE::RCT2, String::ToStd(gConfigGeneral.rct2_path));
    }

    // Log base paths
    log_verbose("DIRBASE::RCT1    : %s", env->GetDirectoryPath(DIRBASE::RCT1).c_str());
    log_verbose("DIRBASE::RCT2    : %s", env->GetDirectoryPath(DIRBASE::RCT2).c_str());
    log_verbose("DIRBASE::OPENRCT2: %s", env->GetDirectoryPath(DIRBASE::OPENRCT2).c_str());
    log_verbose("DIRBASE::USER    : %s", env->GetDirectoryPath(DIRBASE::USER).c_str());
    log_verbose("DIRBASE::CONFIG  : %s", env->GetDirectoryPath(DIRBASE::CONFIG).c_str());
    log_verbose("DIRBASE::CACHE   : %s", env->GetDirectoryPath(DIRBASE::CACHE).c_str());

    return env;
}

// clang-format off
const char * PlatformEnvironment::DirectoryNamesRCT2[] =
{
    "Data",                 // DATA
    "Landscapes",           // LANDSCAPE
    nullptr,                // LANGUAGE
    nullptr,                // LOG_CHAT
    nullptr,                // LOG_SERVER
    nullptr,                // NETWORK_KEY
    "ObjData",              // OBJECT
    "Saved Games",          // SAVE
    "Scenarios",            // SCENARIO
    nullptr,                // SCREENSHOT
    nullptr,                // SEQUENCE
    nullptr,                // SHADER
    nullptr,                // THEME
    "Tracks",               // TRACK
};

const char * PlatformEnvironment::DirectoryNamesOpenRCT2[] =
{
    "data",                 // DATA
    "landscape",            // LANDSCAPE
    "language",             // LANGUAGE
    "chatlogs",             // LOG_CHAT
    "serverlogs",           // LOG_SERVER
    "keys",                 // NETWORK_KEY
    "object",               // OBJECT
    "save",                 // SAVE
    "scenario",             // SCENARIO
    "screenshot",           // SCREENSHOT
    "sequence",             // SEQUENCE
    "shaders",              // SHADER
    "themes",               // THEME
    "track",                // TRACK
    "heightmap",            // HEIGHTMAP
    "replay",               // REPLAY
    "desyncs",              // DESYNCS
    "exported",             // EXPORTED_PARK_INFO
};

const char * PlatformEnvironment::FileNames[] =
{
    "config.ini",           // CONFIG
    "hotkeys.dat",          // CONFIG_KEYBOARD
    "objects.idx",          // CACHE_OBJECTS
    "tracks.idx",           // CACHE_TRACKS
    "scenarios.idx",        // CACHE_SCENARIOS
    "Data" PATH_SEPARATOR "mp.dat", // MP_DAT
    "groups.json",          // NETWORK_GROUPS
    "servers.cfg",          // NETWORK_SERVERS
    "users.json",           // NETWORK_USERS
    "highscores.dat",       // SCORES
    "scores.dat",           // SCORES (LEGACY)
    "Saved Games" PATH_SEPARATOR "scores.dat",  // SCORES (RCT2)
    "changelog.txt"         // CHANGELOG
};
// clang-format on

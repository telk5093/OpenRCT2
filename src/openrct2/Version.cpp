/*****************************************************************************
 * Copyright (c) 2014-2020 OpenRCT2 developers
 *
 * For a complete list of all authors, please refer to contributors.md
 * Interested in contributing? Visit https://github.com/OpenRCT2/OpenRCT2
 *
 * OpenRCT2 is licensed under the GNU General Public License version 3.
 *****************************************************************************/

#include "Version.h"

#include "config/Config.h"
#include "core/Console.hpp"
#include "core/Http.h"
#include "core/Json.hpp"

#include <chrono>

#ifdef OPENRCT2_BUILD_INFO_HEADER
#    include OPENRCT2_BUILD_INFO_HEADER
#endif

const char gVersionInfoTag[] =
#ifdef OPENRCT2_VERSION_TAG
    OPENRCT2_VERSION_TAG
#else
    "v" OPENRCT2_VERSION
#endif
    ;

const char gVersionInfoFull[] = OPENRCT2_NAME ", "
#ifdef OPENRCT2_VERSION_TAG
    OPENRCT2_VERSION_TAG
#else
                                              "v" OPENRCT2_VERSION
#endif
#if defined(OPENRCT2_BRANCH) || defined(OPENRCT2_COMMIT_SHA1_SHORT) || !defined(NDEBUG)
                                              " ("
#    if defined(OPENRCT2_BRANCH) && defined(OPENRCT2_COMMIT_SHA1_SHORT)
    OPENRCT2_COMMIT_SHA1_SHORT " on " OPENRCT2_BRANCH
#    elif defined(OPENRCT2_COMMIT_SHA1_SHORT)
    OPENRCT2_COMMIT_SHA1_SHORT
#    elif defined(OPENRCT2_BRANCH)
    OPENRCT2_BRANCH
#    endif
#    ifndef NDEBUG
                                              ", DEBUG"
#    endif
                                              ")"
#endif
#ifdef OPENRCT2_BUILD_SERVER
                                              " provided by " OPENRCT2_BUILD_SERVER
#endif
    ;

NewVersionInfo get_latest_version()
{
    // If the check doesn't succeed, provide current version so we don't bother user
    // with invalid data.
    std::string tag = gVersionInfoTag;
    NewVersionInfo verinfo{ tag, "", "", "" };
#ifndef DISABLE_HTTP
    auto now = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    auto then = gConfigGeneral.last_version_check_time;
    if (then < now - 24 * 60 * 60)
    {
        Http::Request request;
        request.url = "https://api.github.com/repos/OpenRCT2/OpenRCT2/releases/latest";
        request.method = Http::Method::GET;

        Http::Response res;
        try
        {
            res = Do(request);
            if (res.status != Http::Status::OK)
                throw std::runtime_error("bad http status");
        }
        catch (std::exception& e)
        {
            Console::Error::WriteLine("Failed to download '%s', cause %s", request.url.c_str(), e.what());
            return {};
        }

        json_t* root = Json::FromString(res.body);

        auto get_as_string = [root](std::string name) {
            std::string value;
            json_t* json_value = json_object_get(root, name.c_str());
            if (json_is_string(json_value))
            {
                value = (json_string_value(json_value));
            }
            return value;
        };

        verinfo.tag = get_as_string("tag_name");
        verinfo.name = get_as_string("name");
        verinfo.changelog = get_as_string("body");
        verinfo.url = get_as_string("html_url");

        json_decref(root);

        gConfigGeneral.last_version_check_time = now;
        config_save_default();
    }
#endif
    return verinfo;
}

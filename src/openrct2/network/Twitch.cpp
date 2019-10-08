/*****************************************************************************
 * Copyright (c) 2014-2019 OpenRCT2 developers
 *
 * For a complete list of all authors, please refer to contributors.md
 * Interested in contributing? Visit https://github.com/OpenRCT2/OpenRCT2
 *
 * OpenRCT2 is licensed under the GNU General Public License version 3.
 *****************************************************************************/

#ifdef DISABLE_TWITCH

#    include "Twitch.h"

namespace Twitch
{
    void Update()
    {
    }
} // namespace Twitch

#else

#    ifdef DISABLE_HTTP
#        error HTTP must be enabled to use the TWITCH functionality.
#    endif

#    include "../Context.h"
#    include "../Game.h"
#    include "../OpenRCT2.h"
#    include "../PlatformEnvironment.h"
#    include "../actions/GuestSetFlagsAction.hpp"
#    include "../config/Config.h"
#    include "../core/FileStream.hpp"
#    include "../core/Json.hpp"
#    include "../core/Path.hpp"
#    include "../core/String.hpp"
#    include "../drawing/Drawing.h"
#    include "../interface/InteractiveConsole.h"
#    include "../localisation/Localisation.h"
#    include "../management/Finance.h"
#    include "../management/NewsItem.h"
#    include "../peep/Peep.h"
#    include "../platform/platform.h"
#    include "../util/Util.h"
#    include "../world/Sprite.h"
#    include "../world/park.h"
#    include "Http.h"
#    include "Twitch.h"

#    include <jansson.h>
#    include <memory>
#    include <vector>

using namespace OpenRCT2;
using namespace OpenRCT2::Networking;

bool gTwitchEnable = false;

namespace Twitch
{
    enum
    {
        TWITCH_STATE_JOINING,
        TWITCH_STATE_JOINED,
        TWITCH_STATE_WAITING,
        TWITCH_STATE_GET_FOLLOWERS,
        TWITCH_STATE_GET_MESSAGES,
        TWITCH_STATE_LEAVING,
        TWITCH_STATE_LEFT
    };

    enum
    {
        TWITCH_STATUS_OK = 200
    };

    struct AudienceMember
    {
        const char* Name;
        bool IsFollower;
        bool IsInChat;
        bool IsMod;
        bool Exists;
        bool ShouldTrack;

        static AudienceMember FromJson(json_t* json)
        {
            AudienceMember member = {};

            if (!json_is_object(json))
                return member;
            json_t* name = json_object_get(json, "name");
            json_t* isFollower = json_object_get(json, "isFollower");
            json_t* isInChat = json_object_get(json, "inChat");
            json_t* isMod = json_object_get(json, "isMod");

            member.Name = json_string_value(name);
            member.IsFollower = json_is_true(isFollower);
            member.IsInChat = json_is_true(isInChat);
            member.IsMod = json_is_true(isMod);
            member.Exists = false;
            member.ShouldTrack = false;
            return member;
        }
    };

    /**
     * The time between HTTP requests.
     * TODO Ideally, the chat message pulse should be more frequent than the followers / chat members so that news messages etc.
     * have a lower latency.
     */
    constexpr uint32_t PulseTime = 10 * 1000;

    static int32_t _twitchState = TWITCH_STATE_LEFT;
    static bool _twitchIdle = true;
    static uint32_t _twitchLastPulseTick = 0;
    static int32_t _twitchLastPulseOperation = 1;
    static Http::Response _twitchJsonResponse;

    static void Join();
    static void Leave();
    static void GetFollowers();
    static void GetMessages();
    static void ParseFollowers();
    static void ParseMessages();
    static bool ShouldTrackMember(const AudienceMember* member);
    static bool ShouldMemberBeGuest(const AudienceMember* member);
    static void ManageGuestNames(std::vector<AudienceMember>& members);
    static void ParseChatMessage(const char* message);
    static void DoChatMessageNews(const char* message);

    static bool IsTwitchEnabled()
    {
        if (!gTwitchEnable)
            return false;
        if (gScreenFlags & (~SCREEN_FLAGS_PLAYING))
            return false;
        if (String::IsNullOrEmpty(gConfigTwitch.channel))
            return false;
        return true;
    }

    static void ExportParkInfoIntoTextfile(std::string file_name, double export_data, const char* custom_format="%d")
    {
        utf8 buffer[256];
        auto _env = OpenRCT2::CreatePlatformEnvironment();

        auto fs = FileStream(Path::Combine(_env->GetDirectoryPath(DIRBASE::USER, DIRID::EXPORTED_PARK_INFO), file_name), FILE_MODE_WRITE);
        sprintf(buffer, custom_format, export_data);
        fs.Write(buffer, strlen(buffer));
    }

    void ExportParkInfo()
    {
        // Export files
        if (IsTwitchEnabled() && gConfigTwitch.enable_exporting_park_info)
        {
            // Park value
            ExportParkInfoIntoTextfile("park_value.txt", gParkValue / 10);

            // Company Value
            ExportParkInfoIntoTextfile("company_value.txt", gCompanyValue / 10);

            // Park rating
            ExportParkInfoIntoTextfile("park_rating.txt", gParkRating);

            // Guests in park
            ExportParkInfoIntoTextfile("guests_in_park.txt", gNumGuestsInPark);

            // Current cash
            ExportParkInfoIntoTextfile("cash.txt", gCash / 10, "%.2f");

            // Profit of current month from rides
            money32 thisMonthRideIncome = gExpenditureTable[0][RCT_EXPENDITURE_TYPE_PARK_RIDE_TICKETS];
            ExportParkInfoIntoTextfile("profit_from_rides.txt", thisMonthRideIncome / 10, "%f");

            // Profit of current month from shops and stalls
            money32* thisMonthExpenditure = gExpenditureTable[0];
            int32_t thisMonthShopProfit = thisMonthExpenditure[RCT_EXPENDITURE_TYPE_SHOP_SHOP_SALES]
                + thisMonthExpenditure[RCT_EXPENDITURE_TYPE_SHOP_STOCK]
                + thisMonthExpenditure[RCT_EXPENDITURE_TYPE_FOODDRINK_SALES]
                + thisMonthExpenditure[RCT_EXPENDITURE_TYPE_FOODDRINK_STOCK];
            ExportParkInfoIntoTextfile("profit_from_shops.txt", thisMonthShopProfit / 10, "%.2f");
        }
    }

    void Update()
    {
        if (!_twitchIdle)
            return;
        if (IsTwitchEnabled())
        {
            if (game_is_paused())
                return;

            switch (_twitchState)
            {
                case TWITCH_STATE_LEFT:
                {
                    uint32_t currentTime = platform_get_ticks();
                    uint32_t timeSinceLastPulse = currentTime - _twitchLastPulseTick;
                    if (_twitchLastPulseTick == 0 || timeSinceLastPulse > PulseTime)
                    {
                        _twitchLastPulseTick = currentTime;
                        Join();
                    }
                    break;
                }
                case TWITCH_STATE_JOINED:
                {
                    uint32_t currentTime = platform_get_ticks();
                    uint32_t timeSinceLastPulse = currentTime - _twitchLastPulseTick;
                    if (_twitchLastPulseTick == 0 || timeSinceLastPulse > PulseTime)
                    {
                        _twitchLastPulseTick = currentTime;
                        _twitchLastPulseOperation = (_twitchLastPulseOperation + 1) % 2;
                        switch (_twitchLastPulseOperation + TWITCH_STATE_GET_FOLLOWERS)
                        {
                            case TWITCH_STATE_GET_FOLLOWERS:
                                GetFollowers();
                                break;
                            case TWITCH_STATE_GET_MESSAGES:
                                if (gConfigTwitch.enable_news)
                                {
                                    GetMessages();
                                }
                                break;
                        }
                    }
                    break;
                }
                case TWITCH_STATE_GET_FOLLOWERS:
                    ParseFollowers();
                    break;
                case TWITCH_STATE_GET_MESSAGES:
                    ParseMessages();
                    break;
            }
        }
        else
        {
            if (_twitchState != TWITCH_STATE_LEFT)
            {
                Leave();
            }
        }
    }

    /**
     * GET /leave/:join
     */
    static void Join()
    {
        char url[256];

        if (gConfigTwitch.api_url == nullptr || strlen(gConfigTwitch.api_url) == 0)
        {
            auto context = GetContext();
            context->WriteLine("API URL is empty! skipping request...");
            return;
        }
        snprintf(url, sizeof(url), "%s/join/%s", gConfigTwitch.api_url, gConfigTwitch.channel);

        _twitchState = TWITCH_STATE_JOINING;
        _twitchIdle = false;

        Http::Request request;
        request.url = url;
        request.method = Http::Method::GET;

        Http::DoAsync(request, [](Http::Response res) {
            std::shared_ptr<void> _(nullptr, [&](...) { _twitchIdle = true; });

            if (res.status != Http::Status::OK)
            {
                _twitchState = TWITCH_STATE_LEFT;
                GetContext()->WriteLine("Unable to connect to twitch channel.");
                return;
            }

            auto root = Json::FromString(res.body);
            json_t* jsonStatus = json_object_get(root, "status");
            if (json_is_number(jsonStatus) && json_integer_value(jsonStatus) == TWITCH_STATUS_OK)
            {
                _twitchState = TWITCH_STATE_JOINED;
            }
            else
            {
                _twitchState = TWITCH_STATE_LEFT;
            }

            _twitchLastPulseTick = 0;
            GetContext()->WriteLine("Connected to twitch channel.");
        });
    }

    /**
     * GET /leave/:channel
     */
    static void Leave()
    {
        GetContext()->WriteLine("Left twitch channel.");
        _twitchJsonResponse = {};
        _twitchState = TWITCH_STATE_LEFT;
        _twitchLastPulseTick = 0;
        gTwitchEnable = false;

        // TODO reset all peeps with twitch flag

        // HTTP request no longer used as it could be abused
        // char url[256];
        // snprintf(url, sizeof(url), "%sleave/%s", TwitchExtendedBaseUrl, gConfigTwitch.channel);
        // _twitchState = TWITCH_STATE_LEAVING;
        // _twitchIdle = false;
        // http_request_json_async(url, [](http_json_response * jsonResponse) -> void
        // {
        //     http_request_json_dispose(jsonResponse);
        //     _twitchState = TWITCH_STATE_LEFT;
        //     _twitchIdle = true;
        //
        //     GetContext()->WriteLine("Left twitch channel.");
        // });
    }

    /**
     * GET /channel/:channel/audience
     */
    static void GetFollowers()
    {
        char url[256];

        if (gConfigTwitch.api_url == nullptr || strlen(gConfigTwitch.api_url) == 0)
        {
            auto context = GetContext();
            context->WriteLine("API URL is empty! skipping request...");
            return;
        }
        snprintf(url, sizeof(url), "%s/channel/%s/audience", gConfigTwitch.api_url, gConfigTwitch.channel);

        _twitchState = TWITCH_STATE_WAITING;
        _twitchIdle = false;

        Http::DoAsync({ url }, [](Http::Response res) {
            std::shared_ptr<void> _(nullptr, [&](...) { _twitchIdle = true; });

            if (res.status != Http::Status::OK)
            {
                _twitchState = TWITCH_STATE_JOINED;
                return;
            }

            _twitchJsonResponse = res;
            _twitchState = TWITCH_STATE_GET_FOLLOWERS;
        });
    }

    /**
     * GET /channel/:channel/messages
     */
    static void GetMessages()
    {
        char url[256];

        if (gConfigTwitch.api_url == nullptr || strlen(gConfigTwitch.api_url) == 0)
        {
            auto context = GetContext();
            context->WriteLine("API URL is empty! skipping request...");
            return;
        }
        snprintf(url, sizeof(url), "%s/channel/%s/messages", gConfigTwitch.api_url, gConfigTwitch.channel);

        _twitchState = TWITCH_STATE_WAITING;
        _twitchIdle = false;

        Http::DoAsync({ url }, [](Http::Response res) {
            std::shared_ptr<void> _(nullptr, [&](...) { _twitchIdle = true; });

            if (res.status != Http::Status::OK)
            {
                _twitchState = TWITCH_STATE_JOINED;
                return;
            }

            _twitchJsonResponse = res;
            _twitchState = TWITCH_STATE_GET_MESSAGES;
        });
    }

    static void ParseFollowers()
    {
        json_t* root = Json::FromString(_twitchJsonResponse.body);
        if (json_is_array(root))
        {
            std::vector<AudienceMember> members;

            size_t audienceCount = json_array_size(root);
            for (size_t i = 0; i < audienceCount; i++)
            {
                json_t* jsonAudienceMember = json_array_get(root, i);
                auto member = AudienceMember::FromJson(jsonAudienceMember);
                if (!String::IsNullOrEmpty(member.Name))
                {
                    member.ShouldTrack = ShouldTrackMember(&member);
                    if (ShouldMemberBeGuest(&member))
                    {
                        members.push_back(member);
                    }
                }
            }

            ManageGuestNames(members);
        }

        _twitchJsonResponse = {};
        _twitchState = TWITCH_STATE_JOINED;

        gfx_invalidate_screen();
    }

    static void ParseMessages()
    {
        json_t* root = Json::FromString(_twitchJsonResponse.body);
        if (json_is_array(root))
        {
            size_t messageCount = json_array_size(root);
            for (size_t i = 0; i < messageCount; i++)
            {
                json_t* jsonMessage = json_array_get(root, i);
                if (!json_is_object(jsonMessage))
                {
                    continue;
                }

                json_t* jsonText = json_object_get(jsonMessage, "message");
                const char* text = json_string_value(jsonText);
                ParseChatMessage(text);
            }
        }

        _twitchJsonResponse = {};
        _twitchState = TWITCH_STATE_JOINED;
    }

    static bool ShouldTrackMember(const AudienceMember* member)
    {
        if (member->IsInChat && gConfigTwitch.enable_chat_peep_tracking)
        {
            return true;
        }
        else if (member->IsFollower && gConfigTwitch.enable_follower_peep_tracking)
        {
            return true;
        }
        return false;
    }

    static bool ShouldMemberBeGuest(const AudienceMember* member)
    {
        if (gConfigTwitch.enable_chat_peep_names && member->IsInChat)
        {
            return true;
        }
        else if (gConfigTwitch.enable_follower_peep_names && member->IsFollower)
        {
            return true;
        }
        return false;
    }

    static void ManageGuestNames(std::vector<AudienceMember>& members)
    {
        // Check what followers are already in the park
        uint16_t spriteIndex;
        Peep* peep;
        FOR_ALL_GUESTS (spriteIndex, peep)
        {
            if (peep->name != nullptr)
            {
                uint8_t args[32]{};
                char buffer[256]{};
                peep->FormatNameTo(args);
                format_string(buffer, sizeof(buffer), STR_STRINGID, args);

                AudienceMember* member = nullptr;
                for (AudienceMember& m : members)
                {
                    if (String::Equals(buffer, m.Name, true))
                    {
                        member = &m;
                        m.Exists = true;
                        break;
                    }
                }

                if (peep->peep_flags & PEEP_FLAGS_TWITCH)
                {
                    if (member == nullptr)
                    {
                        // Member no longer peep name worthy
                        uint32_t flags = peep->peep_flags & ~(PEEP_FLAGS_TRACKING | PEEP_FLAGS_TWITCH);

                        auto guestSetFlagsAction = GuestSetFlagsAction(peep->sprite_index, flags);
                        GameActions::Execute(&guestSetFlagsAction);

                        // TODO set peep name back to number / real name
                    }
                    else
                    {
                        uint32_t flags = peep->peep_flags;
                        if (member->ShouldTrack)
                        {
                            flags |= (PEEP_FLAGS_TRACKING);
                        }
                        else if (!member->ShouldTrack)
                        {
                            flags &= ~(PEEP_FLAGS_TRACKING);
                        }
                        if (flags != peep->peep_flags)
                        {
                            auto guestSetFlagsAction = GuestSetFlagsAction(peep->sprite_index, flags);
                            GameActions::Execute(&guestSetFlagsAction);
                        }
                    }
                }
                else if (member != nullptr && !(peep->peep_flags & PEEP_FLAGS_LEAVING_PARK))
                {
                    // Peep with same name already exists but not twitch
                    uint32_t flags = peep->peep_flags | PEEP_FLAGS_TWITCH;
                    if (member->ShouldTrack)
                    {
                        flags |= PEEP_FLAGS_TRACKING;
                    }
                    auto guestSetFlagsAction = GuestSetFlagsAction(peep->sprite_index, flags);
                    GameActions::Execute(&guestSetFlagsAction);
                }
            }
        }

        // Rename non-named peeps to followers that aren't currently in the park.
        if (!members.empty())
        {
            size_t memberIndex = SIZE_MAX;
            FOR_ALL_GUESTS (spriteIndex, peep)
            {
                size_t originalMemberIndex = memberIndex;
                for (size_t i = memberIndex + 1; i < members.size(); i++)
                {
                    if (!members[i].Exists)
                    {
                        memberIndex = i;
                        break;
                    }
                }
                if (originalMemberIndex == memberIndex)
                {
                    break;
                }

                AudienceMember* member = &members[memberIndex];
                if (peep->name == nullptr && !(peep->peep_flags & PEEP_FLAGS_LEAVING_PARK))
                {
                    // Rename peep and add flags
                    auto memLen = std::strlen(member->Name) + 1;
                    peep->name = (char*)std::malloc(memLen);
                    if (peep->name != nullptr)
                    {
                        std::memcpy(peep->name, member->Name, memLen);

                        uint32_t flags = peep->peep_flags | PEEP_FLAGS_TWITCH;
                        if (member->ShouldTrack)
                        {
                            flags |= PEEP_FLAGS_TRACKING;
                        }

                        auto guestSetFlagsAction = GuestSetFlagsAction(peep->sprite_index, flags);
                        GameActions::Execute(&guestSetFlagsAction);
                    }
                }
                else
                {
                    // Peep still yet to be found for member
                    memberIndex--;
                }
            }
        }
    }

    static char* strskipwhitespace(const char* str)
    {
        while (*str == ' ' || *str == '\t')
        {
            str++;
        }
        return (char*)str;
    }

    static void ParseChatMessage(const char* message)
    {
        message = strskipwhitespace(message);
        if (!String::StartsWith(message, "!"))
        {
            return;
        }

        // Skip '!'
        message++;

        // Check that command is "news"
        const char *ch, *cmd;
        for (ch = message, cmd = "news"; *cmd != '\0'; ++ch, ++cmd)
        {
            if (*ch != *cmd)
                return;
        }

        if (!isspace(*ch))
            return;

        ch = strskipwhitespace(ch);
        DoChatMessageNews(ch);
    }

    static void DoChatMessageNews(const char* message)
    {
        if (gConfigTwitch.enable_news)
        {
            utf8 buffer[256];
            buffer[0] = (utf8)(uint8_t)FORMAT_TOPAZ;
            safe_strcpy(buffer + 1, message, sizeof(buffer) - 1);

            utf8_remove_formatting(buffer, false);

            // TODO Create a new news item type for twitch which has twitch icon
            news_item_add_to_queue_raw(NEWS_ITEM_BLANK, buffer, 0);
        }
    }
} // namespace Twitch

#endif

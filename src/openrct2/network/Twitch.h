/*****************************************************************************
 * Copyright (c) 2014-2019 OpenRCT2 developers
 *
 * For a complete list of all authors, please refer to contributors.md
 * Interested in contributing? Visit https://github.com/OpenRCT2/OpenRCT2
 *
 * OpenRCT2 is licensed under the GNU General Public License version 3.
 *****************************************************************************/

#ifndef _TWITCH_H_
#define _TWITCH_H_

#include "../common.h"

extern bool gTwitchEnable;

namespace Twitch
{
    void Update();
    void ExportParkInfo();
}

#endif

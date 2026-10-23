// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "GuestFrame.h"
namespace Lsx4::Apple {
struct GuestRunControl;
bool PresentAppleFrame(const GuestFrame& frame);
bool PresentAppleFrameControlled(const GuestFrame& frame,const GuestRunControl* control);
}

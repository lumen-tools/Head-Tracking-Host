// Head Tracking Host
// Copyright (C) 2026 lumen-tools and contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Other systems (Linux test builds): no system volume control.

#include "SystemVolume.h"

namespace sysvol
{
struct Control::Impl {};

Control::Control() : impl (std::make_unique<Impl>()) {}
Control::~Control() = default;

void Control::setDevices (const std::string&, const std::string&) {}
bool Control::readSource (Volume&)          { return false; }
bool Control::readTarget (Volume&)          { return false; }
bool Control::writeSource (float, bool)     { return false; }
bool Control::writeTarget (float, bool)     { return false; }
} // namespace sysvol

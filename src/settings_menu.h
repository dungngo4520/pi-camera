#pragma once

#include "camera_mode.h"

#include <cstdint>
#include <string>
#include <string_view>

namespace picamera {

int settingsTabItemCount(SettingsTab tab);

std::string_view settingsItemLabel(SettingsTab tab, int item);

std::string settingsItemValue(SettingsTab tab, int item, const CameraSettings &s);

void settingsItemAdjustLeft(SettingsTab tab, int item, CameraSettings &s);

void settingsItemAdjustRight(SettingsTab tab, int item, CameraSettings &s);

bool settingsNeedsReconfigure(const CameraSettings &before,
                              const CameraSettings &after);

CameraConfig settingsToCameraConfig(const CameraSettings &s,
                                     uint32_t captureWidth,
                                     uint32_t captureHeight);

}

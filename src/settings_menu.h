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

bool saveSettings(const CameraSettings &s, const std::string &path);

bool loadSettings(CameraSettings &s, const std::string &path);

std::string defaultSettingsPath();

// Picture style preset mapping: returns {brightness, contrast, saturation,
// sharpness} for the given style. Used by the settings menu to apply a
// preset when the user selects a picture style.
struct PictureStyleParams {
    float brightness;
    float contrast;
    float saturation;
    float sharpness;
};
PictureStyleParams pictureStyleParams(PictureStyle style);

// Compute the crop region for an aspect ratio on a given source dimension.
// Returns {cropX, cropY, cropW, cropH} (center-cropped, even-aligned).
struct CropRegion {
    uint32_t x;
    uint32_t y;
    uint32_t w;
    uint32_t h;
};
CropRegion aspectRatioCrop(uint32_t srcW, uint32_t srcH, AspectRatio ratio);

}

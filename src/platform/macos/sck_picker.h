/**
 * @file src/platform/macos/sck_picker.h
 * @brief C++-safe interface to the ScreenCaptureKit content sharing picker.
 */
#pragma once

/**
 * @brief Present the SCContentSharingPicker for the active capture stream on the host's display.
 * @return true if a capture stream is active and the picker will be presented, false otherwise.
 */
bool sck_present_picker();

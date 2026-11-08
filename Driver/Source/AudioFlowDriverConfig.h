// AudioFlowDriverConfig.h
// Shared constants for the AudioFlow HAL plug-in and its clients.
// Keep in sync with Sources/AudioFlowKit/DriverProtocol.swift.

#pragma once

#include <CoreAudio/AudioServerPlugIn.h>

namespace audioflow {

// Bundle identifier of the plug-in (used by apps to find the plug-in object
// via kAudioHardwarePropertyTranslateBundleIDToPlugIn).
#define kAudioFlowDriverBundleID "com.audioflow.driver"

constexpr UInt32 kDeviceCount = 6;
constexpr UInt32 kChannelCount = 2;          // Phase 1: stereo
constexpr UInt32 kBytesPerFrame = kChannelCount * sizeof(Float32);
constexpr UInt32 kRingFrames = 16384;        // also the zero time stamp period
constexpr Float64 kDefaultSampleRate = 48000.0;
constexpr Float64 kSupportedSampleRates[] = {44100.0, 48000.0, 88200.0, 96000.0};
constexpr UInt32 kSupportedSampleRateCount = 4;

// Object IDs. kAudioObjectPlugInObject (1) is the plug-in itself.
// Each device uses three consecutive IDs: device, input stream, output stream.
constexpr AudioObjectID kObjectID_PlugIn = kAudioObjectPlugInObject;
constexpr AudioObjectID kObjectID_FirstDevice = 2;
constexpr UInt32 kObjectsPerDevice = 3;

inline AudioObjectID DeviceObjectID(UInt32 index) { return kObjectID_FirstDevice + index * kObjectsPerDevice; }
inline AudioObjectID InputStreamID(UInt32 index) { return DeviceObjectID(index) + 1; }
inline AudioObjectID OutputStreamID(UInt32 index) { return DeviceObjectID(index) + 2; }

// Custom property on the plug-in object. Data type: CFPropertyList (CFDictionary)
//   { "names": [CFString x 6], "enabled": [CFBoolean x 6] }
// Setting it renames / enables / disables devices and persists the result.
constexpr AudioObjectPropertySelector kAudioFlowPropertyConfig = 'afcf';

#define kAudioFlowConfigKeyNames "names"
#define kAudioFlowConfigKeyEnabled "enabled"
#define kAudioFlowStorageKey "config"

}  // namespace audioflow

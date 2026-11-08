// AudioFlowDriver.cpp
// AudioServerPlugIn that publishes AudioFlow's virtual loopback devices.
//
// Each device is a loopback: audio written to its output stream by any
// client appears on its input stream. The device is clocked from the host
// clock (mach_absolute_time), like Apple's NullAudio sample.
//
// Real-time rules: DoIOOperation / GetZeroTimeStamp never lock, allocate,
// log or call into CF. Ring buffers are allocated once at Initialize.

#include "AudioFlowDriverConfig.h"

#include <CoreAudio/AudioServerPlugIn.h>
#include <CoreFoundation/CoreFoundation.h>
#include <dispatch/dispatch.h>
#include <mach/mach_time.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

using namespace audioflow;

// MARK: - State

namespace {

struct Device {
    UInt32 index = 0;
    std::string name;            // guarded by gStateMutex
    bool enabled = true;         // guarded by gStateMutex

    std::atomic<Float64> sampleRate{kDefaultSampleRate};
    std::atomic<Float64> hostTicksPerFrame{0};

    // IO state
    std::atomic<UInt32> ioClients{0};
    std::atomic<UInt64> anchorHostTime{0};
    std::atomic<UInt64> numberTimeStamps{0};

    std::vector<Float32> ring;   // kRingFrames * kChannelCount, allocated once
};

std::mutex gStateMutex;
Device gDevices[kDeviceCount];
Float64 gHostClockFrequency = 0;   // host ticks per second
AudioServerPlugInHostRef gHost = nullptr;
std::atomic<UInt32> gRefCount{0};

const char* kDefaultNames[kDeviceCount] = {
    "AudioFlow Virtual 1", "AudioFlow Virtual 2", "AudioFlow Virtual 3",
    "AudioFlow Virtual 4", "AudioFlow Monitor",   "AudioFlow Output",
};

#define kManufacturer CFSTR("AudioFlow")
#define kModelUID CFSTR("AudioFlowVirtualDevice")

// MARK: - Object ID helpers

enum class ObjectKind { None, PlugIn, Device, InputStream, OutputStream };

ObjectKind Classify(AudioObjectID objectID, UInt32* outIndex) {
    if (objectID == kObjectID_PlugIn) return ObjectKind::PlugIn;
    if (objectID < kObjectID_FirstDevice) return ObjectKind::None;
    UInt32 offset = objectID - kObjectID_FirstDevice;
    UInt32 index = offset / kObjectsPerDevice;
    if (index >= kDeviceCount) return ObjectKind::None;
    if (outIndex) *outIndex = index;
    switch (offset % kObjectsPerDevice) {
        case 0: return ObjectKind::Device;
        case 1: return ObjectKind::InputStream;
        default: return ObjectKind::OutputStream;
    }
}

bool IsSupportedRate(Float64 rate) {
    for (UInt32 i = 0; i < kSupportedSampleRateCount; ++i)
        if (kSupportedSampleRates[i] == rate) return true;
    return false;
}

void UpdateTicksPerFrame(Device& d) {
    d.hostTicksPerFrame.store(gHostClockFrequency / d.sampleRate.load());
}

CFStringRef CopyDeviceUID(UInt32 index) {
    return CFStringCreateWithFormat(nullptr, nullptr, CFSTR("AudioFlowDevice_%u"), index);
}

AudioStreamBasicDescription MakeFormat(Float64 rate) {
    AudioStreamBasicDescription f = {};
    f.mSampleRate = rate;
    f.mFormatID = kAudioFormatLinearPCM;
    f.mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagsNativeEndian | kAudioFormatFlagIsPacked;
    f.mBytesPerPacket = kBytesPerFrame;
    f.mFramesPerPacket = 1;
    f.mBytesPerFrame = kBytesPerFrame;
    f.mChannelsPerFrame = kChannelCount;
    f.mBitsPerChannel = 32;
    return f;
}

void NotifyAsync(AudioObjectID objectID, std::vector<AudioObjectPropertyAddress> addresses) {
    if (!gHost) return;
    auto* heap = new std::vector<AudioObjectPropertyAddress>(std::move(addresses));
    struct Ctx { AudioObjectID obj; std::vector<AudioObjectPropertyAddress>* addrs; };
    auto* ctx = new Ctx{objectID, heap};
    dispatch_async_f(dispatch_get_global_queue(DISPATCH_QUEUE_PRIORITY_DEFAULT, 0), ctx, [](void* p) {
        auto* c = static_cast<Ctx*>(p);
        if (gHost) gHost->PropertiesChanged(gHost, c->obj, (UInt32)c->addrs->size(), c->addrs->data());
        delete c->addrs;
        delete c;
    });
}

void RequestRateChange(UInt32 index, Float64 rate) {
    if (!gHost) return;
    struct Ctx { AudioObjectID dev; UInt64 rate; };
    auto* ctx = new Ctx{DeviceObjectID(index), (UInt64)rate};
    dispatch_async_f(dispatch_get_global_queue(DISPATCH_QUEUE_PRIORITY_DEFAULT, 0), ctx, [](void* p) {
        auto* c = static_cast<Ctx*>(p);
        if (gHost) gHost->RequestDeviceConfigurationChange(gHost, c->dev, c->rate, nullptr);
        delete c;
    });
}

// MARK: - Config persistence (call with gStateMutex held)

CFDictionaryRef CopyConfigLocked() {
    CFMutableArrayRef names = CFArrayCreateMutable(nullptr, kDeviceCount, &kCFTypeArrayCallBacks);
    CFMutableArrayRef enabled = CFArrayCreateMutable(nullptr, kDeviceCount, &kCFTypeArrayCallBacks);
    for (UInt32 i = 0; i < kDeviceCount; ++i) {
        CFStringRef n = CFStringCreateWithCString(nullptr, gDevices[i].name.c_str(), kCFStringEncodingUTF8);
        CFArrayAppendValue(names, n);
        CFRelease(n);
        CFArrayAppendValue(enabled, gDevices[i].enabled ? kCFBooleanTrue : kCFBooleanFalse);
    }
    const void* keys[] = {CFSTR(kAudioFlowConfigKeyNames), CFSTR(kAudioFlowConfigKeyEnabled)};
    const void* values[] = {names, enabled};
    CFDictionaryRef dict = CFDictionaryCreate(nullptr, keys, values, 2, &kCFTypeDictionaryKeyCallBacks,
                                              &kCFTypeDictionaryValueCallBacks);
    CFRelease(names);
    CFRelease(enabled);
    return dict;
}

std::string StringFromCF(CFStringRef s) {
    if (!s) return {};
    CFIndex len = CFStringGetMaximumSizeForEncoding(CFStringGetLength(s), kCFStringEncodingUTF8) + 1;
    std::string out((size_t)len, '\0');
    if (!CFStringGetCString(s, out.data(), len, kCFStringEncodingUTF8)) return {};
    out.resize(strlen(out.c_str()));
    return out;
}

// Applies a config dictionary. Returns which devices changed name and whether
// the enabled set changed. Ignores malformed entries.
void ApplyConfigLocked(CFDictionaryRef dict, bool changedName[kDeviceCount], bool* enabledChanged) {
    *enabledChanged = false;
    for (UInt32 i = 0; i < kDeviceCount; ++i) changedName[i] = false;
    if (!dict || CFGetTypeID(dict) != CFDictionaryGetTypeID()) return;

    auto names = (CFArrayRef)CFDictionaryGetValue(dict, CFSTR(kAudioFlowConfigKeyNames));
    if (names && CFGetTypeID(names) == CFArrayGetTypeID()) {
        CFIndex n = CFArrayGetCount(names);
        for (CFIndex i = 0; i < n && i < (CFIndex)kDeviceCount; ++i) {
            auto s = (CFStringRef)CFArrayGetValueAtIndex(names, i);
            if (!s || CFGetTypeID(s) != CFStringGetTypeID()) continue;
            std::string name = StringFromCF(s);
            if (name.empty() || name.size() > 128) continue;
            if (name != gDevices[i].name) {
                gDevices[i].name = name;
                changedName[i] = true;
            }
        }
    }
    auto enabled = (CFArrayRef)CFDictionaryGetValue(dict, CFSTR(kAudioFlowConfigKeyEnabled));
    if (enabled && CFGetTypeID(enabled) == CFArrayGetTypeID()) {
        CFIndex n = CFArrayGetCount(enabled);
        for (CFIndex i = 0; i < n && i < (CFIndex)kDeviceCount; ++i) {
            auto b = (CFBooleanRef)CFArrayGetValueAtIndex(enabled, i);
            if (!b || CFGetTypeID(b) != CFBooleanGetTypeID()) continue;
            bool v = CFBooleanGetValue(b);
            if (v != gDevices[i].enabled) {
                // Never disable a device that is in use.
                if (!v && gDevices[i].ioClients.load() > 0) continue;
                gDevices[i].enabled = v;
                *enabledChanged = true;
            }
        }
    }
}

void PersistConfigLocked() {
    if (!gHost) return;
    CFDictionaryRef dict = CopyConfigLocked();
    gHost->WriteToStorage(gHost, CFSTR(kAudioFlowStorageKey), dict);
    CFRelease(dict);
}

UInt32 EnabledDeviceIDsLocked(AudioObjectID* out, UInt32 max) {
    UInt32 n = 0;
    for (UInt32 i = 0; i < kDeviceCount && n < max; ++i)
        if (gDevices[i].enabled) out[n++] = DeviceObjectID(i);
    return n;
}

// MARK: - Interface forward declarations

HRESULT QueryInterface(void* inDriver, REFIID inUUID, LPVOID* outInterface);
ULONG AddRef(void* inDriver);
ULONG Release(void* inDriver);
OSStatus Initialize(AudioServerPlugInDriverRef inDriver, AudioServerPlugInHostRef inHost);
OSStatus CreateDevice(AudioServerPlugInDriverRef, CFDictionaryRef, const AudioServerPlugInClientInfo*, AudioObjectID*);
OSStatus DestroyDevice(AudioServerPlugInDriverRef, AudioObjectID);
OSStatus AddDeviceClient(AudioServerPlugInDriverRef, AudioObjectID, const AudioServerPlugInClientInfo*);
OSStatus RemoveDeviceClient(AudioServerPlugInDriverRef, AudioObjectID, const AudioServerPlugInClientInfo*);
OSStatus PerformDeviceConfigurationChange(AudioServerPlugInDriverRef, AudioObjectID, UInt64, void*);
OSStatus AbortDeviceConfigurationChange(AudioServerPlugInDriverRef, AudioObjectID, UInt64, void*);
Boolean HasProperty(AudioServerPlugInDriverRef, AudioObjectID, pid_t, const AudioObjectPropertyAddress*);
OSStatus IsPropertySettable(AudioServerPlugInDriverRef, AudioObjectID, pid_t, const AudioObjectPropertyAddress*, Boolean*);
OSStatus GetPropertyDataSize(AudioServerPlugInDriverRef, AudioObjectID, pid_t, const AudioObjectPropertyAddress*, UInt32,
                             const void*, UInt32*);
OSStatus GetPropertyData(AudioServerPlugInDriverRef, AudioObjectID, pid_t, const AudioObjectPropertyAddress*, UInt32,
                         const void*, UInt32, UInt32*, void*);
OSStatus SetPropertyData(AudioServerPlugInDriverRef, AudioObjectID, pid_t, const AudioObjectPropertyAddress*, UInt32,
                         const void*, UInt32, const void*);
OSStatus StartIO(AudioServerPlugInDriverRef, AudioObjectID, UInt32);
OSStatus StopIO(AudioServerPlugInDriverRef, AudioObjectID, UInt32);
OSStatus GetZeroTimeStamp(AudioServerPlugInDriverRef, AudioObjectID, UInt32, Float64*, UInt64*, UInt64*);
OSStatus WillDoIOOperation(AudioServerPlugInDriverRef, AudioObjectID, UInt32, UInt32, Boolean*, Boolean*);
OSStatus BeginIOOperation(AudioServerPlugInDriverRef, AudioObjectID, UInt32, UInt32, UInt32,
                          const AudioServerPlugInIOCycleInfo*);
OSStatus DoIOOperation(AudioServerPlugInDriverRef, AudioObjectID, AudioObjectID, UInt32, UInt32, UInt32,
                       const AudioServerPlugInIOCycleInfo*, void*, void*);
OSStatus EndIOOperation(AudioServerPlugInDriverRef, AudioObjectID, UInt32, UInt32, UInt32,
                        const AudioServerPlugInIOCycleInfo*);

AudioServerPlugInDriverInterface gInterface = {
    nullptr,
    QueryInterface,
    AddRef,
    Release,
    Initialize,
    CreateDevice,
    DestroyDevice,
    AddDeviceClient,
    RemoveDeviceClient,
    PerformDeviceConfigurationChange,
    AbortDeviceConfigurationChange,
    HasProperty,
    IsPropertySettable,
    GetPropertyDataSize,
    GetPropertyData,
    SetPropertyData,
    StartIO,
    StopIO,
    GetZeroTimeStamp,
    WillDoIOOperation,
    BeginIOOperation,
    DoIOOperation,
    EndIOOperation,
};
AudioServerPlugInDriverInterface* gInterfacePtr = &gInterface;
AudioServerPlugInDriverRef gDriverRef = &gInterfacePtr;

bool IsOurDriver(AudioServerPlugInDriverRef d) { return d == gDriverRef; }

}  // namespace

// MARK: - Factory

extern "C" __attribute__((visibility("default"))) void* AudioFlowDriver_Create(CFAllocatorRef, CFUUIDRef inRequestedTypeUUID) {
    if (CFEqual(inRequestedTypeUUID, kAudioServerPlugInTypeUUID)) return gDriverRef;
    return nullptr;
}

namespace {

// MARK: - IUnknown

HRESULT QueryInterface(void* inDriver, REFIID inUUID, LPVOID* outInterface) {
    if (inDriver != gDriverRef || !outInterface) return kAudioHardwareBadObjectError;
    CFUUIDRef requested = CFUUIDCreateFromUUIDBytes(nullptr, inUUID);
    HRESULT result = E_NOINTERFACE;
    if (CFEqual(requested, IUnknownUUID) || CFEqual(requested, kAudioServerPlugInDriverInterfaceUUID)) {
        gRefCount.fetch_add(1);
        *outInterface = gDriverRef;
        result = S_OK;
    }
    CFRelease(requested);
    return result;
}

ULONG AddRef(void* inDriver) {
    if (inDriver != gDriverRef) return 0;
    return gRefCount.fetch_add(1) + 1;
}

ULONG Release(void* inDriver) {
    if (inDriver != gDriverRef) return 0;
    UInt32 v = gRefCount.load();
    if (v == 0) return 0;
    return gRefCount.fetch_sub(1) - 1;
}

// MARK: - Basic operations

OSStatus Initialize(AudioServerPlugInDriverRef inDriver, AudioServerPlugInHostRef inHost) {
    if (!IsOurDriver(inDriver)) return kAudioHardwareBadObjectError;
    gHost = inHost;

    mach_timebase_info_data_t tb;
    mach_timebase_info(&tb);
    gHostClockFrequency = (Float64)tb.denom / (Float64)tb.numer * 1000000000.0;

    std::lock_guard<std::mutex> lock(gStateMutex);
    for (UInt32 i = 0; i < kDeviceCount; ++i) {
        Device& d = gDevices[i];
        d.index = i;
        d.name = kDefaultNames[i];
        d.enabled = true;
        d.sampleRate.store(kDefaultSampleRate);
        d.ring.assign((size_t)kRingFrames * kChannelCount, 0.0f);
        UpdateTicksPerFrame(d);
    }

    // Restore persisted configuration.
    CFPropertyListRef stored = nullptr;
    if (gHost->CopyFromStorage(gHost, CFSTR(kAudioFlowStorageKey), &stored) == 0 && stored) {
        bool changed[kDeviceCount];
        bool enabledChanged;
        ApplyConfigLocked((CFDictionaryRef)stored, changed, &enabledChanged);
        CFRelease(stored);
    }
    return kAudioHardwareNoError;
}

OSStatus CreateDevice(AudioServerPlugInDriverRef, CFDictionaryRef, const AudioServerPlugInClientInfo*, AudioObjectID*) {
    return kAudioHardwareUnsupportedOperationError;
}

OSStatus DestroyDevice(AudioServerPlugInDriverRef, AudioObjectID) { return kAudioHardwareUnsupportedOperationError; }

OSStatus AddDeviceClient(AudioServerPlugInDriverRef inDriver, AudioObjectID, const AudioServerPlugInClientInfo*) {
    return IsOurDriver(inDriver) ? kAudioHardwareNoError : kAudioHardwareBadObjectError;
}

OSStatus RemoveDeviceClient(AudioServerPlugInDriverRef inDriver, AudioObjectID, const AudioServerPlugInClientInfo*) {
    return IsOurDriver(inDriver) ? kAudioHardwareNoError : kAudioHardwareBadObjectError;
}

OSStatus PerformDeviceConfigurationChange(AudioServerPlugInDriverRef inDriver, AudioObjectID inDevice,
                                          UInt64 inChangeAction, void*) {
    if (!IsOurDriver(inDriver)) return kAudioHardwareBadObjectError;
    UInt32 index;
    if (Classify(inDevice, &index) != ObjectKind::Device) return kAudioHardwareBadDeviceError;
    Float64 rate = (Float64)inChangeAction;
    if (!IsSupportedRate(rate)) return kAudioHardwareIllegalOperationError;
    Device& d = gDevices[index];
    d.sampleRate.store(rate);
    UpdateTicksPerFrame(d);
    return kAudioHardwareNoError;
}

OSStatus AbortDeviceConfigurationChange(AudioServerPlugInDriverRef inDriver, AudioObjectID, UInt64, void*) {
    return IsOurDriver(inDriver) ? kAudioHardwareNoError : kAudioHardwareBadObjectError;
}

// MARK: - Property support tables

bool PlugInHas(AudioObjectPropertySelector s) {
    switch (s) {
        case kAudioObjectPropertyBaseClass:
        case kAudioObjectPropertyClass:
        case kAudioObjectPropertyOwner:
        case kAudioObjectPropertyManufacturer:
        case kAudioObjectPropertyOwnedObjects:
        case kAudioObjectPropertyCustomPropertyInfoList:
        case kAudioPlugInPropertyDeviceList:
        case kAudioPlugInPropertyTranslateUIDToDevice:
        case kAudioPlugInPropertyResourceBundle:
        case kAudioFlowPropertyConfig:
            return true;
    }
    return false;
}

bool DeviceHas(AudioObjectPropertySelector s) {
    switch (s) {
        case kAudioObjectPropertyBaseClass:
        case kAudioObjectPropertyClass:
        case kAudioObjectPropertyOwner:
        case kAudioObjectPropertyName:
        case kAudioObjectPropertyManufacturer:
        case kAudioObjectPropertyOwnedObjects:
        case kAudioDevicePropertyDeviceUID:
        case kAudioDevicePropertyModelUID:
        case kAudioDevicePropertyTransportType:
        case kAudioDevicePropertyRelatedDevices:
        case kAudioDevicePropertyClockDomain:
        case kAudioDevicePropertyDeviceIsAlive:
        case kAudioDevicePropertyDeviceIsRunning:
        case kAudioDevicePropertyDeviceCanBeDefaultDevice:
        case kAudioDevicePropertyDeviceCanBeDefaultSystemDevice:
        case kAudioDevicePropertyLatency:
        case kAudioDevicePropertyStreams:
        case kAudioObjectPropertyControlList:
        case kAudioDevicePropertySafetyOffset:
        case kAudioDevicePropertyNominalSampleRate:
        case kAudioDevicePropertyAvailableNominalSampleRates:
        case kAudioDevicePropertyIsHidden:
        case kAudioDevicePropertyZeroTimeStampPeriod:
        case kAudioDevicePropertyPreferredChannelsForStereo:
        case kAudioDevicePropertyClockIsStable:
            return true;
    }
    return false;
}

bool StreamHas(AudioObjectPropertySelector s) {
    switch (s) {
        case kAudioObjectPropertyBaseClass:
        case kAudioObjectPropertyClass:
        case kAudioObjectPropertyOwner:
        case kAudioObjectPropertyOwnedObjects:
        case kAudioStreamPropertyIsActive:
        case kAudioStreamPropertyDirection:
        case kAudioStreamPropertyTerminalType:
        case kAudioStreamPropertyStartingChannel:
        case kAudioStreamPropertyLatency:
        case kAudioStreamPropertyVirtualFormat:
        case kAudioStreamPropertyPhysicalFormat:
        case kAudioStreamPropertyAvailableVirtualFormats:
        case kAudioStreamPropertyAvailablePhysicalFormats:
            return true;
    }
    return false;
}

Boolean HasProperty(AudioServerPlugInDriverRef inDriver, AudioObjectID inObjectID, pid_t,
                    const AudioObjectPropertyAddress* inAddress) {
    if (!IsOurDriver(inDriver) || !inAddress) return false;
    switch (Classify(inObjectID, nullptr)) {
        case ObjectKind::PlugIn: return PlugInHas(inAddress->mSelector);
        case ObjectKind::Device: return DeviceHas(inAddress->mSelector);
        case ObjectKind::InputStream:
        case ObjectKind::OutputStream: return StreamHas(inAddress->mSelector);
        default: return false;
    }
}

OSStatus IsPropertySettable(AudioServerPlugInDriverRef inDriver, AudioObjectID inObjectID, pid_t pid,
                            const AudioObjectPropertyAddress* inAddress, Boolean* outIsSettable) {
    if (!IsOurDriver(inDriver)) return kAudioHardwareBadObjectError;
    if (!inAddress || !outIsSettable) return kAudioHardwareIllegalOperationError;
    if (!HasProperty(inDriver, inObjectID, pid, inAddress)) return kAudioHardwareUnknownPropertyError;
    ObjectKind kind = Classify(inObjectID, nullptr);
    switch (inAddress->mSelector) {
        case kAudioFlowPropertyConfig: *outIsSettable = (kind == ObjectKind::PlugIn); break;
        case kAudioDevicePropertyNominalSampleRate: *outIsSettable = (kind == ObjectKind::Device); break;
        case kAudioStreamPropertyVirtualFormat:
        case kAudioStreamPropertyPhysicalFormat:
            *outIsSettable = (kind == ObjectKind::InputStream || kind == ObjectKind::OutputStream);
            break;
        default: *outIsSettable = false; break;
    }
    return kAudioHardwareNoError;
}

// MARK: - Property data

// Writes `value` if there is room. Returns the status.
template <typename T>
OSStatus Put(const T& value, UInt32 inDataSize, UInt32* outDataSize, void* outData) {
    if (inDataSize < sizeof(T)) return kAudioHardwareBadPropertySizeError;
    std::memcpy(outData, &value, sizeof(T));
    *outDataSize = sizeof(T);
    return kAudioHardwareNoError;
}

OSStatus PutIDs(const AudioObjectID* ids, UInt32 count, UInt32 inDataSize, UInt32* outDataSize, void* outData) {
    UInt32 n = std::min<UInt32>(count, inDataSize / sizeof(AudioObjectID));
    if (n) std::memcpy(outData, ids, n * sizeof(AudioObjectID));
    *outDataSize = n * sizeof(AudioObjectID);
    return kAudioHardwareNoError;
}

UInt32 DeviceStreamIDs(UInt32 index, AudioObjectPropertyScope scope, AudioObjectID out[2]) {
    UInt32 n = 0;
    if (scope == kAudioObjectPropertyScopeGlobal || scope == kAudioObjectPropertyScopeInput)
        out[n++] = InputStreamID(index);
    if (scope == kAudioObjectPropertyScopeGlobal || scope == kAudioObjectPropertyScopeOutput)
        out[n++] = OutputStreamID(index);
    return n;
}

OSStatus GetPropertyDataSize(AudioServerPlugInDriverRef inDriver, AudioObjectID inObjectID, pid_t pid,
                             const AudioObjectPropertyAddress* inAddress, UInt32 inQualifierDataSize,
                             const void* inQualifierData, UInt32* outDataSize) {
    if (!IsOurDriver(inDriver)) return kAudioHardwareBadObjectError;
    if (!inAddress || !outDataSize) return kAudioHardwareIllegalOperationError;
    if (!HasProperty(inDriver, inObjectID, pid, inAddress)) return kAudioHardwareUnknownPropertyError;

    UInt32 index = 0;
    ObjectKind kind = Classify(inObjectID, &index);
    AudioObjectPropertySelector s = inAddress->mSelector;

    switch (kind) {
        case ObjectKind::PlugIn:
            switch (s) {
                case kAudioObjectPropertyBaseClass:
                case kAudioObjectPropertyClass: *outDataSize = sizeof(AudioClassID); break;
                case kAudioObjectPropertyOwner: *outDataSize = sizeof(AudioObjectID); break;
                case kAudioObjectPropertyManufacturer:
                case kAudioPlugInPropertyResourceBundle: *outDataSize = sizeof(CFStringRef); break;
                case kAudioObjectPropertyOwnedObjects:
                case kAudioPlugInPropertyDeviceList: {
                    std::lock_guard<std::mutex> lock(gStateMutex);
                    AudioObjectID ids[kDeviceCount];
                    *outDataSize = EnabledDeviceIDsLocked(ids, kDeviceCount) * sizeof(AudioObjectID);
                    break;
                }
                case kAudioPlugInPropertyTranslateUIDToDevice: *outDataSize = sizeof(AudioObjectID); break;
                case kAudioObjectPropertyCustomPropertyInfoList:
                    *outDataSize = sizeof(AudioServerPlugInCustomPropertyInfo);
                    break;
                case kAudioFlowPropertyConfig: *outDataSize = sizeof(CFPropertyListRef); break;
                default: return kAudioHardwareUnknownPropertyError;
            }
            break;

        case ObjectKind::Device:
            switch (s) {
                case kAudioObjectPropertyBaseClass:
                case kAudioObjectPropertyClass: *outDataSize = sizeof(AudioClassID); break;
                case kAudioObjectPropertyOwner:
                case kAudioDevicePropertyRelatedDevices: *outDataSize = sizeof(AudioObjectID); break;
                case kAudioObjectPropertyName:
                case kAudioObjectPropertyManufacturer:
                case kAudioDevicePropertyDeviceUID:
                case kAudioDevicePropertyModelUID: *outDataSize = sizeof(CFStringRef); break;
                case kAudioObjectPropertyOwnedObjects: {
                    AudioObjectID ids[2];
                    *outDataSize = DeviceStreamIDs(index, kAudioObjectPropertyScopeGlobal, ids) * sizeof(AudioObjectID);
                    break;
                }
                case kAudioDevicePropertyStreams: {
                    AudioObjectID ids[2];
                    *outDataSize = DeviceStreamIDs(index, inAddress->mScope, ids) * sizeof(AudioObjectID);
                    break;
                }
                case kAudioObjectPropertyControlList: *outDataSize = 0; break;
                case kAudioDevicePropertyTransportType:
                case kAudioDevicePropertyClockDomain:
                case kAudioDevicePropertyDeviceIsAlive:
                case kAudioDevicePropertyDeviceIsRunning:
                case kAudioDevicePropertyDeviceCanBeDefaultDevice:
                case kAudioDevicePropertyDeviceCanBeDefaultSystemDevice:
                case kAudioDevicePropertyLatency:
                case kAudioDevicePropertySafetyOffset:
                case kAudioDevicePropertyIsHidden:
                case kAudioDevicePropertyZeroTimeStampPeriod:
                case kAudioDevicePropertyClockIsStable: *outDataSize = sizeof(UInt32); break;
                case kAudioDevicePropertyNominalSampleRate: *outDataSize = sizeof(Float64); break;
                case kAudioDevicePropertyAvailableNominalSampleRates:
                    *outDataSize = kSupportedSampleRateCount * sizeof(AudioValueRange);
                    break;
                case kAudioDevicePropertyPreferredChannelsForStereo: *outDataSize = 2 * sizeof(UInt32); break;
                default: return kAudioHardwareUnknownPropertyError;
            }
            break;

        case ObjectKind::InputStream:
        case ObjectKind::OutputStream:
            switch (s) {
                case kAudioObjectPropertyBaseClass:
                case kAudioObjectPropertyClass: *outDataSize = sizeof(AudioClassID); break;
                case kAudioObjectPropertyOwner: *outDataSize = sizeof(AudioObjectID); break;
                case kAudioObjectPropertyOwnedObjects: *outDataSize = 0; break;
                case kAudioStreamPropertyIsActive:
                case kAudioStreamPropertyDirection:
                case kAudioStreamPropertyTerminalType:
                case kAudioStreamPropertyStartingChannel:
                case kAudioStreamPropertyLatency: *outDataSize = sizeof(UInt32); break;
                case kAudioStreamPropertyVirtualFormat:
                case kAudioStreamPropertyPhysicalFormat: *outDataSize = sizeof(AudioStreamBasicDescription); break;
                case kAudioStreamPropertyAvailableVirtualFormats:
                case kAudioStreamPropertyAvailablePhysicalFormats:
                    *outDataSize = kSupportedSampleRateCount * sizeof(AudioStreamRangedDescription);
                    break;
                default: return kAudioHardwareUnknownPropertyError;
            }
            break;

        default: return kAudioHardwareBadObjectError;
    }
    (void)inQualifierDataSize;
    (void)inQualifierData;
    return kAudioHardwareNoError;
}

OSStatus GetPlugInProperty(const AudioObjectPropertyAddress* a, UInt32 qSize, const void* qData, UInt32 inDataSize,
                           UInt32* outDataSize, void* outData) {
    switch (a->mSelector) {
        case kAudioObjectPropertyBaseClass: return Put<AudioClassID>(kAudioObjectClassID, inDataSize, outDataSize, outData);
        case kAudioObjectPropertyClass: return Put<AudioClassID>(kAudioPlugInClassID, inDataSize, outDataSize, outData);
        case kAudioObjectPropertyOwner:
            return Put<AudioObjectID>(kAudioObjectUnknown, inDataSize, outDataSize, outData);
        case kAudioObjectPropertyManufacturer:
            return Put<CFStringRef>((CFStringRef)CFRetain(kManufacturer), inDataSize, outDataSize, outData);
        case kAudioPlugInPropertyResourceBundle:
            return Put<CFStringRef>((CFStringRef)CFRetain(CFSTR("")), inDataSize, outDataSize, outData);
        case kAudioObjectPropertyOwnedObjects:
        case kAudioPlugInPropertyDeviceList: {
            std::lock_guard<std::mutex> lock(gStateMutex);
            AudioObjectID ids[kDeviceCount];
            UInt32 n = EnabledDeviceIDsLocked(ids, kDeviceCount);
            return PutIDs(ids, n, inDataSize, outDataSize, outData);
        }
        case kAudioPlugInPropertyTranslateUIDToDevice: {
            if (qSize != sizeof(CFStringRef) || !qData) return kAudioHardwareBadPropertySizeError;
            CFStringRef uid = *(const CFStringRef*)qData;
            AudioObjectID result = kAudioObjectUnknown;
            std::lock_guard<std::mutex> lock(gStateMutex);
            for (UInt32 i = 0; i < kDeviceCount; ++i) {
                if (!gDevices[i].enabled) continue;
                CFStringRef mine = CopyDeviceUID(i);
                bool match = uid && CFStringCompare(uid, mine, 0) == kCFCompareEqualTo;
                CFRelease(mine);
                if (match) { result = DeviceObjectID(i); break; }
            }
            return Put<AudioObjectID>(result, inDataSize, outDataSize, outData);
        }
        case kAudioObjectPropertyCustomPropertyInfoList: {
            AudioServerPlugInCustomPropertyInfo info = {};
            info.mSelector = kAudioFlowPropertyConfig;
            info.mPropertyDataType = kAudioServerPlugInCustomPropertyDataTypeCFPropertyList;
            info.mQualifierDataType = kAudioServerPlugInCustomPropertyDataTypeNone;
            return Put(info, inDataSize, outDataSize, outData);
        }
        case kAudioFlowPropertyConfig: {
            std::lock_guard<std::mutex> lock(gStateMutex);
            CFPropertyListRef dict = CopyConfigLocked();  // caller releases
            return Put<CFPropertyListRef>(dict, inDataSize, outDataSize, outData);
        }
    }
    return kAudioHardwareUnknownPropertyError;
}

OSStatus GetDeviceProperty(UInt32 index, const AudioObjectPropertyAddress* a, UInt32 inDataSize, UInt32* outDataSize,
                           void* outData) {
    Device& d = gDevices[index];
    switch (a->mSelector) {
        case kAudioObjectPropertyBaseClass: return Put<AudioClassID>(kAudioObjectClassID, inDataSize, outDataSize, outData);
        case kAudioObjectPropertyClass: return Put<AudioClassID>(kAudioDeviceClassID, inDataSize, outDataSize, outData);
        case kAudioObjectPropertyOwner: return Put<AudioObjectID>(kObjectID_PlugIn, inDataSize, outDataSize, outData);
        case kAudioObjectPropertyName: {
            std::lock_guard<std::mutex> lock(gStateMutex);
            CFStringRef s = CFStringCreateWithCString(nullptr, d.name.c_str(), kCFStringEncodingUTF8);
            OSStatus st = Put<CFStringRef>(s, inDataSize, outDataSize, outData);
            if (st != kAudioHardwareNoError) CFRelease(s);
            return st;
        }
        case kAudioObjectPropertyManufacturer:
            return Put<CFStringRef>((CFStringRef)CFRetain(kManufacturer), inDataSize, outDataSize, outData);
        case kAudioDevicePropertyDeviceUID: {
            CFStringRef s = CopyDeviceUID(index);
            OSStatus st = Put<CFStringRef>(s, inDataSize, outDataSize, outData);
            if (st != kAudioHardwareNoError) CFRelease(s);
            return st;
        }
        case kAudioDevicePropertyModelUID:
            return Put<CFStringRef>((CFStringRef)CFRetain(kModelUID), inDataSize, outDataSize, outData);
        case kAudioDevicePropertyTransportType:
            return Put<UInt32>(kAudioDeviceTransportTypeVirtual, inDataSize, outDataSize, outData);
        case kAudioDevicePropertyRelatedDevices:
            return Put<AudioObjectID>(DeviceObjectID(index), inDataSize, outDataSize, outData);
        case kAudioDevicePropertyClockDomain: return Put<UInt32>(0, inDataSize, outDataSize, outData);
        case kAudioDevicePropertyDeviceIsAlive: return Put<UInt32>(1, inDataSize, outDataSize, outData);
        case kAudioDevicePropertyDeviceIsRunning:
            return Put<UInt32>(d.ioClients.load() > 0 ? 1 : 0, inDataSize, outDataSize, outData);
        case kAudioDevicePropertyDeviceCanBeDefaultDevice: return Put<UInt32>(1, inDataSize, outDataSize, outData);
        case kAudioDevicePropertyDeviceCanBeDefaultSystemDevice:
            return Put<UInt32>(1, inDataSize, outDataSize, outData);
        case kAudioDevicePropertyLatency: return Put<UInt32>(0, inDataSize, outDataSize, outData);
        case kAudioDevicePropertySafetyOffset: return Put<UInt32>(0, inDataSize, outDataSize, outData);
        case kAudioDevicePropertyIsHidden: return Put<UInt32>(0, inDataSize, outDataSize, outData);
        case kAudioDevicePropertyClockIsStable: return Put<UInt32>(1, inDataSize, outDataSize, outData);
        case kAudioDevicePropertyZeroTimeStampPeriod: return Put<UInt32>(kRingFrames, inDataSize, outDataSize, outData);
        case kAudioObjectPropertyOwnedObjects:
        case kAudioDevicePropertyStreams: {
            AudioObjectID ids[2];
            AudioObjectPropertyScope scope =
                a->mSelector == kAudioObjectPropertyOwnedObjects ? kAudioObjectPropertyScopeGlobal : a->mScope;
            UInt32 n = DeviceStreamIDs(index, scope, ids);
            return PutIDs(ids, n, inDataSize, outDataSize, outData);
        }
        case kAudioObjectPropertyControlList: *outDataSize = 0; return kAudioHardwareNoError;
        case kAudioDevicePropertyNominalSampleRate: return Put<Float64>(d.sampleRate.load(), inDataSize, outDataSize, outData);
        case kAudioDevicePropertyAvailableNominalSampleRates: {
            UInt32 n = std::min<UInt32>(kSupportedSampleRateCount, inDataSize / sizeof(AudioValueRange));
            auto* ranges = (AudioValueRange*)outData;
            for (UInt32 i = 0; i < n; ++i) ranges[i] = {kSupportedSampleRates[i], kSupportedSampleRates[i]};
            *outDataSize = n * sizeof(AudioValueRange);
            return kAudioHardwareNoError;
        }
        case kAudioDevicePropertyPreferredChannelsForStereo: {
            if (inDataSize < 2 * sizeof(UInt32)) return kAudioHardwareBadPropertySizeError;
            auto* ch = (UInt32*)outData;
            ch[0] = 1;
            ch[1] = 2;
            *outDataSize = 2 * sizeof(UInt32);
            return kAudioHardwareNoError;
        }
    }
    return kAudioHardwareUnknownPropertyError;
}

OSStatus GetStreamProperty(UInt32 index, bool isInput, const AudioObjectPropertyAddress* a, UInt32 inDataSize,
                           UInt32* outDataSize, void* outData) {
    Device& d = gDevices[index];
    switch (a->mSelector) {
        case kAudioObjectPropertyBaseClass: return Put<AudioClassID>(kAudioObjectClassID, inDataSize, outDataSize, outData);
        case kAudioObjectPropertyClass: return Put<AudioClassID>(kAudioStreamClassID, inDataSize, outDataSize, outData);
        case kAudioObjectPropertyOwner:
            return Put<AudioObjectID>(DeviceObjectID(index), inDataSize, outDataSize, outData);
        case kAudioObjectPropertyOwnedObjects: *outDataSize = 0; return kAudioHardwareNoError;
        case kAudioStreamPropertyIsActive: return Put<UInt32>(1, inDataSize, outDataSize, outData);
        case kAudioStreamPropertyDirection: return Put<UInt32>(isInput ? 1 : 0, inDataSize, outDataSize, outData);
        case kAudioStreamPropertyTerminalType:
            return Put<UInt32>(isInput ? kAudioStreamTerminalTypeMicrophone : kAudioStreamTerminalTypeSpeaker,
                               inDataSize, outDataSize, outData);
        case kAudioStreamPropertyStartingChannel: return Put<UInt32>(1, inDataSize, outDataSize, outData);
        case kAudioStreamPropertyLatency: return Put<UInt32>(0, inDataSize, outDataSize, outData);
        case kAudioStreamPropertyVirtualFormat:
        case kAudioStreamPropertyPhysicalFormat:
            return Put(MakeFormat(d.sampleRate.load()), inDataSize, outDataSize, outData);
        case kAudioStreamPropertyAvailableVirtualFormats:
        case kAudioStreamPropertyAvailablePhysicalFormats: {
            UInt32 n = std::min<UInt32>(kSupportedSampleRateCount, inDataSize / sizeof(AudioStreamRangedDescription));
            auto* out = (AudioStreamRangedDescription*)outData;
            for (UInt32 i = 0; i < n; ++i) {
                out[i].mFormat = MakeFormat(kSupportedSampleRates[i]);
                out[i].mSampleRateRange = {kSupportedSampleRates[i], kSupportedSampleRates[i]};
            }
            *outDataSize = n * sizeof(AudioStreamRangedDescription);
            return kAudioHardwareNoError;
        }
    }
    return kAudioHardwareUnknownPropertyError;
}

OSStatus GetPropertyData(AudioServerPlugInDriverRef inDriver, AudioObjectID inObjectID, pid_t pid,
                         const AudioObjectPropertyAddress* inAddress, UInt32 inQualifierDataSize,
                         const void* inQualifierData, UInt32 inDataSize, UInt32* outDataSize, void* outData) {
    if (!IsOurDriver(inDriver)) return kAudioHardwareBadObjectError;
    if (!inAddress || !outDataSize || (!outData && inDataSize > 0)) return kAudioHardwareIllegalOperationError;
    if (!HasProperty(inDriver, inObjectID, pid, inAddress)) return kAudioHardwareUnknownPropertyError;

    UInt32 index = 0;
    switch (Classify(inObjectID, &index)) {
        case ObjectKind::PlugIn:
            return GetPlugInProperty(inAddress, inQualifierDataSize, inQualifierData, inDataSize, outDataSize, outData);
        case ObjectKind::Device: return GetDeviceProperty(index, inAddress, inDataSize, outDataSize, outData);
        case ObjectKind::InputStream: return GetStreamProperty(index, true, inAddress, inDataSize, outDataSize, outData);
        case ObjectKind::OutputStream:
            return GetStreamProperty(index, false, inAddress, inDataSize, outDataSize, outData);
        default: return kAudioHardwareBadObjectError;
    }
}

OSStatus SetPropertyData(AudioServerPlugInDriverRef inDriver, AudioObjectID inObjectID, pid_t pid,
                         const AudioObjectPropertyAddress* inAddress, UInt32, const void*, UInt32 inDataSize,
                         const void* inData) {
    if (!IsOurDriver(inDriver)) return kAudioHardwareBadObjectError;
    if (!inAddress || !inData) return kAudioHardwareIllegalOperationError;
    if (!HasProperty(inDriver, inObjectID, pid, inAddress)) return kAudioHardwareUnknownPropertyError;

    UInt32 index = 0;
    ObjectKind kind = Classify(inObjectID, &index);

    if (kind == ObjectKind::PlugIn && inAddress->mSelector == kAudioFlowPropertyConfig) {
        if (inDataSize != sizeof(CFPropertyListRef)) return kAudioHardwareBadPropertySizeError;
        CFPropertyListRef plist = *(const CFPropertyListRef*)inData;
        if (!plist || CFGetTypeID(plist) != CFDictionaryGetTypeID()) return kAudioHardwareIllegalOperationError;

        bool changedName[kDeviceCount];
        bool enabledChanged = false;
        {
            std::lock_guard<std::mutex> lock(gStateMutex);
            ApplyConfigLocked((CFDictionaryRef)plist, changedName, &enabledChanged);
            PersistConfigLocked();
        }
        for (UInt32 i = 0; i < kDeviceCount; ++i) {
            if (changedName[i])
                NotifyAsync(DeviceObjectID(i),
                            {{kAudioObjectPropertyName, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain}});
        }
        if (enabledChanged) {
            NotifyAsync(kObjectID_PlugIn,
                        {{kAudioPlugInPropertyDeviceList, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain},
                         {kAudioObjectPropertyOwnedObjects, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain}});
        }
        return kAudioHardwareNoError;
    }

    if (kind == ObjectKind::Device && inAddress->mSelector == kAudioDevicePropertyNominalSampleRate) {
        if (inDataSize != sizeof(Float64)) return kAudioHardwareBadPropertySizeError;
        Float64 rate = *(const Float64*)inData;
        if (!IsSupportedRate(rate)) return kAudioHardwareIllegalOperationError;
        if (rate != gDevices[index].sampleRate.load()) RequestRateChange(index, rate);
        return kAudioHardwareNoError;
    }

    if ((kind == ObjectKind::InputStream || kind == ObjectKind::OutputStream) &&
        (inAddress->mSelector == kAudioStreamPropertyVirtualFormat ||
         inAddress->mSelector == kAudioStreamPropertyPhysicalFormat)) {
        if (inDataSize != sizeof(AudioStreamBasicDescription)) return kAudioHardwareBadPropertySizeError;
        auto* f = (const AudioStreamBasicDescription*)inData;
        AudioStreamBasicDescription ours = MakeFormat(f->mSampleRate);
        if (f->mFormatID != ours.mFormatID || f->mChannelsPerFrame != ours.mChannelsPerFrame ||
            f->mBitsPerChannel != ours.mBitsPerChannel || f->mBytesPerFrame != ours.mBytesPerFrame ||
            (f->mFormatFlags & kAudioFormatFlagIsFloat) == 0)
            return kAudioDeviceUnsupportedFormatError;
        if (!IsSupportedRate(f->mSampleRate)) return kAudioDeviceUnsupportedFormatError;
        if (f->mSampleRate != gDevices[index].sampleRate.load()) RequestRateChange(index, f->mSampleRate);
        return kAudioHardwareNoError;
    }

    return kAudioHardwareUnsupportedOperationError;
}

// MARK: - IO

OSStatus StartIO(AudioServerPlugInDriverRef inDriver, AudioObjectID inDevice, UInt32) {
    if (!IsOurDriver(inDriver)) return kAudioHardwareBadObjectError;
    UInt32 index;
    if (Classify(inDevice, &index) != ObjectKind::Device) return kAudioHardwareBadDeviceError;
    Device& d = gDevices[index];
    if (d.ioClients.fetch_add(1) == 0) {
        std::fill(d.ring.begin(), d.ring.end(), 0.0f);
        d.numberTimeStamps.store(0);
        d.anchorHostTime.store(mach_absolute_time());
        NotifyAsync(inDevice,
                    {{kAudioDevicePropertyDeviceIsRunning, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain}});
    }
    return kAudioHardwareNoError;
}

OSStatus StopIO(AudioServerPlugInDriverRef inDriver, AudioObjectID inDevice, UInt32) {
    if (!IsOurDriver(inDriver)) return kAudioHardwareBadObjectError;
    UInt32 index;
    if (Classify(inDevice, &index) != ObjectKind::Device) return kAudioHardwareBadDeviceError;
    Device& d = gDevices[index];
    UInt32 prev = d.ioClients.load();
    if (prev == 0) return kAudioHardwareIllegalOperationError;
    if (d.ioClients.fetch_sub(1) == 1) {
        NotifyAsync(inDevice,
                    {{kAudioDevicePropertyDeviceIsRunning, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain}});
    }
    return kAudioHardwareNoError;
}

OSStatus GetZeroTimeStamp(AudioServerPlugInDriverRef inDriver, AudioObjectID inDevice, UInt32, Float64* outSampleTime,
                          UInt64* outHostTime, UInt64* outSeed) {
    if (!IsOurDriver(inDriver)) return kAudioHardwareBadObjectError;
    UInt32 index;
    if (Classify(inDevice, &index) != ObjectKind::Device) return kAudioHardwareBadDeviceError;
    Device& d = gDevices[index];

    UInt64 now = mach_absolute_time();
    Float64 ticksPerRing = d.hostTicksPerFrame.load() * (Float64)kRingFrames;
    UInt64 count = d.numberTimeStamps.load();
    UInt64 anchor = d.anchorHostTime.load();
    UInt64 next = anchor + (UInt64)((Float64)(count + 1) * ticksPerRing);
    if (next <= now) {
        ++count;
        d.numberTimeStamps.store(count);
    }
    *outSampleTime = (Float64)(count * kRingFrames);
    *outHostTime = anchor + (UInt64)((Float64)count * ticksPerRing);
    *outSeed = 1;
    return kAudioHardwareNoError;
}

OSStatus WillDoIOOperation(AudioServerPlugInDriverRef inDriver, AudioObjectID, UInt32, UInt32 inOperationID,
                           Boolean* outWillDo, Boolean* outWillDoInPlace) {
    if (!IsOurDriver(inDriver)) return kAudioHardwareBadObjectError;
    bool will = inOperationID == kAudioServerPlugInIOOperationReadInput ||
                inOperationID == kAudioServerPlugInIOOperationWriteMix;
    if (outWillDo) *outWillDo = will;
    if (outWillDoInPlace) *outWillDoInPlace = true;
    return kAudioHardwareNoError;
}

OSStatus BeginIOOperation(AudioServerPlugInDriverRef inDriver, AudioObjectID, UInt32, UInt32, UInt32,
                          const AudioServerPlugInIOCycleInfo*) {
    return IsOurDriver(inDriver) ? kAudioHardwareNoError : kAudioHardwareBadObjectError;
}

OSStatus DoIOOperation(AudioServerPlugInDriverRef inDriver, AudioObjectID inDevice, AudioObjectID inStream, UInt32,
                       UInt32 inOperationID, UInt32 inFrames, const AudioServerPlugInIOCycleInfo* inCycle,
                       void* ioMainBuffer, void*) {
    if (!IsOurDriver(inDriver)) return kAudioHardwareBadObjectError;
    UInt32 index;
    if (Classify(inDevice, &index) != ObjectKind::Device || !inCycle || !ioMainBuffer) return kAudioHardwareNoError;
    Device& d = gDevices[index];
    Float32* ring = d.ring.data();
    auto* buf = (Float32*)ioMainBuffer;

    if (inOperationID == kAudioServerPlugInIOOperationWriteMix && inStream == OutputStreamID(index)) {
        UInt64 start = (UInt64)inCycle->mOutputTime.mSampleTime % kRingFrames;
        UInt32 first = std::min<UInt32>(inFrames, kRingFrames - (UInt32)start);
        std::memcpy(ring + start * kChannelCount, buf, first * kBytesPerFrame);
        if (first < inFrames) std::memcpy(ring, buf + first * kChannelCount, (inFrames - first) * kBytesPerFrame);
    } else if (inOperationID == kAudioServerPlugInIOOperationReadInput && inStream == InputStreamID(index)) {
        UInt64 start = (UInt64)inCycle->mInputTime.mSampleTime % kRingFrames;
        UInt32 first = std::min<UInt32>(inFrames, kRingFrames - (UInt32)start);
        std::memcpy(buf, ring + start * kChannelCount, first * kBytesPerFrame);
        std::memset(ring + start * kChannelCount, 0, first * kBytesPerFrame);
        if (first < inFrames) {
            std::memcpy(buf + first * kChannelCount, ring, (inFrames - first) * kBytesPerFrame);
            std::memset(ring, 0, (inFrames - first) * kBytesPerFrame);
        }
    }
    return kAudioHardwareNoError;
}

OSStatus EndIOOperation(AudioServerPlugInDriverRef inDriver, AudioObjectID, UInt32, UInt32, UInt32,
                        const AudioServerPlugInIOCycleInfo*) {
    return IsOurDriver(inDriver) ? kAudioHardwareNoError : kAudioHardwareBadObjectError;
}

}  // namespace

#import "audio_device.h"
#import "coreaudio_helpers.h"

#include "src/logging.h"

#import <AVFoundation/AVFoundation.h>
#import <CoreAudio/CoreAudio.h>

@implementation AudioDevice

- (instancetype)initFromAudioDeviceID:(AudioDeviceID)device_id {
  OSStatus status;
  uint32_t transportType = 0;
  CFStringRef uid_string = NULL;
  CFStringRef name = NULL;
  uint32_t outputStreamCount = 0;
  uint32_t size = sizeof(uint32_t);
  AudioObjectPropertyAddress addr = {kAudioHardwarePropertyDefaultOutputDevice, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain};

  if (device_id == kAudioObjectUnknown) {
    // use default output device
    size = sizeof(device_id);
    addr.mSelector = kAudioHardwarePropertyDefaultOutputDevice;
    status = AudioObjectGetPropertyData(kAudioObjectSystemObject, &addr, outputElement, nil, &size, &device_id);
    if (status != noErr) {
      BOOST_LOG(error) << "Error getting kAudioHardwarePropertyDefaultOutputDevice: " << CA_Status(status);
      goto out;
    }
  }

  // Get the transport type of the device to check whether it's an aggregate.
  size = sizeof(uint32_t);
  addr.mSelector = kAudioDevicePropertyTransportType;
  status = AudioObjectGetPropertyData(device_id, &addr, outputElement, nil, &size, &transportType);
  if (status != noErr) {
    BOOST_LOG(error) << "Error getting device kAudioDevicePropertyTransportType: " << CA_Status(status);
    goto out;
  }

  // Get the device's UID
  size = sizeof(uid_string);
  addr.mSelector = kAudioDevicePropertyDeviceUID;
  addr.mScope    = kAudioObjectPropertyScopeGlobal;
  status = AudioObjectGetPropertyData(device_id, &addr, outputElement, nil, &size, &uid_string);
  if (status != noErr) {
    BOOST_LOG(error) << "Error getting device kAudioDevicePropertyDeviceUID: " << CA_Status(status);
    goto out;
  }

  // Get the device's name
  size = sizeof(name);
  addr.mSelector = kAudioObjectPropertyName;
  addr.mScope    = kAudioObjectPropertyScopeGlobal;
  status = AudioObjectGetPropertyData(device_id, &addr, outputElement, nil, &size, &name);
  if (status != noErr) {
    BOOST_LOG(error) << "Error getting device kAudioObjectPropertyName: " << CA_Status(status);
    goto out;
  }

  // Check that device has at least one output stream
  addr.mSelector = kAudioDevicePropertyStreams;
  addr.mScope    = kAudioObjectPropertyScopeOutput;
  status = AudioObjectGetPropertyDataSize(device_id, &addr, outputElement, nil, &outputStreamCount);
  if (status != noErr) {
    BOOST_LOG(error) << "Error getting device kAudioDevicePropertyStreams: " << CA_Status(status);
    goto out;
  }

  self = [super init];
  if (self) {
    _id          = device_id;
    _uid         = [NSString stringWithString:(NSString*)uid_string];
    _name        = [NSString stringWithString:(NSString*)name];
    _isAggregate = transportType == kAudioDeviceTransportTypeAggregate;
    _isOutput    = outputStreamCount != 0;
  }

out:
  if (!self) self = nullptr;
  if (uid_string) CFRelease(uid_string);
  if (name) CFRelease(name);

  return self;
}

+ (NSArray<AudioDevice *> *)getAllDevices {
  NSMutableArray *result = [[NSMutableArray alloc] init];
  AudioDeviceID *list = NULL;
  uint32_t listSize = 0;
  OSStatus status;

  {
    AudioObjectPropertyAddress addr = {kAudioHardwarePropertyDevices, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain};
    status = AudioObjectGetPropertyDataSize(kAudioObjectSystemObject, &addr, outputElement, nil, &listSize);
    if (status != noErr) {
      BOOST_LOG(error) << "Error getting list of audio devices: " << CA_Status(status);
      goto out;
    }

    list = (AudioDeviceID *)malloc(listSize);
    status = AudioObjectGetPropertyData(kAudioObjectSystemObject, &addr, outputElement, nil, &listSize, list);
    if (status != noErr) {
      BOOST_LOG(error) << "Error getting list of audio devices: " << CA_Status(status);
      goto out;
    }
  }

  {
    uint32_t deviceCount = listSize / sizeof(AudioDeviceID);
    for (uint32_t n = 0; n < deviceCount; n++) {
      AudioDevice *device = [[AudioDevice alloc] initFromAudioDeviceID:(AudioDeviceID)list[n]];
      if (device != nil) {
        [result addObject:device];
      }
    }
  }

out:
  if (list != nil) free(list);

  return result;
}

+ (AudioDevice *)defaultOutputDevice {
  return [[AudioDevice alloc] initFromAudioDeviceID:kAudioObjectUnknown];
}

@end

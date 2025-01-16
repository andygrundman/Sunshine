#import "audio_device.h"
#import "coreaudio_helpers.h"

#include "src/logging.h"

#import <AVFoundation/AVFoundation.h>
#import <CoreAudio/CoreAudio.h>

@implementation AudioDevice

- (instancetype)initFromAudioDeviceID:(AudioDeviceID)device_id {
  OSStatus status;
  CFStringRef name = NULL;
  CFStringRef uid_string = NULL;
  uint32_t size = sizeof(uint32_t);

  if (device_id == kAudioObjectUnknown) {
    // use default output device
    size = sizeof(device_id);

    AudioObjectPropertyAddress addr = {kAudioHardwarePropertyDefaultOutputDevice, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain};
    OSStatus status = AudioObjectGetPropertyData(kAudioObjectSystemObject, &addr, outputElement, nil, &size, &device_id);
    if (status != noErr) {
      BOOST_LOG(error) << "Error getting default output device: " << CA_Status(status);
      return nil;
    }
  }

  // Get the transport type of the device to check whether it's an aggregate.
  AudioObjectPropertyAddress addr = {kAudioDevicePropertyTransportType, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain};
  uint32_t transportType = 0;
  status = AudioObjectGetPropertyData(device_id, &addr, outputElement, nil, &size, &transportType);
  if (status != noErr) {
    BOOST_LOG(error) << "Error getting transport type for device: " << CA_Status(status);
    return nil;
  }

  // Get the device's UID and name
  size = sizeof(uid_string);
  addr.mSelector = kAudioDevicePropertyDeviceUID;
  status = AudioObjectGetPropertyData(device_id, &addr, outputElement, nil, &size, &uid_string);
  if (status != noErr) {
    BOOST_LOG(error) << "Error getting device UID: " << CA_Status(status);
    goto out;
  }

  size = sizeof(name);
  addr.mSelector = kAudioObjectPropertyName;
  status = AudioObjectGetPropertyData(device_id, &addr, outputElement, nil, &size, &name);
  if (status != noErr) {
    BOOST_LOG(error) << "Error getting device name: " << CA_Status(status);
    goto out;
  }

  // XXX kAudioDevicePropertyStreams ?

  self = [super init];
   if (self) {
     _id          = device_id;
     _uid         = [NSString stringWithString:(NSString*)uid_string];
     _name        = [NSString stringWithString:(NSString*)name];
     _isAggregate = transportType == kAudioDeviceTransportTypeAggregate;
   }

out:
  if (!self) self = nil;
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

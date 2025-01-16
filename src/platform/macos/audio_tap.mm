/**
 * @file src/platform/macos/audio_tap.m
 * @brief macOS 14.2+ system audio capture
 */

#import "audio_device.h"
#import "audio_tap.h"
#import "coreaudio_helpers.h"

#include "src/logging.h"

#include <CoreAudio/AudioHardware.h>
#include <CoreAudio/AudioHardwareTapping.h>
#include <CoreAudio/CATapDescription.h>

using namespace std::literals;

static void logTapData(AudioObjectID);

static OSStatus ioproc(AudioObjectID,
                       const AudioTimeStamp *,
                       const AudioBufferList *inInputData,
                       const AudioTimeStamp *,
                       AudioBufferList *outOutputData,
                       const AudioTimeStamp *,
                       void* inClientData) noexcept;

@implementation AudioTap

// TODO
+ (NSArray<AVCaptureDevice *> *)microphones {
  return nil;
}

+ (NSArray<AudioDevice *> *)getOutputDevices {
  NSMutableArray *result = [[NSMutableArray alloc] init];

  NSArray<AudioDevice *> *deviceList = [AudioDevice getAllDevices];
  for (AudioDevice *device in deviceList) {
    if ([device isOutput]) {
      [result addObject:device];
    }
  }

  return result;
}

- (instancetype)initWithDeviceUID:(NSString *)audioSinkUID sampleRate:(UInt32)sampleRate frameSize:(UInt32)frameSize channels:(UInt8)channels {
  self = [super init];
  if (!self) {
    return nullptr;
  }

  BOOST_LOG(info) << "Attempting to create audio tap on output device: "sv << [audioSinkUID UTF8String];

  AudioObjectID tap_id = 0;
  AudioObjectID aggregate_device_id = 0;
  AudioDeviceIOProcID tap_io_proc_id = 0;

  // Create a new tap on all processes
  NSArray<NSNumber *> *toExclude = @[];
  CATapDescription *tapDesc = [[CATapDescription alloc] initExcludingProcesses:toExclude
                                                                  andDeviceUID:audioSinkUID
                                                                    withStream:0];
  if (tapDesc == nil) {
    return nullptr;
  }

  [tapDesc setName:@"Sunshine Tap"];
  //[tapDesc setExclusive:YES];             // flip the list of processes we tap to mean all processes
  [tapDesc setMixdown:YES];               // the tap will be a stereo mixdown XXX future support for 5.1/7.1
  [tapDesc setMuteBehavior:CATapUnmuted]; // XXX CATapMuted if Moonlight requests no audio on host
  [tapDesc setPrivate:YES];

  OSStatus status = AudioHardwareCreateProcessTap(tapDesc, &tap_id);
  if (status != noErr) {
    BOOST_LOG(error) << "Error calling AudioHardwareCreateProcessTap: "sv << CA_Status(status);
    goto err_cleanup;
  }

  logTapData(tap_id);

  // Create a new aggregate device and add the tap
  {
    NSArray<NSDictionary *> *tapList = @[
      @{
        @kAudioSubTapUIDKey : (NSString *)[[tapDesc UUID] UUIDString],
        @kAudioSubTapDriftCompensationKey : @YES,
      },
    ];

    // XXX the example app at https://github.com/insidegui/AudioCap adds the audio device to the aggregate
    // but Apple's example doesn't...
    NSArray<NSDictionary *> *subDeviceList = @[
      @{
        @kAudioSubDeviceUIDKey : audioSinkUID,
      },
    ];

    NSDictionary *aggregate_device_properties = @{
      @kAudioAggregateDeviceNameKey : @"SunshineAggregateDevice",
      @kAudioAggregateDeviceUIDKey : @"dev.lizardbyte.sunshine.AggregateDevice",
      @kAudioAggregateDeviceMainSubDeviceKey : audioSinkUID,
      @kAudioAggregateDeviceIsPrivateKey : @YES,
      @kAudioAggregateDeviceIsStackedKey : @NO,
      @kAudioAggregateDeviceTapAutoStartKey : @NO,
      //@kAudioAggregateDeviceSubDeviceListKey : subDeviceList,
      @kAudioAggregateDeviceTapListKey : tapList,
    };

    status = AudioHardwareCreateAggregateDevice((CFDictionaryRef)aggregate_device_properties, &aggregate_device_id);
    if (status != noErr) {
      BOOST_LOG(error) << "Error calling AudioHardwareCreateAggregateDevice: "sv << CA_Status(status);
      goto err_cleanup;
    }
  }

  // Attach callback to the aggregate device and start it
  {
    status = AudioDeviceCreateIOProcID(aggregate_device_id, ioproc, (__bridge void *)self, &tap_io_proc_id);
    if (status != noErr) {
      BOOST_LOG(error) << "Error calling AudioHardwareCreateAggregateDevice: "sv << CA_Status(status);
      goto err_cleanup;
    }

    status = AudioDeviceStart(aggregate_device_id, tap_io_proc_id);
    if (status != noErr) {
      BOOST_LOG(error) << "Error calling AudioDeviceStart for aggregate device: "sv << CA_Status(status);
      goto err_cleanup;
    }
  }

  self.samplesArrivedSignal = [[NSCondition alloc] init];
  TPCircularBufferInit(&self->audioSampleBuffer, kBufferLength * channels);

  _tapUID            = audioSinkUID;
  _tapID             = tap_id;
  _aggregateDeviceID = aggregate_device_id;
  _tapIOProcID       = tap_io_proc_id;

  BOOST_LOG(info) << "Created audio tap on output device: "sv << [self.tapUID UTF8String];

  return self;

  // unwind everything in case something failed
err_cleanup:
  if (tap_io_proc_id != 0) {
    AudioDeviceStop(aggregate_device_id, tap_io_proc_id);
    AudioDeviceDestroyIOProcID(aggregate_device_id, tap_io_proc_id);
  }

  if (aggregate_device_id != 0) {
    AudioHardwareDestroyAggregateDevice(aggregate_device_id);
  }

  if (tap_id != 0) {
    AudioHardwareDestroyProcessTap(tap_id);
  }

  return nullptr;
}

- (void)dealloc {
  if (self.tapIOProcID != 0) {
    AudioDeviceStop(self.aggregateDeviceID, self.tapIOProcID);
    AudioDeviceDestroyIOProcID(self.aggregateDeviceID, self.tapIOProcID);
  }

  if (self.aggregateDeviceID != 0) {
    AudioHardwareDestroyAggregateDevice(self.aggregateDeviceID);
  }

  if (self.tapID != 0) {
    AudioHardwareDestroyProcessTap(self.tapID);
    BOOST_LOG(info) << "Removed audio tap on output device: "sv << [self.tapUID UTF8String];
  }

  // make sure nothing gets stuck on this signal
  [self.samplesArrivedSignal signal];
  [self.samplesArrivedSignal release];
  TPCircularBufferCleanup(&audioSampleBuffer);

  [super dealloc];
}

@end

static void logTapData(AudioObjectID id) {
  OSStatus status;
  CFStringRef uidStr = nil;
  CFStringRef descriptionStr = nil;
  AudioObjectPropertyAddress addr = {kAudioTapPropertyUID, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain};

  {
    UInt32 size = sizeof(uidStr);
    addr.mSelector = kAudioTapPropertyUID;
    status = AudioObjectGetPropertyData(id, &addr, outputElement, nil, &size, &uidStr);
    if (status != noErr) {
      BOOST_LOG(error) << "Error getting kAudioTapPropertyUID: " << CA_Status(status);
      goto out;
    }
    BOOST_LOG(info) << "kAudioTapPropertyUID: "sv << [NSString stringWithFormat:@"%@", uidStr].UTF8String;
  }

  {
    UInt32 size = sizeof(descriptionStr);
    addr.mSelector = kAudioTapPropertyDescription;
    status = AudioObjectGetPropertyData(id, &addr, outputElement, nil, &size, &descriptionStr);
    if (status != noErr) {
      BOOST_LOG(error) << "Error getting kAudioTapPropertyDescription: " << CA_Status(status);
      goto out;
    }
    BOOST_LOG(info) << "kAudioTapPropertyDescription: "sv << [NSString stringWithFormat:@"%@", descriptionStr].UTF8String;
  }

  {
    AudioStreamBasicDescription asbd;
    UInt32 size = sizeof(AudioStreamBasicDescription);
    addr.mSelector = kAudioTapPropertyFormat;
    status = AudioObjectGetPropertyData(id, &addr, outputElement, nil, &size, &asbd);
    if (status != noErr) {
      BOOST_LOG(error) << "Error getting kAudioTapPropertyFormat: " << CA_Status(status);
      goto out;
    }
    BOOST_LOG(info) << "kAudioTapPropertyFormat:"sv << CA_PrintASBD(&asbd);
  }

out:
  if (uidStr) CFRelease(uidStr);
  if (descriptionStr) CFRelease(descriptionStr);

  return;
}

static OSStatus ioproc(AudioObjectID inDevice,
                       const AudioTimeStamp *inNow,
                       const AudioBufferList *inInputData,
                       const AudioTimeStamp *inInputTime,
                       AudioBufferList *outOutputData,
                       const AudioTimeStamp *inOutputTime,
                       void* inClientData) noexcept {
  auto *me = (__bridge AudioTap *)inClientData;

  // XXX this ioproc doesn't work right, so there's some extra debugging code for now
  static int once = 0;
  if (++once < 5) {
    fprintf(stderr, "inInputData->mNumberBuffers: %u, outOutputData->mNumberBuffers: %u\n",
      inInputData->mNumberBuffers, outOutputData->mNumberBuffers);
    fprintf(stderr, "inDevice: %u, inNow: %llu, inInputTime: %llu, inOutputTime: %llu\n",
          (unsigned int)inDevice, inNow->mHostTime, inInputTime->mHostTime, inOutputTime->mHostTime);
    for (int i = 0; i < inInputData->mNumberBuffers; i++) {
      fprintf(stderr, "input %d:\n", i);
      CA_HexDump((float *)inInputData->mBuffers[i].mData, 32);
    }
  }

  if (inInputData != nullptr && inInputData->mNumberBuffers > 0) {
    //assert(inInputData->mNumberBuffers == 1);

    AudioBuffer buffer = inInputData->mBuffers[0];
    TPCircularBufferProduceBytes(&me->audioSampleBuffer, buffer.mData, buffer.mDataByteSize);
    [me.samplesArrivedSignal signal];
  }

  return kAudioHardwareNoError;
}

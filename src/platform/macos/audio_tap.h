/**
 * @file src/platform/macos/audio_tap.h
 * @brief macOS 14.2+ system audio capture
 */
#pragma once

#import <AVFoundation/AVFoundation.h>

#include "third-party/TPCircularBuffer/TPCircularBuffer.h"

#define kBufferLength 4096

@interface AudioTap: NSObject {
@public
  TPCircularBuffer audioSampleBuffer;
}

@property (nonatomic, assign) AudioObjectID tapID;
@property (nonatomic, assign) AudioObjectID aggregateDeviceID;
@property (nonatomic, assign) AudioDeviceIOProcID tapIOProcID;

// from old av_audio code
@property (nonatomic, assign) NSCondition *samplesArrivedSignal;

+ (NSArray *)getOutputDevices;
+ (NSString *)findAudioSinkUID:(NSString *)name;

- (instancetype)initWithDeviceUID:(NSString *)audioSinkUID sampleRate:(UInt32)sampleRate frameSize:(UInt32)frameSize channels:(UInt8)channels;

@end

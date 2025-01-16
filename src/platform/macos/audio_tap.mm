/**
 * @file src/platform/macos/audio_tap.m
 * @brief macOS 14.2+ system audio capture
 */

#import "audio_tap.h"
#import "audio_device.h"
#import "coreaudio_helpers.h"

#include "src/logging.h"

@implementation AudioTap

// TODO
+ (NSArray<AVCaptureDevice *> *)microphones {
  return nil;
}

+ (NSArray<NSString *> *)getOutputDevices {
  NSMutableArray *result = [[NSMutableArray alloc] init];

  // Always put default first
  [result addObject:[[AudioDevice defaultOutputDevice] name]];

  NSArray<AudioDevice *> *deviceList = [AudioDevice getAllDevices];
  for (AudioDevice *device in deviceList) {
    if (![device isInput]) {
      [result addObject:[device name]];
    }
  }

  return result;
}

+ (NSString *)findAudioSinkUID:(NSString *)name {

  return nil;
}

- (instancetype)initWithDeviceUID:(NSString *)audioSinkUID sampleRate:(UInt32)sampleRate frameSize:(UInt32)frameSize channels:(UInt8)channels {
  // TODO

  return self;
}

// TODO
// - (void)dealloc {
//   // make sure we don't process any further samples
//   self.audioConnection = nil;
//   // make sure nothing gets stuck on this signal
//   [self.samplesArrivedSignal signal];
//   [self.samplesArrivedSignal release];
//   TPCircularBufferCleanup(&audioSampleBuffer);
//   [super dealloc];
// }


// TODO
- (void)captureOutput:(AVCaptureOutput *)output
  didOutputSampleBuffer:(CMSampleBufferRef)sampleBuffer
         fromConnection:(AVCaptureConnection *)connection {
  // if (connection == self.audioConnection) {
  //   AudioBufferList audioBufferList;
  //   CMBlockBufferRef blockBuffer;

  //   CMSampleBufferGetAudioBufferListWithRetainedBlockBuffer(sampleBuffer, NULL, &audioBufferList, sizeof(audioBufferList), NULL, NULL, 0, &blockBuffer);

  //   // NSAssert(audioBufferList.mNumberBuffers == 1, @"Expected interleaved PCM format but buffer contained %u streams", audioBufferList.mNumberBuffers);

  //   // this is safe, because an interleaved PCM stream has exactly one buffer,
  //   // and we don't want to do sanity checks in a performance critical exec path
  //   AudioBuffer audioBuffer = audioBufferList.mBuffers[0];

  //   TPCircularBufferProduceBytes(&self->audioSampleBuffer, audioBuffer.mData, audioBuffer.mDataByteSize);
  //   [self.samplesArrivedSignal signal];
  // }
}

@end

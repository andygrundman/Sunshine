/**
 * @file src/platform/macos/audio_device.h
 * @brief a logical audio device
 */
#pragma once

#import <AVFoundation/AVFoundation.h>

@interface AudioDevice: NSObject

@property (nonatomic, assign) AudioDeviceID id;
@property (nonatomic, copy) NSString *uid;
@property (nonatomic, copy) NSString *name;
@property (nonatomic, assign) bool isAggregate;
@property (nonatomic, assign) bool isInput;

+ (NSArray<AudioDevice *> *)getAllDevices;
+ (AudioDevice *)defaultOutputDevice;

- (instancetype)initFromAudioDeviceID:(AudioDeviceID)id;

@end

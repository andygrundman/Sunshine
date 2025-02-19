#pragma once

#include <string>

#include <AudioToolbox/AudioToolbox.h>
#include <CoreFoundation/CoreFoundation.h>

constexpr AudioUnitElement outputElement{0};
constexpr AudioUnitElement inputElement{1};

static std::string CA_Status(OSStatus error) {
  char errorString[20];

  // See if it appears to be a 4-char-code
  *(uint32_t *)(errorString + 1) = CFSwapInt32HostToBig(error);
  if (isprint(errorString[1]) && isprint(errorString[2]) &&
    isprint(errorString[3]) && isprint(errorString[4])) {
    errorString[0] = errorString[5] = '\'';
    errorString[6] = '\0';
  }
  else {
    // No, format it as an integer
    snprintf(errorString, sizeof(errorString), "%d", (int)error);
  }

  return std::string(errorString);
}

static void CA_FourCC(uint32_t value, char *outFormatIDStr) {
  uint32_t formatID = CFSwapInt32HostToBig(value);
  bcopy(&formatID, outFormatIDStr, 4);
  outFormatIDStr[4] = '\0';
}

// based on mpv ca_print_asbd()
static std::string CA_PrintASBD(const AudioStreamBasicDescription *asbd) {
  char asbdStr[512];
  memset(asbdStr, 0, 512);

  char formatIDStr[5];
  CA_FourCC(asbd->mFormatID, formatIDStr);

  uint32_t flags = asbd->mFormatFlags;
  snprintf(asbdStr, sizeof(asbdStr),
    "%7.1fHz %ubit %s "
    "[%ubpp][%ufpp]"
    "[%ubpf][%uch] "
    "%s %s %s%s%s%s",
    asbd->mSampleRate, asbd->mBitsPerChannel, formatIDStr,
    asbd->mBytesPerPacket, asbd->mFramesPerPacket,
    asbd->mBytesPerFrame, asbd->mChannelsPerFrame,
    (flags & kAudioFormatFlagIsFloat) ? "float" : "int",
    (flags & kAudioFormatFlagIsBigEndian) ? "BE" : "LE",
    (flags & kAudioFormatFlagIsFloat) ? ""
        : ((flags & kAudioFormatFlagIsSignedInteger) ? "S" : "U"),
    (flags & kAudioFormatFlagIsPacked) ? " packed" : "",
    (flags & kAudioFormatFlagIsAlignedHigh) ? " aligned" : "",
    (flags & kAudioFormatFlagIsNonInterleaved) ? " non-interleaved" : " interleaved");

  return std::string(asbdStr);
}

// classic hex dump
static void CA_HexDump(const float *buffer, size_t length)
{
    const uint8_t *bytePtr = (const uint8_t *)buffer;
    size_t bytesToPrint = length * sizeof(float);

    // Print 32 bytes per line
    for (size_t i = 0; i < bytesToPrint; i += 32) {
        printf("%08lx  ", (unsigned long)(bytePtr + i));

        // Print the hex values (32 bytes)
        for (size_t j = 0; j < 32 && (i + j) < bytesToPrint; ++j) {
            printf("%02x ", bytePtr[i + j]);
            if (j == 15) printf(" ");
        }

        printf(" |");

        for (size_t j = 0; j < 32 && (i + j) < bytesToPrint; ++j) {
            uint8_t byte = bytePtr[i + j];
            if (byte >= 32 && byte <= 126)
                printf("%c", byte);
            else
                printf(".");
        }

        printf("|\n");
    }
}

#include "core/audio.h"

#include "core/log.h"

#import <AudioToolbox/AudioToolbox.h>

#include <array>
#include <condition_variable>
#include <cstring>
#include <mutex>

namespace tl::core::audio_device {
namespace {
constexpr size_t kBlockFrames = 1024;
constexpr size_t kBufferCount = 4;
constexpr int kRate = 48000;

struct Device {
    std::mutex mutex;
    std::condition_variable available;
    AudioQueueRef queue = nullptr;
    std::array<AudioQueueBufferRef, kBufferCount> buffers{};
    std::array<bool, kBufferCount> free{};
    bool open = false;
};

Device& device() {
    static Device d;
    return d;
}

void completed(void* context, AudioQueueRef, AudioQueueBufferRef buffer) {
    Device& d = *static_cast<Device*>(context);
    std::lock_guard<std::mutex> guard(d.mutex);
    for (size_t i = 0; i < kBufferCount; ++i) {
        if (d.buffers[i] == buffer) {
            d.free[i] = true;
            d.available.notify_one();
            return;
        }
    }
}
}  // namespace

int open() {
    Device& d = device();
    std::lock_guard<std::mutex> guard(d.mutex);
    if (d.open) return kRate;
    AudioStreamBasicDescription format{};
    format.mSampleRate = kRate;
    format.mFormatID = kAudioFormatLinearPCM;
    // Linear PCM is little-endian on Apple platforms unless BigEndian is set.
    format.mFormatFlags = kAudioFormatFlagIsSignedInteger | kAudioFormatFlagIsPacked;
    format.mBytesPerPacket = 4;
    format.mFramesPerPacket = 1;
    format.mBytesPerFrame = 4;
    format.mChannelsPerFrame = 2;
    format.mBitsPerChannel = 16;
    OSStatus status = AudioQueueNewOutput(&format, completed, &d, nullptr, nullptr, 0, &d.queue);
    if (status != noErr) {
        TL_LOG_WARN("audio: AudioQueueNewOutput failed (%d)", static_cast<int>(status));
        d.queue = nullptr;
        return 0;
    }
    const UInt32 bytes = static_cast<UInt32>(kBlockFrames * 4);
    for (size_t i = 0; i < kBufferCount; ++i) {
        status = AudioQueueAllocateBuffer(d.queue, bytes, &d.buffers[i]);
        if (status != noErr) {
            AudioQueueDispose(d.queue, true);
            d.queue = nullptr;
            d.buffers.fill(nullptr);
            TL_LOG_WARN("audio: AudioQueueAllocateBuffer failed (%d)", static_cast<int>(status));
            return 0;
        }
        d.free[i] = true;
    }
    status = AudioQueueStart(d.queue, nullptr);
    if (status != noErr) {
        AudioQueueDispose(d.queue, true);
        d.queue = nullptr;
        d.buffers.fill(nullptr);
        TL_LOG_WARN("audio: AudioQueueStart failed (%d)", static_cast<int>(status));
        return 0;
    }
    d.open = true;
    return kRate;
}

void close() {
    Device& d = device();
    AudioQueueRef queue = nullptr;
    {
        std::lock_guard<std::mutex> guard(d.mutex);
        if (!d.open) return;
        d.open = false;
        queue = d.queue;
        d.available.notify_all();
    }
    AudioQueueStop(queue, true);
    AudioQueueDispose(queue, true);
    std::lock_guard<std::mutex> guard(d.mutex);
    d.queue = nullptr;
    d.buffers.fill(nullptr);
    d.free.fill(false);
}

size_t block_frames() { return kBlockFrames; }

bool write(const int16_t* samples, size_t frames) {
    if (frames > kBlockFrames) frames = kBlockFrames;
    Device& d = device();
    std::unique_lock<std::mutex> lock(d.mutex);
    d.available.wait(lock, [&] {
        if (!d.open) return true;
        for (bool free : d.free) if (free) return true;
        return false;
    });
    if (!d.open) return false;
    size_t index = 0;
    while (index < kBufferCount && !d.free[index]) ++index;
    d.free[index] = false;
    AudioQueueBufferRef buffer = d.buffers[index];
    std::memcpy(buffer->mAudioData, samples, frames * 4);
    buffer->mAudioDataByteSize = static_cast<UInt32>(frames * 4);
    const OSStatus status = AudioQueueEnqueueBuffer(d.queue, buffer, 0, nullptr);
    if (status != noErr) {
        d.free[index] = true;
        TL_LOG_WARN("audio: AudioQueueEnqueueBuffer failed (%d)", static_cast<int>(status));
        return false;
    }
    return true;
}

void prepare_mixer_thread() {}
}  // namespace tl::core::audio_device

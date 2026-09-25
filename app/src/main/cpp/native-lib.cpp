#include <jni.h>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include "core/pokewalker/pocketwalker.h"

namespace {
std::mutex g_mutex;
std::unique_ptr<PocketWalker> g_emulator;
std::unique_ptr<std::thread> g_thread;

void stopEmulatorLocked() {
    if (g_emulator) {
        g_emulator->Stop();
    }
    if (g_thread && g_thread->joinable()) {
        g_thread->join();
    }
    g_thread.reset();
    g_emulator.reset();
}
}

extern "C"
JNIEXPORT jboolean JNICALL
Java_org_pocketwalker_android_NativeBridge_loadRom(
        JNIEnv* env,
        jobject,
        jbyteArray romBytes) {
    std::scoped_lock lock(g_mutex);
    stopEmulatorLocked();

    const jsize len = env->GetArrayLength(romBytes);
    if (len < 0xC000) {
        return JNI_FALSE;
    }

    RomBuffer rom{};
    env->GetByteArrayRegion(
        romBytes,
        0,
        static_cast<jsize>(rom.size()),
        reinterpret_cast<jbyte*>(rom.data())
    );

    g_emulator = std::make_unique<PocketWalker>(rom);
    g_thread = std::make_unique<std::thread>([] {
        g_emulator->Start();
    });

    return JNI_TRUE;
}

extern "C"
JNIEXPORT void JNICALL
Java_org_pocketwalker_android_NativeBridge_stop(
        JNIEnv*,
        jobject) {
    std::scoped_lock lock(g_mutex);
    stopEmulatorLocked();
}

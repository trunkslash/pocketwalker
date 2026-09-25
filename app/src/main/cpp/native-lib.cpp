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

ButtonType toButton(jint value) {
    switch (value) {
        case 0: return ButtonType::LEFT;
        case 1: return ButtonType::CENTER;
        default: return ButtonType::RIGHT;
    }
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

extern "C"
JNIEXPORT void JNICALL
Java_org_pocketwalker_android_NativeBridge_setButton(
        JNIEnv*,
        jobject,
        jint button,
        jboolean pressed) {
    std::scoped_lock lock(g_mutex);
    if (!g_emulator) return;
    if (pressed) {
        g_emulator->PressButton(toButton(button));
    } else {
        g_emulator->ReleaseButton(toButton(button));
    }
}

extern "C"
JNIEXPORT jbyteArray JNICALL
Java_org_pocketwalker_android_NativeBridge_getFrame(
        JNIEnv* env,
        jobject) {
    std::scoped_lock lock(g_mutex);
    constexpr int SCREEN_W = 96;
    constexpr int SCREEN_H = 64;
    constexpr int PIXELS = SCREEN_W * SCREEN_H;

    jbyteArray out = env->NewByteArray(PIXELS);
    if (!out) return nullptr;

    std::vector<jbyte> pixels(PIXELS, 0);
    if (!g_emulator) {
        env->SetByteArrayRegion(out, 0, PIXELS, pixels.data());
        return out;
    }

    SSD1854DrawInfo* draw = g_emulator->GetDrawInfo();
    if (draw->power_save_mode) {
        env->SetByteArrayRegion(out, 0, PIXELS, pixels.data());
        return out;
    }

    for (int y = 0; y < SCREEN_H; y++) {
        const int page = y / 8 + draw->page_offset;
        if (page < 0 || page >= SSD1854_TOTAL_PAGES) continue;

        const int pageOffset = page * SSD1854_TOTAL_COLUMNS * SSD1854_COLUMN_SIZE;
        const int bit = y % 8;

        for (int x = 0; x < SCREEN_W; x++) {
            const int base = SSD1854_COLUMN_SIZE * x + pageOffset;
            const uint8_t lo = draw->vram.Read8(base);
            const uint8_t hi = draw->vram.Read8(base + 1);
            const uint8_t idx = (((lo >> bit) & 1u) << 1u) | ((hi >> bit) & 1u);
            pixels[y * SCREEN_W + x] = static_cast<jbyte>(idx);
        }
    }

    env->SetByteArrayRegion(out, 0, PIXELS, pixels.data());
    return out;
}

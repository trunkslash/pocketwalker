#include <jni.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "core/pokewalker/pocketwalker.h"

namespace {
std::mutex g_mutex;
std::unique_ptr<PocketWalker> g_emulator;
std::unique_ptr<std::thread> g_thread;

std::mutex g_ir_mutex;
std::atomic<bool> g_ir_running{false};
std::atomic<bool> g_ir_connected{false};
int g_ir_socket = -1;
std::thread g_ir_rx_thread;
std::thread g_ir_tx_thread;
std::string g_ir_status = "Disconnected";

std::mutex g_tx_mutex;
std::condition_variable g_tx_cv;
std::vector<uint8_t> g_tx_buffer;
std::chrono::steady_clock::time_point g_tx_last_byte;
bool g_tx_pending = false;

constexpr int AUDIO_SAMPLE_RATE = 32000;
constexpr float AUDIO_MIN_FREQUENCY = 120.0f;
constexpr float AUDIO_AMPLITUDE = 10000.0f;

std::mutex g_audio_mutex;
float g_audio_frequency = 0.0f;
float g_audio_phase = 0.0f;
bool g_audio_full_volume = true;

void resetAudio() {
    std::scoped_lock lock(g_audio_mutex);
    g_audio_frequency = 0.0f;
    g_audio_phase = 0.0f;
    g_audio_full_volume = true;
}

void pushAudioSample(BuzzerInformation info) {
    std::scoped_lock lock(g_audio_mutex);

    if (info.frequency >= AUDIO_MIN_FREQUENCY) {
        // Timer W frequency represents the buzzer transition timing.
        // A complete square-wave cycle requires two transitions.
        g_audio_frequency = info.frequency;
        g_audio_full_volume = info.is_full_volume;
    } else {
        g_audio_frequency = 0.0f;
    }
}

void setIrStatus(const std::string& value) {
    std::scoped_lock lock(g_ir_mutex);
    g_ir_status = value;
}

bool sendAll(const uint8_t* data, size_t size) {
    std::scoped_lock lock(g_ir_mutex);
    if (g_ir_socket < 0 || !g_ir_connected) return false;
    size_t sent = 0;
    while (sent < size) {
        const ssize_t result = send(g_ir_socket, data + sent, size - sent, MSG_NOSIGNAL);
        if (result <= 0) {
            g_ir_connected = false;
            return false;
        }
        sent += static_cast<size_t>(result);
    }
    return true;
}

void queueIrByte(uint8_t byte) {
    if (!g_ir_connected) return;
    {
        std::scoped_lock lock(g_tx_mutex);
        g_tx_buffer.push_back(byte);
        g_tx_last_byte = std::chrono::steady_clock::now();
        g_tx_pending = true;
    }
    g_tx_cv.notify_one();
}

void irTxLoop() {
    std::unique_lock lock(g_tx_mutex);
    while (g_ir_running) {
        g_tx_cv.wait(lock, [] { return !g_ir_running || g_tx_pending; });
        if (!g_ir_running) break;
        const auto deadline = g_tx_last_byte + std::chrono::milliseconds(5);
        if (g_tx_cv.wait_until(lock, deadline, [deadline] {
                return !g_ir_running || g_tx_last_byte > deadline - std::chrono::milliseconds(5);
            })) {
            if (!g_ir_running) break;
            continue;
        }
        if (std::chrono::steady_clock::now() < g_tx_last_byte + std::chrono::milliseconds(5)) continue;
        std::vector<uint8_t> packet;
        packet.swap(g_tx_buffer);
        g_tx_pending = false;
        lock.unlock();
        if (!packet.empty() && !sendAll(packet.data(), packet.size())) setIrStatus("Disconnected");
        lock.lock();
    }
}

void closeIrSocket() {
    g_ir_running = false;
    g_tx_cv.notify_all();
    int fd = -1;
    {
        std::scoped_lock lock(g_ir_mutex);
        fd = g_ir_socket;
        g_ir_socket = -1;
    }
    if (fd >= 0) {
        shutdown(fd, SHUT_RDWR);
        close(fd);
    }
    if (g_ir_rx_thread.joinable() && g_ir_rx_thread.get_id() != std::this_thread::get_id()) g_ir_rx_thread.join();
    if (g_ir_tx_thread.joinable() && g_ir_tx_thread.get_id() != std::this_thread::get_id()) g_ir_tx_thread.join();
    {
        std::scoped_lock lock(g_tx_mutex);
        g_tx_buffer.clear();
        g_tx_pending = false;
    }
    g_ir_connected = false;
    setIrStatus("Disconnected");
}

void stopEmulatorLocked() {
    closeIrSocket();
    if (g_emulator) g_emulator->Stop();
    if (g_thread && g_thread->joinable()) g_thread->join();
    g_thread.reset();
    g_emulator.reset();
    resetAudio();
}

ButtonType toButton(jint value) {
    switch (value) {
        case 0: return ButtonType::LEFT;
        case 1: return ButtonType::CENTER;
        default: return ButtonType::RIGHT;
    }
}
}

extern "C" JNIEXPORT jboolean JNICALL
Java_org_pocketwalker_android_NativeBridge_loadRom(JNIEnv* env, jobject, jbyteArray romBytes) {
    std::scoped_lock lock(g_mutex);
    stopEmulatorLocked();
    const jsize len = env->GetArrayLength(romBytes);
    if (len < 0xC000) return JNI_FALSE;
    RomBuffer rom{};
    env->GetByteArrayRegion(romBytes, 0, static_cast<jsize>(rom.size()), reinterpret_cast<jbyte*>(rom.data()));
    g_emulator = std::make_unique<PocketWalker>(rom);
    g_emulator->OnTransmitIR([](uint8_t byte) { queueIrByte(byte); });
    g_emulator->OnSamplePushed([](BuzzerInformation info) { pushAudioSample(info); });
    g_thread = std::make_unique<std::thread>([] { g_emulator->Start(); });
    return JNI_TRUE;
}

extern "C" JNIEXPORT void JNICALL
Java_org_pocketwalker_android_NativeBridge_stop(JNIEnv*, jobject) {
    std::scoped_lock lock(g_mutex);
    stopEmulatorLocked();
}

extern "C" JNIEXPORT void JNICALL
Java_org_pocketwalker_android_NativeBridge_setButton(JNIEnv*, jobject, jint button, jboolean pressed) {
    std::scoped_lock lock(g_mutex);
    if (!g_emulator) return;
    if (pressed) g_emulator->PressButton(toButton(button));
    else g_emulator->ReleaseButton(toButton(button));
}


extern "C" JNIEXPORT void JNICALL
Java_org_pocketwalker_android_NativeBridge_setSyntheticSteps(
        JNIEnv*, jobject, jboolean enabled) {
    std::scoped_lock lock(g_mutex);

    if (!g_emulator)
        return;

    g_emulator->UseSyntheticSteps(enabled == JNI_TRUE);
}
extern "C" JNIEXPORT jbyteArray JNICALL
Java_org_pocketwalker_android_NativeBridge_getFrame(JNIEnv* env, jobject) {
    std::scoped_lock lock(g_mutex);
    constexpr int SCREEN_W = 96, SCREEN_H = 64, PIXELS = SCREEN_W * SCREEN_H;
    jbyteArray out = env->NewByteArray(PIXELS);
    if (!out) return nullptr;
    std::vector<jbyte> pixels(PIXELS, 0);
    if (!g_emulator) {
        env->SetByteArrayRegion(out, 0, PIXELS, pixels.data());
        return out;
    }
    SSD1854DrawInfo* draw = g_emulator->GetDrawInfo();
    if (!draw->power_save_mode) {
        for (int y = 0; y < SCREEN_H; ++y) {
            const int page = y / 8 + draw->page_offset;
            if (page < 0 || page >= SSD1854_TOTAL_PAGES) continue;
            const int pageOffset = page * SSD1854_TOTAL_COLUMNS * SSD1854_COLUMN_SIZE;
            const int bit = y % 8;
            for (int x = 0; x < SCREEN_W; ++x) {
                const int base = SSD1854_COLUMN_SIZE * x + pageOffset;
                const uint8_t lo = draw->vram.Read8(base), hi = draw->vram.Read8(base + 1);
                pixels[y * SCREEN_W + x] = static_cast<jbyte>((((lo >> bit) & 1u) << 1u) | ((hi >> bit) & 1u));
            }
        }
    }
    env->SetByteArrayRegion(out, 0, PIXELS, pixels.data());
    return out;
}

extern "C" JNIEXPORT jboolean JNICALL
Java_org_pocketwalker_android_NativeBridge_connectIr(JNIEnv* env, jobject, jstring hostString, jint port) {
    if (!g_emulator || port <= 0 || port > 65535) return JNI_FALSE;
    closeIrSocket();
    const char* hostChars = env->GetStringUTFChars(hostString, nullptr);
    std::string host(hostChars ? hostChars : "");
    if (hostChars) env->ReleaseStringUTFChars(hostString, hostChars);
    if (host.empty()) return JNI_FALSE;
    setIrStatus("Connecting...");
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* result = nullptr;
    const std::string portText = std::to_string(port);
    if (getaddrinfo(host.c_str(), portText.c_str(), &hints, &result) != 0) {
        setIrStatus("Host lookup failed");
        return JNI_FALSE;
    }
    int fd = -1;
    for (addrinfo* rp = result; rp != nullptr; rp = rp->ai_next) {
        fd = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
        if (fd < 0) continue;
        int one = 1;
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
        if (connect(fd, rp->ai_addr, rp->ai_addrlen) == 0) break;
        close(fd);
        fd = -1;
    }
    freeaddrinfo(result);
    if (fd < 0) {
        setIrStatus("Connection failed");
        return JNI_FALSE;
    }
    {
        std::scoped_lock lock(g_ir_mutex);
        g_ir_socket = fd;
    }
    g_ir_running = true;
    g_ir_connected = true;
    setIrStatus("Connected");
    g_ir_tx_thread = std::thread(irTxLoop);
    g_ir_rx_thread = std::thread([] {
        uint8_t buffer[1024];
        while (g_ir_running) {
            int fdLocal;
            {
                std::scoped_lock lock(g_ir_mutex);
                fdLocal = g_ir_socket;
            }
            if (fdLocal < 0) break;
            const ssize_t count = recv(fdLocal, buffer, sizeof(buffer), 0);
            if (count <= 0) break;
            std::scoped_lock emulatorLock(g_mutex);
            if (!g_emulator) break;
            for (ssize_t i = 0; i < count; ++i) g_emulator->ReceiveIR(buffer[i]);
        }
        g_ir_connected = false;
        setIrStatus("Disconnected");
    });
    return JNI_TRUE;
}

extern "C" JNIEXPORT void JNICALL
Java_org_pocketwalker_android_NativeBridge_disconnectIr(JNIEnv*, jobject) { closeIrSocket(); }

extern "C" JNIEXPORT jstring JNICALL
Java_org_pocketwalker_android_NativeBridge_irStatus(JNIEnv* env, jobject) {
    std::scoped_lock lock(g_ir_mutex);
    return env->NewStringUTF(g_ir_status.c_str());
}

extern "C" JNIEXPORT jshortArray JNICALL
Java_org_pocketwalker_android_NativeBridge_getAudioSamples(
        JNIEnv* env, jobject, jint maxSamples) {

    if (maxSamples <= 0)
        return env->NewShortArray(0);

    std::vector<int16_t> samples(static_cast<size_t>(maxSamples));

    {
        std::scoped_lock lock(g_audio_mutex);

        const float frequency = g_audio_frequency;
        const float amplitude =
            g_audio_full_volume ? AUDIO_AMPLITUDE : AUDIO_AMPLITUDE * 0.5f;

        if (frequency >= AUDIO_MIN_FREQUENCY) {
            const float phaseStep =
                frequency / static_cast<float>(AUDIO_SAMPLE_RATE);

            for (jint i = 0; i < maxSamples; ++i) {
                // The real PokéWalker buzzer is driven by a square wave.
                samples[static_cast<size_t>(i)] =
                    g_audio_phase < 0.5f
                        ? static_cast<int16_t>(amplitude)
                        : static_cast<int16_t>(-amplitude);

                g_audio_phase += phaseStep;

                if (g_audio_phase >= 1.0f)
                    g_audio_phase -= 1.0f;
            }
        } else {
            std::fill(samples.begin(), samples.end(), 0);
        }
    }

    jshortArray out = env->NewShortArray(maxSamples);

    if (!out)
        return nullptr;

    env->SetShortArrayRegion(
        out,
        0,
        maxSamples,
        reinterpret_cast<const jshort*>(samples.data())
    );

    return out;
}

extern "C" JNIEXPORT jbyteArray JNICALL
Java_org_pocketwalker_android_NativeBridge_getEeprom(
        JNIEnv* env, jobject) {

    std::scoped_lock lock(g_mutex);

    if (!g_emulator)
        return env->NewByteArray(0);

    const auto eeprom = g_emulator->GetEepromBuffer();

    jbyteArray result =
        env->NewByteArray(static_cast<jsize>(eeprom.size()));

    if (!result)
        return nullptr;

    env->SetByteArrayRegion(
        result,
        0,
        static_cast<jsize>(eeprom.size()),
        reinterpret_cast<const jbyte*>(eeprom.data())
    );

    return result;
}

extern "C" JNIEXPORT jboolean JNICALL
Java_org_pocketwalker_android_NativeBridge_setEeprom(
        JNIEnv* env, jobject, jbyteArray data) {

    std::scoped_lock lock(g_mutex);

    if (!g_emulator || !data)
        return JNI_FALSE;

    const jsize len = env->GetArrayLength(data);

    if (len != EEPROM_SIZE)
        return JNI_FALSE;

    EepromBuffer buffer{};

    env->GetByteArrayRegion(
        data,
        0,
        len,
        reinterpret_cast<jbyte*>(buffer.data())
    );

    g_emulator->SetEepromBuffer(buffer);

    return JNI_TRUE;
}


extern "C" JNIEXPORT jint JNICALL
Java_org_pocketwalker_android_NativeBridge_getSessionSteps(
        JNIEnv*, jobject) {
    std::scoped_lock lock(g_mutex);

    if (!g_emulator)
        return 0;

    return static_cast<jint>(g_emulator->GetSessionSteps());
}

extern "C" JNIEXPORT jint JNICALL
Java_org_pocketwalker_android_NativeBridge_getTotalSteps(
        JNIEnv*, jobject) {
    std::scoped_lock lock(g_mutex);

    if (!g_emulator)
        return 0;

    return static_cast<jint>(g_emulator->GetTotalSteps());
}

extern "C" JNIEXPORT void JNICALL
Java_org_pocketwalker_android_NativeBridge_restoreSteps(
        JNIEnv*, jobject, jint sessionSteps, jint totalSteps) {
    std::scoped_lock lock(g_mutex);

    if (!g_emulator)
        return;

    g_emulator->RestoreSteps(
        static_cast<uint32_t>(sessionSteps),
        static_cast<uint32_t>(totalSteps)
    );
}

#include <jni.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>
#include <atomic>
#include <chrono>
#include <condition_variable>
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

// Match the desktop Qt frontend: collect consecutive IR bytes and flush the
// accumulated packet after 5 ms of inactivity.
std::mutex g_tx_mutex;
std::condition_variable g_tx_cv;
std::vector<uint8_t> g_tx_buffer;
std::chrono::steady_clock::time_point g_tx_last_byte;
bool g_tx_pending = false;

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

        // If another byte arrived while the timer was expiring, restart the
        // 5 ms idle period just like QTimer::start(5) in the desktop frontend.
        if (std::chrono::steady_clock::now() < g_tx_last_byte + std::chrono::milliseconds(5))
            continue;

        std::vector<uint8_t> packet;
        packet.swap(g_tx_buffer);
        g_tx_pending = false;
        lock.unlock();
        if (!packet.empty() && !sendAll(packet.data(), packet.size()))
            setIrStatus("Disconnected");
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
    if (g_ir_rx_thread.joinable() && g_ir_rx_thread.get_id() != std::this_thread::get_id())
        g_ir_rx_thread.join();
    if (g_ir_tx_thread.joinable() && g_ir_tx_thread.get_id() != std::this_thread::get_id())
        g_ir_tx_thread.join();

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
    env->GetByteArrayRegion(romBytes, 0, static_cast<jsize>(rom.size()),
                            reinterpret_cast<jbyte*>(rom.data()));

    g_emulator = std::make_unique<PocketWalker>(rom);
    g_emulator->OnTransmitIR([](uint8_t byte) { queueIrByte(byte); });
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

extern "C" JNIEXPORT jbyteArray JNICALL
Java_org_pocketwalker_android_NativeBridge_getFrame(JNIEnv* env, jobject) {
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
    if (!draw->power_save_mode) {
        for (int y = 0; y < SCREEN_H; ++y) {
            const int page = y / 8 + draw->page_offset;
            if (page < 0 || page >= SSD1854_TOTAL_PAGES) continue;
            const int pageOffset = page * SSD1854_TOTAL_COLUMNS * SSD1854_COLUMN_SIZE;
            const int bit = y % 8;
            for (int x = 0; x < SCREEN_W; ++x) {
                const int base = SSD1854_COLUMN_SIZE * x + pageOffset;
                const uint8_t lo = draw->vram.Read8(base);
                const uint8_t hi = draw->vram.Read8(base + 1);
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
Java_org_pocketwalker_android_NativeBridge_disconnectIr(JNIEnv*, jobject) {
    closeIrSocket();
}

extern "C" JNIEXPORT jstring JNICALL
Java_org_pocketwalker_android_NativeBridge_irStatus(JNIEnv* env, jobject) {
    std::scoped_lock lock(g_ir_mutex);
    return env->NewStringUTF(g_ir_status.c_str());
}

package org.pocketwalker.android

object NativeBridge {
    init {
        System.loadLibrary("pocketwalker_jni")
    }

    external fun loadRom(romBytes: ByteArray): Boolean
    external fun stop()
}

package org.pocketwalker.android

object NativeBridge {
    init {
        System.loadLibrary("pocketwalker_jni")
    }

    external fun loadRom(romBytes: ByteArray): Boolean
    external fun stop()
    external fun setButton(button: Int, pressed: Boolean)
    external fun getFrame(): ByteArray

    external fun connectIr(host: String, port: Int): Boolean
    external fun disconnectIr()
    external fun irStatus(): String
}

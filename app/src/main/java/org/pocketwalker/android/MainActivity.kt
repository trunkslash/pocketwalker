package org.pocketwalker.android

import android.media.AudioAttributes
import android.media.AudioFormat
import android.media.AudioTrack
import android.net.Uri
import android.os.Bundle
import android.provider.OpenableColumns
import android.text.InputType
import android.view.Gravity
import android.view.MotionEvent
import android.view.ViewGroup
import android.widget.Button
import android.widget.EditText
import android.widget.LinearLayout
import android.widget.TextView
import androidx.activity.result.contract.ActivityResultContracts
import androidx.appcompat.app.AppCompatActivity
import java.util.concurrent.atomic.AtomicBoolean
import android.util.Log

class MainActivity : AppCompatActivity() {
    private lateinit var status: TextView
    private lateinit var irStatus: TextView
    private var audioTrack: AudioTrack? = null
    private var audioThread: Thread? = null
    private val audioRunning = AtomicBoolean(false)

    private val romPicker = registerForActivityResult(ActivityResultContracts.OpenDocument()) { uri: Uri? ->
        if (uri != null) loadRom(uri)
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        startAudio()

        val root = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            gravity = Gravity.CENTER_HORIZONTAL
            setPadding(32, 24, 32, 24)
        }
        root.addView(TextView(this).apply { text = "PocketWalker"; textSize = 28f; gravity = Gravity.CENTER })
        root.addView(PocketWalkerDisplay(this).apply {
            layoutParams = LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, 620).apply { topMargin = 16; bottomMargin = 16 }
        })

        val controls = LinearLayout(this).apply { orientation = LinearLayout.HORIZONTAL; gravity = Gravity.CENTER }
        listOf("◀" to 0, "●" to 1, "▶" to 2).forEach { (label, code) ->
            controls.addView(Button(this).apply {
                text = label; textSize = 24f
                layoutParams = LinearLayout.LayoutParams(0, 130, 1f).apply { marginStart = 8; marginEnd = 8 }
                setOnTouchListener { _, event ->
                    when (event.actionMasked) {
                        MotionEvent.ACTION_DOWN -> { NativeBridge.setButton(code, true); true }
                        MotionEvent.ACTION_UP, MotionEvent.ACTION_CANCEL -> { NativeBridge.setButton(code, false); true }
                        else -> false
                    }
                }
            })
        }
        root.addView(controls)

val stepControls = LinearLayout(this).apply {
    orientation = LinearLayout.HORIZONTAL
    gravity = Gravity.CENTER
}

stepControls.addView(Button(this).apply {
    text = "START WALKING"
    layoutParams = LinearLayout.LayoutParams(
        0,
        ViewGroup.LayoutParams.WRAP_CONTENT,
        1f
    )
    setOnClickListener {
        NativeBridge.setSyntheticSteps(true)
    }
})

stepControls.addView(Button(this).apply {
    text = "STOP WALKING"
    layoutParams = LinearLayout.LayoutParams(
        0,
        ViewGroup.LayoutParams.WRAP_CONTENT,
        1f
    )
    setOnClickListener {
        NativeBridge.setSyntheticSteps(false)
    }
})

root.addView(stepControls)
        root.addView(Button(this).apply { text = "Choose PokéWalker ROM"; setOnClickListener { romPicker.launch(arrayOf("application/octet-stream", "*/*")) } })
        status = TextView(this).apply { text = "No ROM loaded"; textSize = 14f; gravity = Gravity.CENTER }
        root.addView(status)

        root.addView(TextView(this).apply { text = "melonDS-IR"; textSize = 20f; gravity = Gravity.CENTER; setPadding(0, 20, 0, 4) })
        val hostInput = EditText(this).apply {
            hint = "melonDS PC IP (e.g. 192.168.1.100)"; setSingleLine(true); inputType = InputType.TYPE_CLASS_TEXT
            layoutParams = LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT)
        }
        root.addView(hostInput)
        val portInput = EditText(this).apply {
            hint = "Port"; setText("8081"); setSingleLine(true); inputType = InputType.TYPE_CLASS_NUMBER
            layoutParams = LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT)
        }
        root.addView(portInput)

        val irButtons = LinearLayout(this).apply { orientation = LinearLayout.HORIZONTAL; gravity = Gravity.CENTER }
        irButtons.addView(Button(this).apply {
            text = "Connect IR"; layoutParams = LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f)
            setOnClickListener {
                val host = hostInput.text.toString().trim(); val port = portInput.text.toString().toIntOrNull()
                if (host.isEmpty() || port == null) irStatus.text = "Enter a valid host and port"
                else Thread { NativeBridge.connectIr(host, port); runOnUiThread { irStatus.text = "IR: ${NativeBridge.irStatus()}" } }.start()
            }
        })
        irButtons.addView(Button(this).apply {
            text = "Disconnect"; layoutParams = LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f)
            setOnClickListener { NativeBridge.disconnectIr(); irStatus.text = "IR: ${NativeBridge.irStatus()}" }
        })
        root.addView(irButtons)
        irStatus = TextView(this).apply { text = "IR: Disconnected"; textSize = 14f; gravity = Gravity.CENTER }
        root.addView(irStatus)
        setContentView(root)
        loadSavedRom()
    }

    private fun startAudio() {
        val sampleRate = 32000
        val minBuffer = AudioTrack.getMinBufferSize(sampleRate, AudioFormat.CHANNEL_OUT_MONO, AudioFormat.ENCODING_PCM_16BIT)
        val bufferSize = maxOf(minBuffer, sampleRate / 10 * 2)
        audioTrack = AudioTrack.Builder()
            .setAudioAttributes(AudioAttributes.Builder().setUsage(AudioAttributes.USAGE_GAME).setContentType(AudioAttributes.CONTENT_TYPE_MUSIC).build())
            .setAudioFormat(AudioFormat.Builder().setEncoding(AudioFormat.ENCODING_PCM_16BIT).setSampleRate(sampleRate).setChannelMask(AudioFormat.CHANNEL_OUT_MONO).build())
            .setBufferSizeInBytes(bufferSize)
            .setTransferMode(AudioTrack.MODE_STREAM)
            .build().also { it.play() }

        audioRunning.set(true)
        audioThread = Thread {
            while (audioRunning.get()) {
                val samples = NativeBridge.getAudioSamples(1024)
                if (samples.isNotEmpty()) audioTrack?.write(samples, 0, samples.size, AudioTrack.WRITE_BLOCKING)
                else try { Thread.sleep(2) } catch (_: InterruptedException) { break }
            }
        }.apply { name = "PocketWalkerAudio"; start() }
    }

    private fun stopAudio() {
        audioRunning.set(false)
        audioThread?.interrupt()
        try { audioThread?.join(250) } catch (_: InterruptedException) {}
        audioThread = null
        audioTrack?.run { try { stop() } catch (_: IllegalStateException) {}; release() }
        audioTrack = null
    }

    private fun loadRom(uri: Uri) {
    try {
        val bytes = contentResolver.openInputStream(uri)?.use { it.readBytes() }

        if (bytes == null) {
            status.text = "Could not read ROM"
            return
        }

        val ok = NativeBridge.loadRom(bytes)

        if (ok) {
            // Save a private copy for future launches
            openFileOutput("pokewalker.rom", MODE_PRIVATE).use {
                it.write(bytes)
            }

            status.text = "ROM loaded: ${displayName(uri)}"
        } else {
            status.text = "ROM must be at least 48 KiB"
        }
    } catch (t: Throwable) {
        status.text = "Load failed: ${t.message ?: t.javaClass.simpleName}"
    }

}

private fun loadSavedRom() {
    try {
        val romFile = getFileStreamPath("pokewalker.rom")

        Log.d(
            "PocketWalker",
            "loadSavedRom called: path=${romFile.absolutePath}, exists=${romFile.exists()}, size=${if (romFile.exists()) romFile.length() else 0}"
        )

        if (!romFile.exists()) {
            status.text = "No saved ROM found"
            return
        }

        val bytes = romFile.readBytes()

        Log.d(
            "PocketWalker",
            "Read saved ROM: ${bytes.size} bytes"
        )

        val ok = NativeBridge.loadRom(bytes)

        Log.d(
            "PocketWalker",
            "NativeBridge.loadRom result: $ok"
        )

if (ok) {
    val eepromFile = getFileStreamPath("pokewalker.eeprom")

    if (eepromFile.exists()) {
        val eeprom = eepromFile.readBytes()

        if (eeprom.size == 65536) {
            val restored = NativeBridge.setEeprom(eeprom)
            Log.d("PocketWalker", "EEPROM restored: $restored")
        } else {
            Log.e("PocketWalker", "Invalid EEPROM size: ${eeprom.size}")
        }
    }
}

        status.text = if (ok) {
            "Saved PokéWalker ROM loaded"
        } else {
            "Saved ROM could not be loaded"
        }

    } catch (t: Throwable) {
        Log.e("PocketWalker", "Saved ROM load failed", t)

        status.text =
            "Saved ROM load failed: ${t.message ?: t.javaClass.simpleName}"
    }
}


private fun saveEeprom() {
    try {
        val eeprom = NativeBridge.getEeprom()

        if (eeprom.size == 65536) {
            openFileOutput("pokewalker.eeprom", MODE_PRIVATE).use {
                it.write(eeprom)
            }

            Log.d("PocketWalker", "EEPROM saved: ${eeprom.size} bytes")
        } else {
            Log.e("PocketWalker", "EEPROM not saved; size=${eeprom.size}")
        }
    } catch (t: Throwable) {
        Log.e("PocketWalker", "EEPROM save failed", t)
    }
}
private fun displayName(uri: Uri): String {
        contentResolver.query(uri, null, null, null, null)?.use { cursor ->
            val index = cursor.getColumnIndex(OpenableColumns.DISPLAY_NAME)
            if (index >= 0 && cursor.moveToFirst()) return cursor.getString(index)
        }
        return uri.lastPathSegment ?: "ROM"
    }

   override fun onStop() {
    saveEeprom()
    super.onStop()
}

     override fun onDestroy() {
        stopAudio()
        saveEeprom()
        NativeBridge.stop()
        super.onDestroy()
    }
}

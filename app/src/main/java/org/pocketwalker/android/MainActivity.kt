package org.pocketwalker.android

import android.net.Uri
import android.os.Bundle
import android.provider.OpenableColumns
import android.view.Gravity
import android.view.MotionEvent
import android.view.ViewGroup
import android.widget.Button
import android.widget.LinearLayout
import android.widget.TextView
import androidx.activity.result.contract.ActivityResultContracts
import androidx.appcompat.app.AppCompatActivity

class MainActivity : AppCompatActivity() {
    private lateinit var status: TextView

    private val romPicker = registerForActivityResult(ActivityResultContracts.OpenDocument()) { uri: Uri? ->
        if (uri != null) loadRom(uri)
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)

        val root = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            gravity = Gravity.CENTER_HORIZONTAL
            setPadding(32, 32, 32, 32)
        }

        root.addView(TextView(this).apply {
            text = "PocketWalker"
            textSize = 28f
            gravity = Gravity.CENTER
        })

        root.addView(PocketWalkerDisplay(this).apply {
            layoutParams = LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT,
                720
            ).apply {
                topMargin = 24
                bottomMargin = 24
            }
        })

        val controls = LinearLayout(this).apply {
            orientation = LinearLayout.HORIZONTAL
            gravity = Gravity.CENTER
        }

        listOf("◀" to 0, "●" to 1, "▶" to 2).forEach { (label, code) ->
            controls.addView(Button(this).apply {
                text = label
                textSize = 24f
                layoutParams = LinearLayout.LayoutParams(0, 150, 1f).apply {
                    marginStart = 8
                    marginEnd = 8
                }
                setOnTouchListener { _, event ->
                    when (event.actionMasked) {
                        MotionEvent.ACTION_DOWN -> {
                            NativeBridge.setButton(code, true)
                            true
                        }
                        MotionEvent.ACTION_UP, MotionEvent.ACTION_CANCEL -> {
                            NativeBridge.setButton(code, false)
                            true
                        }
                        else -> false
                    }
                }
            })
        }
        root.addView(controls)

        root.addView(Button(this).apply {
            text = "Choose PokéWalker ROM"
            setOnClickListener {
                romPicker.launch(arrayOf("application/octet-stream", "*/*"))
            }
        })

        status = TextView(this).apply {
            text = "No ROM loaded"
            textSize = 16f
            gravity = Gravity.CENTER
            setPadding(0, 20, 0, 0)
        }
        root.addView(status)

        setContentView(root)
    }

    private fun loadRom(uri: Uri) {
        try {
            val bytes = contentResolver.openInputStream(uri)?.use { it.readBytes() }
            if (bytes == null) {
                status.text = "Could not read ROM"
                return
            }

            val ok = NativeBridge.loadRom(bytes)
            status.text = if (ok) "ROM loaded: ${displayName(uri)}" else "ROM must be at least 48 KiB"
        } catch (t: Throwable) {
            status.text = "Load failed: ${t.message ?: t.javaClass.simpleName}"
        }
    }

    private fun displayName(uri: Uri): String {
        contentResolver.query(uri, null, null, null, null)?.use { cursor ->
            val index = cursor.getColumnIndex(OpenableColumns.DISPLAY_NAME)
            if (index >= 0 && cursor.moveToFirst()) return cursor.getString(index)
        }
        return uri.lastPathSegment ?: "ROM"
    }

    override fun onDestroy() {
        NativeBridge.stop()
        super.onDestroy()
    }
}

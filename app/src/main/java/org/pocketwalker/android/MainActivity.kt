package org.pocketwalker.android

import android.app.Activity
import android.content.Intent
import android.graphics.Color
import android.net.Uri
import android.os.Bundle
import android.provider.OpenableColumns
import android.view.Gravity
import android.view.ViewGroup
import android.widget.Button
import android.widget.LinearLayout
import android.widget.TextView
import androidx.activity.result.contract.ActivityResultContracts
import androidx.appcompat.app.AppCompatActivity

class MainActivity : AppCompatActivity() {

    private lateinit var status: TextView

    private val romPicker = registerForActivityResult(
        ActivityResultContracts.OpenDocument()
    ) { uri: Uri? ->
        if (uri != null) {
            loadRom(uri)
        }
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)

        val root = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            gravity = Gravity.CENTER_HORIZONTAL
            setPadding(32, 48, 32, 48)
            setBackgroundColor(Color.rgb(230, 230, 220))
        }

        val title = TextView(this).apply {
            text = "PocketWalker"
            textSize = 30f
            gravity = Gravity.CENTER
        }

        val display = TextView(this).apply {
            text = "LCD output coming next"
            textSize = 20f
            gravity = Gravity.CENTER
            setTextColor(Color.rgb(20, 35, 20))
            setBackgroundColor(Color.rgb(175, 190, 150))
            layoutParams = LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT,
                520
            ).apply {
                topMargin = 48
                bottomMargin = 32
            }
        }

        val controls = LinearLayout(this).apply {
            orientation = LinearLayout.HORIZONTAL
            gravity = Gravity.CENTER
        }

        listOf("◀", "●", "▶").forEach { label ->
            controls.addView(Button(this).apply {
                text = label
                textSize = 24f
                layoutParams = LinearLayout.LayoutParams(0, 150, 1f).apply {
                    marginStart = 8
                    marginEnd = 8
                }
            })
        }

        val chooseRom = Button(this).apply {
            text = "Choose PokéWalker ROM"
            setOnClickListener {
                romPicker.launch(arrayOf("application/octet-stream", "*/*"))
            }
        }

        status = TextView(this).apply {
            text = "No ROM loaded"
            textSize = 16f
            gravity = Gravity.CENTER
            setPadding(0, 24, 0, 0)
        }

        root.addView(title)
        root.addView(display)
        root.addView(controls)
        root.addView(chooseRom)
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
            status.text = if (ok) {
                "ROM loaded: ${displayName(uri)}"
            } else {
                "ROM must be at least 48 KiB"
            }
        } catch (t: Throwable) {
            status.text = "Load failed: ${t.message ?: t.javaClass.simpleName}"
        }
    }

    private fun displayName(uri: Uri): String {
        contentResolver.query(uri, null, null, null, null)?.use { cursor ->
            val index = cursor.getColumnIndex(OpenableColumns.DISPLAY_NAME)
            if (index >= 0 && cursor.moveToFirst()) {
                return cursor.getString(index)
            }
        }
        return uri.lastPathSegment ?: "ROM"
    }

    override fun onDestroy() {
        NativeBridge.stop()
        super.onDestroy()
    }
}

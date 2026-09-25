package org.pocketwalker.android

import android.content.Context
import android.graphics.Canvas
import android.graphics.Paint
import android.view.View

class PocketWalkerDisplay(context: Context) : View(context) {
    private val paint = Paint().apply { isAntiAlias = false }
    private val palette = intArrayOf(
        0xFFBFC9A3.toInt(),
        0xFF7F8E68.toInt(),
        0xFF44513A.toInt(),
        0xFF172018.toInt()
    )

    override fun onDraw(canvas: Canvas) {
        super.onDraw(canvas)

        val frame = NativeBridge.getFrame()
        val scale = minOf(width / 96f, height / 64f)
        val drawW = 96f * scale
        val drawH = 64f * scale
        val ox = (width - drawW) / 2f
        val oy = (height - drawH) / 2f

        paint.color = palette[0]
        canvas.drawRect(0f, 0f, width.toFloat(), height.toFloat(), paint)

        for (y in 0 until 64) {
            for (x in 0 until 96) {
                paint.color = palette[frame[y * 96 + x].toInt() and 0x03]
                val left = ox + x * scale
                val top = oy + y * scale
                canvas.drawRect(left, top, left + scale, top + scale, paint)
            }
        }

        postInvalidateDelayed(33)
    }
}

package dev.acecode.android

import android.graphics.PixelFormat
import android.graphics.Point
import android.os.Build
import android.view.LayoutInflater
import android.view.WindowInsets
import android.view.Gravity
import android.view.View
import android.view.WindowManager
import android.widget.Button
import android.widget.TextView
import org.json.JSONObject

/** A non-focusing approval leaves the observed app in front while the user answers. */
class PhoneApproval(private val activity: MainActivity, private val answer: (String, String) -> Unit) {
    private var pending: JSONObject? = null
    private var window: View? = null
    private var manager: WindowManager? = null
    private var shown = ""
    var foreground = true
        set(value) { field = value; refresh() }

    fun update(request: JSONObject?) { pending = request; refresh() }

    fun serviceChanged() { hide(); refresh() }

    private fun refresh() {
        val request = pending
        val service = PhoneAccessibility.current
        val id = request?.optString("requestId").orEmpty()
        if (foreground || id.isBlank() || service == null) { hide(); return }
        if (shown == id) return
        hide()
        val wm = service.getSystemService(WindowManager::class.java)
        val label = if (request!!.optString("tool") == "mcp_phone_tap") {
            val args = request.optJSONObject("args")
            val name = service.targetLabel(args?.optString("screen"), args?.optInt("id") ?: 0)
            activity.getString(R.string.approve_phone_tap, name)
        } else activity.getString(R.string.approve_phone_open, request.optJSONObject("args")?.optString("app").orEmpty().take(80))
        val bar = LayoutInflater.from(activity).inflate(R.layout.phone_approval, null)
        bar.findViewById<TextView>(R.id.approval_label).text = label
        fun bindButton(viewId: Int, choice: String) {
            bar.findViewById<Button>(viewId).setOnClickListener {
                if (pending?.optString("requestId") == id) { update(null); answer(id, choice) }
            }
        }
        bindButton(R.id.approval_deny, "deny")
        bindButton(R.id.approval_allow, "allow")
        val margin = activity.resources.getDimensionPixelSize(R.dimen.ace_screen_margin)
        val maxWidth = activity.resources.getDimensionPixelSize(R.dimen.ace_card_max_width)
        // Use the overlay's display, not the background activity's possibly smaller task bounds.
        val availableWidth = if (Build.VERSION.SDK_INT >= 30) {
            val metrics = wm.currentWindowMetrics
            val insets = metrics.windowInsets.getInsetsIgnoringVisibility(
                WindowInsets.Type.systemBars() or WindowInsets.Type.displayCutout())
            metrics.bounds.width() - insets.left - insets.right
        } else {
            @Suppress("DEPRECATION")
            val size = Point().also { wm.defaultDisplay.getSize(it) }
            size.x
        }
        val layout = WindowManager.LayoutParams(
            minOf(maxWidth, (availableWidth - margin * 2).coerceAtLeast(1)),
            WindowManager.LayoutParams.WRAP_CONTENT,
            WindowManager.LayoutParams.TYPE_ACCESSIBILITY_OVERLAY,
            WindowManager.LayoutParams.FLAG_NOT_FOCUSABLE or WindowManager.LayoutParams.FLAG_NOT_TOUCH_MODAL,
            PixelFormat.TRANSLUCENT).apply {
            gravity = Gravity.TOP or Gravity.CENTER_HORIZONTAL
            y = margin
            // Keep the window inside system bars/cutouts; no fullscreen or screen-wide touch layer.
            if (Build.VERSION.SDK_INT >= 30) {
                setFitInsetsTypes(WindowInsets.Type.systemBars() or WindowInsets.Type.displayCutout())
                setFitInsetsIgnoringVisibility(true)
            }
        }
        try { wm.addView(bar, layout) } catch (_: WindowManager.BadTokenException) { return }
        window = bar; manager = wm; shown = id
    }

    private fun hide() {
        window?.let { runCatching { manager?.removeViewImmediate(it) } }
        window = null; manager = null; shown = ""
    }
}

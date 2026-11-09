package dev.acecode.android

import android.accessibilityservice.AccessibilityService
import android.app.KeyguardManager
import android.graphics.Rect
import android.view.accessibility.AccessibilityEvent
import android.view.accessibility.AccessibilityNodeInfo
import org.json.JSONArray
import org.json.JSONObject
import java.security.MessageDigest

/** The OS creates this service only after the user grants accessibility access. */
class PhoneAccessibility : AccessibilityService() {
    companion object {
        @Volatile var current: PhoneAccessibility? = null
            private set
        // System callbacks and activity attachment both run on Android's main thread.
        private val listeners = mutableSetOf<() -> Unit>()
        fun subscribe(listener: () -> Unit) { listeners.add(listener) }
        fun unsubscribe(listener: () -> Unit) { listeners.remove(listener) }
    }

    override fun onServiceConnected() {
        current = this
        listeners.toList().forEach { it() }
    }
    override fun onAccessibilityEvent(event: AccessibilityEvent?) {}
    override fun onInterrupt() {}
    override fun onDestroy() {
        if (current === this) {
            current = null
            listeners.toList().forEach { it() }
        }
        super.onDestroy()
    }

    data class Screen(val json: JSONObject, val targets: List<AccessibilityNodeInfo>)

    fun targetLabel(screen: String?, id: Int): String {
        val state = runCatching { observe().json }.getOrNull()
        if (state?.optString("screen") == screen && state != null) {
            val row = state.optJSONArray("controls")?.optJSONObject(id - 1)
            val label = row?.optString("text").orEmpty().ifBlank { row?.optString("label").orEmpty() }
            if (label.isNotBlank()) return label.take(50)
        }
        return "#$id"
    }

    fun checkUnlocked() {
        check(current === this) { "Phone control authorization was revoked." }
        check(!getSystemService(KeyguardManager::class.java).isDeviceLocked) { "Unlock the phone first" }
    }

    fun observe(): Screen {
        checkUnlocked()
        val root = rootInActiveWindow ?: error("No accessible window. Unlock the phone and try again.")
        val pkg = root.packageName?.toString().orEmpty()
        check(pkg.isNotBlank() && pkg !in setOf(packageName, "com.android.systemui", "com.android.settings") &&
            !pkg.contains("permissioncontroller") && !pkg.contains("packageinstaller")) {
            "This protected window requires user interaction."
        }
        val rows = JSONArray()
        val targets = mutableListOf<AccessibilityNodeInfo>()
        var visited = 0
        fun walk(node: AccessibilityNodeInfo, clickableParent: AccessibilityNodeInfo?, depth: Int) {
            if (!node.isVisibleToUser) return
            check(++visited <= 2400 && depth <= 60) { "Screen is too large to inspect safely." }
            check(!node.isPassword) { "A password field is present. Continue on the phone." }
            val target = if (node.isClickable && node.isEnabled) node else clickableParent
            val text = node.text?.toString().orEmpty().take(200)
            val description = node.contentDescription?.toString().orEmpty().take(200)
            if ((node.isClickable || text.isNotBlank() || description.isNotBlank()) && rows.length() < 240) {
                val bounds = Rect().also(node::getBoundsInScreen)
                val row = JSONObject().put("id", rows.length() + 1)
                    .put("role", node.className?.toString()?.substringAfterLast('.'))
                    .put("bounds", bounds.flattenToString()).put("tappable", target != null && node.isEnabled)
                if (text.isNotBlank()) row.put("text", text)
                if (description.isNotBlank()) row.put("label", description)
                if (node.isSelected || node.isChecked) row.put("selected", true)
                rows.put(row)
                targets += target ?: node
            }
            for (index in 0 until node.childCount) node.getChild(index)?.let { walk(it, target, depth + 1) }
        }
        walk(root, null, 0)
        val state = JSONObject().put("package", pkg).put("window", root.windowId).put("controls", rows)
        val hash = MessageDigest.getInstance("SHA-256").digest(state.toString().toByteArray())
        state.put("screen", android.util.Base64.encodeToString(hash, android.util.Base64.NO_WRAP))
        return Screen(state, targets)
    }

    fun tap(screen: String, id: Int) {
        val current = observe()
        check(screen == current.json.getString("screen")) { "Screen changed. Read the screen again before tapping." }
        val row = current.json.getJSONArray("controls").optJSONObject(id - 1)
        require(row != null && row.getBoolean("tappable")) { "Choose a tappable control on this screen." }
        val node = current.targets[id - 1]
        check(node.refresh() && node.isEnabled && node.isVisibleToUser && node.performAction(AccessibilityNodeInfo.ACTION_CLICK)) {
            "The control did not accept the tap. Read the screen again."
        }
    }
}

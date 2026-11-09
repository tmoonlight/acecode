package dev.acecode.android

import android.content.Context
import android.content.Intent
import android.content.SharedPreferences
import android.graphics.Bitmap
import android.graphics.BitmapFactory
import android.graphics.BitmapRegionDecoder
import android.graphics.Canvas
import android.graphics.Paint
import android.graphics.PixelFormat
import android.graphics.Rect
import android.graphics.RectF
import android.provider.Settings
import android.util.LruCache
import android.view.Gravity
import android.view.MotionEvent
import android.view.View
import android.view.ViewConfiguration
import android.view.WindowManager
import org.json.JSONArray
import org.json.JSONObject
import kotlin.math.abs
import kotlin.math.roundToInt

/** Owns the touchable pet window for exactly one foreground-service lifetime. */
class FloatingPet(private val context: Context) : SharedPreferences.OnSharedPreferenceChangeListener {
    private val prefs = context.getSharedPreferences("phone-ui", Context.MODE_PRIVATE)
    private val wm = context.getSystemService(WindowManager::class.java)
    private val size get() = (68 * context.resources.displayMetrics.density).roundToInt()
    private var pet: PetSprite? = null
    private var character = ""
    private var params: WindowManager.LayoutParams? = null

    init { prefs.registerOnSharedPreferenceChangeListener(this); refresh() }

    override fun onSharedPreferenceChanged(prefs: SharedPreferences, key: String?) { refresh() }

    @Suppress("DEPRECATION")
    private fun space(): android.graphics.Point {
        val screen = android.graphics.Point()
        wm.defaultDisplay.getSize(screen)
        return android.graphics.Point((screen.x - size).coerceAtLeast(0), (screen.y - size).coerceAtLeast(0))
    }

    fun refresh() {
        val selected = prefs.getString("pet", "off") ?: "off"
        if (selected !in KINDS || !Settings.canDrawOverlays(context) || prefs.getBoolean("activityVisible", false)) {
            hide()
            return
        }
        if (pet == null || character != selected) {
            hide()
            val sprite = PetSprite(context, selected)
            val area = space()
            val layout = WindowManager.LayoutParams(size, size, WindowManager.LayoutParams.TYPE_APPLICATION_OVERLAY,
                WindowManager.LayoutParams.FLAG_NOT_FOCUSABLE or WindowManager.LayoutParams.FLAG_NOT_TOUCH_MODAL,
                PixelFormat.TRANSLUCENT).apply {
                gravity = Gravity.TOP or Gravity.LEFT
                x = if (prefs.getBoolean("petRight", true)) area.x else 0
                y = (prefs.getFloat("petY", .45f) * area.y).roundToInt()
            }
            sprite.contentDescription = context.getString(R.string.floating_pet)
            sprite.setOnClickListener {
                context.startActivity(Intent(context, MainActivity::class.java)
                    .addFlags(Intent.FLAG_ACTIVITY_NEW_TASK or Intent.FLAG_ACTIVITY_SINGLE_TOP))
            }
            enableDrag(sprite, layout)
            try { wm.addView(sprite, layout) } catch (_: WindowManager.BadTokenException) { sprite.dispose(); return }
            pet = sprite
            params = layout
            character = selected
        }
        pet?.state(prefs.getString("petActivity", "idle") ?: "idle")
    }

    fun reposition() {
        val view = pet ?: return
        val layout = params ?: return
        val area = space()
        layout.x = if (prefs.getBoolean("petRight", true)) area.x else 0
        layout.y = (prefs.getFloat("petY", .45f) * area.y).roundToInt()
        wm.updateViewLayout(view, layout)
    }

    @Suppress("ClickableViewAccessibility")
    private fun enableDrag(view: View, layout: WindowManager.LayoutParams) {
        var downX = 0f
        var downY = 0f
        var originX = 0
        var originY = 0
        var moved = false
        val slop = ViewConfiguration.get(context).scaledTouchSlop
        view.setOnTouchListener { _, event ->
            when (event.actionMasked) {
                MotionEvent.ACTION_DOWN -> {
                    downX = event.rawX; downY = event.rawY
                    originX = layout.x; originY = layout.y; moved = false
                }
                MotionEvent.ACTION_MOVE -> {
                    val dx = event.rawX - downX
                    val dy = event.rawY - downY
                    moved = moved || abs(dx) > slop || abs(dy) > slop
                    if (moved) {
                        val area = space()
                        layout.x = (originX + dx.roundToInt()).coerceIn(0, area.x)
                        layout.y = (originY + dy.roundToInt()).coerceIn(0, area.y)
                        wm.updateViewLayout(view, layout)
                    }
                }
                MotionEvent.ACTION_UP -> if (moved) {
                    val area = space()
                    prefs.edit().putBoolean("petRight", layout.x > area.x / 2)
                        .putFloat("petY", layout.y.toFloat() / area.y.coerceAtLeast(1)).apply()
                    reposition()
                } else view.performClick()
                MotionEvent.ACTION_CANCEL -> reposition()
            }
            true
        }
    }

    private fun hide() {
        pet?.let { wm.removeViewImmediate(it); it.dispose() }
        pet = null
        params = null
    }

    fun close() { prefs.unregisterOnSharedPreferenceChangeListener(this); hide() }

    companion object { val KINDS = setOf("whale", "clippy", "rover") }
}

/** Render the original ACECode atlas and frame timing without loading a second browser engine. */
@Suppress("DEPRECATION")
private class PetSprite(context: Context, private val kind: String) : View(context) {
    private val folder = if (kind == "whale") "pets/whale" else "pets/classic/$kind"
    private val agent = context.assets.open("$folder/agent.js").bufferedReader().use {
        val js = it.readText()
        JSONObject(js.substring(js.indexOf('{'), js.lastIndexOf('}') + 1))
    }
    private val dimensions = agent.getJSONArray("framesize")
    private val frameW = dimensions.getInt(0)
    private val frameH = dimensions.getInt(1)
    private val atlas = context.assets.open("$folder/map.png").use { BitmapRegionDecoder.newInstance(it, false)!! }
    private val cache = LruCache<Int, Bitmap>(16)
    private val paint = Paint(Paint.FILTER_BITMAP_FLAG)
    private var frames = JSONArray()
    private var index = 0
    private var activity = ""
    private val advance = Runnable { index = (index + 1) % frames.length(); invalidate() }

    fun state(value: String) {
        if (activity == value) return
        activity = value
        val actions = agent.getJSONObject("animations")
        val names = when (value) {
            "working" -> listOf("working", "Processing", "Searching", "Thinking")
            "waiting" -> listOf("wait", "GetAttention", "Alert")
            "done" -> listOf("celebrate", "Congratulate", "Wave")
            else -> emptyList()
        } + listOf("idle", "Idle1_1", "Idle", "RestPose")
        val action = names.first { actions.has(it) }
        frames = actions.getJSONObject(action).getJSONArray("frames")
        index = 0
        removeCallbacks(advance)
        invalidate()
    }

    override fun onDraw(canvas: Canvas) {
        val frame = frames.optJSONObject(index) ?: return
        val images = frame.optJSONArray("images")
        val scale = minOf(width.toFloat() / frameW, height.toFloat() / frameH)
        val dx = (width - frameW * scale) / 2
        val dy = (height - frameH * scale) / 2
        val dest = RectF(dx, dy, width - dx, height - dy)
        for (i in 0 until (images?.length() ?: 0)) {
            val pos = images!!.getJSONArray(i)
            val x = pos.getInt(0)
            val y = pos.getInt(1)
            val key = y * atlas.width + x
            val image = cache.get(key) ?: atlas.decodeRegion(Rect(x, y, x + frameW, y + frameH), BitmapFactory.Options())
                ?.also { cache.put(key, it) } ?: continue
            canvas.drawBitmap(image, null, dest, paint)
        }
        removeCallbacks(advance)
        postDelayed(advance, frame.optLong("duration", 200).coerceIn(50, 10000))
    }

    fun dispose() { removeCallbacks(advance); cache.evictAll(); atlas.recycle() }
}

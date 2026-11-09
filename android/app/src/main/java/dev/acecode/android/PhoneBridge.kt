package dev.acecode.android

import android.content.Context
import android.content.Intent
import android.net.LocalServerSocket
import android.os.Process
import android.os.SystemClock
import android.provider.AlarmClock
import org.json.JSONArray
import org.json.JSONObject
import java.io.Closeable
import java.io.File
import java.util.UUID
import kotlin.concurrent.thread

/** One private, same-UID endpoint per daemon lifetime. adb is never used by the app. */
class PhoneBridge(private val context: Context, paths: RuntimePaths) : Closeable {
    private val endpoint = "ace-phone-" + UUID.randomUUID()
    private val socket = LocalServerSocket(endpoint)
    @Volatile private var closed = false

    init {
        try {
            val dir = File(paths.acecodeDir, "android").apply { mkdirs() }
            context.assets.open("phone/phone_mcp.py").use { input ->
                File(dir, "phone_mcp.py").outputStream().use(input::copyTo)
            }
            File(dir, "endpoint").writeText(endpoint)
            thread(name = "acecode-phone", isDaemon = true) { serve() }
        } catch (e: Exception) {
            socket.close()
            throw e
        }
    }

    private fun serve() {
        while (!closed) {
            val peer = try { socket.accept() } catch (_: Exception) { break }
            peer.use {
                try {
                    if (it.peerCredentials.uid != Process.myUid()) return@use
                    it.soTimeout = 2000
                    val input = java.io.DataInputStream(it.inputStream)
                    val length = input.readInt()
                    if (length !in 1..8192) return@use
                    val data = ByteArray(length).also { bytes -> input.readFully(bytes) }
                    val reply = try {
                        val request = JSONObject(String(data, Charsets.UTF_8))
                        JSONObject().put("result", call(request.getString("tool"), request.getJSONObject("args")))
                    } catch (e: Exception) {
                        JSONObject().put("error", e.message ?: "Phone action failed")
                    }
                    it.outputStream.write(reply.toString().toByteArray())
                } catch (_: java.io.IOException) { /* The model cancelled or the daemon exited. */ }
            }
        }
    }

    private fun call(tool: String, args: JSONObject): Any {
        if (tool == "status") return JSONObject().put("host", "Android").put("shell", "Ubuntu")
            .put("accessibility", PhoneAccessibility.current != null)
        val service = PhoneAccessibility.current
            ?: error("Phone control is off. In ACECode Settings > Tools > Phone control, enable the Android accessibility service.")
        service.checkUnlocked()
        val pm = context.packageManager
        return when (tool) {
            "apps" -> {
                val list = JSONArray()
                val query = args.optString("query")
                val intent = Intent(Intent.ACTION_MAIN).addCategory(Intent.CATEGORY_LAUNCHER)
                pm.queryIntentActivities(intent, 0).distinctBy { it.activityInfo.packageName }.forEach {
                    val name = it.loadLabel(pm).toString()
                    val pkg = it.activityInfo.packageName
                    if (name.contains(query, true) || pkg.contains(query, true)) list.put(JSONObject().put("name", name).put("package", pkg))
                }
                list
            }
            "open" -> {
                val app = args.getString("app")
                val intent = if (app.equals("clock", true) || app == "时钟" || app == "時鐘") {
                    // vivo protects SHOW_ALARMS with its alarm-setting permission.
                    // Resolve the system clock by capability, then open its normal launcher.
                    val alarms = Intent(AlarmClock.ACTION_SHOW_ALARMS)
                    val clockPackage = alarms.resolveActivity(pm)?.packageName
                        ?: error("No system Clock found. Use apps to find its package.")
                    pm.getLaunchIntentForPackage(clockPackage) ?: alarms
                } else pm.getLaunchIntentForPackage(app) ?: error("App not found. Use apps to find its package.")
                val pkg = intent.resolveActivity(pm)?.packageName ?: error("No installed app handles this action.")
                service.startActivity(intent.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK))
                afterAction { it.optString("package") == pkg }
            }
            "screen" -> service.observe().json
            "tap" -> {
                val before = args.getString("screen")
                service.tap(before, args.getInt("id"))
                afterAction { it.optString("screen") != before }
            }
            else -> error("Unknown phone tool")
        }
    }

    private fun afterAction(ready: (JSONObject) -> Boolean): JSONObject {
        val until = SystemClock.elapsedRealtime() + 3000
        var last: JSONObject? = null
        while (!closed && SystemClock.elapsedRealtime() < until) {
            Thread.sleep(120)
            val service = PhoneAccessibility.current ?: break
            val screen = runCatching { service.observe().json }.getOrNull() ?: continue
            last = screen
            if (ready(screen)) return screen
        }
        return JSONObject().put("actionDispatched", true)
            .put("observation", last ?: JSONObject.NULL)
            .put("message", "The transition was not verified. Read the screen before taking another action.")
    }

    override fun close() {
        closed = true
        socket.close()
    }
}

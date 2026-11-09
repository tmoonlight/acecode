package dev.acecode.android

import android.content.Context
import android.net.ConnectivityManager
import android.system.Os
import android.system.OsConstants
import android.util.Log
import java.io.File
import java.io.FileOutputStream
import java.net.HttpURLConnection
import java.net.InetAddress
import java.net.ServerSocket
import java.net.URL
import java.security.SecureRandom
import java.util.TimeZone
import java.util.concurrent.CopyOnWriteArraySet
import java.util.concurrent.TimeUnit
import kotlin.concurrent.thread

sealed class RuntimeState {
    data object Idle : RuntimeState()
    data class Installing(val fraction: Float) : RuntimeState()
    data object Starting : RuntimeState()
    data class Running(val port: Int, val token: String) : RuntimeState()
    data class Failed(val message: String) : RuntimeState()
    data object Stopped : RuntimeState()

    val busy get() = this is Installing || this is Starting || this is Running
}

/** 进程级单例:管运行环境安装、proot/daemon 进程的启停与状态广播。 */
object DaemonController {
    private const val TAG = "ACECodeRuntime"
    private const val READY_TIMEOUT_MS = 90_000L
    private const val LOG_LIMIT = 4L shl 20

    private val lock = Any()
    private val listeners = CopyOnWriteArraySet<(RuntimeState) -> Unit>()
    @Volatile private var process: Process? = null
    @Volatile private var stopping = false

    @Volatile var state: RuntimeState = RuntimeState.Idle
        private set

    fun addListener(listener: (RuntimeState) -> Unit) {
        listeners += listener
        listener(state)
    }

    fun removeListener(listener: (RuntimeState) -> Unit) {
        listeners -= listener
    }

    private fun publish(next: RuntimeState) {
        state = next
        for (l in listeners) l(next)
    }

    fun start(context: Context) {
        val app = context.applicationContext
        synchronized(lock) {
            if (state.busy) return
            stopping = false
            publish(RuntimeState.Installing(0f))
        }
        thread(name = "acecode-runtime", isDaemon = true) { run(app) }
    }

    fun stop(context: Context) {
        val app = context.applicationContext
        val p = synchronized(lock) {
            stopping = true
            process
        }
        if (p == null) {
            if (!state.busy) publish(RuntimeState.Stopped)
            return
        }
        thread(name = "acecode-runtime-stop", isDaemon = true) {
            // 先让 daemon 自己收尾(proot 会忽略 SIGTERM),超时再连同 proot 一起结束
            readDaemonPid(RuntimePaths(app))?.let { pid ->
                try { Os.kill(pid, OsConstants.SIGTERM) } catch (e: Exception) { Log.w(TAG, "SIGTERM $pid", e) }
            }
            if (!p.waitFor(8, TimeUnit.SECONDS)) p.destroyForcibly()
        }
    }

    private fun run(context: Context) {
        val paths = RuntimePaths(context)
        var phone: PhoneBridge? = null
        try {
            val installer = RuntimeInstaller(context, paths)
            installer.ensureInstalled { publish(RuntimeState.Installing(it)) }
            if (stopping) {
                publish(RuntimeState.Stopped)
                return
            }
            publish(RuntimeState.Starting)
            installer.writeNetworkFiles(dnsServers(context))
            phone = PhoneBridge(context, paths)

            val prefs = context.getSharedPreferences("runtime", Context.MODE_PRIVATE)
            val token = prefs.getString("token", null) ?: newToken().also { prefs.edit().putString("token", it).apply() }
            val port = pickPort(prefs.getInt("port", 0)).also { prefs.edit().putInt("port", it).apply() }

            AndroidFsOps.deleteTree(paths.prootTmp)
            paths.prootTmp.mkdirs()
            paths.logs.mkdirs()

            val spec = ProotCommand.Spec(
                nativeLibDir = paths.nativeLibDir.path,
                rootfs = paths.rootfs.path,
                home = paths.home.path,
                prootTmp = paths.prootTmp.path,
                port = port,
                token = token,
                timeZone = TimeZone.getDefault().id,
                staticDir = devWebDir(context, paths),
            )
            val builder = ProcessBuilder(ProotCommand.argv(spec))
                .directory(paths.files)
                .redirectErrorStream(true)
            builder.environment().putAll(ProotCommand.hostEnvironment(spec))
            val p = builder.start()
            synchronized(lock) { process = p }
            pumpLog(p, paths)

            waitUntilReady(p, port, token)
            publish(RuntimeState.Running(port, token))

            val code = p.waitFor()
            synchronized(lock) { process = null }
            publish(if (stopping) RuntimeState.Stopped else RuntimeState.Failed(exitMessage(code, paths)))
        } catch (e: Exception) {
            Log.e(TAG, "runtime failed", e)
            process?.destroyForcibly()
            synchronized(lock) { process = null }
            publish(if (stopping) RuntimeState.Stopped else RuntimeState.Failed(e.message ?: e.toString()))
        } finally {
            phone?.close()
        }
    }

    private fun waitUntilReady(p: Process, port: Int, token: String) {
        val deadline = System.currentTimeMillis() + READY_TIMEOUT_MS
        while (System.currentTimeMillis() < deadline) {
            if (stopping) throw IllegalStateException("stopped")
            if (!p.isAlive) throw IllegalStateException("ACECode exited during startup (${p.exitValue()})")
            if (healthy(port, token)) return
            Thread.sleep(300)
        }
        throw IllegalStateException("ACECode did not become ready in ${READY_TIMEOUT_MS / 1000}s")
    }

    fun healthy(port: Int, token: String): Boolean = try {
        val conn = URL("http://127.0.0.1:$port/api/health").openConnection() as HttpURLConnection
        conn.connectTimeout = 1000
        conn.readTimeout = 2000
        conn.setRequestProperty("X-ACECode-Token", token)
        try { conn.responseCode == 200 } finally { conn.disconnect() }
    } catch (_: Exception) {
        false
    }

    private fun pumpLog(p: Process, paths: RuntimePaths) {
        val log = paths.daemonLog
        if (log.exists()) log.renameTo(File(paths.logs, "daemon.prev.log"))
        thread(name = "acecode-runtime-log", isDaemon = true) {
            var out = FileOutputStream(log, true)
            var written = 0L
            try {
                val buf = ByteArray(8192)
                val input = p.inputStream
                while (true) {
                    val n = input.read(buf)
                    if (n < 0) break
                    if (written > LOG_LIMIT) {
                        out.close()
                        log.renameTo(File(paths.logs, "daemon.prev.log"))
                        out = FileOutputStream(log, false)
                        written = 0
                    }
                    out.write(buf, 0, n)
                    out.flush()
                    written += n
                }
            } catch (_: Exception) {
            } finally {
                out.close()
            }
        }
    }

    private fun exitMessage(code: Int, paths: RuntimePaths): String {
        val tail = try { paths.daemonLog.readLines().takeLast(12).joinToString("\n") } catch (_: Exception) { "" }
        return "ACECode exited ($code)" + if (tail.isNotBlank()) "\n\n$tail" else ""
    }

    /** 调试包且 home/.acecode/dev-web/index.html 存在时,daemon 改用这份前端(见 android/README.md)。 */
    private fun devWebDir(context: Context, paths: RuntimePaths): String? {
        val debuggable = context.applicationInfo.flags and android.content.pm.ApplicationInfo.FLAG_DEBUGGABLE != 0
        if (!debuggable || !File(paths.home, ".acecode/dev-web/index.html").isFile) return null
        return "/root/.acecode/dev-web"
    }

    private fun readDaemonPid(paths: RuntimePaths): Int? =
        try { paths.daemonPidFile.readText().trim().toInt() } catch (_: Exception) { null }

    private fun dnsServers(context: Context): List<String> {
        val found = try {
            val cm = context.getSystemService(ConnectivityManager::class.java)
            cm.getLinkProperties(cm.activeNetwork)?.dnsServers.orEmpty()
                .mapNotNull { it.hostAddress?.substringBefore('%') }
        } catch (_: Exception) {
            emptyList()
        }
        // glibc 最多用 3 个;设备给的放前面,再补公共 DNS 兜底
        return (found.take(2) + listOf("1.1.1.1", "223.5.5.5", "8.8.8.8")).distinct().take(3)
    }

    private fun pickPort(saved: Int): Int {
        val loopback = InetAddress.getByName("127.0.0.1")
        if (saved in 1024..65535) {
            try { ServerSocket(saved, 1, loopback).close(); return saved } catch (_: Exception) {}
        }
        ServerSocket(0, 1, loopback).use { return it.localPort }
    }

    private fun newToken(): String {
        val bytes = ByteArray(32)
        SecureRandom().nextBytes(bytes)
        return bytes.joinToString("") { "%02x".format(it) }
    }
}

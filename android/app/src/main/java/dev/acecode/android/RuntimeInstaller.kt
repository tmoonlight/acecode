package dev.acecode.android

import android.content.Context
import org.json.JSONObject
import java.io.BufferedInputStream
import java.io.File
import java.io.FilterInputStream
import java.io.InputStream
import java.util.zip.GZIPInputStream

/**
 * 把 APK 里的运行环境解到 App 私有目录。
 *
 * 两份归档分开记版本:Ubuntu 小系统很少变,acecode 每个版本都变;
 * 只升级 acecode 时不用重新解整个小系统。
 */
class RuntimeInstaller(private val context: Context, private val paths: RuntimePaths) {

    data class Manifest(val rootfs: String, val acecode: String, val acecodeVersion: String)

    fun manifest(): Manifest {
        val json = context.assets.open("$ASSET_DIR/manifest.json").use { JSONObject(it.readBytes().decodeToString()) }
        return Manifest(json.getString("rootfs"), json.getString("acecode"), json.optString("acecodeVersion"))
    }

    fun ensureInstalled(onProgress: (Float) -> Unit) {
        val m = manifest()
        val state = readState()
        val needRootfs = state.optString("rootfs") != m.rootfs || !File(paths.rootfs, "usr/bin/env").exists()
        val needAcecode = needRootfs || state.optString("acecode") != m.acecode ||
            !File(paths.acecodeDir, "acecode").exists()

        if (needRootfs || needAcecode) {
            val total = (if (needRootfs) assetLength(ROOTFS) else 0L) + (if (needAcecode) assetLength(ACECODE) else 0L)
            val progress = Progress(total, onProgress)

            if (needRootfs) {
                AndroidFsOps.deleteTree(paths.rootfsStaging)
                extract(ROOTFS, paths.rootfsStaging, progress)
                AndroidFsOps.deleteTree(paths.rootfs)
                if (!paths.rootfsStaging.renameTo(paths.rootfs)) error("cannot move runtime into place")
                writeState(rootfs = m.rootfs, acecode = "")
            }
            if (needAcecode) {
                AndroidFsOps.deleteTree(paths.acecodeDir)
                extract(ACECODE, paths.acecodeDir, progress)
                writeState(rootfs = m.rootfs, acecode = m.acecode)
            }
        }
        prepareHome()
        onProgress(1f)
    }

    /** 每次启动都重写:网络会变,/etc/hosts 在导出的镜像里可能是空的。 */
    fun writeNetworkFiles(dnsServers: List<String>) {
        val etc = File(paths.rootfs, "etc")
        File(etc, "resolv.conf").apply { delete() }.writeText(dnsServers.joinToString("") { "nameserver $it\n" })
        File(etc, "hosts").apply { delete() }.writeText(
            "127.0.0.1 localhost\n::1 localhost ip6-localhost ip6-loopback\n"
        )
        registerAndroidGroups(File(etc, "group"))
    }

    /**
     * App 进程带着安卓的附加组(inet 3003 等),Ubuntu 的 /etc/group 不认识,
     * 每开一个终端 bash 都会打印 "groups: cannot find name for group ID ..."。补上占位名即可。
     */
    private fun registerAndroidGroups(groupFile: File) {
        val gids = try {
            File("/proc/self/status").readLines()
                .firstOrNull { it.startsWith("Groups:") }
                ?.substringAfter(':')?.trim()?.split(Regex("\\s+"))
                ?.mapNotNull { it.toIntOrNull() }.orEmpty()
        } catch (_: Exception) {
            emptyList()
        }
        val existing = try { groupFile.readLines() } catch (_: Exception) { emptyList() }
        val known = existing.mapNotNull { it.split(':').getOrNull(2)?.toIntOrNull() }.toSet()
        val missing = gids.filter { it !in known }.distinct()
        if (missing.isNotEmpty()) groupFile.appendText(missing.joinToString("") { "aid_$it:x:$it:\n" })
    }

    private fun prepareHome() {
        File(paths.home, "projects").mkdirs()
        for (name in listOf(".bashrc", ".profile")) {
            val dst = File(paths.home, name)
            val src = File(paths.rootfs, "etc/skel/$name")
            if (!dst.exists() && src.isFile) src.copyTo(dst)
        }
    }

    private fun extract(asset: String, dest: File, progress: Progress) {
        context.assets.open("$ASSET_DIR/$asset").use { raw ->
            val counting = CountingInputStream(BufferedInputStream(raw, 1 shl 16), progress)
            GZIPInputStream(counting, 1 shl 16).use { tar ->
                TarExtractor(AndroidFsOps).extract(tar, dest) { progress.report() }
            }
        }
    }

    private fun assetLength(asset: String): Long =
        try { context.assets.openFd("$ASSET_DIR/$asset").use { it.length } } catch (_: Exception) { 0L }

    private fun readState(): JSONObject =
        try { JSONObject(paths.state.readText()) } catch (_: Exception) { JSONObject() }

    private fun writeState(rootfs: String, acecode: String) {
        val tmp = File(paths.state.path + ".tmp")
        tmp.writeText(JSONObject().put("rootfs", rootfs).put("acecode", acecode).toString())
        if (!tmp.renameTo(paths.state)) error("cannot write ${paths.state}")
    }

    private class Progress(private val total: Long, private val sink: (Float) -> Unit) {
        var done = 0L
        private var lastReported = -1

        fun report() {
            if (total <= 0) return
            val permille = (done * 1000 / total).toInt().coerceIn(0, 999)
            if (permille != lastReported) {
                lastReported = permille
                sink(permille / 1000f)
            }
        }
    }

    private class CountingInputStream(input: InputStream, private val progress: Progress) : FilterInputStream(input) {
        override fun read(): Int = super.read().also { if (it >= 0) progress.done++ }
        override fun read(b: ByteArray, off: Int, len: Int): Int =
            super.read(b, off, len).also { if (it > 0) progress.done += it }
    }

    companion object {
        const val ASSET_DIR = "runtime"
        const val ROOTFS = "rootfs.tgz"
        const val ACECODE = "acecode.tgz"
    }
}

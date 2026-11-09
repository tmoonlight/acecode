package dev.acecode.android

import android.content.Context
import java.io.File

/**
 * App 私有目录里的运行环境布局。
 *
 * 用户数据(home)放在 Ubuntu 小系统外面,再 bind 到 /root:
 * 升级 App 换新的小系统时不会丢会话、配置和项目。
 */
class RuntimePaths(context: Context) {
    val files: File = context.filesDir
    val rootfs = File(files, "rootfs")
    val rootfsStaging = File(files, "rootfs.new")
    val home = File(files, "home")
    val logs = File(files, "logs")
    val state = File(files, "runtime-state.json")
    val prootTmp = File(context.cacheDir, "proot-tmp")
    val nativeLibDir = File(context.applicationInfo.nativeLibraryDir)

    val acecodeDir get() = File(rootfs, "opt/acecode")
    val daemonLog get() = File(logs, "daemon.log")
    val daemonPidFile get() = File(home, ".acecode/run/daemon.pid")
}

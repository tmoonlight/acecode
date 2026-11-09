package dev.acecode.android

/**
 * 拼出「用 proot 进入 Ubuntu 小系统并启动 acecode daemon」的命令行和环境变量。
 * 纯函数,不碰 Android API,便于单测。
 */
object ProotCommand {

    data class Spec(
        val nativeLibDir: String,
        val rootfs: String,
        val home: String,
        val prootTmp: String,
        val port: Int,
        val token: String,
        val timeZone: String,
        val workDir: String = "/root/projects",
        /** 仅调试包:用推进 home 里的前端构建替换内嵌前端,改前端不必重编 Linux 包。 */
        val staticDir: String? = null,
    )

    const val GUEST_ACECODE = "/opt/acecode/acecode"
    const val GUEST_PATH = "/opt/acecode:/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin"

    fun argv(spec: Spec): List<String> = buildList {
        add("${spec.nativeLibDir}/libproot.so")
        add("--kill-on-exit")
        add("--link2symlink")
        add("-0")
        add("-r"); add(spec.rootfs)
        for (bind in binds(spec)) { add("-b"); add(bind) }
        add("-w"); add(spec.workDir)
        add("/usr/bin/env"); add("-i")
        addAll(guestEnvironment(spec).map { (k, v) -> "$k=$v" })
        add(GUEST_ACECODE)
        add("daemon")
        add("--foreground")
        add("--port=${spec.port}")
        add("--token=${spec.token}")
        // 安卓上所有 App 共用回环地址,本机请求也必须带 token
        add("--require-local-token")
        spec.staticDir?.let { add("--static-dir=$it") }
        add("--cwd=${spec.workDir}")
    }

    /** proot 自身的环境(宿主侧):库目录、loader 位置、临时目录。 */
    fun hostEnvironment(spec: Spec): Map<String, String> = linkedMapOf(
        "LD_LIBRARY_PATH" to spec.nativeLibDir,
        "PROOT_LOADER" to "${spec.nativeLibDir}/libproot-loader.so",
        "PROOT_TMP_DIR" to spec.prootTmp,
    )

    fun binds(spec: Spec): List<String> = listOf(
        "/dev",
        "/proc",
        "/sys",
        "/dev/urandom:/dev/random",
        "/proc/self/fd:/dev/fd",
        "${spec.rootfs}/tmp:/dev/shm",
        "${spec.home}:/root",
    )

    /** 小系统里看到的环境:清掉安卓的变量,只给 Linux 程序需要的。 */
    fun guestEnvironment(spec: Spec): Map<String, String> = linkedMapOf(
        "ACECODE_ANDROID_HOST" to "1",
        "HOME" to "/root",
        "USER" to "root",
        "LOGNAME" to "root",
        "SHELL" to "/bin/bash",
        "PATH" to GUEST_PATH,
        "LANG" to "C.UTF-8",
        "TERM" to "xterm-256color",
        "COLORTERM" to "truecolor",
        "TMPDIR" to "/tmp",
        "TZ" to spec.timeZone,
    )
}

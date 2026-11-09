package dev.acecode.android

import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Test

class ProotCommandTest {

    private val spec = ProotCommand.Spec(
        nativeLibDir = "/data/app/x/lib/arm64",
        rootfs = "/data/user/0/dev.acecode.android/files/rootfs",
        home = "/data/user/0/dev.acecode.android/files/home",
        prootTmp = "/data/user/0/dev.acecode.android/cache/proot-tmp",
        port = 23456,
        token = "tok",
        timeZone = "Asia/Taipei",
    )

    // 场景: App 启动 daemon。
    // 期望: 必带 --require-local-token —— 安卓所有 App 共用 127.0.0.1,不带的话
    //       别的 App 不需要 token 就能读到模型密钥、让 agent 执行命令。
    // 回归: 2026-11-07 模拟器验证时 /api/models 对本机请求明文返回了 api_key。
    @Test
    fun daemonAlwaysRequiresLocalToken() {
        val argv = ProotCommand.argv(spec)
        val daemonArgs = argv.subList(argv.indexOf(ProotCommand.GUEST_ACECODE), argv.size)
        assertEquals(
            listOf(
                ProotCommand.GUEST_ACECODE, "daemon", "--foreground",
                "--port=23456", "--token=tok", "--require-local-token", "--cwd=/root/projects",
            ),
            daemonArgs,
        )
    }

    // 场景: 调试包里推了一份前端构建到 home/.acecode/dev-web。
    // 期望: daemon 多带 --static-dir 指向小系统里的路径;没推时(正式包)命令行与上面完全一致。
    @Test
    fun devWebDirAddsStaticDirOnlyWhenPresent() {
        val argv = ProotCommand.argv(spec.copy(staticDir = "/root/.acecode/dev-web"))
        assertTrue(argv.contains("--static-dir=/root/.acecode/dev-web"))
        assertTrue(ProotCommand.argv(spec).none { it.startsWith("--static-dir") })
    }

    // 场景: 升级 App 会整个替换 Ubuntu 小系统。
    // 期望: 用户目录在小系统外面,通过 -b 挂到 /root,替换小系统不丢会话和项目。
    @Test
    fun homeLivesOutsideRootfs() {
        val argv = ProotCommand.argv(spec)
        val bindValues = argv.withIndex().filter { (i, _) -> i > 0 && argv[i - 1] == "-b" }.map { it.value }
        assertTrue(bindValues.contains("${spec.home}:/root"))
        assertEquals(spec.rootfs, argv[argv.indexOf("-r") + 1])
    }

    // 场景: proot 进入小系统后执行 env -i 清空安卓的环境变量。
    // 期望: 小系统里只看到 Linux 程序需要的变量;时区跟随手机设置。
    //       proot 自己的库路径 / loader / 临时目录只出现在宿主侧环境里。
    @Test
    fun guestAndHostEnvironmentsAreSeparate() {
        val argv = ProotCommand.argv(spec)
        val envStart = argv.indexOf("-i") + 1
        val guestVars = argv.subList(envStart, argv.indexOf(ProotCommand.GUEST_ACECODE))
        assertTrue(guestVars.contains("HOME=/root"))
        assertTrue(guestVars.contains("ACECODE_ANDROID_HOST=1"))
        assertTrue(guestVars.contains("TZ=Asia/Taipei"))
        assertTrue(guestVars.none { it.startsWith("LD_LIBRARY_PATH=") || it.startsWith("PROOT_") })

        val host = ProotCommand.hostEnvironment(spec)
        assertEquals(spec.nativeLibDir, host["LD_LIBRARY_PATH"])
        assertEquals("${spec.nativeLibDir}/libproot-loader.so", host["PROOT_LOADER"])
        assertEquals(spec.prootTmp, host["PROOT_TMP_DIR"])
        assertEquals("${spec.nativeLibDir}/libproot.so", argv.first())
    }
}

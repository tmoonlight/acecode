package dev.acecode.android

import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Assert.fail
import org.junit.Test
import java.io.ByteArrayInputStream
import java.io.ByteArrayOutputStream
import java.io.File
import java.io.IOException
import java.io.InputStream

class TarExtractorTest {

    /** 不碰真实磁盘的假文件系统:记录每个操作,文件内容放内存里。 */
    private class FakeFs : FsOps {
        val files = HashMap<String, ByteArray>()
        val dirs = HashSet<String>()
        val links = HashMap<String, String>()
        val modes = HashMap<String, Int>()
        /** 模拟「某个目录其实是指向外部的符号链接」。 */
        val canonicalOverrides = HashMap<String, String>()

        override fun mkdirs(dir: File) { dirs += dir.path }
        override fun writeFile(file: File, input: InputStream) { files[file.path] = input.readBytes() }
        override fun copyFile(from: File, to: File) {
            files[to.path] = files[from.path] ?: throw IOException("missing ${from.path}")
        }
        override fun symlink(link: File, target: String) { links[link.path] = target }
        override fun chmod(path: File, mode: Int) { modes[path.path] = mode }
        override fun deleteIfExists(path: File) { files -= path.path; links -= path.path }
        override fun canonical(path: File): File = File(canonicalOverrides[path.path] ?: path.path)
    }

    private val root = File("/r")

    // 场景: 解 Ubuntu 小系统,里面同时有目录、普通文件、符号链接、硬链接。
    // 期望: 文件内容和权限位照搬;符号链接原样保留目标;
    //       硬链接复制成独立文件(安卓禁止普通进程建硬链接,PoC 里 tar 就因此报过错)。
    @Test
    fun extractsFilesDirsSymlinksAndHardLinks() {
        val tar = TarBuilder()
            .dir("usr/")
            .dir("usr/bin/")
            .file("usr/bin/perl", "PERL".toByteArray(), mode = 0x1ed) // 0755
            .hardLink("usr/bin/perl5.38.2", "usr/bin/perl")
            .symlink("bin", "usr/bin")
            .build()
        val fs = FakeFs()

        val count = TarExtractor(fs).extract(ByteArrayInputStream(tar), root)

        assertEquals(5, count)
        assertEquals("PERL", String(fs.files[File(root, "usr/bin/perl").path]!!))
        assertEquals("PERL", String(fs.files[File(root, "usr/bin/perl5.38.2").path]!!))
        assertEquals("usr/bin", fs.links[File(root, "bin").path])
        assertEquals(0x1ed, fs.modes[File(root, "usr/bin/perl").path])
    }

    // 场景: Docker 导出的 tar 对超过 100 字节的路径用 pax 扩展头记录完整路径。
    // 期望: 用 pax 里的 path 覆盖 ustar 头里被截断的名字,且只作用于紧随其后的那一个条目。
    @Test
    fun paxPathOverridesTruncatedName() {
        val longName = "usr/share/" + "a".repeat(120) + "/file.txt"
        val tar = TarBuilder()
            .pax(mapOf("path" to longName))
            .file("truncated-name", "X".toByteArray())
            .file("next.txt", "Y".toByteArray())
            .build()
        val fs = FakeFs()

        TarExtractor(fs).extract(ByteArrayInputStream(tar), root)

        assertEquals("X", String(fs.files[File(root, longName).path]!!))
        assertEquals("Y", String(fs.files[File(root, "next.txt").path]!!))
        assertNull(fs.files[File(root, "truncated-name").path])
    }

    // 场景: 归档里有 ../ 条目(被篡改或打包脚本出错)。
    // 期望: 直接报错,绝不写到解包目录外面。
    @Test
    fun rejectsParentTraversal() {
        val tar = TarBuilder().file("../evil", "E".toByteArray()).build()
        try {
            TarExtractor(FakeFs()).extract(ByteArrayInputStream(tar), root)
            fail("expected IOException")
        } catch (e: IOException) {
            assertTrue(e.message!!.contains(".."))
        }
    }

    // 场景: 前面的条目把某个目录做成了指向外部的符号链接,后面的条目往这个目录里写文件。
    // 期望: 写入前按真实路径检查,发现逃出解包目录就报错。
    @Test
    fun rejectsWritesThroughEscapingSymlinkDir() {
        val tar = TarBuilder().file("etc/passwd", "P".toByteArray()).build()
        val fs = FakeFs().apply { canonicalOverrides[File(root, "etc").path] = "/system/etc" }
        try {
            TarExtractor(fs).extract(ByteArrayInputStream(tar), root)
            fail("expected IOException")
        } catch (e: IOException) {
            assertTrue(e.message!!.contains("escapes"))
        }
    }

    // 场景: 解析 tar 头里的数字字段。
    // 期望: 八进制(前导零、空格、\0 结尾)和 GNU base-256(超大文件)都能读对。
    @Test
    fun parsesOctalAndBase256Numbers() {
        val octal = ByteArray(12).also { "00000001750\u0000".toByteArray().copyInto(it) }
        assertEquals(1000L, TarExtractor.parseNumber(octal, 0, 12))

        val base256 = ByteArray(12).also { it[0] = 0x80.toByte(); it[10] = 0x01; it[11] = 0x00 }
        assertEquals(256L, TarExtractor.parseNumber(base256, 0, 12))
    }

    /** 最小的 ustar 写入器,只为构造测试数据。 */
    private class TarBuilder {
        private val out = ByteArrayOutputStream()

        fun dir(name: String) = entry(name, '5', ByteArray(0), 0x1ed, "")
        fun file(name: String, data: ByteArray, mode: Int = 0x1a4) = entry(name, '0', data, mode, "")
        fun symlink(name: String, target: String) = entry(name, '2', ByteArray(0), 0x1ff, target)
        fun hardLink(name: String, target: String) = entry(name, '1', ByteArray(0), 0x1a4, target)
        fun pax(records: Map<String, String>): TarBuilder {
            val body = ByteArrayOutputStream()
            for ((k, v) in records) {
                val rest = " $k=$v\n"
                var len = rest.toByteArray().size + 1
                while ((len.toString() + rest).toByteArray().size != len) len++
                body.write((len.toString() + rest).toByteArray())
            }
            return entry("PaxHeaders/x", 'x', body.toByteArray(), 0x1a4, "")
        }

        fun build(): ByteArray {
            out.write(ByteArray(1024))
            return out.toByteArray()
        }

        private fun entry(name: String, type: Char, data: ByteArray, mode: Int, link: String): TarBuilder {
            val h = ByteArray(512)
            put(h, 0, name.take(100))
            put(h, 100, "%07o".format(mode))
            put(h, 108, "0000000")
            put(h, 116, "0000000")
            put(h, 124, "%011o".format(data.size))
            put(h, 136, "00000000000")
            h[156] = type.code.toByte()
            put(h, 157, link)
            put(h, 257, "ustar")
            put(h, 263, "00")
            for (i in 148 until 156) h[i] = ' '.code.toByte()
            put(h, 148, "%06o".format(h.sumOf { it.toInt() and 0xff }) + "\u0000 ")
            out.write(h)
            out.write(data)
            out.write(ByteArray(((512 - data.size % 512) % 512)))
            return this
        }

        private fun put(h: ByteArray, off: Int, s: String) = s.toByteArray().copyInto(h, off)
    }
}

package dev.acecode.android

import java.io.EOFException
import java.io.File
import java.io.FilterInputStream
import java.io.IOException
import java.io.InputStream

/** 解包时用到的文件系统操作;真实实现见 [AndroidFsOps],单测用假实现。 */
interface FsOps {
    fun mkdirs(dir: File)
    fun writeFile(file: File, input: InputStream)
    fun copyFile(from: File, to: File)
    fun symlink(link: File, target: String)
    fun chmod(path: File, mode: Int)
    /** 删除路径本身;是符号链接时只删链接。 */
    fun deleteIfExists(path: File)
    /** 解析符号链接后的绝对路径,用来确认写入点没有逃出解包根目录。 */
    fun canonical(path: File): File
}

/**
 * 流式解 tar(ustar / pax / GNU 长文件名)。
 *
 * 安卓不允许普通进程建硬链接,硬链接一律复制成独立文件;
 * 设备文件、FIFO 直接跳过(proot 会把宿主的 /dev bind 进来)。
 */
class TarExtractor(private val fs: FsOps) {

    fun extract(input: InputStream, root: File, onEntry: (() -> Unit)? = null): Int {
        fs.mkdirs(root)
        val rootCanonical = fs.canonical(root).path
        val verifiedDirs = HashSet<String>()
        val header = ByteArray(BLOCK.toInt())
        var paxPath: String? = null
        var paxLink: String? = null
        var paxSize: Long? = null
        var gnuLongName: String? = null
        var gnuLongLink: String? = null
        var count = 0

        while (readBlock(input, header)) {
            if (header.all { it.toInt() == 0 }) break

            val type = header[156].toInt().toChar()
            val size = paxSize ?: parseNumber(header, 124, 12)
            when (type) {
                'x' -> {
                    val records = parsePax(readData(input, size))
                    records["path"]?.let { paxPath = it }
                    records["linkpath"]?.let { paxLink = it }
                    records["size"]?.toLongOrNull()?.let { paxSize = it }
                    continue
                }
                'g' -> { readData(input, size); continue }
                'L' -> { gnuLongName = cString(readData(input, size)); continue }
                'K' -> { gnuLongLink = cString(readData(input, size)); continue }
            }

            val name = gnuLongName ?: paxPath ?: headerName(header)
            val link = gnuLongLink ?: paxLink ?: cString(header, 157, 100)
            val mode = parseNumber(header, 100, 8).toInt()
            paxPath = null; paxLink = null; paxSize = null
            gnuLongName = null; gnuLongLink = null

            val rel = sanitize(name)
            if (rel == null) {
                skipData(input, size)
                continue
            }
            val dest = File(root, rel)
            ensureInsideRoot(dest.parentFile ?: root, rootCanonical, verifiedDirs)

            when (type) {
                '0', '\u0000', '7' -> {
                    fs.deleteIfExists(dest)
                    val body = BoundedInputStream(input, size)
                    fs.writeFile(dest, body)
                    body.drain()
                    skipPadding(input, size)
                    fs.chmod(dest, (mode and 0x1ff) or 0x180)
                }
                '1' -> {
                    val source = sanitize(link)
                        ?: throw IOException("hard link outside archive: $name -> $link")
                    fs.deleteIfExists(dest)
                    fs.copyFile(File(root, source), dest)
                    skipData(input, size)
                }
                '2' -> {
                    fs.deleteIfExists(dest)
                    fs.symlink(dest, link)
                    skipData(input, size)
                }
                '5' -> {
                    fs.mkdirs(dest)
                    fs.chmod(dest, (mode and 0x1ff) or 0x1c0)
                    skipData(input, size)
                }
                else -> skipData(input, size)
            }
            count++
            onEntry?.invoke()
        }
        return count
    }

    private fun ensureInsideRoot(dir: File, rootCanonical: String, verified: MutableSet<String>) {
        if (dir.path in verified) return
        val resolved = fs.canonical(dir).path
        if (resolved != rootCanonical && !resolved.startsWith(rootCanonical + File.separator)) {
            throw IOException("archive entry escapes the target directory: $dir -> $resolved")
        }
        fs.mkdirs(dir)
        verified += dir.path
    }

    private fun readData(input: InputStream, size: Long): ByteArray {
        if (size > MAX_META) throw IOException("tar metadata entry too large: $size")
        val data = ByteArray(size.toInt())
        readFully(input, data)
        skipPadding(input, size)
        return data
    }

    private fun skipData(input: InputStream, size: Long) {
        skipFully(input, size)
        skipPadding(input, size)
    }

    private fun skipPadding(input: InputStream, size: Long) {
        val pad = (BLOCK - size % BLOCK) % BLOCK
        skipFully(input, pad)
    }

    private class BoundedInputStream(input: InputStream, private var remaining: Long) : FilterInputStream(input) {
        override fun read(): Int {
            if (remaining <= 0) return -1
            val b = super.read()
            if (b < 0) throw EOFException("truncated tar entry")
            remaining--
            return b
        }

        override fun read(b: ByteArray, off: Int, len: Int): Int {
            if (remaining <= 0) return -1
            val n = super.read(b, off, minOf(len.toLong(), remaining).toInt())
            if (n < 0) throw EOFException("truncated tar entry")
            remaining -= n
            return n
        }

        override fun close() { /* 不关底层 tar 流 */ }

        fun drain() = skipFully(`in`, remaining).also { remaining = 0 }
    }

    companion object {
        private const val BLOCK = 512L
        private const val MAX_META = 1L shl 20

        /** 去掉开头的 ./ 和 /;含 .. 的条目一律拒绝;根目录本身返回 null。 */
        fun sanitize(path: String): String? {
            val parts = path.replace('\\', '/').split('/').filter { it.isNotEmpty() && it != "." }
            if (parts.any { it == ".." }) throw IOException("archive entry with '..': $path")
            return if (parts.isEmpty()) null else parts.joinToString("/")
        }

        fun parseNumber(header: ByteArray, offset: Int, length: Int): Long {
            if (header[offset].toInt() and 0x80 != 0) {
                // GNU base-256:首字节最高位置 1,其余按大端二进制
                var value = (header[offset].toLong() and 0x7f)
                for (i in offset + 1 until offset + length) value = (value shl 8) or (header[i].toLong() and 0xff)
                return value
            }
            var value = 0L
            for (i in offset until offset + length) {
                val c = header[i].toInt().toChar()
                if (c == '\u0000' || c == ' ') { if (value != 0L) break else continue }
                if (c !in '0'..'7') break
                value = value * 8 + (c - '0')
            }
            return value
        }

        fun parsePax(data: ByteArray): Map<String, String> {
            val out = HashMap<String, String>()
            var pos = 0
            while (pos < data.size) {
                var space = pos
                while (space < data.size && data[space] != ' '.code.toByte()) space++
                val len = String(data, pos, space - pos, Charsets.US_ASCII).toIntOrNull() ?: break
                if (len <= 0 || pos + len > data.size) break
                val record = String(data, space + 1, pos + len - space - 2, Charsets.UTF_8)
                val eq = record.indexOf('=')
                if (eq > 0) out[record.substring(0, eq)] = record.substring(eq + 1)
                pos += len
            }
            return out
        }

        private fun headerName(header: ByteArray): String {
            val name = cString(header, 0, 100)
            val posix = header[257] == 'u'.code.toByte() && header[262].toInt() == 0
            val prefix = if (posix) cString(header, 345, 155) else ""
            return if (prefix.isEmpty()) name else "$prefix/$name"
        }

        private fun cString(data: ByteArray, offset: Int = 0, length: Int = data.size): String {
            var end = offset
            while (end < offset + length && data[end].toInt() != 0) end++
            return String(data, offset, end - offset, Charsets.UTF_8)
        }

        private fun readBlock(input: InputStream, block: ByteArray): Boolean {
            var read = 0
            while (read < block.size) {
                val n = input.read(block, read, block.size - read)
                if (n < 0) {
                    if (read == 0) return false
                    throw EOFException("truncated tar header")
                }
                read += n
            }
            return true
        }

        private fun readFully(input: InputStream, data: ByteArray) {
            var read = 0
            while (read < data.size) {
                val n = input.read(data, read, data.size - read)
                if (n < 0) throw EOFException("truncated tar entry")
                read += n
            }
        }

        private fun skipFully(input: InputStream, count: Long) {
            var left = count
            val scratch = ByteArray(8192)
            while (left > 0) {
                val n = input.read(scratch, 0, minOf(left, scratch.size.toLong()).toInt())
                if (n < 0) throw EOFException("truncated tar entry")
                left -= n
            }
        }
    }
}

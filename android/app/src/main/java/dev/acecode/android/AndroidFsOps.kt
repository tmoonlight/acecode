package dev.acecode.android

import android.system.Os
import java.io.File
import java.io.IOException
import java.io.InputStream
import java.nio.file.FileVisitResult
import java.nio.file.Files
import java.nio.file.LinkOption
import java.nio.file.Path
import java.nio.file.SimpleFileVisitor
import java.nio.file.StandardCopyOption
import java.nio.file.attribute.BasicFileAttributes

object AndroidFsOps : FsOps {
    override fun mkdirs(dir: File) {
        Files.createDirectories(dir.toPath())
    }

    override fun writeFile(file: File, input: InputStream) {
        Files.copy(input, file.toPath(), StandardCopyOption.REPLACE_EXISTING)
    }

    override fun copyFile(from: File, to: File) {
        Files.copy(from.toPath(), to.toPath(), StandardCopyOption.REPLACE_EXISTING, LinkOption.NOFOLLOW_LINKS)
    }

    override fun symlink(link: File, target: String) {
        Os.symlink(target, link.path)
    }

    override fun chmod(path: File, mode: Int) {
        Os.chmod(path.path, mode)
    }

    override fun deleteIfExists(path: File) {
        val p = path.toPath()
        if (Files.isDirectory(p, LinkOption.NOFOLLOW_LINKS)) deleteTree(path) else Files.deleteIfExists(p)
    }

    override fun canonical(path: File): File = path.canonicalFile

    /** 递归删除,不跟随符号链接(小系统里有指向 / 的绝对链接)。 */
    fun deleteTree(dir: File) {
        val root = dir.toPath()
        if (!Files.exists(root, LinkOption.NOFOLLOW_LINKS)) return
        Files.walkFileTree(root, object : SimpleFileVisitor<Path>() {
            override fun preVisitDirectory(d: Path, attrs: BasicFileAttributes): FileVisitResult {
                // 只读目录里的文件删不掉,先补上写权限
                try { Os.chmod(d.toString(), 0x1c0) } catch (_: Exception) {}
                return FileVisitResult.CONTINUE
            }

            override fun visitFile(f: Path, attrs: BasicFileAttributes): FileVisitResult {
                Files.delete(f)
                return FileVisitResult.CONTINUE
            }

            override fun visitFileFailed(f: Path, exc: IOException): FileVisitResult {
                Files.deleteIfExists(f)
                return FileVisitResult.CONTINUE
            }

            override fun postVisitDirectory(d: Path, exc: IOException?): FileVisitResult {
                Files.delete(d)
                return FileVisitResult.CONTINUE
            }
        })
    }
}

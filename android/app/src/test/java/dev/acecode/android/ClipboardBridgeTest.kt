package dev.acecode.android

import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

class ClipboardBridgeTest {

    // 场景: 网页「粘贴」按钮经 aceAndroid 通道请求读剪贴板。
    // 期望: 回包带同一个 id 和剪贴板文字;引号、反斜杠、换行、控制字符都按 JSON 转义,
    //       网页 JSON.parse 后拿到的就是原文。
    // 回归: 2026-11-08 vivo 上密码框弹安全键盘无法粘贴 API key,改由 App 代读剪贴板。
    @Test
    fun readClipboardRepliesWithEscapedText() {
        val reply = ClipboardBridge.reply("""{"type":"readClipboard","id":"clip-1-ab"}""") { "sk-\"a\\b\nc\u0001" }
        assertEquals("""{"id":"clip-1-ab","ok":true,"text":"sk-\"a\\b\nc\u0001"}""", reply)
    }

    // 场景: 剪贴板为空 / 读取被系统拒绝(例如 App 不在前台)。
    // 期望: 回 ok:false 和原因,网页据此提示「剪贴板里没有文字」,不会一直等。
    @Test
    fun emptyOrFailingClipboardRepliesWithError() {
        assertEquals(
            """{"id":"x","ok":false,"error":"clipboard empty"}""",
            ClipboardBridge.reply("""{"type":"readClipboard","id":"x"}""") { null },
        )
        val failed = ClipboardBridge.reply("""{"type":"readClipboard","id":"y"}""") { throw SecurityException("denied") }
        assertEquals("""{"id":"y","ok":false,"error":"denied"}""", failed)
    }

    // 场景: 网页发来看不懂的报文(别的类型、没有 id、id 里夹着引号想注入 JSON)。
    // 期望: 一律不回包,也不去读剪贴板。
    @Test
    fun unknownOrMalformedRequestsAreIgnored() {
        var reads = 0
        val read = { reads++; "secret" }
        assertNull(ClipboardBridge.reply(null, read))
        assertNull(ClipboardBridge.reply("""{"type":"writeClipboard","id":"a"}""", read))
        assertNull(ClipboardBridge.reply("""{"type":"readClipboard"}""", read))
        assertNull(ClipboardBridge.reply("""{"type":"readClipboard","id":"a\",\"ok\":true"}""", read))
        assertNull(ClipboardBridge.reply("not json", read))
        assertEquals(0, reads)
        assertTrue(ClipboardBridge.quote(" ").contains("\\u2028"))
    }
}

package dev.acecode.android

/**
 * 网页「粘贴」按钮的读剪贴板通道(前端 web/src/lib/systemClipboard.js)。
 *
 * 安卓 WebView 不让网页读剪贴板,密码框又会弹出禁止粘贴的安全键盘,所以由 App 代读。
 * 通道只注册给 ACECode 页面自己的源(MainActivity.installClipboardBridge),并且只认主框架,
 * 对话里内嵌的可视化小框(srcdoc,源是 null)拿不到。
 *
 * 这里只做报文处理,不碰 Android API,便于在电脑上跑单测。
 */
object ClipboardBridge {
    const val OBJECT_NAME = "aceAndroid"

    private val TYPE = Regex("\"type\"\\s*:\\s*\"([^\"]*)\"")
    private val ID = Regex("\"id\"\\s*:\\s*\"([A-Za-z0-9_-]{1,64})\"")

    /** 处理网页发来的请求,返回要回给网页的 JSON;看不懂的请求返回 null(不回包)。 */
    fun reply(request: String?, readText: () -> String?): String? {
        if (request == null) return null
        if (TYPE.find(request)?.groupValues?.get(1) != "readClipboard") return null
        val id = ID.find(request)?.groupValues?.get(1) ?: return null
        val text = try {
            readText()
        } catch (e: Exception) {
            return """{"id":"$id","ok":false,"error":${quote(e.message ?: "clipboard read failed")}}"""
        }
        if (text == null) return """{"id":"$id","ok":false,"error":"clipboard empty"}"""
        return """{"id":"$id","ok":true,"text":${quote(text)}}"""
    }

    fun quote(value: String): String = buildString(value.length + 2) {
        append('"')
        for (c in value) {
            when {
                c == '"' -> append("\\\"")
                c == '\\' -> append("\\\\")
                c == '\n' -> append("\\n")
                c == '\r' -> append("\\r")
                c == '\t' -> append("\\t")
                c < ' ' || c == ' ' || c == ' ' -> append("\\u%04x".format(c.code))
                else -> append(c)
            }
        }
        append('"')
    }
}

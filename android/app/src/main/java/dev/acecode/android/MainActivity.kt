package dev.acecode.android

import android.annotation.SuppressLint
import android.app.Activity
import android.app.AlertDialog
import android.app.NotificationManager
import android.content.ActivityNotFoundException
import android.content.ClipboardManager
import android.content.Intent
import android.content.pm.ApplicationInfo
import android.content.res.Configuration
import android.net.Uri
import android.os.Build
import android.os.Bundle
import android.provider.Settings
import android.view.View
import android.webkit.ValueCallback
import android.webkit.WebChromeClient
import android.webkit.WebResourceRequest
import android.webkit.WebView
import android.webkit.WebViewClient
import android.widget.Button
import android.widget.ProgressBar
import android.widget.TextView
import org.json.JSONObject
import androidx.webkit.WebSettingsCompat
import androidx.webkit.WebViewCompat
import androidx.webkit.WebViewFeature

class MainActivity : Activity() {

    private lateinit var web: WebView
    private lateinit var statusPanel: View
    private lateinit var progress: ProgressBar
    private lateinit var statusText: TextView
    private lateinit var actionButton: Button

    /** 当前 WebView 加载的是哪一个 daemon(端口 + token),变了才重新加载。 */
    private var loadedOrigin: String? = null
    private var fileCallback: ValueCallback<Array<Uri>>? = null
    private var startedFromFocus = false
    private var clipboardBridgeOrigin: String? = null
    private val approval = PhoneApproval(this) { id, choice ->
        val data = JSONObject().put("requestId", id).put("choice", choice)
        web.evaluateJavascript("window.dispatchEvent(new CustomEvent('ace-phone-decision',{detail:$data}))", null)
    }

    private val listener: (RuntimeState) -> Unit = { state -> runOnUiThread { render(state) } }
    private val accessibilityChanged: () -> Unit = {
        approval.serviceChanged()
        web.evaluateJavascript("window.dispatchEvent(new Event('ace-phone-resume'))", null)
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        setContentView(R.layout.activity_main)
        web = findViewById(R.id.web)
        statusPanel = findViewById(R.id.status_panel)
        progress = findViewById(R.id.progress)
        statusText = findViewById(R.id.status_text)
        actionButton = findViewById(R.id.action_button)
        actionButton.setOnClickListener { RuntimeService.start(this) }

        setupWebView()
        PhoneAccessibility.subscribe(accessibilityChanged)
    }

    override fun onWindowFocusChanged(hasFocus: Boolean) {
        super.onWindowFocusChanged(hasFocus)
        if (hasFocus && ::web.isInitialized) web.evaluateJavascript("window.dispatchEvent(new Event('ace-phone-resume'))", null)
        if (!hasFocus || startedFromFocus) return
        startedFromFocus = true
        // 等界面真正显示后再建通知渠道、起服务:targetSdk 28 的 App 在安卓 13+ 上,
        // 系统只在「前台可见的界面建渠道」时才弹通知授权框。
        RuntimeService.ensureChannel(this)
        if (!DaemonController.state.busy) RuntimeService.start(this)
    }

    override fun onStart() {
        super.onStart()
        approval.foreground = true
        getSharedPreferences("phone-ui", MODE_PRIVATE).edit().putBoolean("activityVisible", true).apply()
        DaemonController.addListener(listener)
    }

    override fun onStop() {
        approval.foreground = false
        getSharedPreferences("phone-ui", MODE_PRIVATE).edit().putBoolean("activityVisible", false).apply()
        DaemonController.removeListener(listener)
        super.onStop()
    }

    override fun onDestroy() {
        PhoneAccessibility.unsubscribe(accessibilityChanged)
        approval.update(null)
        fileCallback?.onReceiveValue(null)
        fileCallback = null
        web.destroy()
        super.onDestroy()
    }

    override fun onConfigurationChanged(newConfig: Configuration) {
        super.onConfigurationChanged(newConfig)
        theme.applyStyle(R.style.Theme_ACECode, true)
        // Refresh only the native status panel; keep the running WebView and bridges intact.
        val parent = statusPanel.parent as android.view.ViewGroup
        val index = parent.indexOfChild(statusPanel)
        parent.removeView(statusPanel)
        statusPanel = layoutInflater.inflate(R.layout.status_panel, parent, false)
        parent.addView(statusPanel, index)
        progress = statusPanel.findViewById(R.id.progress)
        statusText = statusPanel.findViewById(R.id.status_text)
        actionButton = statusPanel.findViewById(R.id.action_button)
        actionButton.setOnClickListener { RuntimeService.start(this) }
        parent.setBackgroundColor(getColor(R.color.ace_bg))
        window.statusBarColor = getColor(R.color.ace_bg)
        window.navigationBarColor = getColor(R.color.ace_bg)
        val lightBars = resources.getBoolean(R.bool.ace_light_bars)
        val barFlags = View.SYSTEM_UI_FLAG_LIGHT_STATUS_BAR or
            (if (Build.VERSION.SDK_INT >= 26) View.SYSTEM_UI_FLAG_LIGHT_NAVIGATION_BAR else 0)
        window.decorView.systemUiVisibility = (window.decorView.systemUiVisibility and barFlags.inv()) or
            (if (lightBars) barFlags else 0)
        render(DaemonController.state)
        approval.serviceChanged()
        applyDarkMode()
    }

    /**
     * 返回键先交给网页:设置、对话框、侧栏浮层、菜单还开着就关最上面那一层
     * (前端 lib/mobileShell.js 的 __aceHandleBack);都关完了才把 App 退到后台,daemon 继续跑。
     */
    @Deprecated("Activity#onBackPressed is fine for targetSdk 28")
    override fun onBackPressed() {
        if (web.visibility != View.VISIBLE) {
            moveTaskToBack(true)
            return
        }
        web.evaluateJavascript("(window.__aceHandleBack ? window.__aceHandleBack() : false)") { handled ->
            if (handled == "true") return@evaluateJavascript
            if (web.canGoBack()) web.goBack() else moveTaskToBack(true)
        }
    }

    private fun render(state: RuntimeState) {
        when (state) {
            is RuntimeState.Running -> {
                val origin = "http://127.0.0.1:${state.port}"
                if (loadedOrigin != "$origin#${state.token}") {
                    loadedOrigin = "$origin#${state.token}"
                    installClipboardBridge(origin)
                    // 前端从 ?token= 读出后存进 sessionStorage 并从地址栏去掉
                    web.loadUrl("$origin/?token=${state.token}")
                }
                web.visibility = View.VISIBLE
                statusPanel.visibility = View.GONE
                askNotificationsOnce()
                return
            }
            is RuntimeState.Installing -> {
                showStatus(getString(R.string.status_preparing), null)
                progress.isIndeterminate = state.fraction <= 0f
                progress.progress = (state.fraction * 1000).toInt()
            }
            is RuntimeState.Idle, is RuntimeState.Starting -> {
                showStatus(getString(R.string.status_starting), null)
                progress.isIndeterminate = true
            }
            is RuntimeState.Failed -> showStatus(state.message, getString(R.string.action_retry))
            is RuntimeState.Stopped -> showStatus(getString(R.string.status_stopped), getString(R.string.action_start))
        }
    }

    private fun showStatus(text: String, action: String?) {
        web.visibility = View.GONE
        statusPanel.visibility = View.VISIBLE
        statusText.text = text
        progress.visibility = if (action == null) View.VISIBLE else View.GONE
        actionButton.visibility = if (action == null) View.GONE else View.VISIBLE
        if (action != null) {
            actionButton.text = action
            loadedOrigin = null
        }
    }

    @SuppressLint("SetJavaScriptEnabled")
    private fun setupWebView() {
        if (applicationInfo.flags and ApplicationInfo.FLAG_DEBUGGABLE != 0) WebView.setWebContentsDebuggingEnabled(true)
        with(web.settings) {
            javaScriptEnabled = true
            domStorageEnabled = true
            databaseEnabled = true
            allowFileAccess = false
            allowContentAccess = true
            mediaPlaybackRequiresUserGesture = false
            setSupportMultipleWindows(false)
            userAgentString = "$userAgentString ACECodeAndroid/${BuildInfo.versionName(this@MainActivity)}"
        }
        applyDarkMode()
        web.webViewClient = object : WebViewClient() {
            override fun shouldOverrideUrlLoading(view: WebView, request: WebResourceRequest): Boolean {
                val url = request.url
                // 网页里内嵌框(对话内可视化等)的跳转、about:/blob:/data: 留在 WebView 里,
                // 只有主页面要去外部网址(或 mailto / tel)时才交给系统浏览器。
                if (!request.isForMainFrame) return false
                val external = when (url.scheme?.lowercase()) {
                    "http", "https" -> url.host != "127.0.0.1"
                    "mailto", "tel" -> true
                    else -> false
                }
                if (external) openExternal(url)
                return external
            }

            override fun onRenderProcessGone(view: WebView, detail: android.webkit.RenderProcessGoneDetail): Boolean {
                // 渲染进程被系统回收:整页重开,daemon 不受影响
                recreate()
                return true
            }
        }
        web.webChromeClient = object : WebChromeClient() {
            override fun onShowFileChooser(
                view: WebView,
                callback: ValueCallback<Array<Uri>>,
                params: FileChooserParams,
            ): Boolean {
                fileCallback?.onReceiveValue(null)
                fileCallback = callback
                val intent = params.createIntent().apply {
                    if (params.mode == FileChooserParams.MODE_OPEN_MULTIPLE) putExtra(Intent.EXTRA_ALLOW_MULTIPLE, true)
                }
                return try {
                    startActivityForResult(intent, REQUEST_FILES)
                    true
                } catch (_: ActivityNotFoundException) {
                    fileCallback = null
                    false
                }
            }
        }
    }

    @Deprecated("startActivityForResult is fine without AndroidX")
    override fun onActivityResult(requestCode: Int, resultCode: Int, data: Intent?) {
        if (requestCode != REQUEST_FILES) return super.onActivityResult(requestCode, resultCode, data)
        val callback = fileCallback ?: return
        fileCallback = null
        if (resultCode != RESULT_OK || data == null) return callback.onReceiveValue(null)
        val clip = data.clipData
        val uris = if (clip != null) Array(clip.itemCount) { clip.getItemAt(it).uri } else listOfNotNull(data.data).toTypedArray()
        callback.onReceiveValue(uris)
    }

    @Suppress("DEPRECATION")
    private fun applyDarkMode() {
        // targetSdk < 33:由 force dark 决定网页看到的 prefers-color-scheme;
        // 只用网页自己的深色主题,不让 WebView 算法反色。
        val night = (resources.configuration.uiMode and Configuration.UI_MODE_NIGHT_MASK) == Configuration.UI_MODE_NIGHT_YES
        if (WebViewFeature.isFeatureSupported(WebViewFeature.FORCE_DARK_STRATEGY)) {
            WebSettingsCompat.setForceDarkStrategy(web.settings, WebSettingsCompat.DARK_STRATEGY_WEB_THEME_DARKENING_ONLY)
        }
        if (WebViewFeature.isFeatureSupported(WebViewFeature.FORCE_DARK)) {
            WebSettingsCompat.setForceDark(
                web.settings,
                if (night) WebSettingsCompat.FORCE_DARK_ON else WebSettingsCompat.FORCE_DARK_OFF,
            )
        }
    }

    /**
     * 安卓 13+ 上 targetSdk 28 的 App 默认没有通知权限,常驻通知(带「停止」)会被藏起来,
     * 而系统自带的补弹授权框实测不出现。界面第一次就绪时问一次,点「以后」就不再打扰。
     */
    private fun askNotificationsOnce() {
        if (Build.VERSION.SDK_INT < 33) return
        if (getSystemService(NotificationManager::class.java).areNotificationsEnabled()) return
        val prefs = getSharedPreferences("ui", MODE_PRIVATE)
        if (prefs.getBoolean("notification_prompt_shown", false)) return
        prefs.edit().putBoolean("notification_prompt_shown", true).apply()
        AlertDialog.Builder(this)
            .setMessage(R.string.notification_prompt)
            .setPositiveButton(R.string.action_open_settings) { _, _ ->
                try {
                    startActivity(
                        Intent(Settings.ACTION_APP_NOTIFICATION_SETTINGS)
                            .putExtra(Settings.EXTRA_APP_PACKAGE, packageName)
                    )
                } catch (_: ActivityNotFoundException) {
                }
            }
            .setNegativeButton(R.string.action_later, null)
            .show()
    }

    /**
     * 「粘贴」按钮用的读剪贴板通道(见 ClipboardBridge)。只注册给当前 daemon 页面的源,
     * 只回主框架;端口变了就换一个源重新注册。
     */
    private fun installClipboardBridge(origin: String) {
        if (!WebViewFeature.isFeatureSupported(WebViewFeature.WEB_MESSAGE_LISTENER)) return
        if (clipboardBridgeOrigin == origin) return
        if (clipboardBridgeOrigin != null) {
            WebViewCompat.removeWebMessageListener(web, ClipboardBridge.OBJECT_NAME)
            WebViewCompat.removeWebMessageListener(web, "acePhone")
        }
        WebViewCompat.addWebMessageListener(web, ClipboardBridge.OBJECT_NAME, setOf(origin)) { _, message, _, isMainFrame, replyProxy ->
            if (!isMainFrame) return@addWebMessageListener
            ClipboardBridge.reply(message.data, ::readClipboardText)?.let { replyProxy.postMessage(it) }
        }
        clipboardBridgeOrigin = origin
        WebViewCompat.addWebMessageListener(web, "acePhone", setOf(origin)) { _, message, _, main, reply ->
            if (!main || message.data.orEmpty().length > 2048) return@addWebMessageListener
            val request = runCatching { JSONObject(message.data ?: "") }.getOrNull() ?: return@addWebMessageListener
            val result = try { phoneSettings(request) } catch (e: Exception) {
                JSONObject().put("error", e.localizedMessage ?: "Android settings unavailable")
            }
            reply.postMessage(result.put("id", request.optString("id")).toString())
        }
    }

    private fun phoneSettings(request: JSONObject): JSONObject {
        val prefs = getSharedPreferences("phone-ui", MODE_PRIVATE)
        when (request.getString("action")) {
            "status" -> Unit
            "approval" -> approval.update(request.optJSONObject("request"))
            "accessibility" -> {
                if (PhoneAccessibility.current != null) PhoneAccessibility.current?.disableSelf()
                else startActivity(Intent(Settings.ACTION_ACCESSIBILITY_SETTINGS))
            }
            "pet" -> {
                val kind = request.getString("kind")
                require(kind == "off" || kind in FloatingPet.KINDS)
                if (kind != "off" && !Settings.canDrawOverlays(this)) {
                    startActivity(Intent(Settings.ACTION_MANAGE_OVERLAY_PERMISSION, Uri.parse("package:$packageName")))
                }
                prefs.edit().putString("pet", kind).apply()
            }
            "activity" -> {
                val state = request.getString("state")
                require(state in setOf("idle", "working", "waiting", "done"))
                prefs.edit().putString("petActivity", state).apply()
            }
            else -> error("Unknown phone setting")
        }
        val overlay = Settings.canDrawOverlays(this)
        return JSONObject().put("accessibility", PhoneAccessibility.current != null)
            .put("pet", if (overlay) prefs.getString("pet", "off") else "off")
            .put("overlay", overlay)
    }

    private fun readClipboardText(): String? {
        val clip = getSystemService(ClipboardManager::class.java)?.primaryClip ?: return null
        if (clip.itemCount == 0) return null
        return clip.getItemAt(0).coerceToText(this)?.toString()
    }

    private fun openExternal(url: Uri) {
        try {
            startActivity(Intent(Intent.ACTION_VIEW, url).addCategory(Intent.CATEGORY_BROWSABLE))
        } catch (_: ActivityNotFoundException) {
        }
    }

    companion object {
        private const val REQUEST_FILES = 1
    }
}

object BuildInfo {
    fun versionName(activity: Activity): String =
        try { activity.packageManager.getPackageInfo(activity.packageName, 0).versionName ?: "" } catch (_: Exception) { "" }
}

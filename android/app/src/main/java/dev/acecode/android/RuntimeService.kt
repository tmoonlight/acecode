package dev.acecode.android

import android.app.Notification
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.PendingIntent
import android.app.Service
import android.content.Context
import android.content.Intent
import android.os.IBinder
import android.os.PowerManager

/**
 * 前台服务:App 切到后台时 agent 还要继续干活,
 * 系统只会放过带常驻通知的前台服务。
 */
class RuntimeService : Service() {

    private var wakeLock: PowerManager.WakeLock? = null
    private var pet: FloatingPet? = null
    private val listener: (RuntimeState) -> Unit = { state ->
        if (state is RuntimeState.Stopped || state is RuntimeState.Failed) shutdown()
    }

    override fun onBind(intent: Intent?): IBinder? = null

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        if (intent?.action == ACTION_STOP) {
            DaemonController.stop(this)
            shutdown()
            return START_NOT_STICKY
        }
        startForeground(NOTIFICATION_ID, notification())
        if (pet == null) pet = FloatingPet(this)
        acquireWakeLock()
        DaemonController.removeListener(listener)
        DaemonController.start(this)
        DaemonController.addListener(listener)
        return START_STICKY
    }

    override fun onDestroy() {
        pet?.close()
        pet = null
        DaemonController.removeListener(listener)
        releaseWakeLock()
        if (DaemonController.state.busy) DaemonController.stop(this)
        super.onDestroy()
    }

    override fun onConfigurationChanged(newConfig: android.content.res.Configuration) {
        super.onConfigurationChanged(newConfig)
        pet?.reposition()
    }

    private fun shutdown() {
        DaemonController.removeListener(listener)
        releaseWakeLock()
        stopForeground(STOP_FOREGROUND_REMOVE)
        stopSelf()
    }

    private fun notification(): Notification {
        ensureChannel(this)
        val open = PendingIntent.getActivity(
            this, 0, Intent(this, MainActivity::class.java).addFlags(Intent.FLAG_ACTIVITY_SINGLE_TOP),
            PendingIntent.FLAG_IMMUTABLE or PendingIntent.FLAG_UPDATE_CURRENT,
        )
        val stop = PendingIntent.getService(
            this, 1, Intent(this, RuntimeService::class.java).setAction(ACTION_STOP),
            PendingIntent.FLAG_IMMUTABLE or PendingIntent.FLAG_UPDATE_CURRENT,
        )
        return Notification.Builder(this, CHANNEL_ID)
            .setSmallIcon(R.drawable.ic_notification)
            .setContentTitle(getString(R.string.app_name))
            .setContentText(getString(R.string.notification_running))
            .setContentIntent(open)
            .setOngoing(true)
            .addAction(Notification.Action.Builder(null, getString(R.string.action_stop), stop).build())
            .build()
    }

    private fun acquireWakeLock() {
        if (wakeLock?.isHeld == true) return
        val pm = getSystemService(PowerManager::class.java)
        wakeLock = pm.newWakeLock(PowerManager.PARTIAL_WAKE_LOCK, "acecode:runtime").apply {
            setReferenceCounted(false)
            acquire()
        }
    }

    private fun releaseWakeLock() {
        wakeLock?.let { if (it.isHeld) it.release() }
        wakeLock = null
    }

    companion object {
        private const val CHANNEL_ID = "runtime"
        private const val NOTIFICATION_ID = 1
        const val ACTION_START = "dev.acecode.android.action.START"
        const val ACTION_STOP = "dev.acecode.android.action.STOP"

        fun start(context: Context) {
            context.startForegroundService(Intent(context, RuntimeService::class.java).setAction(ACTION_START))
        }

        /**
         * targetSdk 28 的 App 在安卓 13+ 上没有通知权限时,常驻通知会被系统藏起来;
         * 系统只会在「界面在前台时创建通知渠道」那一刻自动弹授权框,所以由 MainActivity 先建渠道。
         */
        fun ensureChannel(context: Context) {
            context.getSystemService(NotificationManager::class.java).createNotificationChannel(
                NotificationChannel(CHANNEL_ID, context.getString(R.string.channel_runtime), NotificationManager.IMPORTANCE_LOW)
            )
        }
    }
}

package cn.xing.thirdpartyapp.hppro

import android.app.Notification
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.Service
import android.content.Intent
import android.os.IBinder
import androidx.core.app.NotificationCompat

class ProxyService : Service() {

    companion object {
        const val CHANNEL_ID = "hp_pro_channel"
        const val EXTRA_DEVICE_ID = "deviceId"
        const val ACTION_STOP = "cn.xing.thirdpartyapp.hppro.STOP"

        @Volatile var isRunning = false
            private set
    }

    private val listener = object : HpBridge.Listener {
        override fun onStatus(state: Int, msg: String) {
            updateNotification(HpBridge.stateName(state) + " " + msg)
            MainActivity.pushStatus(state, msg)
        }
        override fun onLog(line: String) = MainActivity.pushLog(line)
    }

    override fun onCreate() {
        super.onCreate()
        val nm = getSystemService(NotificationManager::class.java)
        nm.createNotificationChannel(
            NotificationChannel(CHANNEL_ID, "HP-PRO 隧道", NotificationManager.IMPORTANCE_LOW)
        )
    }

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        if (intent?.action == ACTION_STOP) {
            stopSelf()
            return START_NOT_STICKY
        }
        startForeground(1, buildNotification("启动中"))
        val server = "socket.hpproxy.cn:6666"
        val deviceId = intent?.getStringExtra(EXTRA_DEVICE_ID) ?: return START_NOT_STICKY

        if (!isRunning) {
            isRunning = true
            HpBridge.quicInit()
            val ok = HpBridge.startClient(server, deviceId, listener)
            val okQuic = HpBridge.startQuic(server, deviceId, listener)
            MainActivity.pushLog(if (ok) "[jni] TCP cmd 通道已启动" else "[jni] TCP 启动失败")
            MainActivity.pushLog(if (okQuic) "[jni] QUIC 数据通道已启动" else "[jni] QUIC 启动失败")
        }
        return START_STICKY
    }

    override fun onDestroy() {
        HpBridge.stopClient()
        HpBridge.stopQuic()
        isRunning = false
        MainActivity.pushStatus(6, "服务已停止")
        super.onDestroy()
    }

    override fun onBind(intent: Intent?): IBinder? = null

    private fun buildNotification(text: String): Notification =
        NotificationCompat.Builder(this, CHANNEL_ID)
            .setSmallIcon(android.R.drawable.stat_notify_sync)
            .setContentTitle("HP-PRO")
            .setContentText(text)
            .setOngoing(true)
            .build()

    private fun updateNotification(text: String) {
        getSystemService(NotificationManager::class.java).notify(1, buildNotification(text))
    }
}

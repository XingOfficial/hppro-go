package cn.xing.thirdpartyapp.hppro

object HpBridge {
    init {
        System.loadLibrary("hpcore")
        System.loadLibrary("hpcore_quic")
    }

    interface Listener {
        fun onStatus(state: Int, msg: String)
        fun onLog(line: String)
    }

    external fun startClient(server: String, deviceId: String, listener: Listener): Boolean
    external fun stopClient()
    external fun isRunning(): Boolean

    fun quicInit() = nativeQuicInit()
    external fun startQuic(server: String, deviceId: String, listener: Listener): Boolean
    external fun stopQuic()
    private external fun nativeQuicInit()

    fun stateName(state: Int): String = when (state) {
        0 -> "空闲"; 1 -> "连接中"; 2 -> "注册中"; 3 -> "在线"
        4 -> "重连中"; 5 -> "失败"; 6 -> "已关闭"; else -> "未知($state)"
    }
}

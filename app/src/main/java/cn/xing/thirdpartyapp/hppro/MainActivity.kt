package cn.xing.thirdpartyapp.hppro

import android.content.Context
import android.content.Intent
import android.os.Bundle
import androidx.activity.ComponentActivity
import androidx.activity.compose.setContent
import androidx.compose.foundation.layout.*
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.unit.dp
import kotlinx.coroutines.flow.MutableStateFlow

class MainActivity : ComponentActivity() {

    companion object {
        val statusFlow = MutableStateFlow(0 to "空闲")
        val logFlow = MutableStateFlow<List<String>>(emptyList())
        private const val MAX_LOG = 300

        fun pushStatus(state: Int, msg: String) {
            statusFlow.value = state to msg
        }

        fun pushLog(line: String) {
            val ts = java.text.SimpleDateFormat("HH:mm:ss", java.util.Locale.US)
                .format(java.util.Date())
            logFlow.value = (logFlow.value + "$ts $line").takeLast(MAX_LOG)
        }

        fun stateName(state: Int): String = when (state) {
            0 -> "空闲"; 1 -> "连接中"; 2 -> "注册中"; 3 -> "在线"
            4 -> "重连中"; 5 -> "失败"; 6 -> "已关闭"; else -> "未知"
        }
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        setContent { HpApp() }
    }
}

@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun HpApp() {
    val ctx = LocalContext.current
    val prefs = remember { ctx.getSharedPreferences("config", Context.MODE_PRIVATE) }

    val (state, statusMsg) = MainActivity.statusFlow.collectAsState().value

    var showSettings by remember { mutableStateOf(false) }
    var running by remember { mutableStateOf(ProxyService.isRunning) }

    Column(
        Modifier.padding(16.dp).fillMaxSize(),
        verticalArrangement = Arrangement.spacedBy(16.dp)
    ) {
        Text("HP Pro", style = MaterialTheme.typography.headlineSmall)

        StatusCard(state, statusMsg)

        Row(horizontalArrangement = Arrangement.spacedBy(12.dp)) {
            Button(
                onClick = {
                    val id = prefs.getString("deviceId", "") ?: ""
                    if (id.length != 32) {
                        MainActivity.pushLog("[ui] 请先在设置里填写 32 位设备ID")
                        showSettings = true
                        return@Button
                    }
                    val intent = Intent(ctx, ProxyService::class.java)
                    intent.putExtra(ProxyService.EXTRA_DEVICE_ID, id)
                    ctx.startForegroundService(intent)
                    running = true
                },
                enabled = !running
            ) { Text("连接") }

            OutlinedButton(
                onClick = {
                    ctx.stopService(Intent(ctx, ProxyService::class.java))
                    running = false
                },
                enabled = running
            ) { Text("断开") }
        }

        Row(horizontalArrangement = Arrangement.spacedBy(12.dp)) {
            TextButton(onClick = { showSettings = true }) { Text("设置") }
            TextButton(onClick = {
                ctx.startActivity(Intent(ctx, LogActivity::class.java))
            }) { Text("日志") }
        }
    }

    if (showSettings) {
        var input by remember(showSettings) { mutableStateOf(prefs.getString("deviceId", "") ?: "") }
        AlertDialog(
            onDismissRequest = { showSettings = false },
            title = { Text("设置") },
            text = {
                Column(verticalArrangement = Arrangement.spacedBy(8.dp)) {
                    OutlinedTextField(
                        value = input,
                        onValueChange = { v -> input = v.filter { it.isLetterOrDigit() }.take(32) },
                        label = { Text("设备ID（32位）") },
                        singleLine = true,
                        supportingText = { Text("长度 ${input.length}/32") },
                        modifier = Modifier.fillMaxWidth()
                    )
                    Text(
                        "服务器：socket.hpproxy.cn:6666",
                        style = MaterialTheme.typography.bodySmall
                    )
                }
            },
            confirmButton = {
                TextButton(onClick = {
                    prefs.edit().putString("deviceId", input).apply()
                    MainActivity.pushLog("[ui] 设备ID 已保存")
                    showSettings = false
                }) { Text("保存") }
            },
            dismissButton = {
                TextButton(onClick = { showSettings = false }) { Text("取消") }
            }
        )
    }
}

@Composable
fun StatusCard(state: Int, msg: String) {
    val color = when (state) {
        3 -> MaterialTheme.colorScheme.primary
        1, 2, 4 -> MaterialTheme.colorScheme.tertiary
        5 -> MaterialTheme.colorScheme.error
        else -> MaterialTheme.colorScheme.outline
    }
    val label = MainActivity.stateName(state)
    Card(Modifier.fillMaxWidth()) {
        Text(
            "● $label · $msg",
            Modifier.padding(16.dp),
            color = color,
            style = MaterialTheme.typography.bodyLarge
        )
    }
}

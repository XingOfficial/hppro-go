
#include <jni.h>
#include <android/log.h>
#include <atomic>
#include <mutex>
#include <string>
#include "hp_client.h"

#define TAG "hpcore"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__)

static hp::HpClient g_client;
static JavaVM* g_vm = nullptr;
static jobject g_listener = nullptr;
static jmethodID g_onStatus = nullptr;
static jmethodID g_onLog = nullptr;
static std::mutex g_cbMutex;

static void dispatchStatus(int state, const std::string& msg) {
    std::lock_guard<std::mutex> lk(g_cbMutex);
    if (!g_vm || !g_listener) return;
    JNIEnv* env = nullptr;
    bool attached = false;
    if (g_vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6) != JNI_OK) {
        if (g_vm->AttachCurrentThread(&env, nullptr) != JNI_OK) return;
        attached = true;
    }
    jstring jmsg = env->NewStringUTF(msg.c_str());
    env->CallVoidMethod(g_listener, g_onStatus, static_cast<jint>(state), jmsg);
    env->DeleteLocalRef(jmsg);
    if (env->ExceptionCheck()) env->ExceptionClear();
    
    (void)attached;
}

static void dispatchLog(const std::string& line) {
    std::lock_guard<std::mutex> lk(g_cbMutex);
    if (!g_vm || !g_listener) return;
    JNIEnv* env = nullptr;
    if (g_vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6) != JNI_OK) {
        if (g_vm->AttachCurrentThread(&env, nullptr) != JNI_OK) return;
    }
    jstring jline = env->NewStringUTF(line.c_str());
    env->CallVoidMethod(g_listener, g_onLog, jline);
    env->DeleteLocalRef(jline);
    if (env->ExceptionCheck()) env->ExceptionClear();
}

extern "C" JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM* vm, void*) {
    g_vm = vm;
    return JNI_VERSION_1_6;
}

extern "C" JNIEXPORT jboolean JNICALL
Java_cn_xing_thirdpartyapp_hppro_HpBridge_startClient(JNIEnv* env, jobject ,
                                        jstring server, jstring deviceId, jobject listener) {
    if (g_client.running()) return JNI_FALSE;

    const char* s = env->GetStringUTFChars(server, nullptr);
    const char* d = env->GetStringUTFChars(deviceId, nullptr);
    hp::ClientConfig cfg;
    
    std::string serverStr(s);
    auto colon = serverStr.rfind(':');
    if (colon != std::string::npos) {
        cfg.serverHost = serverStr.substr(0, colon);
        cfg.serverPort = static_cast<uint16_t>(std::stoi(serverStr.substr(colon + 1)));
    } else {
        cfg.serverHost = serverStr;
        cfg.serverPort = 6666;
    }
    cfg.deviceId = d;
    env->ReleaseStringUTFChars(server, s);
    env->ReleaseStringUTFChars(deviceId, d);

    {
        std::lock_guard<std::mutex> lk(g_cbMutex);
        if (g_listener) { env->DeleteGlobalRef(g_listener); g_listener = nullptr; }
        jclass cls = env->GetObjectClass(listener);
        g_onStatus = env->GetMethodID(cls, "onStatus", "(ILjava/lang/String;)V");
        g_onLog = env->GetMethodID(cls, "onLog", "(Ljava/lang/String;)V");
        env->DeleteLocalRef(cls);
        g_listener = env->NewGlobalRef(listener);
    }

    bool ok = g_client.start(cfg,
        [](hp::State st, const std::string& msg) { dispatchStatus(static_cast<int>(st), msg); },
        [](const std::string& line) { dispatchLog(line); },
        [](const hp::CmdMessage& msg) {
            char buf[256];
            snprintf(buf, sizeof(buf), "cmd type=%d key=%s v=%s",
                     static_cast<int>(msg.type), msg.key.c_str(), msg.version.c_str());
            dispatchLog(std::string("[cmd] ") + buf);
        });
    LOGI("startClient ok=%d host=%s port=%u", ok, cfg.serverHost.c_str(), cfg.serverPort);
    return ok ? JNI_TRUE : JNI_FALSE;
}

extern "C" JNIEXPORT void JNICALL
Java_cn_xing_thirdpartyapp_hppro_HpBridge_stopClient(JNIEnv*, jobject) {
    g_client.stop();
    std::lock_guard<std::mutex> lk(g_cbMutex);
    
}

extern "C" JNIEXPORT jboolean JNICALL
Java_cn_xing_thirdpartyapp_hppro_HpBridge_isRunning(JNIEnv*, jobject) {
    return g_client.running() ? JNI_TRUE : JNI_FALSE;
}

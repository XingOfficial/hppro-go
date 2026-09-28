#include <jni.h>
#include <android/log.h>
#include <mutex>
#include <string>

#include "quic_client.h"

#define TAG "hpquic"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__)

static hp::QuicClient g_quic;
static JavaVM *g_vm = nullptr;
static jobject g_listener = nullptr;
static jmethodID g_onStatus = nullptr;
static jmethodID g_onLog = nullptr;
static std::mutex g_cbMutex;

static void dispatchStatus(int state, const std::string &msg) {
    std::lock_guard<std::mutex> lk(g_cbMutex);
    if (!g_vm || !g_listener) return;
    JNIEnv *env = nullptr;
    bool attached = false;
    if (g_vm->GetEnv(reinterpret_cast<void **>(&env), JNI_VERSION_1_6) != JNI_OK) {
        if (g_vm->AttachCurrentThread(&env, nullptr) != JNI_OK) return;
        attached = true;
    }
    jstring jmsg = env->NewStringUTF(msg.c_str());
    env->CallVoidMethod(g_listener, g_onStatus, static_cast<jint>(state), jmsg);
    env->DeleteLocalRef(jmsg);
    if (env->ExceptionCheck()) env->ExceptionClear();
    if (attached) g_vm->DetachCurrentThread();
}

static void dispatchLog(const std::string &line) {
    std::lock_guard<std::mutex> lk(g_cbMutex);
    if (!g_vm || !g_listener) return;
    JNIEnv *env = nullptr;
    bool attached = false;
    if (g_vm->GetEnv(reinterpret_cast<void **>(&env), JNI_VERSION_1_6) != JNI_OK) {
        if (g_vm->AttachCurrentThread(&env, nullptr) != JNI_OK) return;
        attached = true;
    }
    jstring jline = env->NewStringUTF(line.c_str());
    env->CallVoidMethod(g_listener, g_onLog, jline);
    env->DeleteLocalRef(jline);
    if (env->ExceptionCheck()) env->ExceptionClear();
    if (attached) g_vm->DetachCurrentThread();
}

static std::string toStd(JNIEnv *env, jstring s) {
    if (!s) return "";
    const char *p = env->GetStringUTFChars(s, nullptr);
    std::string out = p ? p : "";
    if (p) env->ReleaseStringUTFChars(s, p);
    return out;
}

extern "C" JNIEXPORT void JNICALL
Java_cn_xing_thirdpartyapp_hppro_HpBridge_nativeQuicInit(JNIEnv *env, jobject) {
    if (!g_vm) env->GetJavaVM(&g_vm);
}

extern "C" JNIEXPORT jboolean JNICALL
Java_cn_xing_thirdpartyapp_hppro_HpBridge_startQuic(JNIEnv *env, jobject, jstring server,
                                                    jstring deviceId, jobject listener) {
    std::string serverStr = toStd(env, server);
    std::string host = serverStr;
    uint16_t port = 6666;
    auto colon = serverStr.rfind(':');
    if (colon != std::string::npos) {
        host = serverStr.substr(0, colon);
        port = static_cast<uint16_t>(std::stoi(serverStr.substr(colon + 1)));
    }

    {
        std::lock_guard<std::mutex> lk(g_cbMutex);
        if (g_listener) {
            env->DeleteGlobalRef(g_listener);
            g_listener = nullptr;
        }
        g_listener = env->NewGlobalRef(listener);
        if (!g_onStatus) {
            jclass cls = env->GetObjectClass(listener);
            g_onStatus = env->GetMethodID(cls, "onStatus", "(ILjava/lang/String;)V");
            g_onLog = env->GetMethodID(cls, "onLog", "(Ljava/lang/String;)V");
            env->DeleteLocalRef(cls);
        }
    }

    hp::QuicConfig cfg;
    cfg.host = host;
    cfg.port = port;
    cfg.deviceId = toStd(env, deviceId);

    hp::QuicEvent ev;
    ev.onStatus = [](hp::QState s, const std::string &m) { dispatchStatus(static_cast<int>(s), m); };
    ev.onLog = [](const std::string &l) { dispatchLog(l); };
    bool ok = g_quic.start(cfg, ev);
    LOGI("startQuic ok=%d host=%s port=%u", ok, host.c_str(), port);
    return ok ? JNI_TRUE : JNI_FALSE;
}

extern "C" JNIEXPORT void JNICALL
Java_cn_xing_thirdpartyapp_hppro_HpBridge_stopQuic(JNIEnv *, jobject) {
    g_quic.stop();
}

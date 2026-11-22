#include "libc/log_relay.h"

#ifdef A32_HOST_RUNNER
LogRelay log_relay_connect(int, const sockaddr_in&) { return LogRelay::NotLog; }
#else
#include <jni.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <string>
#include <thread>
#include <vector>

#include "common.h"
#include "jni/handles.h"

// System.getProperty("a32.logrelay"): the base URL of the private server, empty when unset.
static std::string relay_base(JNIEnv* jni) {
  jclass system = jni->FindClass("java/lang/System");
  jmethodID get = system ? jni->GetStaticMethodID(system, "getProperty", "(Ljava/lang/String;)Ljava/lang/String;") : nullptr;
  if (!get) {
    if (jni->ExceptionCheck())
      jni->ExceptionClear();
    return "";
  }
  jstring key = jni->NewStringUTF("a32.logrelay");
  jstring value = (jstring)jni->CallStaticObjectMethod(system, get, key);
  std::string base;
  if (value) {
    const char* chars = jni->GetStringUTFChars(value, nullptr);
    base = chars;
    jni->ReleaseStringUTFChars(value, chars);
    jni->DeleteLocalRef(value);
  }
  jni->DeleteLocalRef(key);
  if (jni->ExceptionCheck())
    jni->ExceptionClear();
  return base;
}

// POST `body` to `url` with HttpURLConnection. Returns the HTTP status, or -1 on any Java exception.
static int http_post(JNIEnv* jni, const std::string& url, const std::vector<char>& body) {
  auto fail = [&]() {
    if (jni->ExceptionCheck())
      jni->ExceptionClear();
    return -1;
  };
  jclass url_class = jni->FindClass("java/net/URL");
  jclass conn_class = jni->FindClass("java/net/HttpURLConnection");
  jclass stream_class = jni->FindClass("java/io/OutputStream");
  if (!url_class || !conn_class || !stream_class)
    return fail();

  jstring jurl = jni->NewStringUTF(url.c_str());
  jobject u = jni->NewObject(url_class, jni->GetMethodID(url_class, "<init>", "(Ljava/lang/String;)V"), jurl);
  if (!u)
    return fail();
  jobject conn = jni->CallObjectMethod(u, jni->GetMethodID(url_class, "openConnection", "()Ljava/net/URLConnection;"));
  if (!conn)
    return fail();

  jni->CallVoidMethod(conn, jni->GetMethodID(conn_class, "setRequestMethod", "(Ljava/lang/String;)V"),
                      jni->NewStringUTF("POST"));
  jni->CallVoidMethod(conn, jni->GetMethodID(conn_class, "setDoOutput", "(Z)V"), JNI_TRUE);
  jni->CallVoidMethod(conn, jni->GetMethodID(conn_class, "setConnectTimeout", "(I)V"), 6000);
  jni->CallVoidMethod(conn, jni->GetMethodID(conn_class, "setReadTimeout", "(I)V"), 6000);
  jni->CallVoidMethod(conn, jni->GetMethodID(conn_class, "setFixedLengthStreamingMode", "(I)V"), (jint)body.size());
  jni->CallVoidMethod(conn, jni->GetMethodID(conn_class, "setRequestProperty", "(Ljava/lang/String;Ljava/lang/String;)V"),
                      jni->NewStringUTF("Content-Type"), jni->NewStringUTF("application/octet-stream"));
  if (jni->ExceptionCheck())
    return fail();

  jobject out = jni->CallObjectMethod(conn, jni->GetMethodID(conn_class, "getOutputStream", "()Ljava/io/OutputStream;"));
  if (!out)
    return fail();
  jbyteArray bytes = jni->NewByteArray((jsize)body.size());
  jni->SetByteArrayRegion(bytes, 0, (jsize)body.size(), (const jbyte*)body.data());
  jni->CallVoidMethod(out, jni->GetMethodID(stream_class, "write", "([B)V"), bytes);
  jni->CallVoidMethod(out, jni->GetMethodID(stream_class, "close", "()V"));
  if (jni->ExceptionCheck())
    return fail();

  jint code = jni->CallIntMethod(conn, jni->GetMethodID(conn_class, "getResponseCode", "()I"));
  if (jni->ExceptionCheck())
    return fail();
  jni->CallVoidMethod(conn, jni->GetMethodID(conn_class, "disconnect", "()V"));
  if (jni->ExceptionCheck())
    jni->ExceptionClear();
  return code;
}

// Reads what the game writes until it closes (or goes quiet), posts it, and answers with the 4-byte acknowledgement.
static void relay_worker(int fd, std::string base) {
  using clock = std::chrono::steady_clock;
  std::vector<char> message;
  char buffer[4096];
  auto deadline = clock::now() + std::chrono::seconds(10);
  int quiet_ms = 3000;  // idle limit before the first byte; shortened once data has arrived
  while (clock::now() < deadline) {
    pollfd pfd = {fd, POLLIN, 0};
    int ready = poll(&pfd, 1, quiet_ms);
    if (ready <= 0)
      break;
    ssize_t n = recv(fd, buffer, sizeof buffer, 0);
    if (n <= 0)
      break;
    message.insert(message.end(), buffer, buffer + n);
    quiet_ms = 1500;
    if (message.size() > 64 * 1024)
      break;
  }

  if (!message.empty()) {
    int code = http_post(env(), base + "/logrelay", message);
    LOG("log relay: %zu bytes to %s -> HTTP %d", message.size(), base.c_str(), code);
    const char ack[4] = {0, 0, 0, 0};
    send(fd, ack, sizeof ack, MSG_NOSIGNAL);
    g_vm->DetachCurrentThread();
  }
  close(fd);
}

LogRelay log_relay_connect(int fd, const sockaddr_in& target) {
  if (target.sin_family != AF_INET || ntohs(target.sin_port) != LOG_PORT)
    return LogRelay::NotLog;
  std::string base = relay_base(env());
  if (base.empty())
    return LogRelay::Refused;
  int pair[2];
  if (socketpair(AF_UNIX, SOCK_STREAM, 0, pair) != 0)
    return LogRelay::Refused;
  if (dup2(pair[0], fd) < 0) {
    close(pair[0]);
    close(pair[1]);
    return LogRelay::Refused;
  }
  close(pair[0]);
  std::thread(relay_worker, pair[1], base).detach();
  return LogRelay::Relayed;
}
#endif

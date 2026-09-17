/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 huienming */

/*
 * Java 绑定的 JNI 胶水。
 *
 * 这一层只做三件事，别的什么都不干：
 *   1. Java 字符串 <-> UTF-8（拿到的都是库自己的文本，不经系统代码页）；
 *   2. jlong 当不透明句柄传（指针值），出参用 long[] / String[] 回填；
 *   3. 订阅：把 Java 回调对象钉成全局引用，回调从原生读取线程上抛回
 *      DeviceClient.onSampleNative(topic, handle) —— 快照由 Java 侧在回调里做完。
 *
 * 真正的库调用全部走 nclink_shim（bindings/native/nclink_shim.c，声明见
 * nclink_shim.h），和 C# / Python 绑定共用同一份垫片。
 */

#include <jni.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "nclink_shim.h"

#if defined(_MSC_VER)
/* MSVC 把 strdup 放在非标准名字下，改名而不是开 _CRT_NONSTDC_NO_DEPRECATE。 */
#define strdup _strdup
#endif

#define HANDLE(value) ((void *)(intptr_t)(value))
#define PTR(value) ((jlong)(intptr_t)(value))

/* --------------------------------------------------------------- 工具 -- */

static jstring to_jstring(JNIEnv *env, const char *text)
{
    return text != NULL ? (*env)->NewStringUTF(env, text) : NULL;
}

/** 借一份 jstring 的 UTF-8 内容（调用方 free）。NULL 输入得 NULL。 */
static char *from_jstring(JNIEnv *env, jstring text)
{
    const char *chars;
    char *copy;

    if (text == NULL) {
        return NULL;
    }
    chars = (*env)->GetStringUTFChars(env, text, NULL);
    if (chars == NULL) {
        return NULL;                     /* 内存不足，异常已挂上 */
    }
    copy = strdup(chars);
    (*env)->ReleaseStringUTFChars(env, text, chars);
    return copy;
}

static void set_long(JNIEnv *env, jlongArray array, jint index, jlong value)
{
    if (array != NULL && (*env)->GetArrayLength(env, array) > index) {
        (*env)->SetLongArrayRegion(env, array, index, 1, &value);
    }
}

/** 把一份 malloc 出来的文本放进 String[]，然后释放它。 */
static void set_string(JNIEnv *env, jobjectArray array, jint index, char *owned)
{
    if (array != NULL && (*env)->GetArrayLength(env, array) > index) {
        jstring text = to_jstring(env, owned);
        (*env)->SetObjectArrayElement(env, array, index, text);
        if (text != NULL) {
            (*env)->DeleteLocalRef(env, text);
        }
    }
    if (owned != NULL) {
        nclshim_free(owned);
    }
}

/** 把一个 malloc 出来的文本转成 jstring 再释放（返回 NULL 表示没有值）。 */
static jstring take_jstring(JNIEnv *env, char *owned)
{
    jstring text;

    if (owned == NULL) {
        return NULL;
    }
    text = to_jstring(env, owned);
    nclshim_free(owned);
    return text;
}

/* ----------------------------------------------------------- 订阅回调 -- */

static JavaVM *g_vm = NULL;

/* 垫片要的 host 是 [函数指针, 用户数据] 两格；这里用户数据是 jni_host。 */
typedef struct {
    void *slots[2];
    jobject target;     /* DeviceClient 的全局引用 */
} jni_host;

static void jni_call(JNIEnv *env, jni_host *host, const char *method,
                     const char *topic, const void *msg)
{
    jclass cls;
    jmethodID mid;
    jstring jtopic;

    cls = (*env)->GetObjectClass(env, host->target);
    if (cls == NULL) {
        return;
    }
    mid = (*env)->GetMethodID(env, cls, method, "(Ljava/lang/String;J)V");
    if (mid == NULL) {
        (*env)->ExceptionClear(env);
        (*env)->DeleteLocalRef(env, cls);
        return;
    }
    jtopic = to_jstring(env, topic);
    (*env)->CallVoidMethod(env, host->target, mid, jtopic, PTR(msg));
    /* Java 侧的回调自己会吞异常（记在 lastCallbackError 上）；真漏出来就
     * 打印并清掉，绝不让它穿回 C 线程。 */
    if ((*env)->ExceptionCheck(env)) {
        (*env)->ExceptionDescribe(env);
        (*env)->ExceptionClear(env);
    }
    if (jtopic != NULL) {
        (*env)->DeleteLocalRef(env, jtopic);
    }
    (*env)->DeleteLocalRef(env, cls);
}

static void dispatch(JNIEnv *env, jni_host *host, const char *method,
                     const char *topic, const void *msg)
{
    if (host == NULL || host->target == NULL) {
        return;
    }
    (*env)->PushLocalFrame(env, 8);
    jni_call(env, host, method, topic, msg);
    (*env)->PopLocalFrame(env, NULL);
}

static void jni_sample_thunk(void *user, const char *topic, const void *msg)
{
    jni_host *host = (jni_host *)user;
    JNIEnv *env = NULL;
    jboolean attached = JNI_FALSE;

    if (host == NULL || g_vm == NULL) {
        return;
    }
    /* 回调跑在库自己的读取线程上（不是 Java 创建的），所以得按需挂上去。 */
    if ((*g_vm)->GetEnv(g_vm, (void **)&env, JNI_VERSION_1_8) != JNI_OK) {
        if ((*g_vm)->AttachCurrentThread(g_vm, (void **)&env, NULL) != JNI_OK) {
            return;
        }
        attached = JNI_TRUE;
    }
    dispatch(env, host, "onSampleNative", topic, msg);
    if (attached) {
        (*g_vm)->DetachCurrentThread(g_vm);
    }
}

static void jni_event_thunk(void *user, const char *topic, const void *msg)
{
    jni_host *host = (jni_host *)user;
    JNIEnv *env = NULL;
    jboolean attached = JNI_FALSE;

    if (host == NULL || g_vm == NULL) {
        return;
    }
    if ((*g_vm)->GetEnv(g_vm, (void **)&env, JNI_VERSION_1_8) != JNI_OK) {
        if ((*g_vm)->AttachCurrentThread(g_vm, (void **)&env, NULL) != JNI_OK) {
            return;
        }
        attached = JNI_TRUE;
    }
    dispatch(env, host, "onEventNative", topic, msg);
    if (attached) {
        (*g_vm)->DetachCurrentThread(g_vm);
    }
}

JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM *vm, void *reserved)
{
    (void)reserved;
    g_vm = vm;
    return JNI_VERSION_1_8;
}

/* ------------------------------------------------------------ 进程级 -- */

JNIEXPORT jstring JNICALL Java_com_nclink_Native_version(JNIEnv *env, jclass cls)
{
    (void)cls;
    return to_jstring(env, nclshim_version());
}

JNIEXPORT jstring JNICALL Java_com_nclink_Native_errName(JNIEnv *env, jclass cls,
                                                         jint code)
{
    (void)cls;
    return to_jstring(env, nclshim_err_name((int)code));
}

JNIEXPORT void JNICALL Java_com_nclink_Native_free(JNIEnv *env, jclass cls, jlong ptr)
{
    (void)env;
    (void)cls;
    nclshim_free(HANDLE(ptr));
}

JNIEXPORT void JNICALL Java_com_nclink_Native_envSetRoot(JNIEnv *env, jclass cls,
                                                         jstring root)
{
    char *text = from_jstring(env, root);
    (void)cls;
    nclshim_env_set_root(text);
    free(text);
}

JNIEXPORT jstring JNICALL Java_com_nclink_Native_envRoot(JNIEnv *env, jclass cls)
{
    (void)cls;
    return to_jstring(env, nclshim_env_root());
}

JNIEXPORT jstring JNICALL Java_com_nclink_Native_deviceModel(JNIEnv *env, jclass cls)
{
    (void)cls;
    return to_jstring(env, nclshim_device_model());
}

JNIEXPORT jint JNICALL Java_com_nclink_Native_logInit(JNIEnv *env, jclass cls,
                                                      jstring dir)
{
    char *text = from_jstring(env, dir);
    int rc = nclshim_log_init(text);
    (void)cls;
    free(text);
    return (jint)rc;
}

JNIEXPORT void JNICALL Java_com_nclink_Native_logShutdown(JNIEnv *env, jclass cls)
{
    (void)env;
    (void)cls;
    nclshim_log_shutdown();
}

JNIEXPORT void JNICALL Java_com_nclink_Native_logSetLevel(JNIEnv *env, jclass cls,
                                                          jint level)
{
    (void)env;
    (void)cls;
    nclshim_log_set_level((int)level);
}

JNIEXPORT void JNICALL Java_com_nclink_Native_logSetConsole(JNIEnv *env, jclass cls,
                                                            jint enabled)
{
    (void)env;
    (void)cls;
    nclshim_log_set_console((int)enabled);
}

/* ---------------------------------------------------------------- JSON -- */

JNIEXPORT jlong JNICALL Java_com_nclink_Native_jsonParse(JNIEnv *env, jclass cls,
                                                         jstring text)
{
    char *raw = from_jstring(env, text);
    const void *json = nclshim_json_parse(raw);
    (void)cls;
    free(raw);
    return PTR(json);
}

JNIEXPORT jlong JNICALL Java_com_nclink_Native_jsonClone(JNIEnv *env, jclass cls,
                                                         jlong json)
{
    (void)env;
    (void)cls;
    return PTR(nclshim_json_clone(HANDLE(json)));
}

JNIEXPORT void JNICALL Java_com_nclink_Native_jsonFree(JNIEnv *env, jclass cls,
                                                       jlong json)
{
    (void)env;
    (void)cls;
    nclshim_json_free(HANDLE(json));
}

JNIEXPORT jstring JNICALL Java_com_nclink_Native_jsonWrite(JNIEnv *env, jclass cls,
                                                           jlong json)
{
    (void)cls;
    return take_jstring(env, nclshim_json_write(HANDLE(json)));
}

JNIEXPORT jint JNICALL Java_com_nclink_Native_jsonType(JNIEnv *env, jclass cls,
                                                       jlong json)
{
    (void)env;
    (void)cls;
    return (jint)nclshim_json_type(HANDLE(json));
}

JNIEXPORT jint JNICALL Java_com_nclink_Native_jsonIsNull(JNIEnv *env, jclass cls,
                                                         jlong json)
{
    (void)env;
    (void)cls;
    return (jint)nclshim_json_is_null(HANDLE(json));
}

JNIEXPORT jint JNICALL Java_com_nclink_Native_jsonAsLong(JNIEnv *env, jclass cls,
                                                         jlong json, jlongArray out)
{
    long long value = 0;
    int rc;
    (void)cls;
    rc = nclshim_json_as_int(HANDLE(json), &value);
    set_long(env, out, 0, (jlong)value);
    return (jint)rc;
}

JNIEXPORT jint JNICALL Java_com_nclink_Native_jsonAsDouble(JNIEnv *env, jclass cls,
                                                           jlong json,
                                                           jdoubleArray out)
{
    double value = 0;
    jdouble copy;
    int rc;

    (void)cls;
    rc = nclshim_json_as_double(HANDLE(json), &value);
    if (rc != 0 && out != NULL && (*env)->GetArrayLength(env, out) > 0) {
        copy = (jdouble)value;
        (*env)->SetDoubleArrayRegion(env, out, 0, 1, &copy);
    }
    return (jint)rc;
}

JNIEXPORT jint JNICALL Java_com_nclink_Native_jsonAsBool(JNIEnv *env, jclass cls,
                                                         jlong json, jintArray out)
{
    int value = 0;
    jint copy;
    int rc;

    (void)cls;
    rc = nclshim_json_as_bool(HANDLE(json), &value);
    if (rc != 0 && out != NULL && (*env)->GetArrayLength(env, out) > 0) {
        copy = (jint)value;
        (*env)->SetIntArrayRegion(env, out, 0, 1, &copy);
    }
    return (jint)rc;
}

JNIEXPORT jstring JNICALL Java_com_nclink_Native_jsonString(JNIEnv *env, jclass cls,
                                                            jlong json)
{
    (void)cls;
    return to_jstring(env, nclshim_json_string(HANDLE(json)));
}

JNIEXPORT jstring JNICALL Java_com_nclink_Native_jsonText(JNIEnv *env, jclass cls,
                                                          jlong json)
{
    (void)cls;
    return take_jstring(env, nclshim_json_text(HANDLE(json)));
}

JNIEXPORT jint JNICALL Java_com_nclink_Native_jsonCount(JNIEnv *env, jclass cls,
                                                        jlong json)
{
    (void)env;
    (void)cls;
    return (jint)nclshim_json_count(HANDLE(json));
}

JNIEXPORT jlong JNICALL Java_com_nclink_Native_jsonArrayGet(JNIEnv *env, jclass cls,
                                                            jlong json, jint index)
{
    (void)env;
    (void)cls;
    return PTR(nclshim_json_array_get(HANDLE(json), (int)index));
}

JNIEXPORT jlong JNICALL Java_com_nclink_Native_jsonObjectValueAt(JNIEnv *env,
                                                                 jclass cls,
                                                                 jlong json,
                                                                 jint index)
{
    (void)env;
    (void)cls;
    return PTR(nclshim_json_object_value_at(HANDLE(json), (int)index));
}

JNIEXPORT jstring JNICALL Java_com_nclink_Native_jsonObjectKeyAt(JNIEnv *env,
                                                                 jclass cls,
                                                                 jlong json,
                                                                 jint index)
{
    (void)cls;
    return to_jstring(env, nclshim_json_object_key_at(HANDLE(json), (int)index));
}

JNIEXPORT jlong JNICALL Java_com_nclink_Native_jsonObjectGet(JNIEnv *env, jclass cls,
                                                             jlong json, jstring key)
{
    char *raw = from_jstring(env, key);
    const void *found = nclshim_json_object_get(HANDLE(json), raw);
    (void)cls;
    free(raw);
    return PTR(found);
}

/* ----------------------------------------------------------- 模型 / 节点 -- */

JNIEXPORT jlong JNICALL Java_com_nclink_Native_modelParse(JNIEnv *env, jclass cls,
                                                          jstring text)
{
    char *raw = from_jstring(env, text);
    const void *root = nclshim_model_parse(raw);
    (void)cls;
    free(raw);
    return PTR(root);
}

JNIEXPORT void JNICALL Java_com_nclink_Native_modelFree(JNIEnv *env, jclass cls,
                                                        jlong root)
{
    (void)env;
    (void)cls;
    nclshim_model_free(HANDLE(root));
}

JNIEXPORT jstring JNICALL Java_com_nclink_Native_modelWrite(JNIEnv *env, jclass cls,
                                                            jlong root)
{
    (void)cls;
    return take_jstring(env, nclshim_model_write(HANDLE(root)));
}

JNIEXPORT jlong JNICALL Java_com_nclink_Native_modelFindById(JNIEnv *env, jclass cls,
                                                             jlong root, jstring id)
{
    char *raw = from_jstring(env, id);
    const void *node = nclshim_model_find_by_id(HANDLE(root), raw);
    (void)cls;
    free(raw);
    return PTR(node);
}

#define NODE_INT(JavaName, ShimFn)                                             \
    JNIEXPORT jint JNICALL Java_com_nclink_Native_##JavaName(                   \
        JNIEnv *env, jclass cls, jlong node)                                    \
    {                                                                           \
        (void)env;                                                              \
        (void)cls;                                                              \
        return (jint)ShimFn(HANDLE(node));                                      \
    }

#define NODE_LONG(JavaName, ShimFn)                                            \
    JNIEXPORT jlong JNICALL Java_com_nclink_Native_##JavaName(                  \
        JNIEnv *env, jclass cls, jlong node)                                    \
    {                                                                           \
        (void)env;                                                              \
        (void)cls;                                                              \
        return (jlong)ShimFn(HANDLE(node));                                     \
    }

#define NODE_STR(JavaName, ShimFn)                                             \
    JNIEXPORT jstring JNICALL Java_com_nclink_Native_##JavaName(                \
        JNIEnv *env, jclass cls, jlong node)                                    \
    {                                                                           \
        (void)cls;                                                              \
        return to_jstring(env, ShimFn(HANDLE(node)));                           \
    }

NODE_INT(nodeType, nclshim_node_type)
NODE_STR(nodeTypeName, nclshim_node_type_name)
NODE_STR(nodeName, nclshim_node_name)
NODE_STR(nodeId, nclshim_node_id)
NODE_STR(nodePath, nclshim_node_path)
NODE_STR(nodeDescription, nclshim_node_description)
NODE_STR(nodeNumber, nclshim_node_number)
NODE_STR(nodeDataType, nclshim_node_data_type)
NODE_STR(nodeValueType, nclshim_node_value_type)
NODE_STR(nodeMapping, nclshim_node_mapping)
NODE_STR(nodeSource, nclshim_node_source)
NODE_STR(nodeVersion, nclshim_node_version)
NODE_STR(nodeGuid, nclshim_node_guid)
NODE_INT(nodeSettable, nclshim_node_settable)
NODE_INT(nodeIsSampleChannel, nclshim_node_is_sample_channel)
NODE_LONG(nodeSampleInterval, nclshim_node_sample_interval)
NODE_LONG(nodeUploadInterval, nclshim_node_upload_interval)
NODE_INT(nodeSampleItemCount, nclshim_node_sample_item_count)

JNIEXPORT jlong JNICALL Java_com_nclink_Native_nodeValue(JNIEnv *env, jclass cls,
                                                         jlong node)
{
    (void)env;
    (void)cls;
    return PTR(nclshim_node_value(HANDLE(node)));
}

JNIEXPORT jstring JNICALL Java_com_nclink_Native_nodeSampleItemPath(JNIEnv *env,
                                                                    jclass cls,
                                                                    jlong node,
                                                                    jint index)
{
    (void)cls;
    return take_jstring(env, nclshim_node_sample_item_path(HANDLE(node), (int)index));
}

JNIEXPORT jint JNICALL Java_com_nclink_Native_nodeCount(JNIEnv *env, jclass cls,
                                                        jlong node, jint kind)
{
    (void)env;
    (void)cls;
    return (jint)nclshim_node_count(HANDLE(node), (int)kind);
}

JNIEXPORT jlong JNICALL Java_com_nclink_Native_nodeAt(JNIEnv *env, jclass cls,
                                                      jlong node, jint kind,
                                                      jint index)
{
    (void)env;
    (void)cls;
    return PTR(nclshim_node_at(HANDLE(node), (int)kind, (int)index));
}

/* ------------------------------------------------------------ 报文 / 采样 -- */

JNIEXPORT jlong JNICALL Java_com_nclink_Native_messageParse(JNIEnv *env, jclass cls,
                                                            jstring topic,
                                                            jbyteArray payload)
{
    char *raw_topic;
    jbyte *bytes;
    jsize length;
    const void *msg;

    (void)cls;
    if (payload == NULL) {
        return 0;
    }
    raw_topic = from_jstring(env, topic);
    length = (*env)->GetArrayLength(env, payload);
    bytes = (*env)->GetByteArrayElements(env, payload, NULL);
    if (bytes == NULL) {
        free(raw_topic);
        return 0;
    }
    msg = nclshim_message_parse(raw_topic, bytes, (int)length);
    (*env)->ReleaseByteArrayElements(env, payload, bytes, JNI_ABORT);
    free(raw_topic);
    return PTR(msg);
}

JNIEXPORT void JNICALL Java_com_nclink_Native_messageFree(JNIEnv *env, jclass cls,
                                                          jlong msg)
{
    (void)env;
    (void)cls;
    nclshim_message_free(HANDLE(msg));
}

JNIEXPORT jstring JNICALL Java_com_nclink_Native_messageWrite(JNIEnv *env, jclass cls,
                                                              jlong msg)
{
    (void)cls;
    return take_jstring(env, nclshim_message_write(HANDLE(msg)));
}

JNIEXPORT jint JNICALL Java_com_nclink_Native_messageType(JNIEnv *env, jclass cls,
                                                          jlong msg)
{
    (void)env;
    (void)cls;
    return (jint)nclshim_message_type(HANDLE(msg));
}

#define SAMPLE_INT(JavaName, ShimFn)                                           \
    JNIEXPORT jint JNICALL Java_com_nclink_Native_##JavaName(                   \
        JNIEnv *env, jclass cls, jlong msg)                                     \
    {                                                                           \
        (void)env;                                                              \
        (void)cls;                                                              \
        return (jint)ShimFn(HANDLE(msg));                                       \
    }

#define SAMPLE_INT1(JavaName, ShimFn)                                          \
    JNIEXPORT jint JNICALL Java_com_nclink_Native_##JavaName(                   \
        JNIEnv *env, jclass cls, jlong msg, jint index)                         \
    {                                                                           \
        (void)env;                                                              \
        (void)cls;                                                              \
        return (jint)ShimFn(HANDLE(msg), (int)index);                           \
    }

#define SAMPLE_LONG(JavaName, ShimFn)                                          \
    JNIEXPORT jlong JNICALL Java_com_nclink_Native_##JavaName(                  \
        JNIEnv *env, jclass cls, jlong msg)                                     \
    {                                                                           \
        (void)env;                                                              \
        (void)cls;                                                              \
        return (jlong)ShimFn(HANDLE(msg));                                      \
    }

#define SAMPLE_STR(JavaName, ShimFn)                                           \
    JNIEXPORT jstring JNICALL Java_com_nclink_Native_##JavaName(                \
        JNIEnv *env, jclass cls, jlong msg)                                     \
    {                                                                           \
        (void)cls;                                                              \
        return to_jstring(env, ShimFn(HANDLE(msg)));                            \
    }

#define SAMPLE_STR1(JavaName, ShimFn)                                          \
    JNIEXPORT jstring JNICALL Java_com_nclink_Native_##JavaName(                \
        JNIEnv *env, jclass cls, jlong msg, jint index)                         \
    {                                                                           \
        (void)cls;                                                              \
        return to_jstring(env, ShimFn(HANDLE(msg), (int)index));                \
    }

SAMPLE_INT(sampleRows, nclshim_sample_rows)
SAMPLE_INT(sampleColumns, nclshim_sample_columns)
SAMPLE_INT(sampleIsComplete, nclshim_sample_is_complete)
SAMPLE_STR(sampleId, nclshim_sample_id)
SAMPLE_STR(sampleBeginTime, nclshim_sample_begin_time)
SAMPLE_LONG(sampleInterval, nclshim_sample_interval)
SAMPLE_LONG(sampleUploadInterval, nclshim_sample_upload_interval)
SAMPLE_INT1(sampleColumnSlots, nclshim_sample_column_slots)
SAMPLE_INT1(sampleColumnPoints, nclshim_sample_column_points)
SAMPLE_INT1(sampleColumnNested, nclshim_sample_column_nested)
SAMPLE_STR1(samplePath, nclshim_sample_path)
SAMPLE_STR1(sampleColumnEncoding, nclshim_sample_column_encoding)

JNIEXPORT jstring JNICALL Java_com_nclink_Native_sampleHeader(JNIEnv *env, jclass cls,
                                                              jlong msg,
                                                              jstring separator)
{
    char *sep = from_jstring(env, separator);
    char *header = nclshim_sample_header(HANDLE(msg), sep != NULL ? sep : " ");
    (void)cls;
    free(sep);
    return take_jstring(env, header);
}

JNIEXPORT jlong JNICALL Java_com_nclink_Native_sampleValueAt(JNIEnv *env, jclass cls,
                                                             jlong msg, jint row,
                                                             jint col)
{
    (void)env;
    (void)cls;
    return PTR(nclshim_sample_value_at(HANDLE(msg), (int)row, (int)col));
}

JNIEXPORT jlong JNICALL Java_com_nclink_Native_sampleColumnValueAt(JNIEnv *env,
                                                                   jclass cls,
                                                                   jlong msg,
                                                                   jint col,
                                                                   jint index)
{
    (void)env;
    (void)cls;
    return PTR(nclshim_sample_column_value_at(HANDLE(msg), (int)col, (int)index));
}

SAMPLE_STR(eventId, nclshim_event_id)
SAMPLE_STR(eventTime, nclshim_event_time)
SAMPLE_STR(eventKey, nclshim_event_key)

JNIEXPORT jlong JNICALL Java_com_nclink_Native_eventValue(JNIEnv *env, jclass cls,
                                                          jlong msg)
{
    (void)env;
    (void)cls;
    return PTR(nclshim_event_value(HANDLE(msg)));
}

/* ---------------------------------------------------------------- 客户端 -- */

JNIEXPORT jint JNICALL Java_com_nclink_Native_open(JNIEnv *env, jclass cls,
                                                   jstring uri, jstring user,
                                                   jstring password)
{
    char *raw_uri = from_jstring(env, uri);
    char *raw_user = from_jstring(env, user);
    char *raw_pass = from_jstring(env, password);
    int rc;

    (void)cls;
    rc = nclshim_open(raw_uri, raw_user, raw_pass);
    free(raw_uri);
    free(raw_user);
    free(raw_pass);
    return (jint)rc;
}

JNIEXPORT jint JNICALL Java_com_nclink_Native_openEx(
    JNIEnv *env, jclass cls, jstring uri, jstring user, jstring password,
    jstring ca_file, jstring client_cert, jstring client_key, jstring server_name,
    jboolean verify_peer)
{
    char *raw_uri = from_jstring(env, uri);
    char *raw_user = from_jstring(env, user);
    char *raw_pass = from_jstring(env, password);
    char *raw_ca = from_jstring(env, ca_file);
    char *raw_cert = from_jstring(env, client_cert);
    char *raw_key = from_jstring(env, client_key);
    char *raw_name = from_jstring(env, server_name);
    int rc;

    (void)cls;
    rc = nclshim_open_ex(raw_uri, raw_user, raw_pass, raw_ca, raw_cert, raw_key,
                         raw_name, verify_peer != JNI_FALSE ? 1 : 0);
    free(raw_uri);
    free(raw_user);
    free(raw_pass);
    free(raw_ca);
    free(raw_cert);
    free(raw_key);
    free(raw_name);
    return (jint)rc;
}

JNIEXPORT jint JNICALL Java_com_nclink_Native_tlsAvailable(JNIEnv *env, jclass cls)
{
    (void)env;
    (void)cls;
    return (jint)nclshim_tls_available();
}

JNIEXPORT void JNICALL Java_com_nclink_Native_close(JNIEnv *env, jclass cls)
{
    (void)env;
    (void)cls;
    nclshim_close();
}

JNIEXPORT jint JNICALL Java_com_nclink_Native_isOpen(JNIEnv *env, jclass cls)
{
    (void)env;
    (void)cls;
    return (jint)nclshim_is_open();
}

JNIEXPORT jlong JNICALL Java_com_nclink_Native_clientGet(JNIEnv *env, jclass cls,
                                                         jstring sn)
{
    char *raw = from_jstring(env, sn);
    const void *client = nclshim_client_get(raw);
    (void)cls;
    free(raw);
    return PTR(client);
}

JNIEXPORT jint JNICALL Java_com_nclink_Native_clientProbe(JNIEnv *env, jclass cls,
                                                          jlong client,
                                                          jint timeout_ms,
                                                          jlongArray out)
{
    const void *model = NULL;
    int rc;

    (void)cls;
    rc = nclshim_client_probe(HANDLE(client), (unsigned)timeout_ms, (void **)&model);
    set_long(env, out, 0, PTR(model));
    return (jint)rc;
}

JNIEXPORT jint JNICALL Java_com_nclink_Native_clientSetRootNode(JNIEnv *env,
                                                                jclass cls,
                                                                jlong client,
                                                                jlong root)
{
    (void)env;
    (void)cls;
    return (jint)nclshim_client_set_root_node(HANDLE(client), HANDLE(root));
}

JNIEXPORT jint JNICALL Java_com_nclink_Native_clientGetValue(JNIEnv *env, jclass cls,
                                                             jlong client,
                                                             jstring path,
                                                             jint timeout_ms,
                                                             jlongArray out)
{
    char *raw = from_jstring(env, path);
    const void *json = NULL;
    int rc;

    (void)cls;
    rc = nclshim_client_get_value(HANDLE(client), raw, (unsigned)timeout_ms, &json);
    set_long(env, out, 0, PTR(json));
    free(raw);
    return (jint)rc;
}

JNIEXPORT jint JNICALL Java_com_nclink_Native_clientGetValueRange(
    JNIEnv *env, jclass cls, jlong client, jstring path, jint start, jint end,
    jint timeout_ms, jlongArray out)
{
    char *raw = from_jstring(env, path);
    const void *json = NULL;
    int rc;

    (void)cls;
    rc = nclshim_client_get_value_range(HANDLE(client), raw, (int)start, (int)end,
                                        (unsigned)timeout_ms, &json);
    set_long(env, out, 0, PTR(json));
    free(raw);
    return (jint)rc;
}

JNIEXPORT jint JNICALL Java_com_nclink_Native_clientGetLength(JNIEnv *env,
                                                              jclass cls,
                                                              jlong client,
                                                              jstring path,
                                                              jint timeout_ms,
                                                              jlongArray out)
{
    char *raw = from_jstring(env, path);
    long long length = 0;
    int rc;

    (void)cls;
    rc = nclshim_client_get_length(HANDLE(client), raw, (unsigned)timeout_ms, &length);
    set_long(env, out, 0, (jlong)length);
    free(raw);
    return (jint)rc;
}

JNIEXPORT jint JNICALL Java_com_nclink_Native_clientSetValue(JNIEnv *env, jclass cls,
                                                             jlong client,
                                                             jstring path,
                                                             jstring value,
                                                             jint timeout_ms)
{
    char *raw_path = from_jstring(env, path);
    char *raw_value = from_jstring(env, value);
    int rc;

    (void)cls;
    rc = nclshim_client_set_value(HANDLE(client), raw_path, raw_value,
                                  (unsigned)timeout_ms);
    free(raw_path);
    free(raw_value);
    return (jint)rc;
}

JNIEXPORT jint JNICALL Java_com_nclink_Native_clientSetValueIndex(
    JNIEnv *env, jclass cls, jlong client, jstring path, jstring value, jint index,
    jint timeout_ms)
{
    char *raw_path = from_jstring(env, path);
    char *raw_value = from_jstring(env, value);
    int rc;

    (void)cls;
    rc = nclshim_client_set_value_index(HANDLE(client), raw_path, raw_value,
                                        (int)index, (unsigned)timeout_ms);
    free(raw_path);
    free(raw_value);
    return (jint)rc;
}

JNIEXPORT jint JNICALL Java_com_nclink_Native_clientMethodCall(
    JNIEnv *env, jclass cls, jlong client, jstring method, jstring params,
    jboolean check, jint timeout_ms, jobjectArray out)
{
    char *raw_method = from_jstring(env, method);
    char *raw_params = from_jstring(env, params);
    char *reply = NULL;
    int rc;

    (void)cls;
    rc = nclshim_client_method_call(HANDLE(client), raw_method, raw_params,
                                    check != JNI_FALSE ? 1 : 0,
                                    (unsigned)timeout_ms, &reply);
    set_string(env, out, 0, reply);
    free(raw_method);
    free(raw_params);
    return (jint)rc;
}

JNIEXPORT jint JNICALL Java_com_nclink_Native_clientPing(JNIEnv *env, jclass cls,
                                                         jlong client,
                                                         jint timeout_ms)
{
    (void)env;
    (void)cls;
    return (jint)nclshim_client_ping(HANDLE(client), (unsigned)timeout_ms);
}

JNIEXPORT jstring JNICALL Java_com_nclink_Native_clientGetId(JNIEnv *env, jclass cls,
                                                             jlong client,
                                                             jstring path)
{
    char *raw = from_jstring(env, path);
    char *id = nclshim_client_get_id(HANDLE(client), raw);
    (void)cls;
    free(raw);
    return take_jstring(env, id);
}

JNIEXPORT jstring JNICALL Java_com_nclink_Native_clientGetPath(JNIEnv *env,
                                                               jclass cls,
                                                               jlong client,
                                                               jstring id)
{
    char *raw = from_jstring(env, id);
    char *path = nclshim_client_get_path(HANDLE(client), raw);
    (void)cls;
    free(raw);
    return take_jstring(env, path);
}

JNIEXPORT jint JNICALL Java_com_nclink_Native_clientSubscribeSamples(
    JNIEnv *env, jclass cls, jlong client, jint qos, jobject target,
    jlongArray out_host)
{
    jni_host *host;
    int rc;

    (void)cls;
    if (target == NULL) {
        return -5;                       /* NCL_ERR_INVALID_ARG */
    }
    host = (jni_host *)calloc(1, sizeof(*host));
    if (host == NULL) {
        return -2;                       /* NCL_ERR_NOMEM */
    }
    host->target = (*env)->NewGlobalRef(env, target);
    host->slots[0] = (void *)jni_sample_thunk;
    host->slots[1] = NULL;
    rc = nclshim_client_subscribe_samples(HANDLE(client), (int)qos, host);
    if (rc != 0) {
        (*env)->DeleteGlobalRef(env, host->target);
        free(host);
        return (jint)rc;
    }
    /* 成功时 host 挂在客户端上；把指针交给 Java 存着，退订时原样传给
     * clientUnsubscribeSamples() —— 那边负责 DeleteGlobalRef + free。 */
    set_long(env, out_host, 0, PTR(host));
    return (jint)rc;
}

JNIEXPORT jint JNICALL Java_com_nclink_Native_clientUnsubscribeSamples(JNIEnv *env,
                                                                       jclass cls,
                                                                       jlong client,
                                                                       jlong host)
{
    jni_host *entry = (jni_host *)HANDLE(host);
    int rc;

    (void)cls;
    rc = nclshim_client_unsubscribe_samples(HANDLE(client));
    if (entry != NULL) {
        if (entry->target != NULL) {
            (*env)->DeleteGlobalRef(env, entry->target);
        }
        free(entry);
    }
    return (jint)rc;
}

JNIEXPORT jint JNICALL Java_com_nclink_Native_clientSubscribeEvents(
    JNIEnv *env, jclass cls, jlong client, jint qos, jobject target,
    jlongArray out_host)
{
    jni_host *host;
    int rc;

    (void)cls;
    if (target == NULL) {
        return -5;
    }
    host = (jni_host *)calloc(1, sizeof(*host));
    if (host == NULL) {
        return -2;
    }
    host->target = (*env)->NewGlobalRef(env, target);
    host->slots[0] = (void *)jni_event_thunk;
    host->slots[1] = NULL;
    rc = nclshim_client_subscribe_events(HANDLE(client), (int)qos, host);
    if (rc != 0) {
        (*env)->DeleteGlobalRef(env, host->target);
        free(host);
        return (jint)rc;
    }
    set_long(env, out_host, 0, PTR(host));
    return (jint)rc;
}

JNIEXPORT jint JNICALL Java_com_nclink_Native_clientUnsubscribeEvents(JNIEnv *env,
                                                                      jclass cls,
                                                                      jlong client,
                                                                      jlong host)
{
    jni_host *entry = (jni_host *)HANDLE(host);
    int rc;

    (void)cls;
    rc = nclshim_client_unsubscribe_events(HANDLE(client));
    if (entry != NULL) {
        if (entry->target != NULL) {
            (*env)->DeleteGlobalRef(env, entry->target);
        }
        free(entry);
    }
    return (jint)rc;
}

JNIEXPORT jint JNICALL Java_com_nclink_Native_clientSampleCount(JNIEnv *env,
                                                                jclass cls,
                                                                jlong client)
{
    (void)env;
    (void)cls;
    return (jint)nclshim_client_sample_count(HANDLE(client));
}

JNIEXPORT jint JNICALL Java_com_nclink_Native_clientEventCount(JNIEnv *env,
                                                               jclass cls,
                                                               jlong client)
{
    (void)env;
    (void)cls;
    return (jint)nclshim_client_event_count(HANDLE(client));
}

JNIEXPORT jint JNICALL Java_com_nclink_Native_clientAddSample(JNIEnv *env,
                                                              jclass cls,
                                                              jlong client,
                                                              jstring config,
                                                              jint timeout_ms)
{
    char *raw = from_jstring(env, config);
    int rc;

    (void)cls;
    rc = nclshim_client_add_sample(HANDLE(client), raw, (unsigned)timeout_ms);
    free(raw);
    return (jint)rc;
}

JNIEXPORT jint JNICALL Java_com_nclink_Native_clientRemoveSample(JNIEnv *env,
                                                                 jclass cls,
                                                                 jlong client,
                                                                 jstring id,
                                                                 jint timeout_ms)
{
    char *raw = from_jstring(env, id);
    int rc;

    (void)cls;
    rc = nclshim_client_remove_sample(HANDLE(client), raw, (unsigned)timeout_ms);
    free(raw);
    return (jint)rc;
}

/* ============================================================== server == */

/*
 * 设备端（ncl_server）。
 *
 * 工具方法回调从库的线程池上抛回 DeviceClient 那样的 Java 对象（这里是
 * com.nclink.Server）：按需 AttachCurrentThread，回调结束再 Detach；
 * 出参字符串用 nclshim_strdup 分配（垫片随后自己 free，跨分配器不算错）。
 */

/** 取当前挂起异常的 message（垫片堆上的副本；调用方按需释放）。 */
static char *jni_exception_text(JNIEnv *env)
{
    jthrowable error = (*env)->ExceptionOccurred(env);
    jclass cls;
    jmethodID mid;
    jstring text;
    char *copy;

    (*env)->ExceptionClear(env);
    if (error == NULL) {
        return nclshim_strdup("Java 回调抛出异常");
    }
    cls = (*env)->GetObjectClass(env, error);
    mid = (*env)->GetMethodID(env, cls, "toString", "()Ljava/lang/String;");
    if (mid == NULL) {
        (*env)->ExceptionClear(env);
        (*env)->DeleteLocalRef(env, cls);
        (*env)->DeleteLocalRef(env, error);
        return nclshim_strdup("Java 回调抛出异常");
    }
    text = (jstring)(*env)->CallObjectMethod(env, error, mid);
    if (text != NULL) {
        const char *chars = (*env)->GetStringUTFChars(env, text, NULL);

        copy = nclshim_strdup(chars != NULL ? chars : "Java 回调抛出异常");
        if (chars != NULL) {
            (*env)->ReleaseStringUTFChars(env, text, chars);
        }
        (*env)->DeleteLocalRef(env, text);
    } else {
        copy = nclshim_strdup("Java 回调抛出异常");
    }
    (*env)->DeleteLocalRef(env, cls);
    (*env)->DeleteLocalRef(env, error);
    return copy;
}

/** 一次工具调用：把 (tool, method, params句柄) 抛回 Java，收 JSON 文本回来。 */
static int jni_invoke_tool(JNIEnv *env, jni_host *host, const char *tool,
                           const char *method, const void *params, char **out_json,
                           char **out_reason)
{
    jclass cls;
    jmethodID mid;
    jstring jtool;
    jstring jmethod;
    jstring result;
    const char *chars;

    cls = (*env)->GetObjectClass(env, host->target);
    if (cls == NULL) {
        return -1;
    }
    mid = (*env)->GetMethodID(env, cls, "onToolNative",
                              "(Ljava/lang/String;Ljava/lang/String;J)Ljava/lang/String;");
    if (mid == NULL) {
        (*env)->ExceptionClear(env);
        (*env)->DeleteLocalRef(env, cls);
        return -1;
    }
    jtool = to_jstring(env, tool);
    jmethod = to_jstring(env, method);
    result = (jstring)(*env)->CallObjectMethod(env, host->target, mid, jtool, jmethod,
                                               PTR(params));
    if ((*env)->ExceptionCheck(env)) {
        *out_reason = jni_exception_text(env);
        if (jtool != NULL) {
            (*env)->DeleteLocalRef(env, jtool);
        }
        if (jmethod != NULL) {
            (*env)->DeleteLocalRef(env, jmethod);
        }
        (*env)->DeleteLocalRef(env, cls);
        return -1;
    }
    if (result != NULL) {
        chars = (*env)->GetStringUTFChars(env, result, NULL);
        *out_json = nclshim_strdup(chars);
        (*env)->ReleaseStringUTFChars(env, result, chars);
        (*env)->DeleteLocalRef(env, result);
    }
    if (jtool != NULL) {
        (*env)->DeleteLocalRef(env, jtool);
    }
    if (jmethod != NULL) {
        (*env)->DeleteLocalRef(env, jmethod);
    }
    (*env)->DeleteLocalRef(env, cls);
    return 0;
}

static int jni_tool_callback(void *user, const char *tool, const char *method,
                            const void *params, char **out_json, char **out_reason)
{
    jni_host *host = (jni_host *)user;
    JNIEnv *env = NULL;
    jboolean attached = JNI_FALSE;
    int rc;

    if (host == NULL || g_vm == NULL) {
        return -1;
    }
    if ((*g_vm)->GetEnv(g_vm, (void **)&env, JNI_VERSION_1_8) != JNI_OK) {
        if ((*g_vm)->AttachCurrentThread(g_vm, (void **)&env, NULL) != JNI_OK) {
            return -1;
        }
        attached = JNI_TRUE;
    }
    if ((*env)->PushLocalFrame(env, 16) != 0) {
        if (attached) {
            (*g_vm)->DetachCurrentThread(g_vm);
        }
        return -1;
    }
    rc = jni_invoke_tool(env, host, tool, method, params, out_json, out_reason);
    (*env)->PopLocalFrame(env, NULL);
    if (attached) {
        (*g_vm)->DetachCurrentThread(g_vm);
    }
    return rc;
}

static int jni_publish_callback(void *user, const char *topic, const void *payload,
                                int payload_len)
{
    jni_host *host = (jni_host *)user;
    JNIEnv *env = NULL;
    jboolean attached = JNI_FALSE;
    jclass cls;
    jmethodID mid;
    jstring jtopic;
    jbyteArray body;

    if (host == NULL || g_vm == NULL) {
        return -1;
    }
    if ((*g_vm)->GetEnv(g_vm, (void **)&env, JNI_VERSION_1_8) != JNI_OK) {
        if ((*g_vm)->AttachCurrentThread(g_vm, (void **)&env, NULL) != JNI_OK) {
            return -1;
        }
        attached = JNI_TRUE;
    }
    if ((*env)->PushLocalFrame(env, 16) != 0) {
        if (attached) {
            (*g_vm)->DetachCurrentThread(g_vm);
        }
        return -1;
    }
    cls = (*env)->GetObjectClass(env, host->target);
    mid = cls != NULL ? (*env)->GetMethodID(env, cls, "onPublishNative",
                                            "(Ljava/lang/String;[B)V")
                      : NULL;
    if (mid != NULL) {
        jtopic = to_jstring(env, topic);
        body = (*env)->NewByteArray(env, payload_len);
        if (body != NULL && payload_len > 0) {
            (*env)->SetByteArrayRegion(env, body, 0, payload_len, (const jbyte *)payload);
        }
        (*env)->CallVoidMethod(env, host->target, mid, jtopic,
                               body != NULL ? body : NULL);
        if ((*env)->ExceptionCheck(env)) {
            (*env)->ExceptionDescribe(env);
            (*env)->ExceptionClear(env);
        }
    } else if (cls != NULL) {
        (*env)->ExceptionClear(env);
    }
    if (cls != NULL) {
        (*env)->DeleteLocalRef(env, cls);
    }
    (*env)->PopLocalFrame(env, NULL);
    if (attached) {
        (*g_vm)->DetachCurrentThread(g_vm);
    }
    return 0;
}

/** 一个设备端 = host（回调引用） + 原生句柄。 */
typedef struct {
    jni_host *tool;      /* 工具回调（可空） */
    jni_host *publish;   /* 自研传输（可空） */
} jni_server_hosts;

static jni_host *make_host(JNIEnv *env, jobject target, void *fn)
{
    jni_host *host;

    if (target == NULL) {
        return NULL;
    }
    host = (jni_host *)calloc(1, sizeof(*host));
    if (host == NULL) {
        return NULL;
    }
    host->target = (*env)->NewGlobalRef(env, target);
    host->slots[0] = fn;
    /* 垫片把 slots[1] 当"用户数据"传给回调，所以这里回指自己。 */
    host->slots[1] = host;
    return host;
}

JNIEXPORT jint JNICALL Java_com_nclink_Native_serverCreate(
    JNIEnv *env, jclass cls, jstring sn, jstring model, jstring broker, jstring user,
    jstring password, jobject publish_target, jlongArray out_server, jlongArray out_host)
{
    char *raw_sn = from_jstring(env, sn);
    char *raw_model = from_jstring(env, model);
    char *raw_broker = from_jstring(env, broker);
    char *raw_user = from_jstring(env, user);
    char *raw_pass = from_jstring(env, password);
    jni_host *publish = NULL;
    const void *server;

    (void)cls;
    set_long(env, out_server, 0, 0);
    set_long(env, out_host, 0, 0);
    if (publish_target != NULL) {
        publish = make_host(env, publish_target, (void *)jni_publish_callback);
        if (publish == NULL) {
            free(raw_sn); free(raw_model); free(raw_broker); free(raw_user); free(raw_pass);
            return -2;                      /* NCL_ERR_NOMEM */
        }
    }
    server = nclshim_server_create(raw_sn, raw_model, raw_broker, raw_user, raw_pass,
                                   publish);
    free(raw_sn); free(raw_model); free(raw_broker); free(raw_user); free(raw_pass);
    if (server == NULL) {
        if (publish != NULL) {
            (*env)->DeleteGlobalRef(env, publish->target);
            free(publish);
        }
        return -12;                         /* NCL_ERR_CONNECT：多半是 broker 连不上 */
    }
    set_long(env, out_server, 0, PTR(server));
    set_long(env, out_host, 0, PTR(publish));
    return 0;
}

JNIEXPORT jint JNICALL Java_com_nclink_Native_serverCreateEx(
    JNIEnv *env, jclass cls, jstring sn, jstring model, jstring broker, jstring user,
    jstring password, jstring ca_file, jstring client_cert, jstring client_key,
    jstring server_name, jboolean verify_peer, jobject publish_target,
    jlongArray out_server, jlongArray out_host)
{
    char *raw_sn = from_jstring(env, sn);
    char *raw_model = from_jstring(env, model);
    char *raw_broker = from_jstring(env, broker);
    char *raw_user = from_jstring(env, user);
    char *raw_pass = from_jstring(env, password);
    char *raw_ca = from_jstring(env, ca_file);
    char *raw_cert = from_jstring(env, client_cert);
    char *raw_key = from_jstring(env, client_key);
    char *raw_name = from_jstring(env, server_name);
    jni_host *publish = NULL;
    const void *server;

    (void)cls;
    set_long(env, out_server, 0, 0);
    set_long(env, out_host, 0, 0);
    if (publish_target != NULL) {
        publish = make_host(env, publish_target, (void *)jni_publish_callback);
        if (publish == NULL) {
            free(raw_sn); free(raw_model); free(raw_broker); free(raw_user);
            free(raw_pass); free(raw_ca); free(raw_cert); free(raw_key); free(raw_name);
            return -2;                      /* NCL_ERR_NOMEM */
        }
    }
    server = nclshim_server_create_ex(raw_sn, raw_model, raw_broker, raw_user, raw_pass,
                                      raw_ca, raw_cert, raw_key, raw_name,
                                      verify_peer != JNI_FALSE ? 1 : 0, publish);
    free(raw_sn); free(raw_model); free(raw_broker); free(raw_user); free(raw_pass);
    free(raw_ca); free(raw_cert); free(raw_key); free(raw_name);
    if (server == NULL) {
        if (publish != NULL) {
            (*env)->DeleteGlobalRef(env, publish->target);
            free(publish);
        }
        return -12;                         /* NCL_ERR_CONNECT：多半是 broker 连不上 */
    }
    set_long(env, out_server, 0, PTR(server));
    set_long(env, out_host, 0, PTR(publish));
    return 0;
}

JNIEXPORT void JNICALL Java_com_nclink_Native_serverFree(JNIEnv *env, jclass cls,
                                                        jlong server)
{
    (void)env;
    (void)cls;
    nclshim_server_free(HANDLE(server));
}

/** 放掉一个回调 host（全局引用 + 结构体）：Server 收尾时对每个 host 调一次。 */
JNIEXPORT void JNICALL Java_com_nclink_Native_hostFree(JNIEnv *env, jclass cls,
                                                       jlong host)
{
    jni_host *entry = (jni_host *)HANDLE(host);

    (void)cls;
    if (entry != NULL) {
        if (entry->target != NULL) {
            (*env)->DeleteGlobalRef(env, entry->target);
        }
        free(entry);
    }
}

JNIEXPORT jstring JNICALL Java_com_nclink_Native_serverSn(JNIEnv *env, jclass cls,
                                                          jlong server)
{
    (void)cls;
    return to_jstring(env, nclshim_server_sn(HANDLE(server)));
}

JNIEXPORT jlong JNICALL Java_com_nclink_Native_serverModel(JNIEnv *env, jclass cls,
                                                           jlong server)
{
    (void)env;
    (void)cls;
    return PTR(nclshim_server_model(HANDLE(server)));
}

JNIEXPORT jstring JNICALL Java_com_nclink_Native_serverModelJson(JNIEnv *env,
                                                                 jclass cls,
                                                                 jlong server)
{
    (void)cls;
    return take_jstring(env, nclshim_server_model_json(HANDLE(server)));
}

JNIEXPORT jstring JNICALL Java_com_nclink_Native_serverOpenapiJson(JNIEnv *env,
                                                                   jclass cls,
                                                                   jlong server,
                                                                   jstring base_url)
{
    char *base = from_jstring(env, base_url);
    char *text = nclshim_server_openapi_json(HANDLE(server), base);
    (void)cls;
    free(base);
    return take_jstring(env, text);
}

#define SERVER_INT(JavaName, ShimFn)                                          \
    JNIEXPORT jint JNICALL Java_com_nclink_Native_##JavaName(                  \
        JNIEnv *env, jclass cls, jlong server)                                 \
    {                                                                          \
        (void)env;                                                             \
        (void)cls;                                                             \
        return (jint)ShimFn(HANDLE(server));                                   \
    }

SERVER_INT(serverBindingCount, nclshim_server_binding_count)
SERVER_INT(serverOperationCount, nclshim_server_operation_count)
SERVER_INT(serverSampleCount, nclshim_server_sample_count)
SERVER_INT(serverSampleUploadCount, nclshim_server_sample_upload_count)
SERVER_INT(serverEventCount, nclshim_server_event_count)
SERVER_INT(serverSubscribe, nclshim_server_subscribe)
SERVER_INT(serverRegisterBuiltinTool, nclshim_server_register_builtin_tool)
SERVER_INT(serverRegisterFileTool, nclshim_server_register_file_tool)
SERVER_INT(serverStartFtp, nclshim_server_start_ftp)
SERVER_INT(serverInitSamples, nclshim_server_init_samples)

JNIEXPORT jint JNICALL Java_com_nclink_Native_serverRegisterTool(
    JNIEnv *env, jclass cls, jlong server, jstring tool, jstring methods,
    jstring bindings, jobject target, jlongArray out_host)
{
    char *raw_tool = from_jstring(env, tool);
    char *raw_methods = from_jstring(env, methods);
    char *raw_bindings = from_jstring(env, bindings);
    jni_host *host = make_host(env, target, (void *)jni_tool_callback);
    int rc;

    (void)cls;
    set_long(env, out_host, 0, 0);
    if (host == NULL) {
        free(raw_tool); free(raw_methods); free(raw_bindings);
        return -2;
    }
    rc = nclshim_server_register_tool(HANDLE(server), raw_tool, raw_methods,
                                      raw_bindings, host);
    free(raw_tool); free(raw_methods); free(raw_bindings);
    if (rc != 0) {
        (*env)->DeleteGlobalRef(env, host->target);
        free(host);
        return (jint)rc;
    }
    /* host 交给 Java 存着（close() 时 hostFree 连同全局引用一起放掉）。 */
    set_long(env, out_host, 0, PTR(host));
    return 0;
}

/* ================================================================ file == */

JNIEXPORT jint JNICALL Java_com_nclink_Native_fileStartFtp(JNIEnv *env, jclass cls)
{
    (void)env;
    (void)cls;
    return (jint)nclshim_file_start_ftp();
}

JNIEXPORT jint JNICALL Java_com_nclink_Native_fileStartFtpEx(JNIEnv *env, jclass cls,
                                                            jint port, jstring root,
                                                            jstring user,
                                                            jstring password)
{
    char *raw_root = from_jstring(env, root);
    char *raw_user = from_jstring(env, user);
    char *raw_password = from_jstring(env, password);
    int rc;

    (void)cls;
    rc = nclshim_file_start_ftp_ex((unsigned)port, raw_root, raw_user, raw_password);
    free(raw_root);
    free(raw_user);
    free(raw_password);
    return (jint)rc;
}

JNIEXPORT jint JNICALL Java_com_nclink_Native_serverSetFilePeer(
    JNIEnv *env, jclass cls, jlong server, jstring host, jint port, jstring user,
    jstring password)
{
    char *raw_host = from_jstring(env, host);
    char *raw_user = from_jstring(env, user);
    char *raw_password = from_jstring(env, password);
    int rc;

    (void)cls;
    rc = nclshim_server_set_file_peer(HANDLE(server), raw_host, (unsigned)port,
                                      raw_user, raw_password);
    free(raw_host);
    free(raw_user);
    free(raw_password);
    return (jint)rc;
}

JNIEXPORT void JNICALL Java_com_nclink_Native_fileStopFtp(JNIEnv *env, jclass cls)
{
    (void)env;
    (void)cls;
    nclshim_file_stop_ftp();
}

JNIEXPORT jint JNICALL Java_com_nclink_Native_clientFileWrite(JNIEnv *env, jclass cls,
                                                             jlong client,
                                                             jstring local_path)
{
    char *raw = from_jstring(env, local_path);
    int rc;

    (void)cls;
    rc = nclshim_client_file_write(HANDLE(client), raw);
    free(raw);
    return (jint)rc;
}

JNIEXPORT jstring JNICALL Java_com_nclink_Native_clientFileRead(JNIEnv *env,
                                                               jclass cls,
                                                               jlong client,
                                                               jstring remote_path)
{
    char *raw = from_jstring(env, remote_path);
    char *local = nclshim_client_file_read(HANDLE(client), raw);

    (void)cls;
    free(raw);
    return take_jstring(env, local);
}

JNIEXPORT jstring JNICALL Java_com_nclink_Native_clientFileLlJson(JNIEnv *env,
                                                                 jclass cls,
                                                                 jlong client,
                                                                 jstring remote_dir)
{
    char *raw = from_jstring(env, remote_dir);
    char *json = nclshim_client_file_ll_json(HANDLE(client), raw);

    (void)cls;
    free(raw);
    return take_jstring(env, json);
}

JNIEXPORT jint JNICALL Java_com_nclink_Native_clientFileMkdir(JNIEnv *env, jclass cls,
                                                             jlong client,
                                                             jstring remote_dir)
{
    char *raw = from_jstring(env, remote_dir);
    int rc;

    (void)cls;
    rc = nclshim_client_file_mkdir(HANDLE(client), raw);
    free(raw);
    return (jint)rc;
}

JNIEXPORT jint JNICALL Java_com_nclink_Native_clientFileDelete(JNIEnv *env, jclass cls,
                                                              jlong client,
                                                              jstring remote_path)
{
    char *raw = from_jstring(env, remote_path);
    int rc;

    (void)cls;
    rc = nclshim_client_file_delete(HANDLE(client), raw);
    free(raw);
    return (jint)rc;
}

JNIEXPORT jint JNICALL Java_com_nclink_Native_clientMethodCallFile(
    JNIEnv *env, jclass cls, jlong client, jstring method, jstring params,
    jstring keys, jstring paths, jint timeout_ms, jobjectArray out)
{
    char *raw_method = from_jstring(env, method);
    char *raw_params = from_jstring(env, params);
    char *raw_keys = from_jstring(env, keys);
    char *raw_paths = from_jstring(env, paths);
    char *reply = NULL;
    int rc;

    (void)cls;
    rc = nclshim_client_method_call_file(HANDLE(client), raw_method, raw_params,
                                         raw_keys, raw_paths,
                                         (unsigned)timeout_ms, &reply);
    set_string(env, out, 0, reply);
    free(raw_method);
    free(raw_params);
    free(raw_keys);
    free(raw_paths);
    return (jint)rc;
}

JNIEXPORT jint JNICALL Java_com_nclink_Native_fileNeedCompression(JNIEnv *env,
                                                                 jclass cls,
                                                                 jstring file_name)
{
    char *raw = from_jstring(env, file_name);
    int rc;

    (void)cls;
    rc = nclshim_file_need_compression(raw);
    free(raw);
    return (jint)rc;
}

JNIEXPORT jint JNICALL Java_com_nclink_Native_fileTotalChunks(JNIEnv *env, jclass cls,
                                                             jlong size)
{
    (void)env;
    (void)cls;
    return (jint)nclshim_file_total_chunks((long long)size);
}

JNIEXPORT jstring JNICALL Java_com_nclink_Native_fileChecksum(JNIEnv *env, jclass cls,
                                                             jstring path)
{
    char *raw = from_jstring(env, path);
    char *hex = nclshim_file_checksum(raw);

    (void)cls;
    free(raw);
    return take_jstring(env, hex);
}

JNIEXPORT jstring JNICALL Java_com_nclink_Native_fileAttributeJson(
    JNIEnv *env, jclass cls, jstring path, jstring parent)
{
    char *raw_path = from_jstring(env, path);
    char *raw_parent = from_jstring(env, parent);
    char *json = nclshim_file_attribute_json(raw_path, raw_parent);

    (void)cls;
    free(raw_path);
    free(raw_parent);
    return take_jstring(env, json);
}

JNIEXPORT jint JNICALL Java_com_nclink_Native_serverDispatch(
    JNIEnv *env, jclass cls, jlong server, jstring topic, jbyteArray payload,
    jobjectArray out)
{
    char *raw_topic = from_jstring(env, topic);
    jbyte *bytes;
    jsize length;
    char *reply = NULL;
    int rc;

    (void)cls;
    if (payload == NULL) {
        free(raw_topic);
        return -5;
    }
    length = (*env)->GetArrayLength(env, payload);
    bytes = (*env)->GetByteArrayElements(env, payload, NULL);
    if (bytes == NULL) {
        free(raw_topic);
        return -2;
    }
    rc = nclshim_server_dispatch(HANDLE(server), raw_topic, bytes, (int)length, &reply);
    (*env)->ReleaseByteArrayElements(env, payload, bytes, JNI_ABORT);
    free(raw_topic);
    set_string(env, out, 0, reply);
    return (jint)rc;
}

static jint server_call_method(JNIEnv *env, jlong server, jstring method,
                               jstring params, jboolean check, jobjectArray out)
{
    char *raw_method = from_jstring(env, method);
    char *raw_params = from_jstring(env, params);
    char *reply = NULL;
    int rc;

    rc = check != JNI_FALSE
             ? nclshim_server_check_method_call(HANDLE(server), raw_method, raw_params,
                                                &reply)
             : nclshim_server_invoke_method_call(HANDLE(server), raw_method, raw_params,
                                                 &reply);
    free(raw_method);
    free(raw_params);
    set_string(env, out, 0, reply);
    return (jint)rc;
}

JNIEXPORT jint JNICALL Java_com_nclink_Native_serverInvokeMethodCall(
    JNIEnv *env, jclass cls, jlong server, jstring method, jstring params,
    jobjectArray out)
{
    (void)cls;
    return server_call_method(env, server, method, params, JNI_FALSE, out);
}

JNIEXPORT jint JNICALL Java_com_nclink_Native_serverCheckMethodCall(
    JNIEnv *env, jclass cls, jlong server, jstring method, jstring params,
    jobjectArray out)
{
    (void)cls;
    return server_call_method(env, server, method, params, JNI_TRUE, out);
}

JNIEXPORT jint JNICALL Java_com_nclink_Native_serverAddSample(JNIEnv *env, jclass cls,
                                                             jlong server, jstring config)
{
    char *raw = from_jstring(env, config);
    int rc;

    (void)cls;
    rc = nclshim_server_add_sample(HANDLE(server), raw);
    free(raw);
    return (jint)rc;
}

JNIEXPORT jint JNICALL Java_com_nclink_Native_serverRemoveSample(JNIEnv *env,
                                                                 jclass cls,
                                                                 jlong server,
                                                                 jstring id)
{
    char *raw = from_jstring(env, id);
    int rc;

    (void)cls;
    rc = nclshim_server_remove_sample(HANDLE(server), raw);
    free(raw);
    return (jint)rc;
}

JNIEXPORT void JNICALL Java_com_nclink_Native_serverStopAllSamples(JNIEnv *env,
                                                                   jclass cls,
                                                                   jlong server)
{
    (void)env;
    (void)cls;
    nclshim_server_stop_all_samples(HANDLE(server));
}

JNIEXPORT jint JNICALL Java_com_nclink_Native_serverPushEvent(
    JNIEnv *env, jclass cls, jlong server, jstring event_id, jstring event,
    jlong time_ms, jstring message_id)
{
    char *raw_id = from_jstring(env, event_id);
    char *raw_event = from_jstring(env, event);
    char *raw_message = from_jstring(env, message_id);
    int rc;

    (void)cls;
    if (message_id == NULL && time_ms < 0) {
        rc = nclshim_server_push_event(HANDLE(server), raw_id, raw_event);
    } else {
        rc = nclshim_server_push_event_ex(HANDLE(server), raw_id, raw_event,
                                          (long long)time_ms, raw_message);
    }
    free(raw_id);
    free(raw_event);
    free(raw_message);
    return (jint)rc;
}

/* ============================================================== http/rest == */

/** 一次自定义路由调用：Java 侧返回 String[]{status, content_type, body}。 */
static int jni_route_callback(void *user, const char *method, const char *path,
                              const char *query, const char *body, int *out_status,
                              char **out_content_type, char **out_body)
{
    jni_host *host = (jni_host *)user;
    JNIEnv *env = NULL;
    jboolean attached = JNI_FALSE;
    jclass cls;
    jmethodID mid;
    jstring jmethod;
    jstring jpath;
    jstring jquery;
    jstring jbody;
    jobjectArray result;

    if (host == NULL || g_vm == NULL) {
        return -1;
    }
    if ((*g_vm)->GetEnv(g_vm, (void **)&env, JNI_VERSION_1_8) != JNI_OK) {
        if ((*g_vm)->AttachCurrentThread(g_vm, (void **)&env, NULL) != JNI_OK) {
            return -1;
        }
        attached = JNI_TRUE;
    }
    if ((*env)->PushLocalFrame(env, 24) != 0) {
        if (attached) {
            (*g_vm)->DetachCurrentThread(g_vm);
        }
        return -1;
    }
    cls = (*env)->GetObjectClass(env, host->target);
    mid = cls != NULL
              ? (*env)->GetMethodID(env, cls, "onRouteNative",
                                    "(Ljava/lang/String;Ljava/lang/String;"
                                    "Ljava/lang/String;Ljava/lang/String;)[Ljava/lang/String;")
              : NULL;
    if (mid == NULL) {
        if (cls != NULL) {
            (*env)->ExceptionClear(env);
        }
        (*env)->PopLocalFrame(env, NULL);
        if (attached) {
            (*g_vm)->DetachCurrentThread(g_vm);
        }
        return -1;
    }
    jmethod = to_jstring(env, method);
    jpath = to_jstring(env, path);
    jquery = to_jstring(env, query);
    jbody = to_jstring(env, body);
    result = (jobjectArray)(*env)->CallObjectMethod(env, host->target, mid, jmethod,
                                                    jpath, jquery, jbody);
    if ((*env)->ExceptionCheck(env)) {
        (*env)->ExceptionDescribe(env);
        (*env)->ExceptionClear(env);
        result = NULL;
    }
    if (result != NULL && (*env)->GetArrayLength(env, result) >= 3) {
        jstring status = (jstring)(*env)->GetObjectArrayElement(env, result, 0);
        jstring type = (jstring)(*env)->GetObjectArrayElement(env, result, 1);
        jstring data = (jstring)(*env)->GetObjectArrayElement(env, result, 2);
        const char *chars;

        if (status != NULL) {
            chars = (*env)->GetStringUTFChars(env, status, NULL);
            if (chars != NULL) {
                *out_status = atoi(chars);
                (*env)->ReleaseStringUTFChars(env, status, chars);
            }
        }
        if (type != NULL) {
            chars = (*env)->GetStringUTFChars(env, type, NULL);
            if (chars != NULL) {
                *out_content_type = nclshim_strdup(chars);
                (*env)->ReleaseStringUTFChars(env, type, chars);
            }
        }
        if (data != NULL) {
            chars = (*env)->GetStringUTFChars(env, data, NULL);
            if (chars != NULL) {
                *out_body = nclshim_strdup(chars);
                (*env)->ReleaseStringUTFChars(env, data, chars);
            }
        }
    }
    (*env)->PopLocalFrame(env, NULL);
    if (attached) {
        (*g_vm)->DetachCurrentThread(g_vm);
    }
    return 0;
}

JNIEXPORT jlong JNICALL Java_com_nclink_Native_httpStart(JNIEnv *env, jclass cls,
                                                        jlong server, jint port,
                                                        jboolean with_config)
{
    (void)env;
    (void)cls;
    return PTR(nclshim_http_start((unsigned)port, HANDLE(server),
                                  with_config != JNI_FALSE ? 1 : 0));
}

JNIEXPORT void JNICALL Java_com_nclink_Native_httpFree(JNIEnv *env, jclass cls,
                                                       jlong http)
{
    (void)env;
    (void)cls;
    nclshim_http_free(HANDLE(http));
}

JNIEXPORT jint JNICALL Java_com_nclink_Native_httpPort(JNIEnv *env, jclass cls,
                                                       jlong http)
{
    (void)env;
    (void)cls;
    return (jint)nclshim_http_port(HANDLE(http));
}

JNIEXPORT jint JNICALL Java_com_nclink_Native_httpRequestCount(JNIEnv *env, jclass cls,
                                                               jlong http)
{
    (void)env;
    (void)cls;
    return (jint)nclshim_http_request_count(HANDLE(http));
}

JNIEXPORT void JNICALL Java_com_nclink_Native_httpSetCors(JNIEnv *env, jclass cls,
                                                          jlong http, jboolean enabled)
{
    (void)env;
    (void)cls;
    nclshim_http_set_cors(HANDLE(http), enabled != JNI_FALSE ? 1 : 0);
}

JNIEXPORT jint JNICALL Java_com_nclink_Native_httpRoute(JNIEnv *env, jclass cls,
                                                        jlong http, jstring method,
                                                        jstring path, jobject target,
                                                        jlongArray out_host)
{
    char *raw_method = from_jstring(env, method);
    char *raw_path = from_jstring(env, path);
    jni_host *host = make_host(env, target, (void *)jni_route_callback);
    int rc;

    (void)cls;
    set_long(env, out_host, 0, 0);
    if (host == NULL) {
        free(raw_method);
        free(raw_path);
        return -2;
    }
    rc = nclshim_http_route(HANDLE(http), raw_method, raw_path, host);
    free(raw_method);
    free(raw_path);
    if (rc != 0) {
        (*env)->DeleteGlobalRef(env, host->target);
        free(host);
        return (jint)rc;
    }
    set_long(env, out_host, 0, PTR(host));
    return 0;
}

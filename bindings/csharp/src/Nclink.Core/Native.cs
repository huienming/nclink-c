// SPDX-License-Identifier: MIT
// Copyright (c) 2026 huienming

using System;
using System.Runtime.InteropServices;
using System.Text;

namespace Nclink
{
    /// <summary>
    /// 原生垫片（nclink_shim）的 P/Invoke 声明。
    ///
    /// 库里没有导出符号（静态库），而且托管侧不该碰 C 结构体布局，所以所有调用都经
    /// 过 bindings/native/nclink_shim.c —— 它只暴露不透明句柄、标量和 UTF-8
    /// 文本。字符串统一按 UTF-8 手工编解码（不用 PtrToStringAnsi：那在 Windows 上走
    /// 的是 ANSI 代码页，中文会乱）。
    /// </summary>
    internal static class Native
    {
        internal const string Dll = "nclink_shim";

        /* ------------------------------------------------------------- misc -- */

        [DllImport(Dll, CallingConvention = CallingConvention.Cdecl, EntryPoint = "nclshim_version")]
        internal static extern IntPtr Version();

        [DllImport(Dll, CallingConvention = CallingConvention.Cdecl, EntryPoint = "nclshim_err_name")]
        internal static extern IntPtr ErrName(int code);

        [DllImport(Dll, CallingConvention = CallingConvention.Cdecl, EntryPoint = "nclshim_free")]
        internal static extern void Free(IntPtr ptr);

    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl, EntryPoint = "nclshim_env_set_root")]
    internal static extern void EnvSetRoot(byte[] root);

    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl, EntryPoint = "nclshim_device_model")]
    internal static extern IntPtr DeviceModel();

        [DllImport(Dll, CallingConvention = CallingConvention.Cdecl, EntryPoint = "nclshim_env_root")]
        internal static extern IntPtr EnvRoot();

        [DllImport(Dll, CallingConvention = CallingConvention.Cdecl, EntryPoint = "nclshim_log_init")]
        internal static extern int LogInit(byte[] dir);

        [DllImport(Dll, CallingConvention = CallingConvention.Cdecl, EntryPoint = "nclshim_log_shutdown")]
        internal static extern void LogShutdown();

        [DllImport(Dll, CallingConvention = CallingConvention.Cdecl, EntryPoint = "nclshim_log_set_level")]
        internal static extern void LogSetLevel(int level);

        [DllImport(Dll, CallingConvention = CallingConvention.Cdecl, EntryPoint = "nclshim_log_set_console")]
        internal static extern void LogSetConsole(int enabled);

        /* ------------------------------------------------------------- json -- */

        [DllImport(Dll, CallingConvention = CallingConvention.Cdecl, EntryPoint = "nclshim_json_parse")]
        internal static extern IntPtr JsonParse(byte[] text);

        [DllImport(Dll, CallingConvention = CallingConvention.Cdecl, EntryPoint = "nclshim_json_clone")]
        internal static extern IntPtr JsonClone(IntPtr json);

        [DllImport(Dll, CallingConvention = CallingConvention.Cdecl, EntryPoint = "nclshim_json_free")]
        internal static extern void JsonFree(IntPtr json);

        [DllImport(Dll, CallingConvention = CallingConvention.Cdecl, EntryPoint = "nclshim_json_write")]
        internal static extern IntPtr JsonWrite(IntPtr json);

        [DllImport(Dll, CallingConvention = CallingConvention.Cdecl, EntryPoint = "nclshim_json_type")]
        internal static extern int JsonType(IntPtr json);

        [DllImport(Dll, CallingConvention = CallingConvention.Cdecl, EntryPoint = "nclshim_json_is_null")]
        internal static extern int JsonIsNull(IntPtr json);

        [DllImport(Dll, CallingConvention = CallingConvention.Cdecl, EntryPoint = "nclshim_json_as_int")]
        internal static extern int JsonAsInt(IntPtr json, out long value);

        [DllImport(Dll, CallingConvention = CallingConvention.Cdecl, EntryPoint = "nclshim_json_as_double")]
        internal static extern int JsonAsDouble(IntPtr json, out double value);

        [DllImport(Dll, CallingConvention = CallingConvention.Cdecl, EntryPoint = "nclshim_json_as_bool")]
        internal static extern int JsonAsBool(IntPtr json, out int value);

        [DllImport(Dll, CallingConvention = CallingConvention.Cdecl, EntryPoint = "nclshim_json_string")]
        internal static extern IntPtr JsonString(IntPtr json);

        [DllImport(Dll, CallingConvention = CallingConvention.Cdecl, EntryPoint = "nclshim_json_text")]
        internal static extern IntPtr JsonText(IntPtr json);

        [DllImport(Dll, CallingConvention = CallingConvention.Cdecl, EntryPoint = "nclshim_json_count")]
        internal static extern int JsonCount(IntPtr json);

        [DllImport(Dll, CallingConvention = CallingConvention.Cdecl, EntryPoint = "nclshim_json_array_get")]
        internal static extern IntPtr JsonArrayGet(IntPtr json, int index);

        [DllImport(Dll, CallingConvention = CallingConvention.Cdecl, EntryPoint = "nclshim_json_object_value_at")]
        internal static extern IntPtr JsonObjectValueAt(IntPtr json, int index);

        [DllImport(Dll, CallingConvention = CallingConvention.Cdecl, EntryPoint = "nclshim_json_object_key_at")]
        internal static extern IntPtr JsonObjectKeyAt(IntPtr json, int index);

        [DllImport(Dll, CallingConvention = CallingConvention.Cdecl, EntryPoint = "nclshim_json_object_get")]
        internal static extern IntPtr JsonObjectGet(IntPtr json, byte[] key);

        /* ------------------------------------------------------------ model -- */

        [DllImport(Dll, CallingConvention = CallingConvention.Cdecl, EntryPoint = "nclshim_model_parse")]
        internal static extern IntPtr ModelParse(byte[] json);

        [DllImport(Dll, CallingConvention = CallingConvention.Cdecl, EntryPoint = "nclshim_model_free")]
        internal static extern void ModelFree(IntPtr root);

        [DllImport(Dll, CallingConvention = CallingConvention.Cdecl, EntryPoint = "nclshim_model_write")]
        internal static extern IntPtr ModelWrite(IntPtr root);

        [DllImport(Dll, CallingConvention = CallingConvention.Cdecl, EntryPoint = "nclshim_model_find_by_id")]
        internal static extern IntPtr ModelFindById(IntPtr root, byte[] id);

        [DllImport(Dll, CallingConvention = CallingConvention.Cdecl, EntryPoint = "nclshim_node_type")]
        internal static extern int NodeType(IntPtr node);

        [DllImport(Dll, CallingConvention = CallingConvention.Cdecl, EntryPoint = "nclshim_node_type_name")]
        internal static extern IntPtr NodeTypeName(IntPtr node);

        [DllImport(Dll, CallingConvention = CallingConvention.Cdecl, EntryPoint = "nclshim_node_name")]
        internal static extern IntPtr NodeName(IntPtr node);

        [DllImport(Dll, CallingConvention = CallingConvention.Cdecl, EntryPoint = "nclshim_node_id")]
        internal static extern IntPtr NodeId(IntPtr node);

        [DllImport(Dll, CallingConvention = CallingConvention.Cdecl, EntryPoint = "nclshim_node_path")]
        internal static extern IntPtr NodePath(IntPtr node);

        [DllImport(Dll, CallingConvention = CallingConvention.Cdecl, EntryPoint = "nclshim_node_description")]
        internal static extern IntPtr NodeDescription(IntPtr node);

        [DllImport(Dll, CallingConvention = CallingConvention.Cdecl, EntryPoint = "nclshim_node_number")]
        internal static extern IntPtr NodeNumber(IntPtr node);

        [DllImport(Dll, CallingConvention = CallingConvention.Cdecl, EntryPoint = "nclshim_node_data_type")]
        internal static extern IntPtr NodeDataType(IntPtr node);

        [DllImport(Dll, CallingConvention = CallingConvention.Cdecl, EntryPoint = "nclshim_node_value_type")]
        internal static extern IntPtr NodeValueType(IntPtr node);

        [DllImport(Dll, CallingConvention = CallingConvention.Cdecl, EntryPoint = "nclshim_node_mapping")]
        internal static extern IntPtr NodeMapping(IntPtr node);

        [DllImport(Dll, CallingConvention = CallingConvention.Cdecl, EntryPoint = "nclshim_node_source")]
        internal static extern IntPtr NodeSource(IntPtr node);

        [DllImport(Dll, CallingConvention = CallingConvention.Cdecl, EntryPoint = "nclshim_node_version")]
        internal static extern IntPtr NodeVersion(IntPtr node);

        [DllImport(Dll, CallingConvention = CallingConvention.Cdecl, EntryPoint = "nclshim_node_guid")]
        internal static extern IntPtr NodeGuid(IntPtr node);

        [DllImport(Dll, CallingConvention = CallingConvention.Cdecl, EntryPoint = "nclshim_node_settable")]
        internal static extern int NodeSettable(IntPtr node);

        [DllImport(Dll, CallingConvention = CallingConvention.Cdecl, EntryPoint = "nclshim_node_value")]
        internal static extern IntPtr NodeValue(IntPtr node);

        [DllImport(Dll, CallingConvention = CallingConvention.Cdecl, EntryPoint = "nclshim_node_is_sample_channel")]
        internal static extern int NodeIsSampleChannel(IntPtr node);

        [DllImport(Dll, CallingConvention = CallingConvention.Cdecl, EntryPoint = "nclshim_node_sample_interval")]
        internal static extern long NodeSampleInterval(IntPtr node);

        [DllImport(Dll, CallingConvention = CallingConvention.Cdecl, EntryPoint = "nclshim_node_upload_interval")]
        internal static extern long NodeUploadInterval(IntPtr node);

        [DllImport(Dll, CallingConvention = CallingConvention.Cdecl, EntryPoint = "nclshim_node_sample_item_count")]
        internal static extern int NodeSampleItemCount(IntPtr node);

        [DllImport(Dll, CallingConvention = CallingConvention.Cdecl, EntryPoint = "nclshim_node_sample_item_path")]
        internal static extern IntPtr NodeSampleItemPath(IntPtr node, int index);

        [DllImport(Dll, CallingConvention = CallingConvention.Cdecl, EntryPoint = "nclshim_node_count")]
        internal static extern int NodeCount(IntPtr node, int kind);

        [DllImport(Dll, CallingConvention = CallingConvention.Cdecl, EntryPoint = "nclshim_node_at")]
        internal static extern IntPtr NodeAt(IntPtr node, int kind, int index);

        /* -------------------------------------------------- message / sample -- */

        [DllImport(Dll, CallingConvention = CallingConvention.Cdecl, EntryPoint = "nclshim_message_parse")]
        internal static extern IntPtr MessageParse(byte[] topic, byte[] payload, int payloadLen);

        [DllImport(Dll, CallingConvention = CallingConvention.Cdecl, EntryPoint = "nclshim_message_free")]
        internal static extern void MessageFree(IntPtr msg);

        [DllImport(Dll, CallingConvention = CallingConvention.Cdecl, EntryPoint = "nclshim_message_type")]
        internal static extern int MessageType(IntPtr msg);

        [DllImport(Dll, CallingConvention = CallingConvention.Cdecl, EntryPoint = "nclshim_message_write")]
        internal static extern IntPtr MessageWrite(IntPtr msg);

        [DllImport(Dll, CallingConvention = CallingConvention.Cdecl, EntryPoint = "nclshim_sample_rows")]
        internal static extern int SampleRows(IntPtr msg);

        [DllImport(Dll, CallingConvention = CallingConvention.Cdecl, EntryPoint = "nclshim_sample_columns")]
        internal static extern int SampleColumns(IntPtr msg);

        [DllImport(Dll, CallingConvention = CallingConvention.Cdecl, EntryPoint = "nclshim_sample_is_complete")]
        internal static extern int SampleIsComplete(IntPtr msg);

        [DllImport(Dll, CallingConvention = CallingConvention.Cdecl, EntryPoint = "nclshim_sample_id")]
        internal static extern IntPtr SampleId(IntPtr msg);

        [DllImport(Dll, CallingConvention = CallingConvention.Cdecl, EntryPoint = "nclshim_sample_begin_time")]
        internal static extern IntPtr SampleBeginTime(IntPtr msg);

        [DllImport(Dll, CallingConvention = CallingConvention.Cdecl, EntryPoint = "nclshim_sample_interval")]
        internal static extern long SampleInterval(IntPtr msg);

        [DllImport(Dll, CallingConvention = CallingConvention.Cdecl, EntryPoint = "nclshim_sample_upload_interval")]
        internal static extern long SampleUploadInterval(IntPtr msg);

        [DllImport(Dll, CallingConvention = CallingConvention.Cdecl, EntryPoint = "nclshim_sample_path")]
        internal static extern IntPtr SamplePath(IntPtr msg, int col);

        [DllImport(Dll, CallingConvention = CallingConvention.Cdecl, EntryPoint = "nclshim_sample_header")]
        internal static extern IntPtr SampleHeader(IntPtr msg, byte[] separator);

        [DllImport(Dll, CallingConvention = CallingConvention.Cdecl, EntryPoint = "nclshim_sample_column_slots")]
        internal static extern int SampleColumnSlots(IntPtr msg, int col);

        [DllImport(Dll, CallingConvention = CallingConvention.Cdecl, EntryPoint = "nclshim_sample_column_points")]
        internal static extern int SampleColumnPoints(IntPtr msg, int col);

        [DllImport(Dll, CallingConvention = CallingConvention.Cdecl, EntryPoint = "nclshim_sample_column_nested")]
        internal static extern int SampleColumnNested(IntPtr msg, int col);

        [DllImport(Dll, CallingConvention = CallingConvention.Cdecl, EntryPoint = "nclshim_sample_column_encoding")]
        internal static extern IntPtr SampleColumnEncoding(IntPtr msg, int col);

        [DllImport(Dll, CallingConvention = CallingConvention.Cdecl, EntryPoint = "nclshim_sample_value_at")]
        internal static extern IntPtr SampleValueAt(IntPtr msg, int row, int col);

        [DllImport(Dll, CallingConvention = CallingConvention.Cdecl, EntryPoint = "nclshim_sample_column_value_at")]
        internal static extern IntPtr SampleColumnValueAt(IntPtr msg, int col, int index);

        [DllImport(Dll, CallingConvention = CallingConvention.Cdecl, EntryPoint = "nclshim_event_id")]
        internal static extern IntPtr EventId(IntPtr msg);

        [DllImport(Dll, CallingConvention = CallingConvention.Cdecl, EntryPoint = "nclshim_event_time")]
        internal static extern IntPtr EventTime(IntPtr msg);

        [DllImport(Dll, CallingConvention = CallingConvention.Cdecl, EntryPoint = "nclshim_event_key")]
        internal static extern IntPtr EventKey(IntPtr msg);

        [DllImport(Dll, CallingConvention = CallingConvention.Cdecl, EntryPoint = "nclshim_event_value")]
        internal static extern IntPtr EventValue(IntPtr msg);

        /* ------------------------------------------------------------ client -- */

        [DllImport(Dll, CallingConvention = CallingConvention.Cdecl, EntryPoint = "nclshim_open")]
        internal static extern int Open(byte[] uri, byte[] user, byte[] password);

        [DllImport(Dll, CallingConvention = CallingConvention.Cdecl, EntryPoint = "nclshim_open_ex")]
        internal static extern int OpenEx(byte[] uri, byte[] user, byte[] password, byte[] caFile, byte[] clientCert, byte[] clientKey, byte[] serverName, int verifyPeer);

        [DllImport(Dll, CallingConvention = CallingConvention.Cdecl, EntryPoint = "nclshim_tls_available")]
        internal static extern int TlsAvailable();

        [DllImport(Dll, CallingConvention = CallingConvention.Cdecl, EntryPoint = "nclshim_close")]
        internal static extern void Close();

        [DllImport(Dll, CallingConvention = CallingConvention.Cdecl, EntryPoint = "nclshim_is_open")]
        internal static extern int IsOpen();

        [DllImport(Dll, CallingConvention = CallingConvention.Cdecl, EntryPoint = "nclshim_client_get")]
        internal static extern IntPtr ClientGet(byte[] sn);

        [DllImport(Dll, CallingConvention = CallingConvention.Cdecl, EntryPoint = "nclshim_client_probe")]
        internal static extern int ClientProbe(IntPtr client, uint timeoutMs, out IntPtr model);

        [DllImport(Dll, CallingConvention = CallingConvention.Cdecl, EntryPoint = "nclshim_client_set_root_node")]
        internal static extern int ClientSetRootNode(IntPtr client, IntPtr root);

        [DllImport(Dll, CallingConvention = CallingConvention.Cdecl, EntryPoint = "nclshim_client_get_value")]
        internal static extern int ClientGetValue(IntPtr client, byte[] path, uint timeoutMs, out IntPtr value);

        [DllImport(Dll, CallingConvention = CallingConvention.Cdecl, EntryPoint = "nclshim_client_get_value_range")]
        internal static extern int ClientGetValueRange(IntPtr client, byte[] path, int start, int end, uint timeoutMs, out IntPtr value);

        [DllImport(Dll, CallingConvention = CallingConvention.Cdecl, EntryPoint = "nclshim_client_get_length")]
        internal static extern int ClientGetLength(IntPtr client, byte[] path, uint timeoutMs, out long length);

        [DllImport(Dll, CallingConvention = CallingConvention.Cdecl, EntryPoint = "nclshim_client_set_value")]
        internal static extern int ClientSetValue(IntPtr client, byte[] path, byte[] valueJson, uint timeoutMs);

        [DllImport(Dll, CallingConvention = CallingConvention.Cdecl, EntryPoint = "nclshim_client_set_value_index")]
        internal static extern int ClientSetValueIndex(IntPtr client, byte[] path, byte[] valueJson, int index, uint timeoutMs);

        [DllImport(Dll, CallingConvention = CallingConvention.Cdecl, EntryPoint = "nclshim_client_method_call")]
        internal static extern int ClientMethodCall(IntPtr client, byte[] method, byte[] paramsJson, int check, uint timeoutMs, out IntPtr responseJson);

        [DllImport(Dll, CallingConvention = CallingConvention.Cdecl, EntryPoint = "nclshim_client_method_call_async")]
        internal static extern int ClientMethodCallAsync(IntPtr client, byte[] method, byte[] paramsJson, uint timeoutMs, out IntPtr responseJson);

        [DllImport(Dll, CallingConvention = CallingConvention.Cdecl, EntryPoint = "nclshim_client_method_status")]
        internal static extern int ClientMethodStatus(IntPtr client, byte[] objectId, byte[] handler, uint timeoutMs, out IntPtr responseJson);

        [DllImport(Dll, CallingConvention = CallingConvention.Cdecl, EntryPoint = "nclshim_client_method_result")]
        internal static extern int ClientMethodResult(IntPtr client, byte[] objectId, byte[] handler, uint timeoutMs, out IntPtr responseJson);

        [DllImport(Dll, CallingConvention = CallingConvention.Cdecl, EntryPoint = "nclshim_client_ping")]
        internal static extern int ClientPing(IntPtr client, uint timeoutMs);

        [DllImport(Dll, CallingConvention = CallingConvention.Cdecl, EntryPoint = "nclshim_client_get_id")]
        internal static extern IntPtr ClientGetId(IntPtr client, byte[] path);

        [DllImport(Dll, CallingConvention = CallingConvention.Cdecl, EntryPoint = "nclshim_client_get_path")]
        internal static extern IntPtr ClientGetPath(IntPtr client, byte[] id);

        [DllImport(Dll, CallingConvention = CallingConvention.Cdecl, EntryPoint = "nclshim_client_methods_json")]
        internal static extern int ClientMethodsJson(IntPtr client, out IntPtr json);

        [DllImport(Dll, CallingConvention = CallingConvention.Cdecl, EntryPoint = "nclshim_client_find_method_json")]
        internal static extern int ClientFindMethodJson(IntPtr client, byte[] address, out IntPtr json);

        [DllImport(Dll, CallingConvention = CallingConvention.Cdecl, EntryPoint = "nclshim_client_subscribe_samples")]
        internal static extern int ClientSubscribeSamples(IntPtr client, int qos, IntPtr host);

        [DllImport(Dll, CallingConvention = CallingConvention.Cdecl, EntryPoint = "nclshim_client_unsubscribe_samples")]
        internal static extern int ClientUnsubscribeSamples(IntPtr client);

        [DllImport(Dll, CallingConvention = CallingConvention.Cdecl, EntryPoint = "nclshim_client_subscribe_events")]
        internal static extern int ClientSubscribeEvents(IntPtr client, int qos, IntPtr host);

        [DllImport(Dll, CallingConvention = CallingConvention.Cdecl, EntryPoint = "nclshim_client_unsubscribe_events")]
        internal static extern int ClientUnsubscribeEvents(IntPtr client);

        [DllImport(Dll, CallingConvention = CallingConvention.Cdecl, EntryPoint = "nclshim_client_sample_count")]
        internal static extern int ClientSampleCount(IntPtr client);

        [DllImport(Dll, CallingConvention = CallingConvention.Cdecl, EntryPoint = "nclshim_client_event_count")]
        internal static extern int ClientEventCount(IntPtr client);

        [DllImport(Dll, CallingConvention = CallingConvention.Cdecl, EntryPoint = "nclshim_client_add_sample")]
        internal static extern int ClientAddSample(IntPtr client, byte[] configJson, uint timeoutMs);

        [DllImport(Dll, CallingConvention = CallingConvention.Cdecl, EntryPoint = "nclshim_client_remove_sample")]
        internal static extern int ClientRemoveSample(IntPtr client, byte[] id, uint timeoutMs);

        /* ------------------------------------------------------------ server -- */

        [DllImport(Dll, CallingConvention = CallingConvention.Cdecl, EntryPoint = "nclshim_server_create")]
        internal static extern IntPtr ServerCreate(byte[] sn, byte[] modelJson, byte[] broker, byte[] username, byte[] password, IntPtr publishHost);

        [DllImport(Dll, CallingConvention = CallingConvention.Cdecl, EntryPoint = "nclshim_server_create_ex")]
        internal static extern IntPtr ServerCreateEx(byte[] sn, byte[] modelJson, byte[] broker, byte[] username, byte[] password, byte[] caFile, byte[] clientCert, byte[] clientKey, byte[] serverName, int verifyPeer, IntPtr publishHost);

        [DllImport(Dll, CallingConvention = CallingConvention.Cdecl, EntryPoint = "nclshim_server_free")]
        internal static extern void ServerFree(IntPtr handle);

        [DllImport(Dll, CallingConvention = CallingConvention.Cdecl, EntryPoint = "nclshim_server_sn")]
        internal static extern IntPtr ServerSn(IntPtr handle);

        [DllImport(Dll, CallingConvention = CallingConvention.Cdecl, EntryPoint = "nclshim_server_model")]
        internal static extern IntPtr ServerModel(IntPtr handle);

        [DllImport(Dll, CallingConvention = CallingConvention.Cdecl, EntryPoint = "nclshim_server_model_json")]
        internal static extern IntPtr ServerModelJson(IntPtr handle);

        [DllImport(Dll, CallingConvention = CallingConvention.Cdecl, EntryPoint = "nclshim_server_methods_json")]
        internal static extern int ServerMethodsJson(IntPtr handle, out IntPtr json);

        [DllImport(Dll, CallingConvention = CallingConvention.Cdecl, EntryPoint = "nclshim_server_openapi_json")]
        internal static extern IntPtr ServerOpenapiJson(IntPtr handle, byte[] baseUrl);

        [DllImport(Dll, CallingConvention = CallingConvention.Cdecl, EntryPoint = "nclshim_server_binding_count")]
        internal static extern int ServerBindingCount(IntPtr handle);

        [DllImport(Dll, CallingConvention = CallingConvention.Cdecl, EntryPoint = "nclshim_server_operation_count")]
        internal static extern int ServerOperationCount(IntPtr handle);

        [DllImport(Dll, CallingConvention = CallingConvention.Cdecl, EntryPoint = "nclshim_server_sample_count")]
        internal static extern int ServerSampleCount(IntPtr handle);

        [DllImport(Dll, CallingConvention = CallingConvention.Cdecl, EntryPoint = "nclshim_server_sample_upload_count")]
        internal static extern int ServerSampleUploadCount(IntPtr handle);

        [DllImport(Dll, CallingConvention = CallingConvention.Cdecl, EntryPoint = "nclshim_server_event_count")]
        internal static extern int ServerEventCount(IntPtr handle);

        [DllImport(Dll, CallingConvention = CallingConvention.Cdecl, EntryPoint = "nclshim_server_subscribe")]
        internal static extern int ServerSubscribe(IntPtr handle);

        [DllImport(Dll, CallingConvention = CallingConvention.Cdecl, EntryPoint = "nclshim_server_register_tool")]
        internal static extern int ServerRegisterTool(IntPtr handle, byte[] tool, byte[] methodsJson, byte[] bindingsJson, IntPtr host);

        [DllImport(Dll, CallingConvention = CallingConvention.Cdecl, EntryPoint = "nclshim_server_register_builtin_tool")]
        internal static extern int ServerRegisterBuiltinTool(IntPtr handle);

        [DllImport(Dll, CallingConvention = CallingConvention.Cdecl, EntryPoint = "nclshim_server_register_file_tool")]
        internal static extern int ServerRegisterFileTool(IntPtr handle);

        [DllImport(Dll, CallingConvention = CallingConvention.Cdecl, EntryPoint = "nclshim_server_start_ftp")]
        internal static extern int ServerStartFtp(IntPtr handle);

        [DllImport(Dll, CallingConvention = CallingConvention.Cdecl, EntryPoint = "nclshim_server_dispatch")]
        internal static extern int ServerDispatch(IntPtr handle, byte[] topic, byte[] payload, int payloadLen, out IntPtr responseJson);

        [DllImport(Dll, CallingConvention = CallingConvention.Cdecl, EntryPoint = "nclshim_server_invoke_method_call")]
        internal static extern int ServerInvokeMethodCall(IntPtr handle, byte[] method, byte[] paramsJson, out IntPtr responseJson);

        [DllImport(Dll, CallingConvention = CallingConvention.Cdecl, EntryPoint = "nclshim_server_check_method_call")]
        internal static extern int ServerCheckMethodCall(IntPtr handle, byte[] method, byte[] paramsJson, out IntPtr responseJson);

        [DllImport(Dll, CallingConvention = CallingConvention.Cdecl, EntryPoint = "nclshim_server_invoke_method_call_async")]
        internal static extern int ServerInvokeMethodCallAsync(IntPtr handle, byte[] method, byte[] paramsJson, out IntPtr responseJson);

        [DllImport(Dll, CallingConvention = CallingConvention.Cdecl, EntryPoint = "nclshim_server_invoke_method_status")]
        internal static extern int ServerInvokeMethodStatus(IntPtr handle, byte[] objectId, byte[] handler, out IntPtr responseJson);

        [DllImport(Dll, CallingConvention = CallingConvention.Cdecl, EntryPoint = "nclshim_server_invoke_method_result")]
        internal static extern int ServerInvokeMethodResult(IntPtr handle, byte[] objectId, byte[] handler, out IntPtr responseJson);

        [DllImport(Dll, CallingConvention = CallingConvention.Cdecl, EntryPoint = "nclshim_server_report_method_progress")]
        internal static extern int ServerReportMethodProgress(IntPtr handle, byte[] handler, long process, byte[] status);

        [DllImport(Dll, CallingConvention = CallingConvention.Cdecl, EntryPoint = "nclshim_server_init_samples")]
        internal static extern int ServerInitSamples(IntPtr handle);

        [DllImport(Dll, CallingConvention = CallingConvention.Cdecl, EntryPoint = "nclshim_server_add_sample")]
        internal static extern int ServerAddSample(IntPtr handle, byte[] configJson);

        [DllImport(Dll, CallingConvention = CallingConvention.Cdecl, EntryPoint = "nclshim_server_remove_sample")]
        internal static extern int ServerRemoveSample(IntPtr handle, byte[] id);

        [DllImport(Dll, CallingConvention = CallingConvention.Cdecl, EntryPoint = "nclshim_server_stop_all_samples")]
        internal static extern void ServerStopAllSamples(IntPtr handle);

        [DllImport(Dll, CallingConvention = CallingConvention.Cdecl, EntryPoint = "nclshim_server_push_event")]
        internal static extern int ServerPushEvent(IntPtr handle, byte[] eventId, byte[] eventJson);

        [DllImport(Dll, CallingConvention = CallingConvention.Cdecl, EntryPoint = "nclshim_server_push_event_ex")]
        internal static extern int ServerPushEventEx(IntPtr handle, byte[] eventId, byte[] eventJson, long timeMs, byte[] messageId);

        /* -------------------------------------------------------------- http -- */

        [DllImport(Dll, CallingConvention = CallingConvention.Cdecl, EntryPoint = "nclshim_http_start")]
        internal static extern IntPtr HttpStart(uint port, IntPtr server, int withConfig);

        [DllImport(Dll, CallingConvention = CallingConvention.Cdecl, EntryPoint = "nclshim_http_free")]
        internal static extern void HttpFree(IntPtr handle);

        [DllImport(Dll, CallingConvention = CallingConvention.Cdecl, EntryPoint = "nclshim_http_port")]
        internal static extern int HttpPort(IntPtr handle);

        [DllImport(Dll, CallingConvention = CallingConvention.Cdecl, EntryPoint = "nclshim_http_request_count")]
        internal static extern int HttpRequestCount(IntPtr handle);

        [DllImport(Dll, CallingConvention = CallingConvention.Cdecl, EntryPoint = "nclshim_http_set_cors")]
        internal static extern void HttpSetCors(IntPtr handle, int enabled);

        [DllImport(Dll, CallingConvention = CallingConvention.Cdecl, EntryPoint = "nclshim_http_route")]
        internal static extern int HttpRoute(IntPtr handle, byte[] method, byte[] path, IntPtr host);

        [DllImport(Dll, CallingConvention = CallingConvention.Cdecl, EntryPoint = "nclshim_strdup")]
        internal static extern IntPtr Strdup(byte[] text);

        /* -------------------------------------------------------------- file -- */

        [DllImport(Dll, CallingConvention = CallingConvention.Cdecl, EntryPoint = "nclshim_file_start_ftp")]
        internal static extern int FileStartFtp();

        [DllImport(Dll, CallingConvention = CallingConvention.Cdecl, EntryPoint = "nclshim_file_start_ftp_ex")]
        internal static extern int FileStartFtpEx(uint port, byte[] root, byte[] user, byte[] password);

        [DllImport(Dll, CallingConvention = CallingConvention.Cdecl, EntryPoint = "nclshim_server_set_file_peer")]
        internal static extern int ServerSetFilePeer(IntPtr handle, byte[] host, uint port, byte[] user, byte[] password);

        [DllImport(Dll, CallingConvention = CallingConvention.Cdecl, EntryPoint = "nclshim_file_stop_ftp")]
        internal static extern void FileStopFtp();

        [DllImport(Dll, CallingConvention = CallingConvention.Cdecl, EntryPoint = "nclshim_client_file_channel_open")]
        internal static extern int ClientFileChannelOpen(IntPtr client);

        [DllImport(Dll, CallingConvention = CallingConvention.Cdecl, EntryPoint = "nclshim_client_file_channel_open_ex")]
        internal static extern int ClientFileChannelOpenEx(IntPtr client, byte[] host, uint port, byte[] user, byte[] password);

        [DllImport(Dll, CallingConvention = CallingConvention.Cdecl, EntryPoint = "nclshim_client_file_channel_close")]
        internal static extern int ClientFileChannelClose(IntPtr client);

        [DllImport(Dll, CallingConvention = CallingConvention.Cdecl, EntryPoint = "nclshim_client_file_channel_is_open")]
        internal static extern int ClientFileChannelIsOpen(IntPtr client);

        [DllImport(Dll, CallingConvention = CallingConvention.Cdecl, EntryPoint = "nclshim_client_file_write")]
        internal static extern int ClientFileWrite(IntPtr client, byte[] localFilePath);

        [DllImport(Dll, CallingConvention = CallingConvention.Cdecl, EntryPoint = "nclshim_client_file_read")]
        internal static extern IntPtr ClientFileRead(IntPtr client, byte[] remoteFilePath);

        [DllImport(Dll, CallingConvention = CallingConvention.Cdecl, EntryPoint = "nclshim_client_file_ll_json")]
        internal static extern IntPtr ClientFileLlJson(IntPtr client, byte[] remoteDir);

        [DllImport(Dll, CallingConvention = CallingConvention.Cdecl, EntryPoint = "nclshim_client_file_mkdir")]
        internal static extern int ClientFileMkdir(IntPtr client, byte[] remoteDir);

        [DllImport(Dll, CallingConvention = CallingConvention.Cdecl, EntryPoint = "nclshim_client_file_delete")]
        internal static extern int ClientFileDelete(IntPtr client, byte[] remoteFilePath);

        [DllImport(Dll, CallingConvention = CallingConvention.Cdecl, EntryPoint = "nclshim_client_method_call_file")]
        internal static extern int ClientMethodCallFile(IntPtr client, byte[] method, byte[] paramsJson, byte[] keysJson, byte[] pathsJson, uint timeoutMs, out IntPtr responseJson);

        [DllImport(Dll, CallingConvention = CallingConvention.Cdecl, EntryPoint = "nclshim_file_need_compression")]
        internal static extern int FileNeedCompression(byte[] fileName);

        [DllImport(Dll, CallingConvention = CallingConvention.Cdecl, EntryPoint = "nclshim_file_total_chunks")]
        internal static extern int FileTotalChunks(long size);

        [DllImport(Dll, CallingConvention = CallingConvention.Cdecl, EntryPoint = "nclshim_file_checksum")]
        internal static extern IntPtr FileChecksum(byte[] path);

        [DllImport(Dll, CallingConvention = CallingConvention.Cdecl, EntryPoint = "nclshim_file_attribute_json")]
        internal static extern IntPtr FileAttributeJson(byte[] path, byte[] parent);

        /* ----------------------------------------------------------- helpers -- */

        /// <summary>空终止的 UTF-8 字节串 → string（原生返回 NULL 时得到 null）。</summary>
        internal static string Utf8(IntPtr ptr)
        {
            if (ptr == IntPtr.Zero)
            {
                return null;
            }
            int length = 0;
            while (Marshal.ReadByte(ptr, length) != 0)
            {
                length++;
            }
            if (length == 0)
            {
                return string.Empty;
            }
            byte[] buffer = new byte[length];
            Marshal.Copy(ptr, buffer, 0, length);
            return Encoding.UTF8.GetString(buffer);
        }

        /// <summary>把托管字符串复制成"原生调用期间有效"的 UTF-8 字节串。</summary>
        internal static byte[] Utf8Z(string text)
        {
            if (text == null)
            {
                return null;
            }
            byte[] bytes = Encoding.UTF8.GetBytes(text);
            byte[] buffer = new byte[bytes.Length + 1];
            Array.Copy(bytes, buffer, bytes.Length);
            return buffer;
        }

        /// <summary>取走原生 malloc 出来的字符串并释放它。</summary>
        internal static string TakeUtf8(IntPtr ptr)
        {
            string text = Utf8(ptr);
            if (ptr != IntPtr.Zero)
            {
                Free(ptr);
            }
            return text;
        }
    }
}

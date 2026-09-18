// SPDX-License-Identifier: MIT
// Copyright (c) 2026 huienming

using System;
using System.Collections.Generic;
using System.IO;
using System.Text;

namespace Nclink
{
    /// <summary>
    /// 远端一个文件或目录的属性（字段顺序按规范：fileName / fileType / fileSize /
    /// totalChunks / compressed / checksum / parantDir / modifyTime）。
    /// </summary>
    public sealed class NclFileInfo
    {
        /// <summary>文件名（不含目录）。</summary>
        public string FileName { get; private set; }

        /// <summary>1 = 目录，0 = 文件。</summary>
        public int FileType { get; private set; }

        /// <summary>是不是目录。</summary>
        public bool IsDirectory { get { return FileType == 1; } }

        /// <summary>字节数（目录为 0）。</summary>
        public long FileSize { get; private set; }

        /// <summary>分片数（256 KB 一片）。</summary>
        public int TotalChunks { get; private set; }

        /// <summary>传输时是否压缩（按扩展名判断）。</summary>
        public bool Compressed { get; private set; }

        /// <summary>SHA-256 十六进制；走 FTP 列目录时是修改时间字符串。</summary>
        public string Checksum { get; private set; }

        /// <summary>父目录（规范里的拼写就是 parantDir）。</summary>
        public string ParentDirectory { get; private set; }

        /// <summary>修改时间（epoch 毫秒）。</summary>
        public long ModifyTimeMs { get; private set; }

        internal static NclFileInfo Parse(NclJson json)
        {
            if (json == null)
            {
                return null;
            }
            NclFileInfo info = new NclFileInfo();
            info.FileName = Text(json.Get("fileName"));
            info.FileType = (int)Long(json.Get("fileType"), 0);
            info.FileSize = Long(json.Get("fileSize"), 0);
            info.TotalChunks = (int)Long(json.Get("totalChunks"), 0);
            info.Compressed = json.Get("compressed") != null
                              && json.Get("compressed").AsBool();
            info.Checksum = Text(json.Get("checksum"));
            info.ParentDirectory = Text(json.Get("parantDir"));
            info.ModifyTimeMs = Long(json.Get("modifyTime"), 0);
            return info;
        }

        internal static IList<NclFileInfo> ParseList(string json)
        {
            List<NclFileInfo> items = new List<NclFileInfo>();
            if (string.IsNullOrEmpty(json))
            {
                return items;
            }
            using (NclJson array = NclJson.Parse(json))
            {
                foreach (NclJson item in array.Items())
                {
                    NclFileInfo info = Parse(item);
                    if (info != null)
                    {
                        items.Add(info);
                    }
                }
            }
            return items;
        }

        private static long Long(NclJson json, long defaultValue)
        {
            return json == null ? defaultValue : json.AsLong(defaultValue);
        }

        private static string Text(NclJson json)
        {
            return json == null ? null : json.AsString();
        }

        public override string ToString()
        {
            return IsDirectory
                       ? FileName + "/ (目录)"
                       : FileName + " (" + FileSize + " 字节，"
                         + TotalChunks + " 片" + (Compressed ? "，压缩" : "") + ")";
        }
    }

    /// <summary>
    /// 客户端的文件通道：MQTT 报文里只传 "/temp/&lt;名字&gt;" 令牌，字节走 FTP。
    ///
    /// 方向很重要：**设备是 FTP 客户端**，托管侧是 FTP 服务端（进程级端点
    /// 127.0.0.1:2323，admin / 123456，根 = 安装根，开文件通道时按需起）。本地文件放
    /// &lt;当前目录&gt;/&lt;sn&gt;/&lt;相对路径&gt;：
    ///
    /// <list type="bullet">
    ///   <item>上传：文件先落到那儿，再用同样的相对路径 <see cref="UploadFile"/>
    ///         （<see cref="UploadLocalFile"/> 会替你摆好）；</item>
    ///   <item>下载：字节先落到那儿，<see cref="DownloadFile"/> 返回绝对路径
    ///         （<see cref="DownloadTo"/> 再替你搬到目标）。</item>
    /// </list>
    ///
    /// 传字节之前要先握手：<see cref="OpenFileChannel()"/> 把端点交给设备
    /// （file/openFileChannel），设备随即往那儿拨 FTP。下面的便利方法会自己确保通道
    /// 开着，<see cref="CloseFileChannel()"/> 收回租约并撤销临时账号。
    ///
    /// 设备侧要先把文件工具挂起来：<see cref="NclServer.RegisterFileTool"/>（设备自己
    /// 也服务 FTP 时再加 <see cref="NclServer.StartFtp"/>；不握手、钉静态对端则是
    /// <see cref="NclServer.SetFilePeer"/>）。
    /// </summary>
    public sealed partial class NclDeviceClient
    {
        /* -------------------------------------------------------- 文件通道 -- */

        /// <summary>
        /// 开文件通道（file/openFileChannel）：把托管侧的 FTP 端点交给设备，设备随即
        /// 往那儿拨 FTP 传字节。
        ///
        /// 不传参就是默认：地址 = 到 broker 的本机地址（拿不到就 127.0.0.1）、端口 =
        /// 进程级端点端口（没起就按 2323 起）、账号 = 库临时生成的一对（关闭时撤销）。
        /// 对端不在这台机器上、或端口有映射时用
        /// <see cref="OpenFileChannel(string, int, string, string)"/>。
        ///
        /// 幂等；通道是租约，<see cref="CloseFileChannel()"/> 之前一直是这条对端。
        /// 上传/下载/列目录/建目录/删除这些便利方法也会在没通道时自动开一次。
        /// </summary>
        public void OpenFileChannel()
        {
            ThrowIfDisposed();
            NclinkException.Check(Native.ClientFileChannelOpen(_client),
                                  "OpenFileChannel");
        }

        /// <summary>同上，但显式给出设备要拨的 host / port 和账号（host、port 必填）。</summary>
        public void OpenFileChannel(string host, int port, string username = null,
                                    string password = null)
        {
            ThrowIfDisposed();
            if (string.IsNullOrEmpty(host) || port <= 0)
            {
                throw new ArgumentException("OpenFileChannel 需要 host 和 port");
            }
            NclinkException.Check(
                Native.ClientFileChannelOpenEx(_client, Native.Utf8Z(host), (uint)port,
                                               Native.Utf8Z(username),
                                               Native.Utf8Z(password)),
                "OpenFileChannel");
        }

        /// <summary>收回文件通道：设备停用它的 FTP 连接，库给这条通道加的账号一并撤销（幂等）。</summary>
        public void CloseFileChannel()
        {
            ThrowIfDisposed();
            NclinkException.Check(Native.ClientFileChannelClose(_client),
                                  "CloseFileChannel");
        }

        /// <summary>这条客户端手上有没有文件通道。</summary>
        public bool FileChannelIsOpen
        {
            get
            {
                ThrowIfDisposed();
                return Native.ClientFileChannelIsOpen(_client) != 0;
            }
        }

        /// <summary>
        /// 上传 <paramref name="relativePath"/>（形如 "/demo.txt"）：文件必须已经在
        /// <c>&lt;当前目录&gt;/&lt;sn&gt;&lt;relativePath&gt;</c> 上——用
        /// <see cref="UploadLocalFile"/> 就不用操心这件事。
        /// </summary>
        public void UploadFile(string relativePath)
        {
            ThrowIfDisposed();
            NclinkException.Check(
                Native.ClientFileWrite(_client, Native.Utf8Z(relativePath)),
                "UploadFile");
        }

        /// <summary>
        /// 把本地文件传过去：先摆到 <c>&lt;当前目录&gt;/&lt;sn&gt;&lt;relativePath&gt;</c>
        /// 再走文件通道。relativePath 省略时用本地文件名。
        /// </summary>
        public void UploadLocalFile(string localPath, string relativePath = null)
        {
            ThrowIfDisposed();
            if (string.IsNullOrEmpty(localPath))
            {
                throw new ArgumentNullException("localPath");
            }
            if (string.IsNullOrEmpty(relativePath))
            {
                relativePath = "/" + Path.GetFileName(localPath);
            }
            string staged = StagedPath(relativePath);
            string parent = Path.GetDirectoryName(staged);
            if (!string.IsNullOrEmpty(parent))
            {
                Directory.CreateDirectory(parent);
            }
            if (!string.Equals(Path.GetFullPath(localPath), Path.GetFullPath(staged),
                               StringComparison.OrdinalIgnoreCase))
            {
                File.Copy(localPath, staged, true);
            }
            UploadFile(relativePath);
        }

        /// <summary>
        /// 下载 <paramref name="relativePath"/>，返回落盘后的本地绝对路径
        /// （在 <c>&lt;当前目录&gt;/&lt;sn&gt;/</c> 下面）。
        /// </summary>
        public string DownloadFile(string relativePath)
        {
            ThrowIfDisposed();
            string local = Native.TakeUtf8(
                Native.ClientFileRead(_client, Native.Utf8Z(relativePath)));
            if (string.IsNullOrEmpty(local))
            {
                throw new NclinkException(-1, "DownloadFile",
                                          "下载失败: " + relativePath);
            }
            return local;
        }

        /// <summary>下载并复制到 <paramref name="localPath"/>；返回落点绝对路径。</summary>
        public string DownloadTo(string relativePath, string localPath)
        {
            string staged = DownloadFile(relativePath);
            string parent = Path.GetDirectoryName(localPath);
            if (!string.IsNullOrEmpty(parent))
            {
                Directory.CreateDirectory(parent);
            }
            File.Copy(staged, localPath, true);
            return Path.GetFullPath(localPath);
        }

        /// <summary>列设备上的某个目录（默认根 "/"）。</summary>
        public IList<NclFileInfo> ListFiles(string remoteDir = "/")
        {
            ThrowIfDisposed();
            string json = Native.TakeUtf8(
                Native.ClientFileLlJson(_client, Native.Utf8Z(remoteDir)));
            if (json == null)
            {
                throw new NclinkException(-1, "ListFiles", "列目录失败: " + remoteDir);
            }
            return NclFileInfo.ParseList(json);
        }

        /// <summary>在设备上建目录。</summary>
        public void MakeDirectory(string remoteDir)
        {
            ThrowIfDisposed();
            NclinkException.Check(
                Native.ClientFileMkdir(_client, Native.Utf8Z(remoteDir)),
                "MakeDirectory");
        }

        /// <summary>删设备上的文件或目录（目录递归删）。</summary>
        public void DeleteRemoteFile(string remotePath)
        {
            ThrowIfDisposed();
            NclinkException.Check(
                Native.ClientFileDelete(_client, Native.Utf8Z(remotePath)),
                "DeleteRemoteFile");
        }

        /// <summary>
        /// 带文件参数的方法调用：<paramref name="keys"/> 与 <paramref name="paths"/>
        /// 一一对应，参数里那些键会被换成 "/temp/&lt;名字&gt;"（文件经文件通道送过去），
        /// 应答里 "fileKeys" 列出的值会被换成本地路径。
        /// </summary>
        public NclJson MethodCallFile(string method, string paramsJson, string[] keys,
                                      string[] paths, uint timeoutMs = 5000)
        {
            ThrowIfDisposed();
            IntPtr response;
            NclinkException.Check(
                Native.ClientMethodCallFile(_client, Native.Utf8Z(method),
                                            Native.Utf8Z(paramsJson),
                                            Native.Utf8Z(JsonArray(keys)),
                                            Native.Utf8Z(JsonArray(paths)),
                                            timeoutMs, out response),
                "MethodCallFile");
            return NclJson.TakeText(response);
        }

        /* ------------------------------------------------------------ 小工具 -- */

        /// <summary>
        /// 文件通道的暂存位置：<c>&lt;当前目录&gt;/&lt;sn&gt;&lt;相对路径&gt;</c>
        /// （与 C API 一致）。
        /// </summary>
        public string StagedPath(string relativePath)
        {
            string root = Path.Combine(Directory.GetCurrentDirectory(), _sn);
            if (string.IsNullOrEmpty(relativePath))
            {
                return root;
            }
            string tail = relativePath.Replace('/', Path.DirectorySeparatorChar)
                                      .TrimStart(Path.DirectorySeparatorChar);
            return Path.Combine(root, tail);
        }

        private static string JsonArray(string[] values)
        {
            StringBuilder builder = new StringBuilder("[");
            if (values != null)
            {
                for (int i = 0; i < values.Length; i++)
                {
                    if (i > 0)
                    {
                        builder.Append(',');
                    }
                    builder.Append(NclServer.Quote(values[i]));
                }
            }
            builder.Append(']');
            return builder.ToString();
        }
    }
}

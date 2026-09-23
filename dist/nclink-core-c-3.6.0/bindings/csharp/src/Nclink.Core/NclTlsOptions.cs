// SPDX-License-Identifier: MIT
// Copyright (c) 2026 huienming

namespace Nclink
{
    /// <summary>
    /// <c>ssl://</c> / <c>tls://</c> 连接的 TLS 选项（配合 <see cref="Nclink.Init(string, string, string)"/>
    /// 使用）。不设就是默认：校验证书链与主机名、用平台信任库。
    ///
    /// 只有**带 TLS 编译的库 + 垫片**才起作用（见 <see cref="Nclink.TlsAvailable"/>
    /// 与各绑定 README 里的 "TLS 构建" 一节）。
    /// </summary>
    public sealed class NclTlsOptions
    {
        /// <summary>CA 证书束（PEM）；null = 用平台信任库。</summary>
        public string CaFile { get; set; }

        /// <summary>客户端证书（PEM，双向 TLS 用）。</summary>
        public string ClientCertificate { get; set; }

        /// <summary>客户端私钥（PEM）。</summary>
        public string ClientKey { get; set; }

        /// <summary>SNI / 主机名校验用的名字；null = 用 URL 里的主机名。</summary>
        public string ServerName { get; set; }

        /// <summary>是否校验证书链与主机名（默认 true；自签调试才关）。</summary>
        public bool VerifyPeer { get; set; }

        public NclTlsOptions()
        {
            VerifyPeer = true;
        }
    }
}

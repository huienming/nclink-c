// SPDX-License-Identifier: MIT
// Copyright (c) 2026 huienming

package com.nclink;

/**
 * {@code ssl://} / {@code tls://} 连接的 TLS 选项（配合 {@link Nclink#init} 使用）。
 * 不设就是默认：校验证书链与主机名、用平台信任库。
 *
 * <p>只有**带 TLS 编译的库 + JNI 库**才起作用（见 {@link Nclink#tlsAvailable()}）。
 */
public final class TlsOptions {
    private String caFile;
    private String clientCert;
    private String clientKey;
    private String serverName;
    private boolean verifyPeer = true;

    /** CA 证书束（PEM）；null = 平台信任库。 */
    public TlsOptions caFile(String path) {
        this.caFile = path;
        return this;
    }

    /** 客户端证书与私钥（PEM，双向 TLS 用）。 */
    public TlsOptions clientCert(String cert, String key) {
        this.clientCert = cert;
        this.clientKey = key;
        return this;
    }

    /** SNI / 主机名校验用的名字；null = URL 里的主机名。 */
    public TlsOptions serverName(String name) {
        this.serverName = name;
        return this;
    }

    /** 是否校验证书链与主机名（默认 true；自签调试才关）。 */
    public TlsOptions verifyPeer(boolean enabled) {
        this.verifyPeer = enabled;
        return this;
    }

    String caFile() {
        return caFile;
    }

    String clientCert() {
        return clientCert;
    }

    String clientKey() {
        return clientKey;
    }

    String serverName() {
        return serverName;
    }

    boolean verifyPeer() {
        return verifyPeer;
    }
}

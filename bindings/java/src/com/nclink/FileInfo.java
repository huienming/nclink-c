// SPDX-License-Identifier: MIT
// Copyright (c) 2026 huienming

package com.nclink;

import java.util.ArrayList;
import java.util.List;
import java.util.Map;

/**
 * 远端一个文件或目录的属性（字段顺序按规范：fileName / fileType / fileSize /
 * totalChunks / compressed / checksum / parantDir / modifyTime）。
 */
public final class FileInfo {
    private final String fileName;
    private final int fileType;
    private final long fileSize;
    private final int totalChunks;
    private final boolean compressed;
    private final String checksum;
    private final String parentDir;
    private final long modifyTime;

    FileInfo(String fileName, int fileType, long fileSize, int totalChunks,
             boolean compressed, String checksum, String parentDir, long modifyTime) {
        this.fileName = fileName;
        this.fileType = fileType;
        this.fileSize = fileSize;
        this.totalChunks = totalChunks;
        this.compressed = compressed;
        this.checksum = checksum;
        this.parentDir = parentDir;
        this.modifyTime = modifyTime;
    }

    /** 从属性 JSON 对象建一个（键名照规范的拼写）。 */
    @SuppressWarnings("unchecked")
    static FileInfo fromJavaObject(Object value) {
        Map<?, ?> map = (Map<?, ?>) value;
        return new FileInfo((String) map.get("fileName"),
                            number(map.get("fileType")).intValue(),
                            number(map.get("fileSize")).longValue(),
                            number(map.get("totalChunks")).intValue(),
                            Boolean.TRUE.equals(map.get("compressed")),
                            (String) map.get("checksum"),
                            (String) map.get("parantDir"),
                            number(map.get("modifyTime")).longValue());
    }

    /** 把属性 JSON 数组解析成列表（`nclshim_client_file_ll_json()` 的产物）。 */
    @SuppressWarnings("unchecked")
    static List<FileInfo> parseList(String json) {
        List<FileInfo> items = new ArrayList<FileInfo>();
        if (json == null || json.isEmpty()) {
            return items;
        }
        Json parsed = Json.parse(json);
        try {
            for (Object item : (List<Object>) parsed.toJavaObject()) {
                items.add(fromJavaObject(item));
            }
        } finally {
            parsed.close();
        }
        return items;
    }

    private static Number number(Object value) {
        return value instanceof Number ? (Number) value : Long.valueOf(0);
    }

    public String fileName() {
        return fileName;
    }

    /** 1 = 目录，0 = 文件。 */
    public int fileType() {
        return fileType;
    }

    public boolean isDir() {
        return fileType == 1;
    }

    public long fileSize() {
        return fileSize;
    }

    public int totalChunks() {
        return totalChunks;
    }

    public boolean compressed() {
        return compressed;
    }

    /** SHA-256 十六进制；走 FTP 列目录时是修改时间字符串。 */
    public String checksum() {
        return checksum;
    }

    public String parentDir() {
        return parentDir;
    }

    public long modifyTime() {
        return modifyTime;
    }

    @Override
    public String toString() {
        return isDir() ? fileName + "/ (目录)"
                       : fileName + " (" + fileSize + " 字节，" + totalChunks + " 片"
                         + (compressed ? "，压缩" : "") + ")";
    }
}

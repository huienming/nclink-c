// SPDX-License-Identifier: MIT
// Copyright (c) 2026 huienming

using System;
using System.Collections.Generic;
using System.Globalization;
using System.Text;

namespace Nclink
{
    /// <summary>采样报文里的一列（一个数据项在本轮上报窗口内的数据）。</summary>
    public sealed class NclSampleColumn
    {
        private readonly object[] _values;

        internal NclSampleColumn(string path, int slots, bool isNested, string encoding,
                                 object[] values)
        {
            Path = path;
            Slots = slots;
            IsNested = isNested;
            Encoding = encoding;
            _values = values;
        }

        /// <summary>数据项路径（表头里的名字）。设备段（/MACHINE）不写在这里，要拼回模型的绝对路径就加上它。</summary>
        public string Path { get; private set; }

        /// <summary>槽位数（本轮上报包含多少次采样）。</summary>
        public int Slots { get; private set; }

        /// <summary>该列总点数：标量列 = 槽位数，批量列 = Σ每槽点数。</summary>
        public int Points { get { return _values.Length; } }

        /// <summary>是否批量列（一个槽位里装了一批值，即亚毫秒采样）。</summary>
        public bool IsNested { get; private set; }

        /// <summary>编码方式；null/空表示值按原始 JSON 传输。</summary>
        public string Encoding { get; private set; }

        /// <summary>列内扁平取值（按时间先后）。</summary>
        public object ValueAt(int index)
        {
            if (index < 0 || index >= _values.Length)
            {
                throw new ArgumentOutOfRangeException("index");
            }
            return _values[index];
        }

        /// <summary>列内所有点。</summary>
        public object[] Values { get { return (object[])_values.Clone(); } }

        internal object RawValueAt(int index)
        {
            return index >= 0 && index < _values.Length ? _values[index] : null;
        }

        public override string ToString()
        {
            return Path + ": " + Slots + " 槽位 / " + Points + " 点" +
                   (IsNested ? "（批量）" : string.Empty);
        }
    }

    /// <summary>
    /// 一条 Sample 报文的托管快照。
    ///
    /// 原生报文只在回调期间有效，所以这里在收到时就把它拷成托管对象：值按
    /// long / double / string / bool / null 存，取用不再碰原生内存。原始报文也可以
    /// 通过 <see cref="RawJson"/> 拿到。
    ///
    /// 按行消费与 C 库一致：行数取**数据最多的那一列**（最细的采样率），更粗的列在
    /// 同一段里反复取覆盖该行的第一个点（<see cref="ValueAt"/>）。
    /// </summary>
    public sealed class NclSample
    {
        private readonly List<NclSampleColumn> _columns;
        private readonly int _rows;

        internal NclSample(string topic, string id, string beginTime, long intervalMs,
                           long uploadIntervalMs, bool isComplete, string rawJson,
                           List<NclSampleColumn> columns, int rows)
        {
            Topic = topic;
            Id = id;
            BeginTime = beginTime;
            IntervalMs = intervalMs;
            UploadIntervalMs = uploadIntervalMs;
            IsComplete = isComplete;
            RawJson = rawJson;
            _columns = columns;
            _rows = rows;
        }

        /// <summary>MQTT 主题：Sample/&lt;sn&gt;/&lt;通道id&gt;。</summary>
        public string Topic { get; private set; }

        /// <summary>通道 id。</summary>
        public string Id { get; private set; }

        /// <summary>上报窗口起点（epoch 毫秒，字符串）。</summary>
        public string BeginTime { get; private set; }

        /// <summary>采样周期（毫秒，取模型的 sampleInterval）。</summary>
        public long IntervalMs { get; private set; }

        /// <summary>上报周期（毫秒）。</summary>
        public long UploadIntervalMs { get; private set; }

        /// <summary>原生校验：外层（表头与槽位）对齐。</summary>
        public bool IsComplete { get; private set; }

        /// <summary>原始报文的 JSON 文本（设备发什么就是什么）。</summary>
        public string RawJson { get; private set; }

        /// <summary>列（= 表头项）。</summary>
        public IList<NclSampleColumn> Columns { get { return _columns; } }

        /// <summary>行数 = 数据最多的那一列的点数。</summary>
        public int Rows { get { return _rows; } }

        /// <summary>
        /// 按行取值：第 <paramref name="row"/> 行、第 <paramref name="column"/> 列。
        /// 采样率低的列会连着几行返回同一个点（覆盖该行的第一个点）。
        /// </summary>
        public object ValueAt(int row, int column)
        {
            if (column < 0 || column >= _columns.Count)
            {
                throw new ArgumentOutOfRangeException("column");
            }
            if (row < 0 || row >= _rows)
            {
                throw new ArgumentOutOfRangeException("row");
            }
            return _columns[column].RawValueAt(row * _columns[column].Points / _rows);
        }

        /// <summary>按行取浮点；取不到时返回 default。</summary>
        public double GetDouble(int row, int column, double defaultValue = 0)
        {
            object value = ValueAt(row, column);
            if (value is double)
            {
                return (double)value;
            }
            if (value is long)
            {
                return (long)value;
            }
            return value is string
                       ? (double.TryParse((string)value, NumberStyles.Any,
                                          CultureInfo.InvariantCulture, out var parsed)
                              ? parsed
                              : defaultValue)
                       : defaultValue;
        }

        /// <summary>按行取整数；取不到时返回 default。</summary>
        public long GetLong(int row, int column, long defaultValue = 0)
        {
            object value = ValueAt(row, column);
            if (value is long)
            {
                return (long)value;
            }
            if (value is double)
            {
                return (long)(double)value;
            }
            return value is string
                       ? (long.TryParse((string)value, NumberStyles.Any,
                                        CultureInfo.InvariantCulture, out var parsed)
                              ? parsed
                              : defaultValue)
                       : defaultValue;
        }

        /// <summary>按行取字符串（不是字符串时用 JSON 形式）。</summary>
        public string GetString(int row, int column)
        {
            object value = ValueAt(row, column);
            if (value == null)
            {
                return null;
            }
            return value is double
                       ? ((double)value).ToString("R", CultureInfo.InvariantCulture)
                       : Convert.ToString(value, CultureInfo.InvariantCulture);
        }

        public override string ToString()
        {
            StringBuilder text = new StringBuilder();
            text.Append("Sample ").Append(Topic).Append(" id=").Append(Id)
                .Append(" rows=").Append(_rows).Append(" columns=")
                .Append(_columns.Count);
            return text.ToString();
        }

        /// <summary>回调里把原生报文拷成托管快照。</summary>
        internal static NclSample Capture(IntPtr msg, string topic)
        {
            int columns = Native.SampleColumns(msg);
            List<NclSampleColumn> list = new List<NclSampleColumn>(columns);
            int rows = 0;

            for (int col = 0; col < columns; col++)
            {
                int points = Native.SampleColumnPoints(msg, col);
                object[] values = new object[points];
                for (int i = 0; i < points; i++)
                {
                    values[i] = Read(Native.SampleColumnValueAt(msg, col, i));
                }
                list.Add(new NclSampleColumn(
                    Native.Utf8(Native.SamplePath(msg, col)),
                    Native.SampleColumnSlots(msg, col),
                    Native.SampleColumnNested(msg, col) != 0,
                    Native.Utf8(Native.SampleColumnEncoding(msg, col)), values));
            }
            rows = Native.SampleRows(msg);

            return new NclSample(topic, Native.Utf8(Native.SampleId(msg)),
                                 Native.Utf8(Native.SampleBeginTime(msg)),
                                 Native.SampleInterval(msg),
                                 Native.SampleUploadInterval(msg),
                                 Native.SampleIsComplete(msg) != 0,
                                 Native.TakeUtf8(Native.MessageWrite(msg)), list, rows);
        }

        internal static object Read(IntPtr json)
        {
            switch ((NclJsonType)Native.JsonType(json))
            {
            case NclJsonType.Bool:
                int flag;
                return Native.JsonAsBool(json, out flag) != 0 ? (object)(flag != 0) : null;
            case NclJsonType.Number:
                double dbl;
                if (Native.JsonAsDouble(json, out dbl) != 0)
                {
                    long lng;
                    if (Native.JsonAsInt(json, out lng) != 0)
                    {
                        return lng;
                    }
                    return dbl;
                }
                return null;
            case NclJsonType.String:
                return Native.Utf8(Native.JsonString(json));
            case NclJsonType.Null:
                return null;
            default:
                return Native.TakeUtf8(Native.JsonWrite(json));
            }
        }
    }

    /// <summary>一条 Event 报文的托管快照。</summary>
    public sealed class NclEvent
    {
        internal NclEvent(string topic, string id, string time, string key, object value,
                          string rawJson)
        {
            Topic = topic;
            Id = id;
            Time = time;
            Key = key;
            Value = value;
            RawJson = rawJson;
        }

        public string Topic { get; private set; }

        /// <summary>事件源节点的 id。</summary>
        public string Id { get; private set; }

        /// <summary>事件时间（epoch 毫秒字符串）。</summary>
        public string Time { get; private set; }

        /// <summary>事件键，例如 PART_COUNT。</summary>
        public string Key { get; private set; }

        /// <summary>事件值（long / double / string / bool / null）。</summary>
        public object Value { get; private set; }

        /// <summary>原始报文 JSON 文本。</summary>
        public string RawJson { get; private set; }

        public override string ToString()
        {
            return "Event " + Id + " key=" + Key + " value=" +
                   Convert.ToString(Value, CultureInfo.InvariantCulture);
        }

        internal static NclEvent Capture(IntPtr msg, string topic)
        {
            IntPtr value = Native.EventValue(msg);
            object converted = value != IntPtr.Zero
                                   ? NclSample.Read(value) // 同一个值转换
                                   : null;

            return new NclEvent(topic, Native.Utf8(Native.EventId(msg)),
                                Native.Utf8(Native.EventTime(msg)),
                                Native.Utf8(Native.EventKey(msg)), converted,
                                Native.TakeUtf8(Native.MessageWrite(msg)));
        }
    }
}

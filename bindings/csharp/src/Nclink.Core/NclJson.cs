// SPDX-License-Identifier: MIT
// Copyright (c) 2026 huienming

using System;
using System.Collections.Generic;

namespace Nclink
{
    /// <summary>JSON 值种类（对应 ncl_json_type）。</summary>
    public enum NclJsonType
    {
        Null = 0,
        Bool = 1,
        Number = 2,
        String = 3,
        Array = 4,
        Object = 5
    }

    /// <summary>
    /// 一个 JSON 值：用库自己的解析器（不依赖 Newtonsoft / System.Text.Json），所以
    /// .NET Framework 与 .NET Core 上是同一套行为。
    ///
    /// 生命周期：<see cref="Parse"/> / <see cref="Clone"/> 得到的是**自己拥有**的值，
    /// Dispose 释放；下标/成员访问（<see cref="this[int]"/>、<see cref="Get(string)"/>）
    /// 返回的是**借用**视图，它持有父对象的引用，只要父对象没释放就一直有效。
    /// </summary>
    public sealed class NclJson : IDisposable
    {
        private IntPtr _handle;
        private readonly bool _owns;
        private readonly NclJson _parent;

        private NclJson(IntPtr handle, bool owns, NclJson parent)
        {
            _handle = handle;
            _owns = owns;
            _parent = parent;
        }

        /// <summary>解析 JSON 文本；文本非法时抛 <see cref="NclinkException"/>。</summary>
        public static NclJson Parse(string text)
        {
            if (text == null)
            {
                throw new ArgumentNullException("text");
            }
            IntPtr handle = Native.JsonParse(Native.Utf8Z(text));
            if (handle == IntPtr.Zero)
            {
                throw new NclinkException(-3, "ParseJson", "JSON 解析失败: " + text);
            }
            return new NclJson(handle, true, null);
        }

        /// <summary>深拷贝一份（自有）。</summary>
        public NclJson Clone()
        {
            ThrowIfDisposed();
            IntPtr copy = Native.JsonClone(_handle);
            if (copy == IntPtr.Zero)
            {
                throw new OutOfMemoryException();
            }
            return new NclJson(copy, true, null);
        }

        /// <summary>借用包装（不拥有，父对象活着它就活着）。</summary>
        internal static NclJson Borrowed(IntPtr handle, NclJson parent)
        {
            if (handle == IntPtr.Zero)
            {
                return null;
            }
            return new NclJson(handle, false, parent);
        }

        /// <summary>借用包装（句柄由外部持有，比如客户端返回值）。</summary>
        internal static NclJson Owned(IntPtr handle)
        {
            if (handle == IntPtr.Zero)
            {
                return null;
            }
            return new NclJson(handle, true, null);
        }

        public NclJsonType Type
        {
            get
            {
                ThrowIfDisposed();
                return (NclJsonType)Native.JsonType(_handle);
            }
        }

        public bool IsNull { get { return Type == NclJsonType.Null; } }
        public bool IsArray { get { return Type == NclJsonType.Array; } }
        public bool IsObject { get { return Type == NclJsonType.Object; } }

        /// <summary>数组元素个数 / 对象成员个数。</summary>
        public int Count
        {
            get
            {
                ThrowIfDisposed();
                return Native.JsonCount(_handle);
            }
        }

        /// <summary>数组元素（借用视图）。</summary>
        public NclJson this[int index]
        {
            get
            {
                ThrowIfDisposed();
                return Borrowed(Native.JsonArrayGet(_handle, index), this);
            }
        }

        /// <summary>对象成员（借用视图；不存在返回 null）。</summary>
        public NclJson Get(string key)
        {
            if (key == null)
            {
                throw new ArgumentNullException("key");
            }
            ThrowIfDisposed();
            return Borrowed(Native.JsonObjectGet(_handle, Native.Utf8Z(key)), this);
        }

        /// <summary>对象第 index 个成员的名字（借用视图）。</summary>
        public string KeyAt(int index)
        {
            ThrowIfDisposed();
            return Native.Utf8(Native.JsonObjectKeyAt(_handle, index));
        }

        /// <summary>对象第 index 个成员的值（借用视图）。</summary>
        public NclJson ValueAt(int index)
        {
            ThrowIfDisposed();
            return Borrowed(Native.JsonObjectValueAt(_handle, index), this);
        }

        /// <summary>枚举数组元素 / 对象成员值。</summary>
        public IEnumerable<NclJson> Items()
        {
            int count = Count;
            for (int i = 0; i < count; i++)
            {
                yield return IsArray ? this[i] : ValueAt(i);
            }
        }

        /// <summary>取整数；不是数字或越界时返回 default。</summary>
        public long AsLong(long defaultValue = 0)
        {
            ThrowIfDisposed();
            long value;
            return Native.JsonAsInt(_handle, out value) != 0 ? value : defaultValue;
        }

        /// <summary>取浮点；整数也会转换。</summary>
        public double AsDouble(double defaultValue = 0)
        {
            ThrowIfDisposed();
            double value;
            return Native.JsonAsDouble(_handle, out value) != 0 ? value : defaultValue;
        }

        /// <summary>取布尔。</summary>
        public bool AsBool(bool defaultValue = false)
        {
            ThrowIfDisposed();
            int value;
            return Native.JsonAsBool(_handle, out value) != 0 ? value != 0 : defaultValue;
        }

        /// <summary>取字符串（借用，立刻复制成托管字符串）。</summary>
        public string AsString()
        {
            ThrowIfDisposed();
            return Native.Utf8(Native.JsonString(_handle));
        }

        /// <summary>文本形式：数字原样、字符串去引号。</summary>
        public string AsText()
        {
            ThrowIfDisposed();
            return Native.TakeUtf8(Native.JsonText(_handle));
        }

        /// <summary>紧凑 JSON 文本。</summary>
        public string Encode()
        {
            ThrowIfDisposed();
            return Native.TakeUtf8(Native.JsonWrite(_handle));
        }

        public override string ToString()
        {
            return _handle == IntPtr.Zero ? "(disposed)" : Encode();
        }

        /// <summary>释放自有的值；借用的视图只是断开引用。</summary>
        public void Dispose()
        {
            if (_owns && _handle != IntPtr.Zero)
            {
                Native.JsonFree(_handle);
            }
            _handle = IntPtr.Zero;
        }

        internal IntPtr Handle
        {
            get
            {
                ThrowIfDisposed();
                return _handle;
            }
        }

        private void ThrowIfDisposed()
        {
            if (_handle == IntPtr.Zero)
            {
                throw new ObjectDisposedException("NclJson");
            }
        }
    }
}

// SPDX-License-Identifier: MIT
// Copyright (c) 2026 huienming

using System;

namespace Nclink
{
    /// <summary>库返回非 0 错误码时抛出的异常。</summary>
    public class NclinkException : Exception
    {
        internal NclinkException(int code, string operation, string message)
            : base(message)
        {
            Code = code;
            Operation = operation;
        }

        /// <summary>原生错误码（ncl_err）。</summary>
        public int Code { get; private set; }

        /// <summary>错误码名字，如 "NotFoundException"。</summary>
        public string CodeName
        {
            get { return NameOf(Code); }
        }

        /// <summary>出错的操作名（Probe / GetValue / ...）。</summary>
        public string Operation { get; private set; }

        /// <summary>把错误码翻成名字。</summary>
        public static string NameOf(int code)
        {
            return Native.Utf8(Native.ErrName(code));
        }

        /// <summary>非 0 就抛。</summary>
        internal static void Check(int rc, string operation)
        {
            if (rc == 0)
            {
                return;
            }
            string name = NameOf(rc);
            throw new NclinkException(rc, operation,
                operation + " 失败: " + name + " (" + rc + ")");
        }
    }
}

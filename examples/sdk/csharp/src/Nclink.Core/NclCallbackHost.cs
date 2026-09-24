// SPDX-License-Identifier: MIT
// Copyright (c) 2026 huienming

using System;
using System.Runtime.InteropServices;

namespace Nclink
{
    /// <summary>
    /// 回调宿主：原生侧要的是"两格数组 [函数指针, 用户数据]"（采样 / 事件 / 工具 /
    /// 自研传输 / HTTP 路由回调都是这个约定），用户数据一格放托管对象的
    /// <see cref="GCHandle"/>，回调里再解回来。
    /// </summary>
    internal static class NclCallbackHost
    {
        internal static IntPtr Allocate(Delegate callback, GCHandle anchor)
        {
            IntPtr host = Marshal.AllocHGlobal(2 * IntPtr.Size);
            Marshal.WriteIntPtr(host, 0, Marshal.GetFunctionPointerForDelegate(callback));
            Marshal.WriteIntPtr(host, IntPtr.Size, GCHandle.ToIntPtr(anchor));
            return host;
        }

        internal static void Release(ref IntPtr host, ref GCHandle anchor)
        {
            if (host != IntPtr.Zero)
            {
                Marshal.FreeHGlobal(host);
                host = IntPtr.Zero;
            }
            if (anchor.IsAllocated)
            {
                anchor.Free();
            }
        }
    }
}

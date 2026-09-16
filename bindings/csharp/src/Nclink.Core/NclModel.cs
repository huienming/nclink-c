// SPDX-License-Identifier: MIT
// Copyright (c) 2026 huienming

using System;
using System.Collections.Generic;

namespace Nclink
{
    /// <summary>节点种类（对应 ncl_node_type）。</summary>
    public enum NclNodeKind
    {
        Base = 0,
        Root = 1,
        Device = 2,
        Component = 3,
        DataItem = 4,
        Config = 5
    }

    /// <summary>
    /// 设备模型（一棵树）。probe 拿到的、或自己解析出来的都归它管，Dispose 释放整棵树。
    /// </summary>
    public sealed class NclModel : IDisposable
    {
        private IntPtr _root;

        internal NclModel(IntPtr root)
        {
            _root = root;
        }

        /// <summary>解析模型文档；传 null/空串得到库内置的默认模型。</summary>
        public static NclModel Parse(string json)
        {
            IntPtr root = Native.ModelParse(Native.Utf8Z(json ?? string.Empty));
            if (root == IntPtr.Zero)
            {
                throw new NclinkException(-111, "ParseModel", "模型解析失败");
            }
            return new NclModel(root);
        }

        /// <summary>根节点（借用视图）。</summary>
        public NclNode Root
        {
            get
            {
                ThrowIfDisposed();
                return new NclNode(this, _root);
            }
        }

        /// <summary>按节点 id 查节点（借用视图；找不到返回 null）。</summary>
        public NclNode FindById(string id)
        {
            if (id == null)
            {
                throw new ArgumentNullException("id");
            }
            ThrowIfDisposed();
            IntPtr node = Native.ModelFindById(_root, Native.Utf8Z(id));
            return node == IntPtr.Zero ? null : new NclNode(this, node);
        }

        /// <summary>把整棵树序列化成 JSON 文本。</summary>
        public string ToJson()
        {
            ThrowIfDisposed();
            return Native.TakeUtf8(Native.ModelWrite(_root));
        }

        public override string ToString()
        {
            return _root == IntPtr.Zero ? "(disposed)" : ToJson();
        }

        /// <summary>释放整棵树。</summary>
        public void Dispose()
        {
            if (_root != IntPtr.Zero)
            {
                Native.ModelFree(_root);
                _root = IntPtr.Zero;
            }
        }

        internal IntPtr Handle
        {
            get
            {
                ThrowIfDisposed();
                return _root;
            }
        }

        private void ThrowIfDisposed()
        {
            if (_root == IntPtr.Zero)
            {
                throw new ObjectDisposedException("NclModel");
            }
        }
    }

    /// <summary>
    /// 模型里的一个节点（设备 / 组件 / 配置 / 数据项 / 根）。**借用**视图：只要它所属
    /// 的 <see cref="NclModel"/> 没释放，它就一直有效。
    /// </summary>
    public sealed class NclNode
    {
        private readonly NclModel _model;
        private readonly IntPtr _node;

        internal NclNode(NclModel model, IntPtr node)
        {
            _model = model;
            _node = node;
        }

        /// <summary>节点种类。</summary>
        public NclNodeKind Kind { get { return (NclNodeKind)Native.NodeType(_node); } }

        /// <summary>"type" 字段，例如 AXIS / SERVO_DRIVER / SAMPLE_CHANNEL。</summary>
        public string TypeName { get { return Native.Utf8(Native.NodeTypeName(_node)); } }

        public string Name { get { return Native.Utf8(Native.NodeName(_node)); } }

        public string Id { get { return Native.Utf8(Native.NodeId(_node)); } }

        /// <summary>完整路径，例如 /AXIS@X/POWER（数据项才有意义）。</summary>
        public string Path { get { return Native.Utf8(Native.NodePath(_node)); } }

        public string Description { get { return Native.Utf8(Native.NodeDescription(_node)); } }

        public string Number { get { return Native.Utf8(Native.NodeNumber(_node)); } }

        public string DataType { get { return Native.Utf8(Native.NodeDataType(_node)); } }

        public string ValueType { get { return Native.Utf8(Native.NodeValueType(_node)); } }

        public string Mapping { get { return Native.Utf8(Native.NodeMapping(_node)); } }

        public string Source { get { return Native.Utf8(Native.NodeSource(_node)); } }

        public string Version { get { return Native.Utf8(Native.NodeVersion(_node)); } }

        public string Guid { get { return Native.Utf8(Native.NodeGuid(_node)); } }

        /// <summary>是否可写（数据项）。</summary>
        public bool Settable { get { return Native.NodeSettable(_node) != 0; } }

        /// <summary>数据项在模型里带的值（可能为 null）。</summary>
        public NclJson Value
        {
            get { return NclJson.Borrowed(Native.NodeValue(_node), null); }
        }

        /// <summary>是不是采样通道（SAMPLE_CHANNEL 配置节点）。</summary>
        public bool IsSampleChannel
        {
            get { return Native.NodeIsSampleChannel(_node) != 0; }
        }

        /// <summary>采样通道的采样周期（毫秒）。</summary>
        public long SampleIntervalMs { get { return Native.NodeSampleInterval(_node); } }

        /// <summary>采样通道的上报周期（毫秒）。</summary>
        public long UploadIntervalMs { get { return Native.NodeUploadInterval(_node); } }

        /// <summary>采样通道声明的采样项路径。</summary>
        public string[] SampleItemPaths
        {
            get
            {
                int count = Native.NodeSampleItemCount(_node);
                string[] paths = new string[count];
                for (int i = 0; i < count; i++)
                {
                    paths[i] = Native.TakeUtf8(Native.NodeSampleItemPath(_node, i));
                }
                return paths;
            }
        }

        /// <summary>子设备。</summary>
        public IEnumerable<NclNode> Devices { get { return Children(1); } }

        /// <summary>子组件。</summary>
        public IEnumerable<NclNode> Components { get { return Children(2); } }

        /// <summary>子配置（含采样通道）。</summary>
        public IEnumerable<NclNode> Configs { get { return Children(3); } }

        /// <summary>挂在它下面的数据项。</summary>
        public IEnumerable<NclNode> DataItems { get { return Children(0); } }

        /// <summary>某类子节点的个数：1=设备 2=组件 3=配置 0=数据项。</summary>
        public int ChildCount(int kind)
        {
            return Native.NodeCount(_node, kind);
        }

        private IEnumerable<NclNode> Children(int kind)
        {
            int count = Native.NodeCount(_node, kind);
            for (int i = 0; i < count; i++)
            {
                IntPtr child = Native.NodeAt(_node, kind, i);
                if (child != IntPtr.Zero)
                {
                    yield return new NclNode(_model, child);
                }
            }
        }

        public override string ToString()
        {
            return (TypeName ?? "?") + " " + (Id ?? "?") + " " + (Name ?? "") +
                   (Path != null ? " (" + Path + ")" : string.Empty);
        }
    }
}

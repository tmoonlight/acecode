## 1. 实现集成
- [x] 1.1 审查并纳入侧栏状态去重、稳定回调与托盘限频。
- [x] 1.2 审查并纳入桥接资源释放与有界渲染恢复。
- [x] 1.3 恢复回调使用 LifetimeRef，宿主析构先撤销；开发者工具同步参数改引用。

## 2. 发布补验
- [x] 2.1 当前源码 pnpm test、pnpm build 通过，4542 条正则兼容检查通过。
- [x] 2.2 既有 Windows 验证二进制中 108 项恢复、seed、升级与 LifetimeToken 测试通过；重新编译生产 WebHost 的 renderer_recovery_smoke，通过真实 WebView2 OOM 后第二次加载和三次桥接往返。
- [x] 2.3 严格 OpenSpec、分层、所有权、大小、文档路径、include、映射与差异检查通过。

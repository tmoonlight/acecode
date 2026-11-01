## ADDED Requirements

### Requirement: Windows Desktop 保留一次原生文件系统拖放批次
Windows Desktop SHALL 将一次已接收的 Explorer 拖放中的每个 file-kind 对象作为一个有序路径批次交给原生文件系统物化器，且路径必须来自 WebView2 原生对象或 Windows OLE `CF_HDROP` 而非页面提供的字符串。

#### Scenario: 一次拖入多个文件
- **WHEN** 用户在一次操作中把多个文件拖到受支持的 Desktop 目标
- **THEN** 所有文件路径按传输顺序一起送达，后续文件不得被静默丢弃

#### Scenario: 拖入文件夹
- **WHEN** 用户拖入一个 DOM transfer item 为 file-kind 的文件夹
- **THEN** 系统从原生路径物化并按文件系统类型分类，不得把它当成零字节文件

#### Scenario: 拖入混合批次
- **WHEN** 一次拖放同时包含普通文件和文件夹
- **THEN** 物化器以一个批次收到本次拖放中的每个项目

### Requirement: 会话框按原生类型呈现文件和文件夹
Desktop 会话框 SHALL 把每个已物化的普通文件分别加入为附件，并 SHALL 把每个已物化的文件夹插入为绝对路径 reference，且不得上传文件夹内容。

#### Scenario: 多个普通文件落到会话框
- **WHEN** 包含多个普通文件的原生拖放批次在会话框上释放
- **THEN** 会话框附件区域为每个文件显示一个独立附件项

#### Scenario: 文件夹落到会话框
- **WHEN** 包含文件夹的原生拖放批次在会话框上释放
- **THEN** 文件夹绝对路径 reference 插入到已捕获的会话框光标位置，且不创建文件夹附件

#### Scenario: 文件和文件夹一起落到会话框
- **WHEN** 一个原生拖放批次同时包含普通文件和文件夹
- **THEN** 会话框在同一次操作中加入全部文件附件并插入全部文件夹 reference

### Requirement: 原生文件系统拖放保持落点隔离
Windows Desktop SHALL 只允许拥有新鲜拖放目标标记或原生释放坐标明确命中的受支持界面消费桥接批次，且成功桥接的拖放 SHALL NOT 触发浏览器文件导航或重复处理。

#### Scenario: 拖放目标是会话框
- **WHEN** 用户在会话框上释放文件系统拖放
- **THEN** 会话框只消费该批次一次，终端不得接收它

#### Scenario: 拖放目标是终端
- **WHEN** 用户在终端 tab 上释放文件系统拖放
- **THEN** 该终端收到完整路径批次，会话框保持不变

#### Scenario: Additional-object bridge 接收拖放
- **WHEN** WebView2 additional-object bridge 成功投递至少一个传输对象
- **THEN** 系统阻止 Web drop 默认行为，不得再发生 `file://` 导航或重复兜底处理

### Requirement: 旧 Runtime 兜底保持安全
Windows Desktop MUST 对 additional-object WebView2 API 和原生接口进行能力检测，并 SHALL 在新桥接不可用时保留现有导航拦截。

#### Scenario: JavaScript bridge 不可用
- **WHEN** 当前 WebView2 Runtime 未暴露 `postMessageWithAdditionalObjects`
- **THEN** UI 不得把该批次报告为已桥接，并继续保留现有原生兜底

#### Scenario: 原生 additional-object 接口不可用
- **WHEN** Desktop host 无法查询所需的 WebMessage additional-object 接口
- **THEN** host 安全忽略该特殊消息，且不得解引用不可用接口

### Requirement: WebView2 110 文件拖放进入现有富文本 intake
Windows Desktop MUST 在 WebView2 Runtime 110 以及低于 113 的运行时通过原生 OLE 接收文件路径，且 SHALL 复用当前 composer 文件 intake 和 native 坐标路由，不依赖拖放触发文件导航。

#### Scenario: 110 上从 Explorer 拖入普通文件
- **WHEN** 用户把一个或多个普通文件拖入 WebView2 110 中的会话富文本框
- **THEN** 原生路径批次进入现有 intake 并显示对应的文件 tag，原始绝对路径保留，用户可立即继续输入

#### Scenario: 110 上拖入文件夹和图片
- **WHEN** 一次 Explorer 拖放同时包含普通文件、文件夹和图片
- **THEN** intake 按当前类型规则插入全部路径 tag 和图片附件，不读取文件夹内容，也不将普通文件另存为缓存副本

#### Scenario: 取消和非文件拖动
- **WHEN** 用户取消文件拖放或执行非文件拖动
- **THEN** 取消不插入任何项目，非文件事件仍交给原 WebView 拖放目标

#### Scenario: 关闭和重新创建 WebView 子窗口
- **WHEN** WebView 子窗口因导航重新创建，或 Desktop 关闭
- **THEN** 原生适配器分别刷新新目标或撤销自身注册，且不得覆盖其他目标或调用已销毁宿主

## ADDED Requirements

### Requirement: 整个聊天主栏可接收外部文件
系统 MUST 将当前聊天主栏的标题、会话正文和 composer 共同作为外部文件拖放命中范围，并 MUST 保持侧边栏与右侧预览面板不属于该范围。

#### Scenario: 文件进入已有会话正文
- **WHEN** 用户把一个或多个外部文件拖入已有会话主栏中的标题或正文区域
- **THEN** 系统激活当前聊天主栏的文件释放状态
- **AND** 用户无需先把指针移动到输入卡片上

#### Scenario: 文件进入新会话空态
- **WHEN** 用户把外部文件拖入新会话空态主栏
- **THEN** 系统激活与已有会话一致的文件释放状态

#### Scenario: 非文件内容进入聊天主栏
- **WHEN** 用户在聊天主栏内拖动普通文本或非文件 DataTransfer
- **THEN** 系统不激活文件释放状态
- **AND** 系统不截获该拖拽的默认行为

### Requirement: 拖放状态提供全栏视觉反馈
系统 MUST 在文件位于聊天主栏内时覆盖该主栏显示轻度虚化和降对比反馈，并 MUST 在清晰的中央提示中显示“松开即可添加”。

#### Scenario: 文件在主栏子元素之间移动
- **WHEN** 用户拖着文件在标题、正文和 composer 子元素之间移动
- **THEN** 全栏反馈持续显示且不闪烁

#### Scenario: 环境不支持 backdrop filter
- **WHEN** 浏览器不支持 `backdrop-filter`
- **THEN** 半透明主题遮罩仍清晰表达可释放状态
- **AND** 中央提示保持可读

#### Scenario: 拖拽结束
- **WHEN** 文件离开聊天主栏、拖拽被取消、窗口失焦或释放完成
- **THEN** 系统立即清除全栏反馈和输入卡片激活状态

### Requirement: 全栏释放复用 composer 文件入口
系统 MUST 将聊天主栏任意位置收到的文件 drop 委托给现有 composer 文件处理链路，不得建立第二套附件上传或路径解析逻辑。

#### Scenario: 浏览器文件释放到正文
- **WHEN** 浏览器中的文件在会话正文区域释放
- **THEN** 文件通过现有 `onMediaFiles` 流程进入当前 composer
- **AND** 附件上传、预览和移除行为与在输入卡片释放时一致

#### Scenario: Windows Desktop 原生文件释放
- **WHEN** Windows Desktop 中的本地文件或文件夹在聊天主栏释放
- **THEN** 系统继续通过 WebView2 原生路径桥接处理 additional objects
- **AND** 路径物化结果进入当前 composer

#### Scenario: macOS Desktop 原生文件释放
- **WHEN** macOS Desktop 中的本地文件在聊天主栏释放
- **THEN** 系统继续允许 WKWebView 原生拖放处理
- **AND** 原生返回路径通过现有路径物化入口进入当前 composer

### Requirement: composer tag 具有可辨间距
系统 MUST 为 Slate composer 中的附件及其他内联 tag 提供明确的水平 gutter 和换行呼吸空间，并 MUST 不改变已发送消息徽标的共享布局。

#### Scenario: 连续附件 tag
- **WHEN** composer 中连续存在多个附件 tag
- **THEN** 相邻 tag 之间保留 5px 水平间距
- **AND** tag 不再视觉粘连

#### Scenario: 附件 tag 换行
- **WHEN** 多个附件 tag 因输入区宽度不足而换行
- **THEN** 相邻行保留轻微垂直间距
- **AND** tag 的删除、预览与键盘交互保持不变

### Requirement: Desktop 文件拖入只在进入时前置窗口
系统 MUST 在 Desktop 中的文件首次进入聊天主栏时请求一次主窗口前置，并 MUST 将该请求与文件落下后的 composer 光标恢复分离。系统 MUST NOT 在同一次拖拽的嵌套进入、持续移动或附件异步加入阶段重复请求原生前台。

#### Scenario: 文件进入非前台 ACECode 窗口
- **WHEN** 用户把外部文件首次拖入当前不在前台的 ACECode 聊天主栏
- **THEN** 系统立即通过文件拖入专用桥接尝试显示并前置主窗口
- **AND** 原生路径不使用临时置顶兜底制造持续高亮或闪烁

#### Scenario: 文件在同一次拖拽中跨子元素移动
- **WHEN** 文件已经进入聊天主栏并继续经过标题、正文和 composer 的嵌套元素
- **THEN** 系统不重复请求原生窗口前置
- **AND** 全栏拖入反馈仍持续显示

#### Scenario: 文件落下后用户切换到其他窗口
- **WHEN** 文件已经释放且 composer 开始加入或上传附件
- **THEN** 系统只恢复 composer 内部光标
- **AND** 系统不在稍后的附件状态更新中再次请求原生前台

### Requirement: 当前 composer 对同一文件去重
系统 MUST 让同一文件在单次加入批次和当前 composer 中最多出现一次，并 MUST 只为该文件发起一次附件上传。文件身份 MUST 优先使用规范化绝对来源路径；来源路径不可用时 MUST 使用稳定的浏览器文件元数据签名。

#### Scenario: 同一文件在一批文件中重复
- **WHEN** 同一来源文件在一次加入批次中出现多次
- **THEN** composer 只显示一个附件 tag
- **AND** 系统只发起一次上传

#### Scenario: 再次拖入 composer 已有文件
- **WHEN** 用户再次拖入当前 composer 已经包含或正在上传的同一文件
- **THEN** 系统不新增第二个附件 tag
- **AND** 系统不发起第二次上传

#### Scenario: Windows 路径书写形式不同
- **WHEN** 两个文件的绝对来源路径仅在盘符或字符大小写、正反斜杠上不同
- **THEN** 系统将它们识别为同一文件

#### Scenario: 浏览器无法提供来源路径
- **WHEN** 文件对象没有可用的绝对来源路径
- **THEN** 系统使用文件名、大小、MIME 类型与修改时间组成身份签名
- **AND** 相同签名在当前 composer 中只保留一个附件

#### Scenario: 文件被移除或上传失败后重试
- **WHEN** 用户移除附件，或附件上传失败并从 composer 清除
- **THEN** 系统释放该文件的去重预留
- **AND** 用户可以再次添加该文件

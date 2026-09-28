<!-- refactor-layout-map sha256:e2eb7cc27deba8a1e0bfb8fa3e6771094a4ccb875d33cab20578289cd5198965 -->
源码路径迁移请按 `scripts/refactor/src_layout_map.tsv` 换算；本设计中的历史路径保留。

# 分级主题定制设计

## 工作流

首轮三个单选问题使用用户给定的选项文案。快速只定制主页与样式；高级增加会话背景；深度再增加用户消息背景。所有层级都包含 UI 配色。不限明暗是设计偏好，最终按可读性选择并确认现有 light/dark 模式，不增加第三个渲染模式。

细节按当前范围问答，沿用已有明确答案。透明度明确为 0% 不透明、100% 完全透明，写入参数时换算为 opacity = 1 - 透明度/100。首页输入区建议 10%/30%/50%；图片建议 0%/30%/60%。背景主色给出与用户主题相关的三组具体颜色建议和自定义入口。深度模式还需确定用户消息素材；不把助手回复当作聊天卡片。

保留原生 palette/prototype 两次真实用户确认。位图可用时调用 image_generate；矢量通过本地 SVG/CSS 生成，自带贴图使用用户提供的本地素材。无生图模型、配置缺失或失败时用 HTML + Browser 展示预览，不反复重试收费生成。位图最终素材仍缺失时说明并让用户提供图片或切换素材模式，不能把占位框图作为最终壁纸。

## 资源与参数

保留 schema_version=1 与原有 background/thumbnail。可选描述符 session_background、user_message_background 分别对应固定根文件 session-background.png、user-message-background.png。不允许未知、未声明、重复或缺失文件；全部图片核验 PNG 签名、大小和 SHA-256。本地导入、安装及新增背景还验证解码；内置下载的两张原图保留原有校验规则。主题总体保持 16 MiB 限制。

appearance 增加 home_composer_opacity、home_background_opacity、session_background_opacity、user_message_background_opacity，均为有限 0..1 数值；增加对应三个背景底色（#RRGGBB）。原有 28 色、logo_color、home_title_color、extend_to_titlebar 不变。缺省值保留旧主题表现。颜色和透明度包含在色系确认中，额外图片包含在原型校验和中；修改后必须重新确认。

prototype 接受可选 session_background_path 和 user_message_background_path，全量替换素材并清除旧可选资源引用。install 仍只接受 draft_id。ThemeStore 统一枚举主题定义声明的图片，使本地安装、导入、导出、读取和远程安装校验保持一致。

## 表现

首页图片透明度只作用于壁纸层，首页输入框透明度只作用于背景填充，文字/图标保持不透明。会话图片覆盖聊天主区域的底层，侧栏和弹窗不受影响。用户消息图片只应用于 UserBubble 中的消息正文气泡。切换主题清理全部 CSS 覆盖，资源替换、删除或组件卸载时释放全部图片 URL；旧主题不增加会话或消息背景。

HTML 预览复用用户已确认的深浅框图，每页主页在上、聊天页在下。通过相同参数填充三个区域及输入框；图片可来自位图、可编辑 SVG 或本地贴图。浏览器截图作为 prototype 的 preview_path；SVG 在浏览器中渲染为 PNG 供现有安全图片协议安装，保留 SVG 源稿供用户后续编辑。

## 范围

不改应用布局、通用聊天消息行为、权限控制或生图实现。不覆盖未提交的无关代码。工坊仍需管理员确认才能公开主题；本次仅更新源代码兼容新包，不执行服务器部署。

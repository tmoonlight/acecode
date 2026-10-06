---
name: ACECode 像素办公室
description: 桌面宠物资源的局部视觉规则
colors:
  office-paper: "#fff9e9"
  office-ink: "#27324f"
  office-line: "#a8a6a4"
  office-selected: "#dce7f6"
  scene-outline: "#2b2737"
typography:
  body:
    fontFamily: '"Pixelify Sans", "PingFang SC", "Hiragino Sans GB", "Microsoft YaHei", "Noto Sans SC", system-ui, sans-serif'
    fontSize: "12px"
    lineHeight: 1.4
  label:
    fontFamily: '-apple-system, BlinkMacSystemFont, "PingFang SC", "Segoe UI", sans-serif'
    fontSize: "12px"
    lineHeight: 1.3
  label-compact:
    fontFamily: '-apple-system, BlinkMacSystemFont, "PingFang SC", "Segoe UI", sans-serif'
    fontSize: "11px"
    lineHeight: 1.3
rounded:
  control: "4px"
  bubble: "6px"
spacing:
  control-group: "4px"
components:
  control:
    backgroundColor: "{colors.office-paper}"
    textColor: "{colors.office-ink}"
    typography: "{typography.label}"
    rounded: "{rounded.control}"
    padding: "5px 7px"
  control-selected:
    backgroundColor: "{colors.office-selected}"
    textColor: "{colors.office-ink}"
    rounded: "{rounded.control}"
---

# Design System: ACECode 像素办公室

## Overview

**Creative North Star: "保留原作的像素办公室"**

本规范仅用于桌面宠物资源。视觉权威是已指定的原作；当前实现证据是 [agent_office_pet.html](agent_office_pet.html)。等距房间、家具、角色和透明外缘延续原作，新增会话控件保持紧凑。此处不定义 ACECode 全局品牌或通用页面风格。

**Key Characteristics:**

- 房间是主体，顶部控制条承担会话切换与跟随。
- 场景用像素轮廓和分面明暗，操作文字优先保证中文辨认。
- 同一会话保持稳定布局和角色外观；动画表达真实状态变化。

## Colors

房间保留原有多套地面、墙面和家具调色板；交互层使用独立的浅纸色与深墨色。

### Primary

选中控件使用 `office-selected`，通过整面底色和四边边框共同区分状态。

### Neutral

`office-paper` 用于控件、通知、工具图标底和成员列表；`office-ink` 用于操作文字与工具图标；`office-line` 用于控件及成员列表边界。`scene-outline` 保留给像素精灵轮廓。

**The 分层配色 Rule.** 房间材质色服务于像素物件，操作层的状态色服务于真实交互；两者不互相推断业务状态。

## Typography

场景气泡继承 `body` 的内嵌 Pixelify Sans 与中文回退字体。顶部控件使用 `label`，窄窗口使用 `label-compact`；成员列表使用系统正文。这里没有展示标题字阶。

会话标题保持单行省略，完整工作区和会话名保留在提示与可访问名称中。气泡有宽度上限和省略处理；缩放房间时不应按比例无限缩小操作文字。

## Layout

画布基准为 (344 × 252)，房间整体向下为控制条留出空间。场景随窗口等比缩放，控制条固定在场景顶部，保持一行；窄于 (400px) 时收紧字号与间距，窄于 (250px) 时继续压缩控件内边距。

房间有经典、横排、纵排、L 形、工作岛五套模板，均保留主 agent 和七个子 agent 工位、家具区及机器人路线。布局和配色由会话种子决定。

**The 稳定房间 Rule.** 同一会话的状态刷新不重新随机布置家具或改换角色外观。

控制条的紧凑组间距仅适用于这一小型桌面窗口。展开成员列表限高并独立滚动，不压缩或重排房间。

## Elevation & Depth

家具与墙体通过等距分面、统一轮廓和遮挡顺序形成深度；透明区域保留桌面背景。操作条主要依靠底色和细边框区分层级。

原作气泡带短距离像素投影，属于该场景的既有物件语言；不将此投影扩展为普通按钮、列表或 ACECode 其他页面的阴影规则。

## Shapes

场景以离散像素、等距长方体和精灵轮廓构成。操作控件采用小圆角与完整细边框，气泡保留尾角。工具图标使用与聊天界面共源的 SVG，不用文字字符代替。

整数倍缩放保持清晰像素；非整数倍先做整数倍像素放大，再缩至窗口尺寸，维持像素宽窄的稳定。

## Components

### 会话控件

浅纸色紧凑按钮，选中时整面变色。悬停改变底色，键盘焦点有独立轮廓；跟随开关用方形选中标记，并同步 `aria-checked`。

顶部控制条平时隐藏，鼠标进入办公室时显示，离开后保留 1 秒；重入取消隐藏，键盘焦点保持可见。右侧依次放置 pin 与关闭 SVG 按钮，使用同组按钮的颜色、边框与焦点轮廓；pin 的选中底色与 `aria-pressed` 同步原生置顶状态。隐藏控件不截获透明区域的点击，最小窗口仍保留所有按钮。

### 场景状态

角色动作、气泡和工具图标共同呈现状态。忙碌期间气泡常驻，一行「图标 动作 · 细节」：动作用短的现在进行时动词，细节用系统界面字体（路径、命令、英文推理在像素字体里难以辨认），放不下时从开头裁掉，保留最新的字和文件名；正在输出的气泡带闪烁光标。等待授权、等待回答、失败、停止均有明确文案；工具图标只在执行或准备工具时显示。上下文文件堆属于相应工位，依据当前上下文使用率呈现。

### 交接

成员加入、完成和 agent 间消息用走路与信封表达。进门时门先向屋里打开，人在门槛上淡入后走进来；坐下时沿用原作新人入职的效果——跳一跳、头顶冒星星、说「报到！」。离开时先说「再见」，再以约 1.7 倍速走到门口、在门槛淡出，门随后关上。信封带两格残影的抛物线和黄色标签（任务 / 回报 / 消息 / 新任务），落地时收件人头顶冒「!」。走路沿 2 单位网格寻路、不穿家具。

### 溢出成员

超过可见工位时显示 `+N`，展开后列出成员名称和当前状态。列表随快照更新；已离开的成员移除。列表仍展开时，失效焦点回到溢出入口；溢出消失时关闭列表。

## Do's and Don'ts

### Do:

- **Do** 保留原作家具、透明房间、等距像素构形与稳定会话布局。
- **Do** 让控制条、提示和成员列表保持可读，并把状态变化绑定到真实快照。
- **Do** 为会话按钮和跟随开关保留焦点及可访问状态；减少动态效果偏好下不走路，但保留交接信封（它承载信息）。

### Don't:

- **Don't** 把局部宠物界面改造成卡片仪表盘或给透明场景铺满页面底色。
- **Don't** 用装饰性忙碌动画冒充执行、完成、失败或上下文数据。
- **Don't** 将这份局部规范中的小字号、紧凑间距和像素投影推广为全站默认样式。

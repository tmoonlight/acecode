# 鲸鱼娘

来源：[vlln/whale-girl](https://github.com/vlln/whale-girl)，固定提交 `9c32d7a39a45148076f8ad2fdaad2dc2420eedb3`。原角色作者为 ZipZipPipe，精灵帧由上游项目基于角色设计生成。保留上游 MIT `LICENSE`，完整文件来源与哈希见 `sources.json`。

15 套动作全部保留：idle、working、celebrate、error、disappointed、joy、eat、play、drag、walk、sleep、wake、welcome、think、wait。原始 `manifest.json` 保留每套动作的帧数、帧率、播放模式与特殊时长。

`map.png` 将原 PNG 精灵图按 manifest 顺序纵向排列，每行动作的原 RGBA 像素不变，不缩放、不重绘。每帧 256×256；`agent.js` 将布局、时长和 pingpong 顺序适配为共用播放器格式，额外增加与现有会话状态对应的动作别名。`preview.png` 为 idle 首帧。

页面不直接使用 `map.png`：2026-10-31 起每帧由 Codex 生图按原帧重画成 512×512 的高清版，放在 `hd/`（按动作分组的 WebP 与帧表 `hd/frames.js`），`map.png` 作为重画参考与对齐基准保留原样。流程与脚本见 `../classic/README.md` 的「高清帧」一节。

## 趴边偷看（peek.png）

鲸鱼娘拖到屏幕左/右边缘吸附后改用 `peek.png`：两帧并排（左睁眼、右闭眼），每帧 178×300，透明背景，画面右缘就是她躲着的那条边（贴左边时页面水平镜像）。不来自上游项目，2026-10-16 用 Codex 的内置图像生成（codex-cli 0.162，`image_generation` 功能）绘制，以本目录 idle / welcome / joy 的首帧作为风格参考，要求同一角色设计与画风：从画面右缘探出半个头、双手扒着边、好奇又害羞地偷看。闭眼帧由同一张图做「只闭眼、其余不变」的编辑，再只把眼睛区域贴回睁眼图，眨眼时其余线条不抖动。纯绿背景抠成透明并去绿边，在她扒着的那条边线处裁掉，两帧同裁同缩放。这张原画是原 256 帧的约 1.8 倍，也就是高清帧（512）的 0.9 倍，页面按常规帧缩放 ÷ 0.9 绘制（`classic_pet.js` 的 `PEEK.whale.ratio`），换图或改高清帧尺寸时要重新量这个比例。生成用的是 ZipZipPipe 设计的角色形象，沿用上游许可说明。

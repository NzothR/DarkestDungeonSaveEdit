# UI 美术布局编辑器

这是一个独立运行的布局制作工具，不属于主前端页面。它只扫描已经处理好的图片，保存图片与 UI 关联之间的关系以及相对背景画布的位置和尺寸。

## 启动

工具只使用 Python 标准库，不需要额外安装依赖。默认使用当前目录下的 `ui-art-assets` 作为美术资源目录，并把布局保存为 `ui-layout.json`：

```powershell
conda activate base
python tools/ui_layout_editor.py
```

也可以显式指定目录和输出文件：

```powershell
python tools/ui_layout_editor.py `
  --assets-dir "D:\Project\DarkestDungeonSaveEdit\ui-art-assets" `
  --output "D:\Project\DarkestDungeonSaveEdit\config\town-layout.json"
```

启动后工具会打开浏览器。使用 `Ctrl+C` 停止本地服务，`--no-browser` 可以禁止自动打开浏览器。

工具每次请求资源列表时都会重新扫描目录。把新图片放入目录后点击“刷新资源”即可看到最新文件。支持 PNG、JPG、JPEG、WEBP、GIF、BMP 和 SVG。

## 编辑流程

1. 点击顶部“添加组件”。
2. 搜索并选择一张图片。
3. 第一个组件必须选择背景关联。背景图片加载后，它的实际像素尺寸会成为画布尺寸和宽高比。
4. 选择预设关联，或点击“新增关联”填写名称、ID 和放置方式。
5. 普通组件会放在背景画布中央并立即进入编辑模式。拖动组件改变相对位置，拖动右下角控制点按比例缩放。
6. 点击“编辑完成”退出编辑模式。之后右键组件可以再次编辑。
7. 点击底部“保存”，输入文件名后写入布局 JSON。文件名只能是文件名本身，扩展名不是 `.json` 时会自动补上；每次保存可以写入同一输出目录下的不同布局文件。

背景和底栏是特殊关联：

- `town_background`：自动填满整个画布，不进入编辑模式。
- `bottom_bar`：自动贴在画布底部，不进入编辑模式。

预设关联的 ID 都遵循变量名规则。自定义关联的 ID 也必须以英文或下划线开头，只能包含英文、数字和下划线，例如 `dlc_crimson_court_banner`。名称用于编辑器显示，ID 会写入布局文件并供主程序调用。

## 布局文件

保存文件包含：

- `canvas`：固定为 `1920×1080`；
- `associations`：关联 ID、显示名称和放置类型；
- `components`：图片相对路径、关联 ID、位置、尺寸和层级。

图片路径始终是相对于 `--assets-dir` 的路径。服务端会拒绝绝对路径、`..` 路径和未在资源目录内的文件，避免布局文件引用目录外的内容。

主前端后续只需要读取这个布局文件，将 `associationId` 映射到具体功能即可。新增装饰图片或可点击关联不需要修改编辑器代码，直接通过“新增关联”保存即可。

## 通用化设计

画布、组件、关联和几何数据不包含小镇专用字段。画布尺寸由背景图片决定，不再固定为 1920×1080；组件坐标、宽高都相对于这张背景画布保存。当前小镇名称列表位于 `tools/ui_layout_editor/presets.json`，也可以通过 `--preset-file` 换成其他界面的预设关联：

```powershell
python tools/ui_layout_editor.py --preset-file .\config\hero-editor-presets.json
```

因此后续制作英雄编辑界面时可以复用同一个编辑器，只需要提供新的背景、关联预设和美术资源目录。`free` 关联允许拖动缩放，`background` 与 `bottom_bar` 是目前定义的固定放置类型；其他特殊放置规则可以继续扩展为新的 placement 类型。没有背景时不能放置其他组件，也不能保存布局。

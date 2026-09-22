# Third-party notices

LanFileTransfer 使用以下第三方组件。发布者应随二进制分发相应许可证文本，并根据实际 Qt/FFmpeg 构建配置复核义务。

## Qt 6

本项目使用 Qt Core、Gui、QML、Quick、Network、SQL、Widgets、Multimedia 和 Concurrent。开源版 Qt 各模块通常按 LGPLv3/GPLv3 等条款提供，亦可使用商业许可证。Windows 发布包采用动态链接 Qt DLL，不修改 Qt 本身。

- 项目：https://www.qt.io/
- 许可证：https://www.qt.io/licensing/

## FFmpeg

Qt Multimedia 在当前构建中使用 FFmpeg 后端。实际许可取决于所分发 FFmpeg 构建及其启用的编解码器；当前 Qt 日志报告 LGPL 版本。

- 项目：https://ffmpeg.org/
- 许可证：https://ffmpeg.org/legal.html

## Material Icons

界面使用 Material Icons 字体，按 Apache License 2.0 提供。原许可证文本位于 `img/MATERIAL_ICONS_LICENSE.txt`，Windows 打包脚本会将其复制到发布目录。


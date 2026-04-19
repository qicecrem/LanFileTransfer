import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Controls.Material

Item {
    id: desktopRoot
    anchors.fill: parent

    // 全局拖拽遮罩反馈（最高层级）
    Rectangle {
        id: dropOverlay
        anchors.fill: parent
        z: 100
        color: "#CCFAFAFA" // 半透明毛玻璃感
        visible: dropArea.containsDrag

        Rectangle {
            anchors.centerIn: parent
            width: 350
            height: 220
            color: "transparent"
            border.color: Material.accentColor
            border.width: 3

            radius: 16

            ColumnLayout {
                anchors.centerIn: parent
                spacing: 15
                Text {
                    text: "📁"
                    font.pixelSize: 64
                    Layout.alignment: Qt.AlignHCenter
                }
                Label {
                    text: root.targetIp !== "" ? "松开鼠标，发送至 " + root.targetName : "⚠️ 请先在左侧选择目标设备"
                    font.pixelSize: 18
                    font.bold: true
                    color: root.targetIp !== "" ? Material.primaryTextColor : Material.color(Material.Red)
                    Layout.alignment: Qt.AlignHCenter
                }
            }
        }
    }

    DropArea {
        id: dropArea
        anchors.fill: parent
        onDropped: drop => {
            if (root.targetIp !== "" && drop.hasUrls) {
                root.transferSvc.sendFiles(drop.urls, root.targetIp, root.targetPort);
            } else if (root.targetIp === "") {
                root.showToast("⚠️ 请先在左侧选择目标设备！");
            }
        }
    }

    SplitView {
        anchors.fill: parent
        orientation: Qt.Horizontal
        handle: Rectangle {
            implicitWidth: 1
            color: Material.dividerColor
        }

        // ==========================================
        // 左侧：设备发现区 (视觉层级：导航侧边栏)
        // ==========================================
        Rectangle {
            SplitView.preferredWidth: 280
            SplitView.minimumWidth: 240
            color: Material.dialogColor // 侧边栏底色略深

            ColumnLayout {
                anchors.fill: parent
                spacing: 0

                // === 修改：统一的左侧 Header（包含本机信息和在线设备标题） ===
                Pane {
                    Layout.fillWidth: true
                    Material.elevation: 1
                    padding: 10

                    ColumnLayout {
                        anchors.fill: parent
                        spacing: 10

                        // 1. 本机名称设置
                        RowLayout {
                            Layout.fillWidth: true
                            Label {
                                text: "💻"
                                font.pixelSize: 18
                            }
                            TextField {
                                Layout.fillWidth: true
                                Layout.preferredHeight: 40
                                text: root.discoverySvc.deviceName
                                placeholderText: "本机名称"
                                font.bold: true
                                onEditingFinished: {
                                    if (text.trim() !== "") {
                                        root.discoverySvc.deviceName = text.trim();
                                        root.showToast("✅ 本机名称已更新");
                                    } else {
                                        text = root.discoverySvc.deviceName;
                                    }
                                    focus = false;
                                }
                            }
                        }

                        // 分隔线
                        Rectangle {
                            Layout.fillWidth: true
                            height: 1
                            color: Material.dividerColor
                        }

                        // 2. 在线设备标题
                        RowLayout {
                            Layout.fillWidth: true
                            Label {
                                text: "在线设备"
                                font.pixelSize: 15
                                font.bold: true
                                Layout.fillWidth: true
                                color: Material.secondaryTextColor
                            }
                            ToolButton {
                                text: "🔄"
                                font.pixelSize: 16
                                ToolTip.visible: hovered
                                ToolTip.text: "重新扫描"
                                onClicked: {
                                    root.discoverySvc.stopScan();
                                    root.discoverySvc.startScan();
                                }
                            }
                        }
                    }
                }

                ListView {
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    model: root.discoverySvc.deviceList
                    clip: true
                    boundsBehavior: Flickable.StopAtBounds

                    Label {
                        anchors.centerIn: parent
                        text: "正在寻找附近的设备..."
                        color: Material.hintTextColor
                        visible: parent.count === 0
                    }

                    // 列表项过渡动画
                    add: Transition {
                        NumberAnimation {
                            property: "opacity"
                            from: 0
                            to: 1
                            duration: 250
                        }
                        NumberAnimation {
                            property: "y"
                            from: -20
                            duration: 250
                            easing.type: Easing.OutQuad
                        }
                    }

                    delegate: ItemDelegate {
                        id: delegateItem
                        width: ListView.view.width
                        height: 70
                        property bool isSelected: root.targetIp === modelData.ip && root.targetPort === modelData.port

                        background: Rectangle {
                            color: delegateItem.isSelected ? Material.listHighlightColor : (delegateItem.hovered ? "#0A000000" : "transparent")
                            // 左侧强调线
                            Rectangle {
                                width: 4
                                height: parent.height
                                color: Material.accentColor
                                visible: delegateItem.isSelected
                            }
                        }

                        onClicked: {
                            root.targetIp = modelData.ip;
                            root.targetPort = modelData.port;
                            root.targetName = modelData.name;
                        }

                        contentItem: RowLayout {
                            spacing: 15
                            anchors.fill: parent
                            anchors.leftMargin: 15

                            Rectangle {
                                width: 40
                                height: 40
                                radius: 20
                                color: delegateItem.isSelected ? Material.accentColor : "#E0E0E0"
                                Label {
                                    anchors.centerIn: parent
                                    text: "📱"
                                    font.pixelSize: 20
                                }
                            }
                            ColumnLayout {
                                spacing: 2
                                Label {
                                    text: modelData.name
                                    font.bold: true
                                    font.pixelSize: 15
                                    color: delegateItem.isSelected ? Material.accentColor : Material.primaryTextColor
                                }
                                Label {
                                    text: modelData.ip
                                    color: Material.secondaryTextColor
                                    font.pixelSize: 12
                                }
                            }
                        }
                    }
                }

                Button {
                    Layout.fillWidth: true
                    Layout.margins: 10
                    text: "🛡️ 修复不可见"
                    flat: true
                    visible: Qt.platform.os === "windows"
                    onClicked: {
                        if (root.discoverySvc.fixWindowsFirewall()) {
                            root.showToast("✅ 防火墙修复成功");
                            root.discoverySvc.startScan();
                        }
                    }
                }
            }
        }

        // ==========================================
        // 右侧：主要交互区 (焦点区域)
        // ==========================================
        Rectangle {
            SplitView.fillWidth: true
            color: Material.backgroundColor

            ColumnLayout {
                anchors.fill: parent
                spacing: 15

                // 右侧 Header
                ToolBar {

                    Layout.fillWidth: true
                    Material.elevation: 2
                    background: Rectangle {
                        color: Material.dialogColor
                    }

                    RowLayout {
                        anchors.fill: parent
                        anchors.margins: 15
                        Label {
                            text: root.targetIp === "" ? "请先在左侧选择要连接的设备" : "📡 正在与 " + root.targetName + " 互联"
                            font.pixelSize: 18
                            font.bold: true
                            color: root.targetIp === "" ? Material.hintTextColor : Material.primaryTextColor
                            Layout.fillWidth: true
                        }
                    }
                }

                TabBar {
                    id: rightTabBar
                    Layout.fillWidth: true
                    TabButton {
                        text: "📂 文件传输"
                    }
                    TabButton {
                        text: "💬 文本消息"
                    }
                }

                StackLayout {
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    currentIndex: rightTabBar.currentIndex

                    // ---------------- 1. 文件传输视图 ----------------
                    ColumnLayout {
                        Layout.margins: 20
                        spacing: 15

                        RowLayout {
                            Layout.fillWidth: true
                            Button {
                                text: "选择文件发送..."
                                highlighted: true
                                enabled: root.targetIp !== ""
                                onClicked: root.openFileDialog()
                            }
                            Item {
                                Layout.fillWidth: true
                            } // 占位把清除按钮挤到右边
                            Button {
                                text: "清除记录"
                                flat: true
                                onClicked: root.clearHistory()
                            }
                        }

                        ListView {
                            Layout.fillWidth: true
                            Layout.fillHeight: true
                            model: root.sharedTaskModel
                            clip: true
                            spacing: 12

                            Label {
                                anchors.centerIn: parent
                                text: "空空如也\n拖拽文件到窗口直接发送"
                                horizontalAlignment: Text.AlignHCenter
                                color: Material.hintTextColor
                                font.pixelSize: 16
                                visible: parent.count === 0
                            }

                            add: Transition {
                                NumberAnimation {
                                    property: "opacity"
                                    from: 0
                                    to: 1
                                    duration: 300
                                }
                                NumberAnimation {
                                    property: "scale"
                                    from: 0.9
                                    to: 1.0
                                    duration: 300
                                    easing.type: Easing.OutBack
                                }
                            }

                            delegate: Pane {
                                width: ListView.view.width - 10
                                anchors.horizontalCenter: parent.horizontalCenter
                                Material.elevation: 1 // 卡片式阴影
                                padding: 15

                                Menu {
                                    id: contextMenu
                                    MenuItem {
                                        text: "📂 打开所在文件夹"
                                        onClicked: root.transferSvc.openFolder()
                                    }
                                    MenuItem {
                                        text: "❌ 删除记录"
                                        onClicked: root.sharedTaskModel.remove(index)
                                    }
                                }
                                TapHandler {
                                    acceptedButtons: Qt.RightButton
                                    onTapped: contextMenu.popup()
                                }

                                RowLayout {
                                    anchors.fill: parent
                                    spacing: 15
                                    Rectangle {
                                        width: 46
                                        height: 46
                                        radius: 23
                                        color: model.isSender ? "#E3F2FD" : "#E8F5E9"
                                        Label {
                                            anchors.centerIn: parent
                                            text: model.isSender ? "⬆️" : "⬇️"
                                            font.pixelSize: 20
                                        }
                                    }
                                    ColumnLayout {
                                        Layout.fillWidth: true
                                        RowLayout {
                                            Label {
                                                text: model.fileName
                                                font.bold: true
                                                font.pixelSize: 15
                                                elide: Text.ElideMiddle
                                                Layout.fillWidth: true
                                            }
                                            Label {
                                                text: model.sizeStr
                                                color: Material.secondaryTextColor
                                            }
                                        }
                                        ProgressBar {
                                            Layout.fillWidth: true
                                            value: model.progress
                                            Material.accent: model.status.includes("完成") ? Material.Green : model.status.includes("失败") ? Material.Red : Material.Blue
                                        }
                                        Label {
                                            text: model.status + (model.progress > 0 && model.progress < 1 ? " (" + Math.round(model.progress * 100) + "%)" : "")
                                            color: model.status.includes("完成") ? Material.color(Material.Green) : Material.secondaryTextColor
                                            font.pixelSize: 12
                                        }
                                    }
                                }
                            }
                        }

                        RowLayout {
                            Layout.fillWidth: true
                            Label {
                                text: "保存至:"
                                color: Material.secondaryTextColor
                            }
                            TextField {
                                Layout.fillWidth: true
                                readOnly: true
                                text: root.transferSvc.saveDirectory
                                background: null
                                color: Material.secondaryTextColor
                            }
                            Button {
                                text: "更改"
                                flat: true
                                onClicked: root.openFolderDialog()
                            }
                        }
                    }

                    // ---------------- 2. 文本消息视图 ----------------
                    ColumnLayout {
                        Layout.margins: 20
                        spacing: 15

                        ListView {
                            id: chatListView  // 1. 给 ListView 一个显式 ID，避免使用通用的 ListView.view
                            Layout.fillWidth: true
                            Layout.fillHeight: true
                            model: root.sharedChatModel
                            clip: true
                            spacing: 20

                            Label {
                                anchors.centerIn: parent
                                text: "没有任何消息..."
                                color: Material.hintTextColor
                                visible: chatListView.count === 0
                            }

                            delegate: ColumnLayout {
                                // 使用 ListView 的宽度作为基准
                                width: chatListView.width
                                spacing: 4

                                // 聊天气泡背景
                                Rectangle {
                                    id: bubble // 2. 给气泡增加 ID
                                    Layout.alignment: model.isMe ? Qt.AlignRight : Qt.AlignLeft

                                    // ：使用隐式宽度计算，并增加逻辑保护防止 null
                                    width: msgText.width + 24
                                    height: msgText.height + 16
                                    color: model.isMe ? "#95EC69" : Material.dialogColor
                                    radius: 12
                                    Material.elevation: 1

                                    Label {
                                        id: msgText
                                        anchors.centerIn: parent
                                        text: model.message
                                        color: Material.primaryTextColor
                                        wrapMode: Label.WrapAnywhere

                                        // 修复：显式使用 chatListView.width 并增加安全判断
                                        width: Math.min(implicitWidth, (chatListView.width > 0 ? chatListView.width : 300) * 0.75 - 24)
                                    }

                                    // 隐藏的文本编辑框用于剪贴板
                                    TextEdit {
                                        id: clipboardHelper
                                        visible: false
                                    }

                                    // 完美兼容 PC (右键) 和 手机 (长按)
                                    TapHandler {
                                        acceptedButtons: Qt.LeftButton | Qt.RightButton

                                        // 3. 修复：通过 bubble 显式调用函数
                                        onLongPressed: bubble.copyText()

                                        onTapped: (eventPoint, button) => {
                                            if (button === Qt.RightButton) {
                                                bubble.copyText();
                                            }
                                        }
                                    }

                                    // 封装复制函数
                                    function copyText() {
                                        clipboardHelper.text = model.message;
                                        clipboardHelper.selectAll();
                                        clipboardHelper.copy();
                                        root.showToast("✅ 已复制到剪贴板");
                                    }
                                }

                                // 发送者与时间
                                Label {
                                    Layout.alignment: model.isMe ? Qt.AlignRight : Qt.AlignLeft
                                    text: model.senderName + " · " + model.time
                                    color: Material.secondaryTextColor
                                    font.pixelSize: 11
                                    Layout.rightMargin: model.isMe ? 10 : 0
                                    Layout.leftMargin: model.isMe ? 0 : 10
                                }
                            }
                        }

                        RowLayout {
                            Layout.fillWidth: true
                            TextField {
                                id: chatInput
                                Layout.fillWidth: true
                                placeholderText: root.targetIp === "" ? "请先选择设备..." : "输入消息内容，回车发送..."
                                enabled: root.targetIp !== ""
                                onAccepted: sendBtn.clicked()
                            }
                            Button {
                                id: sendBtn
                                text: "发送"
                                highlighted: true
                                enabled: root.targetIp !== ""
                                onClicked: {
                                    root.sendTextMessage(chatInput.text);
                                    chatInput.text = "";
                                }
                            }
                        }
                    }
                }
            }
        }
    }
}

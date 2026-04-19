import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Controls.Material

Item {
    id: mobileRoot
    anchors.fill: parent

    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        // 统一沉浸式 AppBar
        ToolBar {
            Layout.fillWidth: true
            Material.primary: Material.Blue
            Material.elevation: 4

            RowLayout {
                anchors.fill: parent
                anchors.margins: 16
                Label {
                    text: root.targetIp === "" ? "请选择设备" : "连线: " + root.targetName
                    font.pixelSize: 20
                    font.bold: true
                    color: "white"
                    elide: Label.ElideRight
                    Layout.fillWidth: true
                }
            }
        }

        SwipeView {
            id: swipeView
            Layout.fillWidth: true
            Layout.fillHeight: true
            onCurrentIndexChanged: tabBar.currentIndex = currentIndex

            // ==========================================
            // Page 1: 发现设备
            // ==========================================
            Page {
                background: Rectangle {
                    color: Material.backgroundColor
                }

                ColumnLayout {
                    anchors.fill: parent
                    anchors.margins: 10
                    spacing: 10

                    // === 新增：本机名称设置区 ===
                    Pane {
                        Layout.fillWidth: true
                        padding: 10
                        background: Rectangle {
                            color: Material.dialogColor
                            radius: 12
                        }

                        RowLayout {
                            anchors.fill: parent
                            spacing: 10
                            Rectangle {
                                width: 40
                                height: 40
                                radius: 20
                                color: Material.accentColor
                                Label {
                                    anchors.centerIn: parent
                                    text: "🙋"
                                    font.pixelSize: 20
                                }
                            }
                            ColumnLayout {
                                Layout.fillWidth: true
                                spacing: 0
                                Label {
                                    text: "本机名称 (点击修改)"
                                    font.pixelSize: 12
                                    color: Material.secondaryTextColor
                                }
                                TextField {
                                    Layout.fillWidth: true
                                    text: root.discoverySvc.deviceName
                                    font.pixelSize: 16
                                    font.bold: true
                                    background: null // 去掉底边框，保持干净
                                    padding: 0
                                    onEditingFinished: {
                                        if (text.trim() !== "") {
                                            root.discoverySvc.deviceName = text.trim();
                                            root.showToast("✔ 名称已更新，下次广播生效");
                                        } else {
                                            text = root.discoverySvc.deviceName; // 恢复旧值
                                        }
                                        focus = false; // 取消焦点收起键盘
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
                        spacing: 10

                        Label {
                            anchors.centerIn: parent
                            text: "未发现附近的设备\n请确保处于同一WiFi下"
                            color: Material.hintTextColor
                            horizontalAlignment: Text.AlignHCenter
                            visible: parent.count === 0
                        }

                        delegate: ItemDelegate {
                            width: ListView.view.width
                            padding: 15
                            Material.elevation: isSelected ? 3 : 1
                            property bool isSelected: root.targetIp === modelData.ip && root.targetPort === modelData.port

                            // 圆角卡片与边框强调
                            background: Rectangle {
                                color: Material.dialogColor
                                radius: 12
                                border.color: isSelected ? Material.accentColor : "transparent"
                                border.width: 2
                            }

                            onClicked: {
                                root.targetIp = modelData.ip;
                                root.targetPort = modelData.port;
                                root.targetName = modelData.name;
                                swipeView.currentIndex = 1; // 选中后自动跳到传输页
                            }

                            contentItem: RowLayout {
                                spacing: 15
                                Rectangle {
                                    width: 50
                                    height: 50
                                    radius: 25
                                    color: isSelected ? Material.accentColor : "#E0E0E0"
                                    Label {
                                        anchors.centerIn: parent
                                        text: "📱"
                                        font.pixelSize: 24
                                    }
                                }
                                ColumnLayout {
                                    Layout.fillWidth: true
                                    Label {
                                        text: modelData.name
                                        font.bold: true
                                        font.pixelSize: 16
                                        color: isSelected ? Material.accentColor : Material.primaryTextColor
                                    }
                                    Label {
                                        text: modelData.ip
                                        color: Material.secondaryTextColor
                                        font.pixelSize: 13
                                    }
                                }
                            }
                        }
                    }

                    Button {
                        text: "重新扫描"
                        Layout.fillWidth: true
                        Layout.preferredHeight: 50
                        highlighted: true
                        onClicked: {
                            root.discoverySvc.stopScan();
                            root.discoverySvc.startScan();
                        }
                    }
                }
            }

            // ==========================================
            // Page 2: 传输记录与文件发送
            // ==========================================
            Page {
                background: Rectangle {
                    color: Material.backgroundColor
                }

                ListView {
                    id: taskList
                    anchors.fill: parent
                    anchors.margins: 10
                    model: root.sharedTaskModel
                    clip: true
                    spacing: 10

                    Label {
                        anchors.centerIn: parent
                        text: "暂无传输任务\n点击右下角按钮发送文件"
                        horizontalAlignment: Text.AlignHCenter
                        color: Material.hintTextColor
                        visible: parent.count === 0
                    }

                    delegate: Pane {
                        width: ListView.view.width
                        padding: 12
                        Material.elevation: 1
                        background: Rectangle {
                            color: Material.dialogColor
                            radius: 10
                        }

                        // 移动端长按菜单
                        Menu {
                            id: contextMenu
                            x: parent.width / 2
                            y: parent.height / 2
                            MenuItem {
                                text: "打开下载目录"
                                onClicked: root.transferSvc.openFolder()
                            }
                            MenuItem {
                                text: "删除此记录"
                                onClicked: root.sharedTaskModel.remove(index)
                            }
                        }
                        TapHandler {
                            onLongPressed: contextMenu.popup()
                        }

                        RowLayout {
                            anchors.fill: parent
                            spacing: 12
                            Rectangle {
                                width: 44
                                height: 44
                                radius: 22
                                color: model.isSender ? "#E3F2FD" : "#E8F5E9"
                                Label {
                                    anchors.centerIn: parent
                                    text: model.isSender ? "⬆" : "⬇"
                                    font.pixelSize: 18
                                }
                            }
                            ColumnLayout {
                                Layout.fillWidth: true
                                spacing: 2
                                RowLayout {
                                    Label {
                                        text: model.fileName
                                        font.bold: true
                                        elide: Text.ElideMiddle
                                        Layout.fillWidth: true
                                        font.pixelSize: 15
                                    }
                                    Label {
                                        text: model.sizeStr
                                        color: Material.secondaryTextColor
                                        font.pixelSize: 12
                                    }
                                }
                                ProgressBar {
                                    Layout.fillWidth: true
                                    value: model.progress
                                    Material.accent: model.status.includes("完成") ? Material.Green : Material.Blue
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

                // 悬浮操作按钮 (FAB) - 黄金操作区
                RoundButton {
                    text: "+"
                    font.pixelSize: 28
                    width: 64
                    height: 64
                    anchors.right: parent.right
                    anchors.bottom: parent.bottom
                    anchors.margins: 20
                    highlighted: true
                    Material.elevation: 6
                    enabled: root.targetIp !== ""
                    onClicked: root.openFileDialog()
                    opacity: enabled ? 1.0 : 0.5
                }
            }

            // ==========================================
            // Page 3: 聊天通信
            // ==========================================
            Page {
                background: Rectangle {
                    color: Material.backgroundColor
                }

                ColumnLayout {
                    anchors.fill: parent
                    anchors.margins: 12
                    spacing: 10

                    ListView {
                        id: chatListView
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
                            width: chatListView.width
                            spacing: 4

                            Rectangle {
                                id: bubble
                                Layout.alignment: model.isMe ? Qt.AlignRight : Qt.AlignLeft

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
                                    width: Math.min(implicitWidth, (chatListView.width > 0 ? chatListView.width : 300) * 0.75 - 24)
                                }

                                TextEdit {
                                    id: clipboardHelper
                                    visible: false
                                }

                                TapHandler {
                                    acceptedButtons: Qt.LeftButton | Qt.RightButton
                                    onLongPressed: bubble.copyText()
                                    onTapped: (eventPoint, button) => {
                                        if (button === Qt.RightButton) {
                                            bubble.copyText();
                                        }
                                    }
                                }

                                function copyText() {
                                    clipboardHelper.text = model.message;
                                    clipboardHelper.selectAll();
                                    clipboardHelper.copy();
                                    root.showToast("✔ 已复制到剪贴板");
                                }
                            }

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
                            id: mobileChatInput
                            Layout.fillWidth: true
                            placeholderText: root.targetIp === "" ? "请先选择设备..." : "发消息..."
                            enabled: root.targetIp !== ""
                            background: Rectangle {
                                color: Material.dialogColor
                                radius: 24
                            }
                            leftPadding: 16
                        }
                        RoundButton {
                            text: "➡"
                            font.pixelSize: 18
                            highlighted: true
                            enabled: root.targetIp !== ""
                            onClicked: {
                                root.sendTextMessage(mobileChatInput.text);
                                mobileChatInput.text = "";
                            }
                        }
                    }
                }
            }
        }

        // 统一底部导航栏
        TabBar {
            id: tabBar
            Layout.fillWidth: true
            Material.elevation: 8
            onCurrentIndexChanged: swipeView.currentIndex = currentIndex
            TabButton {
                text: " 发现"
            }
            TabButton {
                text: " 传输"
            }
            TabButton {
                text: " 消息"
            }
        }
    }
}

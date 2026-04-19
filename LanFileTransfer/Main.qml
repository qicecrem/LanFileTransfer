import QtQuick
import QtQuick.Controls
import QtQuick.Dialogs

ApplicationWindow {
    id: root // 给根窗口命名，方便子布局调用它的属性和函数
    visible: true
    width: 900
    height: 600
    title: qsTr("飞传")

    // ================== 1. 系统平台判断 ==================
    property bool isMobile: Qt.platform.os === "android" || Qt.platform.os === "ios"

    // 如果是移动端，最大化显示（虽然 Android 默认全屏，但加一下更规范）
    Component.onCompleted: {
        if (isMobile)
            root.visibility = Window.Maximized;
    }

    // ================== 2. 全局共享状态与数据 ==================
    property string targetIp: ""
    property int targetPort: 0
    property string targetName: "未选择设备"

    // 传输任务的公共数据模型（桌面和手机共用）
    property alias sharedTaskModel: _sharedTaskModel
    ListModel {
        id: _sharedTaskModel
    }

    property alias sharedChatModel: _sharedChatModel
    ListModel {
        id: _sharedChatModel
    }

    // ================== 3. 全局共享函数 ==================
    function formatBytes(bytes) {
        if (bytes === 0)
            return "0 B";
        if (bytes < 1024)
            return bytes + " B";
        const k = 1024;
        const sizes = ["B", "KB", "MB", "GB", "TB"];
        const i = Math.floor(Math.log(bytes) / Math.log(k));
        return parseFloat((bytes / Math.pow(k, i)).toFixed(2)) + " " + sizes[i];
    }

    function clearHistory() {
        for (let i = sharedTaskModel.count - 1; i >= 0; i--) {
            let s = sharedTaskModel.get(i).status;
            if (s === "发送完成" || s === "接收完成" || s === "传输中断") {
                sharedTaskModel.remove(i);
            }
        }
    }

    function openFileDialog() {
        fileDialog.open();
    }
    function openFolderDialog() {
        folderDialog.open();
    }
    function showToast(msg) {
        toast.show(msg);
    }

    function getDeviceNameByIp(ip) {
        let list = discoveryService.deviceList;
        for (let i = 0; i < list.length; i++) {
            if (list[i].ip === ip)
                return list[i].name;
        }
        return ip;
    }

    // 发送消息的公共函数
    function sendTextMessage(text) {
        if (targetIp === "" || text.trim() === "")
            return;
        transferSvc.sendText(text, targetIp, targetPort);
        sharedChatModel.append({
            "isMe": true,
            "senderName": "我",
            "message": text,
            "time": new Date().toLocaleTimeString('zh-CN', {
                hour12: false
            })
        });
    }

    // ================== 4. C++ 核心逻辑 ==================
    TransferManager {
        id: transferManager
    }
    DiscoveryService {
        id: discoveryService
        Component.onCompleted: {
            discoveryService.setLocalTcpPort(transferManager.serverPort);
            discoverySvc.startScan();
        }
    }

    // 获取服务供子布局使用
    property var transferSvc: transferManager
    property var discoverySvc: discoveryService

    Connections {
        target: transferManager
        function onTaskAdded(taskId, fileName, isSender, totalBytes) {
            let sizeStr = root.formatBytes(totalBytes);
            sharedTaskModel.insert(0, {
                "taskId": taskId,
                "fileName": fileName,
                "isSender": isSender,
                "sizeStr": sizeStr,
                "progress": 0.0,
                "status": "等待中..."
            });
        }
        function onTaskUpdated(taskId, progress, status) {
            for (let i = 0; i < sharedTaskModel.count; i++) {
                if (sharedTaskModel.get(i).taskId === taskId) {
                    sharedTaskModel.setProperty(i, "progress", progress);
                    sharedTaskModel.setProperty(i, "status", status);
                    break;
                }
            }
            if (status === "发送完成" || status === "接收完成") {
                root.showToast(status);
            } else if (status.includes("失败") || status === "传输中断") {
                root.showToast(status);
            }
        }

        function onTextReceived(ip, text) {
            let name = root.getDeviceNameByIp(ip);
            sharedChatModel.append({
                "isMe": false,
                "senderName": name,
                "message": text,
                "time": new Date().toLocaleTimeString('zh-CN', {
                    hour12: false
                })
            });
            root.showToast("💬 收到来自 " + name + " 的新消息");
        }
    }

    // ================== 5. 全局弹窗组件 ==================
    FileDialog {
        id: fileDialog
        title: "选择要发送的文件"
        fileMode: FileDialog.OpenFiles
        onAccepted: {
            if (root.targetIp !== "") {
                transferManager.sendFiles(selectedFiles, root.targetIp, root.targetPort);
            }
        }
    }

    FolderDialog {
        id: folderDialog
        title: "选择接收文件的保存路径"
        onAccepted: transferManager.setSaveDirectory(selectedFolder)
    }

    // ================== 6. 动态加载 UI 核心 ==================
    Loader {
        anchors.fill: parent
        // 根据平台动态加载对应的 QML 文件
        source: root.isMobile ? "MobileLayout.qml" : "DesktopLayout.qml"       //"MobileLayout.qml"
    }

    // ================== 7. Toast 提示框 ==================
    Rectangle {
        id: toast
        property string message: ""
        anchors.horizontalCenter: parent.horizontalCenter
        anchors.bottom: parent.bottom
        anchors.bottomMargin: root.isMobile ? 80 : 50
        width: toastText.width + 40
        height: 45
        color: "#E6323232"
        radius: 22
        opacity: 0
        z: 999
        Text {
            id: toastText
            anchors.centerIn: parent
            text: toast.message
            color: "white"
            font.pixelSize: 14
        }
        SequentialAnimation on opacity {
            id: toastAnim
            running: false
            NumberAnimation {
                to: 1.0
                duration: 300
            }
            PauseAnimation {
                duration: 2000
            }
            NumberAnimation {
                to: 0
                duration: 500
            }
        }
        function show(msg) {
            message = msg;
            toastAnim.restart();
        }
    }
}

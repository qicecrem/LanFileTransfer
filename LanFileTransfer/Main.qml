pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Dialogs
import QtQuick.Window

ApplicationWindow {
    id: root
    visible: true
    width: 1240; height: 800
    minimumWidth: isMobile ? 360 : 820
    minimumHeight: 560
    title: app.tr("appName")
    color: canvas

    readonly property bool isMobile: Qt.platform.os === "android" || Qt.platform.os === "ios"
    readonly property bool compact: width < 780
    readonly property color navy: "#062B52"
    readonly property color cyan: "#4FD0EE"
    readonly property color cyanDark: "#087B91"
    readonly property color canvas: "#F4F7FA"
    readonly property color surface: "#FFFFFF"
    readonly property color line: "#DCE5EC"
    readonly property color ink: "#153A5B"
    readonly property color muted: "#75899B"
    readonly property string iconFamily: materialIcons.name
    property string targetId: ""
    property string targetIp: ""
    property int targetPort: 0
    property int targetMediaPort: 0
    property string targetName: ""
    property string incomingCallIp: ""
    property string pairingPeerId: ""
    property string pairingPeerName: ""
    property string pairingPeerIp: ""
    property string pendingForgetPeerId: ""
    property string pendingForgetPeerName: ""
    property int pairingRevision: 0
    property var pendingFiles: []

    FontLoader { id: materialIcons; source: "qrc:/MaterialIconsOutlined-Regular.otf" }
    ListModel { id: timeline }
    AppController { id: app }
    TransferManager { id: transfer }
    CallManager { id: calls }
    DiscoveryService {
        id: discovery
        deviceName: app.deviceName
        Component.onCompleted: {
            setLocalTcpPort(transfer.serverPort)
            setLocalMediaPort(calls.serverPort)
            startScan()
        }
        onDeviceListChanged: {
            const peers = deviceList
            for (let i = 0; i < peers.length; ++i)
                transfer.deviceAvailable(peers[i].id, peers[i].name, peers[i].ip, peers[i].port)
        }
    }

    function t(key) { const languageDependency = app.language; return app.tr(key) }
    function copy(zh, en) { return app.language.indexOf("zh") === 0 ? zh : en }
    function peerKeyForIp(ip) {
        const peers = discovery.deviceList
        for (let i = 0; i < peers.length; ++i) if (peers[i].ip === ip) return peers[i].id
        return ip
    }
    function peerNameForIp(ip) {
        const peers = discovery.deviceList
        for (let i = 0; i < peers.length; ++i) if (peers[i].ip === ip) return peers[i].name
        return ip
    }
    function peerForId(id) {
        const peers = discovery.deviceList
        for (let i = 0; i < peers.length; ++i) if (peers[i].id === id) return peers[i]
        return null
    }
    function targetReady() {
        const dependency = pairingRevision
        return targetId !== "" && transfer.isPaired(targetId)
    }
    function connectionStateLabel(state) {
        const labels = {
            "online": copy("在线", "Online"),
            "reconnecting": copy("重连中", "Reconnecting"),
            "offline": copy("离线", "Offline"),
            "pairing": copy("配对中", "Pairing"),
            "unpaired": copy("未配对", "Unpaired")
        }
        return labels[state] || state
    }
    function lastSeenLabel(value) {
        const timestamp = Number(value || 0)
        if (timestamp <= 0) return copy("未知", "Unknown")
        return Qt.formatDateTime(new Date(timestamp), "yyyy-MM-dd HH:mm")
    }
    function openPeer(peer) {
        if (transfer.isPaired(peer.id)) selectPeer(peer)
        else transfer.requestPairing(peer.id, peer.name, peer.ip, peer.port)
    }
    function selectPeer(peer) {
        targetId = peer.id || peer.ip; targetIp = peer.ip; targetPort = peer.port
        targetMediaPort = peer.mediaPort || 0; targetName = peer.name
        timeline.clear()
        const saved = app.messages(targetId)
        for (let i = 0; i < saved.length; ++i) timeline.append(normalizeMessage(saved[i]))
        if (compact) deviceDrawer.close()
        Qt.callLater(function() { chat.positionViewAtEnd() })
    }
    function normalizeMessage(data) {
        return {
            peerKey: String(data.peerKey || ""), peerName: String(data.peerName || ""),
            outgoing: data.outgoing === true || Number(data.outgoing) === 1,
            kind: String(data.kind || "text"), body: String(data.body || ""),
            fileName: String(data.fileName || ""), fileSize: Number(data.fileSize || 0),
            transferId: String(data.transferId || ""), progress: Number(data.progress || 0),
            status: String(data.status || ""), checksum: String(data.checksum || ""),
            createdAt: Number(data.createdAt || Date.now())
        }
    }
    function appendMessage(data) {
        const row = normalizeMessage(data)
        timeline.append(row)
        // 显式写回可确保复用的委托收到角色变更通知。
        timeline.setProperty(timeline.count - 1, "outgoing", row.outgoing)
        Qt.callLater(function() { chat.positionViewAtEnd() })
    }
    function loadDesignPreview() {
        targetId = "preview-peer"; targetIp = "192.168.1.45"; targetPort = 45455
        targetMediaPort = 45456; targetName = "Workstation-Alpha"; timeline.clear()
        timeline.append({peerKey:targetId, peerName:targetName, outgoing:true, kind:"file", body:"",
                         fileName:"deployment_v2.zip", fileSize:256901120, transferId:"preview-1",
                         progress:1, status:"verified", checksum:"e3b0c44298fc1c14", createdAt:Date.now()-120000})
        timeline.append({peerKey:targetId, peerName:targetName, outgoing:false, kind:"file", body:"",
                         fileName:"system_backup_img_04.iso", fileSize:4509715660, transferId:"preview-2",
                         progress:0, status:"paused", checksum:"", createdAt:Date.now()})
    }
    function loadCallPreview() {
        targetId = "preview-peer"; targetIp = "192.168.1.45"; targetMediaPort = 45456
        targetName = "Workstation-Alpha"; callDialog.open()
    }
    function sendMessage() {
        const body = composer.text.trim()
        if (!body || !targetIp || !targetReady()) return
        transfer.sendText(body, targetIp, targetPort)
        const now = Date.now()
        app.addMessage(targetId, targetName, true, "text", body, "", 0, "", "sent")
        appendMessage({peerKey: targetId, peerName: targetName, outgoing: true, kind: "text", body: body,
                       fileName: "", fileSize: 0, transferId: "", progress: 1, status: "sent",
                       checksum: "", createdAt: now})
        composer.clear()
    }
    function clearCurrentConversation() {
        if (!targetId) return
        app.clearConversation(targetId)
        timeline.clear()
        toast.show(root.copy("会话已清空", "Conversation cleared"))
    }
    function sendFilesOrReconnect(files) {
        if (!files || files.length === 0 || !targetId) return
        const stableFiles = []
        for (let i = 0; i < files.length; ++i) stableFiles.push(files[i])
        if (targetReady()) {
            pendingFiles = []
            transfer.sendFiles(stableFiles, targetIp, targetPort)
            return
        }
        pendingFiles = stableFiles
        const peer = peerForId(targetId)
        if (peer) {
            toast.show(root.copy("连接已断开，正在重新配对后发送", "Reconnecting before sending"))
            transfer.requestPairing(peer.id, peer.name, peer.ip, peer.port)
        } else {
            toast.show(root.copy("正在重新连接设备后发送", "Reconnecting before sending"))
            transfer.requestPairing(targetId, targetName, targetIp, targetPort)
        }
    }
    function formatBytes(bytes) {
        if (bytes < 1024) return bytes + " B"
        const units = ["KB", "MB", "GB", "TB"]; let n = bytes / 1024; let i = 0
        while (n >= 1024 && i < units.length - 1) { n /= 1024; ++i }
        return n.toFixed(n < 10 ? 1 : 0) + " " + units[i]
    }
    function displayDirectory(value) {
        if (!value || value.indexOf("content://") !== 0) return value
        try {
            const decoded = decodeURIComponent(value)
            const marker = decoded.indexOf("/tree/")
            const document = marker >= 0 ? decoded.substring(marker + 6) : decoded
            if (document.indexOf("primary:") === 0)
                return root.copy("内部存储/", "Internal storage/") + document.substring(8)
            return document
        } catch (error) {
            return value
        }
    }
    function statusLabel(status) {
        const zh = app.language.indexOf("zh") === 0
        const map = {
            "connecting":["正在连接","Connecting"], "negotiating":["协商传输","Negotiating"],
            "transferring":["正在发送","Sending"], "receiving":["正在接收","Receiving"],
            "resuming":["断点续传","Resuming"], "verifying":["正在校验","Verifying"],
            "verified":["校验通过","Verified"], "paused":["等待续传","Waiting to resume"],
            "checksum-error":["校验失败","Checksum failed"], "read-error":["无法读取","Cannot read"],
            "write-error":["无法写入","Cannot write"], "cancelled":["已取消","Cancelled"],
            "sent":["已发送","Sent"], "received":["已收到","Received"]
        }
        return map[status] ? map[status][zh ? 0 : 1] : status
    }

    onClosing: close => {
        if (isMobile && calls.state !== "idle") {
            close.accepted = false
            app.moveToBackground()
            return
        }
        if (!isMobile && app.minimizeToTray) { close.accepted = false; hide(); app.hideToTray() }
    }
    Connections {
        target: app
        function onRestoreRequested() { root.show(); root.raise(); root.requestActivate() }
        function onQuitRequested() { Qt.quit() }
        function onDownloadDirectoryChanged() { transfer.saveDirectory = app.downloadDirectory }
        function onReceiveDirectorySelected(uri) { transfer.setSaveDirectory(uri) }
        function onDeviceNameChanged() { discovery.deviceName = app.deviceName }
        function onNetworkEnvironmentChanged() { discovery.refreshNetwork() }
        function onDiagnosticBundleReady(path) {
            toast.show(root.copy("诊断包已生成", "Diagnostic package created"))
        }
        function onDiagnosticExportFailed(message) { toast.show(message) }
    }
    Connections {
        target: transfer
        function onSaveDirectoryChanged() { app.downloadDirectory = transfer.saveDirectory }
        function onTextReceived(ip, text) {
            const key = peerKeyForIp(ip), name = peerNameForIp(ip), now = Date.now()
            app.addMessage(key, name, false, "text", text, "", 0, "", "received")
            if (key === targetId || ip === targetIp)
                appendMessage({peerKey:key, peerName:name, outgoing:false, kind:"text", body:text,
                               fileName:"", fileSize:0, transferId:"", progress:1, status:"received",
                               checksum:"", createdAt:now})
            else { app.showNotification(name, text); toast.show(name + ": " + text) }
        }
        function onTaskAdded(taskId, peerIp, fileName, isSender, totalBytes) {
            const key = peerKeyForIp(peerIp), name = peerNameForIp(peerIp), now = Date.now()
            app.addMessage(key, name, isSender, "file", "", fileName, totalBytes, taskId, "connecting")
            if (key === targetId || peerIp === targetIp) {
                for (let i = 0; i < timeline.count; ++i)
                    if (timeline.get(i).transferId === taskId) return
                appendMessage({peerKey:key, peerName:name, outgoing:isSender, kind:"file", body:"",
                               fileName:fileName, fileSize:totalBytes, transferId:taskId, progress:0,
                               status:"connecting", checksum:"", createdAt:now})
            }
        }
        function onTaskRestored(taskId, peerId, peerName, peerIp, fileName, isSender,
                                totalBytes, progress, status, checksum) {
            app.addMessage(peerId, peerName, isSender, "file", "", fileName,
                           totalBytes, taskId, status)
            app.updateTransferSize(taskId, totalBytes)
            app.updateTransfer(taskId, progress, status, checksum)
            if (peerId !== root.targetId && peerIp !== root.targetIp) return
            for (let i = 0; i < timeline.count; ++i)
                if (timeline.get(i).transferId === taskId) return
            appendMessage({peerKey:peerId, peerName:peerName, outgoing:isSender, kind:"file", body:"",
                           fileName:fileName, fileSize:totalBytes, transferId:taskId,
                           progress:progress, status:status, checksum:checksum, createdAt:Date.now()})
        }
        function onTaskUpdated(taskId, progress, status, checksum) {
            app.updateTransfer(taskId, progress, status, checksum)
            for (let i = 0; i < timeline.count; ++i) if (timeline.get(i).transferId === taskId) {
                timeline.setProperty(i, "progress", progress); timeline.setProperty(i, "status", status)
                timeline.setProperty(i, "checksum", checksum); break
            }
            if (status === "verified") toast.show(t("verified"))
        }
        function onTaskSizeResolved(taskId, totalBytes) {
            app.updateTransferSize(taskId, totalBytes)
            for (let i = 0; i < timeline.count; ++i) if (timeline.get(i).transferId === taskId) {
                timeline.setProperty(i, "fileSize", totalBytes)
                break
            }
        }
        function onTransferError(message) { toast.show(message) }
        function onPairingRequested(peerId, peerName, ip) {
            root.pairingPeerId = peerId
            root.pairingPeerName = peerName
            root.pairingPeerIp = ip
            if (!root.isMobile) {
                root.show()
                root.raise()
                root.requestActivate()
            }
            pairingDialog.open()
            app.showNotification(root.copy("设备配对请求", "Device pairing request"), peerName)
        }
        function onPairingStateChanged(peerId, state) {
            ++root.pairingRevision
            if (state === "pairing") {
                toast.show(root.copy("正在等待对方确认配对", "Waiting for pairing approval"))
            } else if (state === "online") {
                const peer = root.peerForId(peerId)
                if (peer) root.selectPeer(peer)
                toast.show(root.copy("已与设备建立长连接", "Persistent connection established"))
                if (root.pendingFiles.length > 0) {
                    const files = root.pendingFiles
                    root.pendingFiles = []
                    Qt.callLater(function() { root.sendFilesOrReconnect(files) })
                }
            } else if (state === "reconnecting") {
                toast.show(root.copy("连接已断开，正在自动重连", "Connection lost—reconnecting"))
            } else if (state === "offline" || state === "unpaired") {
                toast.show(state === "offline"
                           ? root.copy("设备当前离线", "Device is offline")
                           : root.copy("设备尚未配对", "Device is not paired"))
            }
        }
        function onPeerAuthorizationChanged(peerId, ip, allowed) {
            if (!ip) return
            if (allowed) calls.allowPeer(ip)
            else calls.revokePeer(ip)
        }
    }
    Connections {
        target: calls
        function onIncomingCall(ip, mode) {
            incomingCallIp = ip; incomingDialog.open()
            app.showNotification(t("incomingCall"), peerNameForIp(ip))
        }
        function onStateChanged() {
            if (calls.state === "connected" || calls.state === "calling") callDialog.open()
            if (calls.state === "idle") callDialog.close()
        }
        function onCallError(message) { toast.show(message) }
    }

    FileDialog {
        id: fileDialog; title: t("file"); fileMode: FileDialog.OpenFiles
        onAccepted: root.sendFilesOrReconnect(selectedFiles)
    }
    FolderDialog {
        id: folderDialog; title: t("chooseFolder")
        onAccepted: transfer.setSaveDirectory(selectedFolder.toString())
    }

    component IconAction: ToolButton {
        id: control
        property string glyph: ""
        property string tip: ""
        property color iconColor: root.ink
        property color hoverColor: "#EAF1F6"
        property int diameter: 40
        implicitWidth: diameter; implicitHeight: diameter; padding: 0
        ToolTip.visible: hovered && tip.length > 0
        ToolTip.text: tip
        background: Rectangle { radius: 9; color: control.down ? "#D8E7F0" : control.hovered ? control.hoverColor : "transparent" }
        contentItem: Text {
            text: control.glyph; font.family: root.iconFamily; font.pixelSize: Math.round(control.diameter * .52)
            color: control.enabled ? control.iconColor : "#AEBCC7"
            horizontalAlignment: Text.AlignHCenter; verticalAlignment: Text.AlignVCenter
        }
    }
    component SideAction: ItemDelegate {
        id: side
        property string glyph: ""
        property bool selected: false
        implicitHeight: 44; leftPadding: 12; rightPadding: 12
        background: Rectangle { radius: 8; color: side.selected ? root.cyan : side.hovered ? "#103E6B" : "transparent" }
        contentItem: RowLayout {
            spacing: 11
            Text { text: side.glyph; font.family: root.iconFamily; font.pixelSize: 20; color: side.selected ? root.navy : "#A9D5F3" }
            Label { text: side.text; color: side.selected ? root.navy : "#D9EEFC"; font.pixelSize: 13; Layout.fillWidth: true }
        }
    }

    component SettingsField: TextField {
        id: field
        implicitHeight: 48
        leftPadding: 14; rightPadding: 14
        topPadding: 0; bottomPadding: 0
        verticalAlignment: TextInput.AlignVCenter
        selectByMouse: true
        color: root.ink
        placeholderTextColor: root.muted
        font.pixelSize: 14
        background: Rectangle {
            radius: 10
            color: field.readOnly ? "#F7F9FB" : root.surface
            border.width: field.activeFocus ? 2 : 1
            border.color: field.activeFocus ? root.cyanDark : root.line
        }
    }

    component SettingsComboBox: ComboBox {
        id: combo
        implicitHeight: 48
        leftPadding: 14; rightPadding: 42
        topPadding: 0; bottomPadding: 0
        font.pixelSize: 14
        contentItem: Text {
            leftPadding: 0; rightPadding: 0
            text: combo.displayText
            color: root.ink
            font: combo.font
            verticalAlignment: Text.AlignVCenter
            elide: Text.ElideRight
        }
        indicator: Text {
            width: 42; height: combo.height
            x: combo.width - width; y: 0
            text: "expand_more"
            font.family: root.iconFamily; font.pixelSize: 21
            color: root.muted
            horizontalAlignment: Text.AlignHCenter
            verticalAlignment: Text.AlignVCenter
        }
        background: Rectangle {
            radius: 10
            color: root.surface
            border.width: combo.activeFocus ? 2 : 1
            border.color: combo.activeFocus ? root.cyanDark : root.line
        }
        popup: Popup {
            y: combo.height + 6
            width: combo.width
            implicitHeight: contentItem.implicitHeight + 12
            padding: 6
            background: Rectangle {
                radius: 10; color: root.surface; border.color: root.line
            }
            contentItem: ListView {
                clip: true
                implicitHeight: contentHeight
                model: combo.popup.visible ? combo.delegateModel : null
                currentIndex: combo.highlightedIndex
                ScrollIndicator.vertical: ScrollIndicator { }
            }
        }
        delegate: ItemDelegate {
            id: choice
            required property var modelData
            required property int index
            width: combo.width - 12; height: 40
            highlighted: combo.highlightedIndex === index
            contentItem: Label {
                text: choice.modelData
                color: root.ink; font.pixelSize: 13
                verticalAlignment: Text.AlignVCenter
            }
            background: Rectangle {
                radius: 7
                color: choice.highlighted ? "#E7F7FB" : "transparent"
            }
        }
    }

    component SettingsSwitch: Switch {
        id: toggle
        spacing: 12
        implicitHeight: 42
        indicator: Rectangle {
            implicitWidth: 42; implicitHeight: 24
            x: 0; y: Math.round((toggle.height - height) / 2)
            radius: height / 2
            color: toggle.checked ? root.cyanDark : "#CBD7E0"
            border.width: toggle.activeFocus ? 2 : 0
            border.color: root.cyan
            Behavior on color { ColorAnimation { duration: 120 } }
            Rectangle {
                width: 18; height: 18; radius: 9
                x: toggle.checked ? parent.width - width - 3 : 3
                anchors.verticalCenter: parent.verticalCenter
                color: "white"
                Behavior on x { NumberAnimation { duration: 120; easing.type: Easing.OutCubic } }
            }
        }
        contentItem: Label {
            leftPadding: toggle.indicator.width + toggle.spacing
            text: toggle.text
            color: root.ink; font.pixelSize: 13
            verticalAlignment: Text.AlignVCenter
            wrapMode: Text.Wrap
        }
    }

    component ModernButton: Button {
        id: modernButton
        property string glyph: ""
        property bool primary: false
        implicitHeight: 42
        leftPadding: 18; rightPadding: 18; topPadding: 0; bottomPadding: 0
        background: Rectangle {
            radius: 10
            color: modernButton.primary
                   ? (modernButton.down ? "#041F3C" : modernButton.hovered ? "#0B3A68" : root.navy)
                   : (modernButton.down ? "#DCEAF1" : modernButton.hovered ? "#EDF5F8" : root.surface)
            border.width: modernButton.primary ? 0 : 1
            border.color: root.line
        }
        contentItem: RowLayout {
            spacing: 7
            Text {
                visible: modernButton.glyph !== ""
                text: modernButton.glyph; font.family: root.iconFamily; font.pixelSize: 18
                color: modernButton.primary ? "white" : root.ink
            }
            Label {
                text: modernButton.text; font.weight: Font.DemiBold
                color: modernButton.primary ? "white" : root.ink
                horizontalAlignment: Text.AlignHCenter; verticalAlignment: Text.AlignVCenter
            }
        }
    }

    component DevicePanel: Rectangle {
        color: root.navy
        ColumnLayout {
            anchors.fill: parent; anchors.margins: 12; spacing: 9
            RowLayout {
                Layout.fillWidth: true; Layout.margins: 5; Layout.bottomMargin: 18; spacing: 11
                Rectangle {
                    Layout.preferredWidth: 38; Layout.preferredHeight: 38; radius: 10
                    color: "#E7F7FB"
                    Text { anchors.centerIn: parent; text: "device_hub"; font.family: root.iconFamily; font.pixelSize: 23; color: root.navy }
                }
                ColumnLayout {
                    spacing: 0; Layout.fillWidth: true
                    Label { text: t("appName"); color: "white"; font.pixelSize: 23; font.weight: Font.Bold }
                    RowLayout {
                        spacing: 5
                        Rectangle { Layout.preferredWidth: 6; Layout.preferredHeight: 6; radius: 3; color: root.cyan }
                        Label { text: app.deviceName; color: "#9BC8E6"; font.pixelSize: 10; elide: Text.ElideRight; Layout.fillWidth: true }
                    }
                }
            }
            Label {
                text: root.copy("设备", "DEVICES"); color: "#78BCE6"; font.pixelSize: 10
                font.weight: Font.Bold; font.letterSpacing: 1.4; Layout.leftMargin: 10
            }
            TextField {
                id: peerSearch
                Layout.fillWidth: true; Layout.preferredHeight: 38; leftPadding: 38; rightPadding: 10
                placeholderText: t("search"); color: "#E8F5FD"; placeholderTextColor: "#7EA9C5"
                background: Rectangle {
                    radius: 8; color: peerSearch.activeFocus ? "#154A75" : "#0D385F"
                    border.width: peerSearch.activeFocus ? 1 : 0; border.color: root.cyan
                    Text { anchors.left: parent.left; anchors.leftMargin: 11; anchors.verticalCenter: parent.verticalCenter; text: "search"; font.family: root.iconFamily; font.pixelSize: 19; color: "#8FC4E4" }
                }
            }
            ListView {
                Layout.fillWidth: true; Layout.fillHeight: true; clip: true; spacing: 5
                model: discovery.deviceList
                Label {
                    anchors.centerIn: parent; visible: parent.count === 0; width: parent.width - 24
                    text: t("noPeers") + "\n" + t("sameWifi"); wrapMode: Text.Wrap
                    horizontalAlignment: Text.AlignHCenter; color: "#7EA9C5"; lineHeight: 1.5; font.pixelSize: 12
                }
                delegate: ItemDelegate {
                    id: peer
                    required property var modelData
                    width: ListView.view.width; height: 58
                    visible: peerSearch.text === "" || modelData.name.toLowerCase().indexOf(peerSearch.text.toLowerCase()) >= 0
                    onClicked: root.openPeer(modelData)
                    background: Rectangle { radius: 8; color: targetIp === peer.modelData.ip ? root.cyan : peer.hovered ? "#103E6B" : "transparent" }
                    contentItem: RowLayout {
                        spacing: 10
                        Text { text: "computer"; font.family: root.iconFamily; font.pixelSize: 22; color: targetIp === peer.modelData.ip ? root.navy : "#A9D5F3" }
                        ColumnLayout {
                            Layout.fillWidth: true; spacing: 1
                            Label { text: peer.modelData.name; color: targetIp === peer.modelData.ip ? root.navy : "#E7F5FD"; font.pixelSize: 13; font.weight: Font.DemiBold; elide: Text.ElideRight; Layout.fillWidth: true }
                            Label {
                                text: { const dependency = root.pairingRevision; return peer.modelData.ip + (transfer.isPaired(peer.modelData.id) ? root.copy(" · 已配对", " · Paired") : "") }
                                color: targetIp === peer.modelData.ip ? "#24647A" : "#8CB9D5"; font.pixelSize: 10
                            }
                        }
                        Rectangle { Layout.preferredWidth: 6; Layout.preferredHeight: 6; radius: 3; color: targetIp === peer.modelData.ip ? root.navy : root.cyan }
                    }
                }
            }
            Rectangle { Layout.fillWidth: true; Layout.preferredHeight: 1; color: "#19466C" }
            SideAction { Layout.fillWidth: true; glyph: "radar"; text: root.copy("重新发现", "Discovery"); onClicked: { discovery.stopScan(); discovery.startScan() } }
            SideAction { Layout.fillWidth: true; glyph: "settings"; text: t("settings"); onClicked: settingsDrawer.open() }
        }
    }

    DevicePanel { id: desktopDevices; visible: !compact; width: 240; anchors.left: parent.left; anchors.top: parent.top; anchors.bottom: parent.bottom }
    Drawer {
        id: deviceDrawer; width: Math.min(root.width * .84, 300); height: root.height; edge: Qt.LeftEdge
        background: Rectangle { color: root.navy }
        DevicePanel { anchors.fill: parent }
    }
    Drawer {
        id: settingsDrawer; width: Math.min(root.width * .92, 400); height: root.height; edge: Qt.RightEdge
        background: Rectangle { color: root.surface }
        ScrollView {
            id: settingsScroll
            anchors.fill: parent; clip: true
            contentWidth: availableWidth
            ScrollBar.horizontal.policy: ScrollBar.AlwaysOff
            Item {
                width: settingsScroll.availableWidth
                implicitHeight: settingsContent.implicitHeight + 52
                ColumnLayout {
                    id: settingsContent
                    anchors.left: parent.left; anchors.right: parent.right; anchors.top: parent.top
                    anchors.margins: 26; spacing: 16
            RowLayout {
                Layout.fillWidth: true
                ColumnLayout {
                    spacing: 1
                    Label { text: t("settings"); font.pixelSize: 23; font.weight: Font.Bold; color: root.ink }
                    Label { text: root.copy("本机与传输偏好", "Device and transfer preferences"); color: root.muted; font.pixelSize: 11 }
                }
                Item { Layout.fillWidth: true }
                IconAction { glyph: "close"; tip: root.copy("关闭", "Close"); onClicked: settingsDrawer.close() }
            }
            Rectangle { Layout.fillWidth: true; Layout.preferredHeight: 1; color: root.line }
            Label { text: t("deviceName"); color: root.ink; font.weight: Font.DemiBold }
            SettingsField { Layout.fillWidth: true; text: app.deviceName; onEditingFinished: app.deviceName = text }
            Label { text: t("language"); color: root.ink; font.weight: Font.DemiBold }
            SettingsComboBox { Layout.fillWidth: true; model: ["简体中文", "English"]; currentIndex: app.language.indexOf("zh") === 0 ? 0 : 1; onActivated: app.language = currentIndex === 0 ? "zh_CN" : "en_US" }
            Label { text: t("downloads"); color: root.ink; font.weight: Font.DemiBold }
            RowLayout {
                Layout.fillWidth: true; spacing: 8
                SettingsField { Layout.fillWidth: true; text: root.displayDirectory(app.downloadDirectory); readOnly: true }
                IconAction {
                    id: folderButton
                    diameter: 48; glyph: "folder_open"; tip: t("chooseFolder")
                    iconColor: root.cyanDark
                    background: Rectangle {
                        radius: 10; color: folderButton.down ? "#DDF2F7" : folderButton.hovered ? "#EDF8FB" : root.surface
                        border.width: 1; border.color: folderButton.activeFocus ? root.cyanDark : root.line
                    }
                    onClicked: isMobile ? app.chooseReceiveDirectory() : folderDialog.open()
                }
            }
            SettingsSwitch { visible: !isMobile; text: t("minimizeToTray"); checked: app.minimizeToTray; onToggled: app.minimizeToTray = checked }
            SettingsSwitch { visible: isMobile; text: t("keepAwake"); checked: app.keepAwake; onToggled: app.keepAwake = checked }
            Label { text: root.copy("可信设备", "Trusted devices"); color: root.ink; font.weight: Font.DemiBold }
            ModernButton {
                Layout.fillWidth: true; glyph: "devices"
                text: root.copy("管理可信设备", "Manage trusted devices")
                      + " (" + transfer.trustedPeers.length + ")"
                onClicked: trustedDevicesDialog.open()
            }
            Label { text: root.copy("诊断与支持", "Diagnostics and support"); color: root.ink; font.weight: Font.DemiBold }
            Label {
                Layout.fillWidth: true; wrapMode: Text.Wrap
                text: root.copy("导出脱敏运行日志、崩溃信息和系统环境，不包含聊天记录。",
                                "Export redacted logs, crash details and system information. Chat history is excluded.")
                color: root.muted; font.pixelSize: 11
            }
            ModernButton {
                Layout.fillWidth: true; glyph: "bug_report"
                text: root.copy("导出诊断包", "Export diagnostic package")
                onClicked: app.exportDiagnosticBundle()
            }
            Item { Layout.fillWidth: true; Layout.preferredHeight: 4 }
            Rectangle {
                Layout.fillWidth: true; Layout.preferredHeight: 72; radius: 10; color: "#EAF6FA"
                RowLayout {
                    anchors.fill: parent; anchors.margins: 14
                    Text { text: "verified_user"; font.family: root.iconFamily; font.pixelSize: 24; color: root.cyanDark }
                    Label { Layout.fillWidth: true; text: root.copy("SQLite 离线历史 · SHA-256 校验 · 断点续传", "SQLite offline history · SHA-256 · Resume"); wrapMode: Text.Wrap; color: root.ink; font.pixelSize: 11 }
                }
            }
                }
            }
        }
    }

    Rectangle {
        anchors.left: compact ? parent.left : desktopDevices.right
        anchors.right: parent.right; anchors.top: parent.top; anchors.bottom: parent.bottom
        color: root.canvas
        DropArea { anchors.fill: parent; onDropped: drop => { if (root.targetReady() && drop.hasUrls) transfer.sendFiles(drop.urls, targetIp, targetPort) } }
        ColumnLayout {
            anchors.fill: parent; spacing: 0
            Rectangle {
                Layout.fillWidth: true; Layout.preferredHeight: 64; color: root.surface; border.color: root.line
                RowLayout {
                    anchors.fill: parent; anchors.leftMargin: compact ? 6 : 26; anchors.rightMargin: compact ? 6 : 18; spacing: compact ? 2 : 9
                    IconAction { visible: compact; diameter: 36; glyph: "menu"; onClicked: deviceDrawer.open() }
                    Text { visible: !compact; text: targetIp ? "computer" : "devices"; font.family: root.iconFamily; font.pixelSize: 25; color: targetIp ? root.ink : "#9DAEBB" }
                    ColumnLayout {
                        Layout.fillWidth: true; Layout.minimumWidth: 0; spacing: 0
                        Label { text: targetIp ? targetName : t("selectPeer"); color: root.ink; font.pixelSize: 15; font.weight: Font.DemiBold; elide: Text.ElideRight; Layout.fillWidth: true; Layout.minimumWidth: 0 }
                        RowLayout {
                            visible: targetIp !== ""; spacing: 6; Layout.fillWidth: true; Layout.minimumWidth: 0
                            Rectangle { Layout.preferredWidth: 6; Layout.preferredHeight: 6; radius: 3; color: root.cyanDark }
                            Label { Layout.fillWidth: true; Layout.minimumWidth: 0; elide: Text.ElideRight; text: (root.targetReady() ? root.copy("已配对 · 长连接", "Paired · Persistent") : root.copy("连接已断开", "Disconnected")) + " · " + targetIp; color: root.muted; font.pixelSize: 10 }
                        }
                    }
                    IconAction { diameter: compact ? 36 : 40; enabled: root.targetReady() && targetMediaPort > 0; glyph: "videocam"; tip: t("video"); onClicked: calls.startCall(targetIp, targetMediaPort, "camera") }
                    IconAction { diameter: compact ? 36 : 40; enabled: root.targetReady() && targetMediaPort > 0; glyph: "screen_share"; tip: t("screen"); onClicked: calls.startCall(targetIp, targetMediaPort, "screen") }
                    IconAction { diameter: compact ? 36 : 40; visible: targetId !== ""; glyph: "delete_sweep"; tip: t("clear"); onClicked: root.clearCurrentConversation() }
                    IconAction {
                        diameter: compact ? 36 : 40; glyph: "more_horiz"; tip: root.copy("更多", "More"); onClicked: conversationMenu.open()
                        Menu {
                            id: conversationMenu; y: parent.height
                            MenuItem { visible: root.isMobile; text: root.copy("测试摄像头并记录日志", "Test camera and log"); onTriggered: calls.startLocalDiagnostic("camera") }
                            MenuItem { visible: root.isMobile; text: root.copy("测试屏幕共享并记录日志", "Test screen sharing and log"); onTriggered: calls.startLocalDiagnostic("screen") }
                            MenuItem { text: t("clear"); enabled: targetId !== ""; onTriggered: root.clearCurrentConversation() }
                            MenuItem { text: t("settings"); onTriggered: settingsDrawer.open() }
                        }
                    }
                }
            }
            ListView {
                id: chat
                Layout.fillWidth: true; Layout.fillHeight: true; model: timeline; clip: true; spacing: 8
                topMargin: compact ? 18 : 34; bottomMargin: 24
                leftMargin: compact ? 12 : 34; rightMargin: compact ? 12 : 34
                Item {
                    anchors.centerIn: parent; visible: parent.count === 0
                    width: Math.min(parent.width - 40, 420); height: 220
                    ColumnLayout {
                        anchors.fill: parent; spacing: 10
                        Item { Layout.fillHeight: true }
                        Rectangle { Layout.alignment: Qt.AlignHCenter; Layout.preferredWidth: 58; Layout.preferredHeight: 58; radius: 18; color: "#E5F6FA"; Text { anchors.centerIn: parent; text: targetIp ? "forum" : "devices"; font.family: root.iconFamily; font.pixelSize: 29; color: root.cyanDark } }
                        Label { Layout.alignment: Qt.AlignHCenter; text: targetIp ? root.copy("开始会话", "Start a conversation") : root.copy("选择附近设备", "Choose a nearby device"); color: root.ink; font.pixelSize: 18; font.weight: Font.DemiBold }
                        Label { Layout.alignment: Qt.AlignHCenter; Layout.maximumWidth: 360; horizontalAlignment: Text.AlignHCenter; wrapMode: Text.Wrap; text: targetIp ? root.copy("消息和文件共享同一条时间线，传输可断点续传并自动校验。", "Messages and files share one timeline with resume and verification.") : t("sameWifi"); color: root.muted; font.pixelSize: 12 }
                        Item { Layout.fillHeight: true }
                    }
                }
                delegate: Item {
                    id: msg
                    required property string kind
                    required property bool outgoing
                    required property string body
                    required property string fileName
                    required property double fileSize
                    required property string status
                    required property string checksum
                    required property real progress
                    required property double createdAt
                    width: chat.width - chat.leftMargin - chat.rightMargin
                    height: card.height + 18
                    Rectangle {
                        id: card
                        x: msg.kind === "file" ? Math.round((msg.width - width) / 2) : msg.outgoing ? msg.width - width : 0
                        width: msg.kind === "file" ? Math.min(msg.width, compact ? Math.max(280, root.width - 48) : 620) : Math.min(msg.width * (compact ? .88 : .66), Math.max(170, content.implicitWidth + 30))
                        height: msg.kind === "file" ? 132 : content.implicitHeight + 22
                        radius: msg.kind === "file" ? 10 : 14
                        color: msg.kind === "file" ? root.surface : msg.outgoing ? "#DDF7FC" : root.surface
                        border.width: 1; border.color: msg.kind === "file" ? root.line : msg.outgoing ? "#BDEBF4" : "#E2E9EF"
                        ColumnLayout {
                            id: content
                            anchors.fill: parent; anchors.margins: 12
                            spacing: msg.kind === "file" ? 12 : 6
                            TextEdit {
                                visible: msg.kind === "text"; text: msg.body; readOnly: true
                                selectByMouse: true; persistentSelection: true; wrapMode: TextEdit.WrapAnywhere
                                color: root.ink; font.pixelSize: 14
                                Layout.maximumWidth: compact ? root.width * .72 : 520
                                Layout.preferredWidth: Math.min(implicitWidth, Layout.maximumWidth)
                            }
                            RowLayout {
                                visible: msg.kind === "file"; Layout.fillWidth: true; spacing: 14
                                Rectangle { Layout.preferredWidth: 50; Layout.preferredHeight: 50; radius: 9; color: root.navy; Text { anchors.centerIn: parent; text: "folder_zip"; font.family: root.iconFamily; font.pixelSize: 26; color: "white" } }
                                ColumnLayout {
                                    Layout.fillWidth: true; spacing: 3
                                    Label { text: msg.fileName; color: root.ink; font.pixelSize: 14; font.weight: Font.DemiBold; elide: Text.ElideMiddle; Layout.fillWidth: true }
                                    Label { text: formatBytes(msg.fileSize) + " · " + statusLabel(msg.status) + (msg.checksum ? " · SHA-256" : ""); color: msg.status === "verified" ? root.cyanDark : root.muted; font.pixelSize: 10; elide: Text.ElideRight; Layout.fillWidth: true }
                                }
                                Label { text: Math.round(msg.progress * 100) + "%"; visible: msg.status !== "verified"; color: root.cyanDark; font.pixelSize: 11; font.weight: Font.Bold }
                                Text { visible: msg.status === "verified"; text: "check_circle"; font.family: root.iconFamily; font.pixelSize: 22; color: root.cyanDark }
                            }
                            ProgressBar {
                                visible: msg.kind === "file"; Layout.fillWidth: true
                                value: msg.status === "verified" ? 1 : msg.progress
                                background: Rectangle { implicitHeight: 6; radius: 3; color: "#DDE7ED" }
                                contentItem: Item { implicitHeight: 6; Rectangle { width: parent.width * Math.max(0, Math.min(1, msg.status === "verified" ? 1 : msg.progress)); height: 6; radius: 3; color: root.cyanDark } }
                            }
                            RowLayout {
                                Layout.fillWidth: true
                                Text { text: msg.kind === "file" ? "history" : "schedule"; font.family: root.iconFamily; font.pixelSize: 14; color: root.muted }
                                Label { text: Qt.formatTime(new Date(msg.createdAt), "HH:mm"); font.pixelSize: 10; color: root.muted }
                                Item { Layout.fillWidth: true }
                                IconAction {
                                    visible: msg.kind === "text"; diameter: 26; glyph: "content_copy"
                                    tip: root.copy("复制消息", "Copy message")
                                    onClicked: { app.copyToClipboard(msg.body); toast.show(root.copy("已复制", "Copied")) }
                                }
                                Label {
                                    visible: msg.kind === "file" && msg.status === "verified"
                                    text: root.copy("打开文件夹", "Open folder"); color: root.cyanDark; font.pixelSize: 11; font.weight: Font.DemiBold
                                    MouseArea { anchors.fill: parent; cursorShape: Qt.PointingHandCursor; onClicked: transfer.openFolder() }
                                }
                            }
                        }
                    }
                }
            }
            Rectangle {
                Layout.fillWidth: true; Layout.preferredHeight: compact ? 72 : 82; color: root.surface; border.color: root.line
                Rectangle {
                    anchors.fill: parent; anchors.margins: compact ? 9 : 16; radius: 10; color: "#FBFCFD"
                    border.width: 1; border.color: composer.activeFocus ? root.cyanDark : root.line
                    RowLayout {
                        anchors.fill: parent; anchors.leftMargin: 6; anchors.rightMargin: 6; spacing: 2
                        IconAction { enabled: root.targetReady(); glyph: "add_circle_outline"; tip: t("file"); onClicked: fileDialog.open() }
                        TextArea {
                            id: composer
                            Layout.fillWidth: true; Layout.fillHeight: true
                            leftPadding: 6; rightPadding: 6; topPadding: 0; bottomPadding: 0
                            placeholderText: ""; enabled: root.targetReady(); wrapMode: TextEdit.Wrap
                            verticalAlignment: TextEdit.AlignVCenter; clip: true
                            color: root.ink; placeholderTextColor: root.muted; background: Item {}
                            Keys.onReturnPressed: event => { if (!(event.modifiers & Qt.ShiftModifier)) { sendMessage(); event.accepted = true } }
                            Label {
                                anchors.left: parent.left; anchors.leftMargin: composer.leftPadding
                                anchors.right: parent.right; anchors.rightMargin: composer.rightPadding
                                anchors.verticalCenter: parent.verticalCenter
                                visible: composer.text.length === 0
                                text: root.targetReady() ? t("message") : targetIp ? root.copy("连接已断开，重新点击设备配对", "Disconnected—tap the device to pair") : t("selectPeer")
                                color: root.muted; font.pixelSize: composer.font.pixelSize
                                elide: Text.ElideRight
                                verticalAlignment: Text.AlignVCenter
                            }
                        }
                        IconAction {
                            enabled: root.targetReady() && composer.text.trim() !== ""; glyph: "send"; tip: t("send")
                            iconColor: enabled ? "white" : "#AEBCC7"; hoverColor: enabled ? "#103E6B" : "transparent"
                            background: Rectangle { radius: 9; color: parent.enabled ? root.navy : "#E4EBF0" }
                            onClicked: sendMessage()
                        }
                    }
                }
            }
        }
    }

    Dialog {
        id: pairingDialog
        modal: true; x: Math.round((root.width - width) / 2); y: Math.round((root.height - height) / 2)
        implicitWidth: 390; width: Math.min(root.width - 32, implicitWidth)
        padding: 24; standardButtons: Dialog.NoButton
        closePolicy: Popup.NoAutoClose
        background: Rectangle { radius: 14; color: root.surface; border.color: root.line }
        contentItem: ColumnLayout {
            spacing: 16
            RowLayout {
                spacing: 14
                Rectangle { Layout.preferredWidth: 48; Layout.preferredHeight: 48; radius: 14; color: "#E1F6FB"; Text { anchors.centerIn: parent; text: "link"; font.family: root.iconFamily; font.pixelSize: 25; color: root.cyanDark } }
                ColumnLayout {
                    Layout.fillWidth: true; spacing: 2
                    Label { text: root.copy("设备配对请求", "Device pairing request"); color: root.ink; font.pixelSize: 18; font.weight: Font.Bold }
                    Label { text: root.pairingPeerName; color: root.muted; font.pixelSize: 12; elide: Text.ElideRight; Layout.fillWidth: true }
                }
            }
            Label {
                Layout.fillWidth: true; wrapMode: Text.Wrap
                text: root.copy("接受后将建立长连接，用于持续收发消息并控制文件和通话入口。", "Accept to keep a persistent connection for messages, files, and calls.")
                color: root.ink; font.pixelSize: 12
            }
            RowLayout {
                Layout.fillWidth: true; Item { Layout.fillWidth: true }
                ModernButton { text: t("decline"); onClicked: { transfer.rejectPairing(root.pairingPeerId); pairingDialog.close() } }
                ModernButton { text: t("accept"); primary: true; onClicked: { transfer.acceptPairing(root.pairingPeerId); pairingDialog.close() } }
            }
        }
    }

    Dialog {
        id: trustedDevicesDialog
        anchors.centerIn: parent
        width: Math.min(root.width - 32, 560)
        height: Math.min(root.height - 48, 620)
        modal: true; padding: 0; standardButtons: Dialog.NoButton
        background: Rectangle { radius: 18; color: root.surface; border.color: root.line }
        contentItem: ColumnLayout {
            spacing: 0
            RowLayout {
                Layout.fillWidth: true; Layout.margins: 22; Layout.bottomMargin: 16
                ColumnLayout {
                    spacing: 2
                    Label { text: root.copy("可信设备", "Trusted devices"); color: root.ink; font.pixelSize: 20; font.weight: Font.Bold }
                    Label { text: root.copy("管理已保存的局域网设备授权", "Manage saved LAN device access"); color: root.muted; font.pixelSize: 11 }
                }
                Item { Layout.fillWidth: true }
                IconAction { glyph: "close"; tip: root.copy("关闭", "Close"); onClicked: trustedDevicesDialog.close() }
            }
            Rectangle { Layout.fillWidth: true; Layout.preferredHeight: 1; color: root.line }
            ListView {
                id: trustedDevicesList
                Layout.fillWidth: true; Layout.fillHeight: true
                Layout.margins: 18; clip: true; spacing: 8
                model: transfer.trustedPeers
                ScrollIndicator.vertical: ScrollIndicator { }
                Label {
                    anchors.centerIn: parent; visible: trustedDevicesList.count === 0
                    width: parent.width - 32; horizontalAlignment: Text.AlignHCenter
                    text: root.copy("还没有可信设备\n首次配对成功后会显示在这里",
                                    "No trusted devices yet\nDevices appear here after pairing")
                    color: root.muted; wrapMode: Text.Wrap; lineHeight: 1.35
                }
                delegate: Rectangle {
                    id: trustedDeviceRow
                    required property var modelData
                    width: ListView.view.width; height: 92; radius: 12
                    color: "#F7FAFC"; border.color: root.line
                    RowLayout {
                        anchors.fill: parent; anchors.margins: 12; spacing: 11
                        Rectangle {
                            Layout.preferredWidth: 42; Layout.preferredHeight: 42; radius: 11
                            color: trustedDeviceRow.modelData.online ? "#DDF6EA" : "#E8EEF3"
                            Text {
                                anchors.centerIn: parent; text: "computer"; font.family: root.iconFamily; font.pixelSize: 23
                                color: trustedDeviceRow.modelData.online ? "#16845B" : root.muted
                            }
                        }
                        ColumnLayout {
                            Layout.fillWidth: true; spacing: 2
                            Label {
                                Layout.fillWidth: true; text: trustedDeviceRow.modelData.name || trustedDeviceRow.modelData.id
                                color: root.ink; font.pixelSize: 14; font.weight: Font.DemiBold; elide: Text.ElideRight
                            }
                            Label {
                                Layout.fillWidth: true
                                text: root.connectionStateLabel(trustedDeviceRow.modelData.state)
                                      + " · " + root.lastSeenLabel(trustedDeviceRow.modelData.lastSeen)
                                color: trustedDeviceRow.modelData.online ? "#16845B" : root.muted
                                font.pixelSize: 11; elide: Text.ElideRight
                            }
                            Label {
                                Layout.fillWidth: true; text: trustedDeviceRow.modelData.id
                                color: "#98A8B5"; font.pixelSize: 9; elide: Text.ElideMiddle
                            }
                        }
                        IconAction {
                            glyph: "delete_outline"; iconColor: "#B54A4A"
                            tip: root.copy("解除配对", "Forget device")
                            onClicked: {
                                root.pendingForgetPeerId = trustedDeviceRow.modelData.id
                                root.pendingForgetPeerName = trustedDeviceRow.modelData.name
                                forgetPeerDialog.open()
                            }
                        }
                    }
                }
            }
        }
    }

    Dialog {
        id: forgetPeerDialog
        anchors.centerIn: parent
        width: Math.min(root.width - 40, 430)
        modal: true; padding: 24; standardButtons: Dialog.NoButton
        background: Rectangle { radius: 16; color: root.surface; border.color: root.line }
        contentItem: ColumnLayout {
            spacing: 14
            Label { text: root.copy("解除设备配对？", "Forget this device?"); color: root.ink; font.pixelSize: 18; font.weight: Font.Bold }
            Label {
                Layout.fillWidth: true; wrapMode: Text.Wrap; color: root.muted; lineHeight: 1.3
                text: root.copy("将撤销“" + root.pendingForgetPeerName + "”的本机授权并取消未完成传输。聊天记录和已接收文件会保留。",
                                "This revokes local access for “" + root.pendingForgetPeerName + "” and cancels unfinished transfers. Chat history and received files are kept.")
            }
            RowLayout {
                Layout.fillWidth: true; spacing: 10
                Item { Layout.fillWidth: true }
                ModernButton { text: root.copy("取消", "Cancel"); onClicked: forgetPeerDialog.close() }
                ModernButton {
                    text: root.copy("解除配对", "Forget"); primary: true
                    onClicked: {
                        const forgottenId = root.pendingForgetPeerId
                        transfer.forgetPeer(forgottenId)
                        if (root.targetId === forgottenId) {
                            root.targetId = ""; root.targetIp = ""; root.targetPort = 0
                            root.targetMediaPort = 0; root.targetName = ""; timeline.clear()
                        }
                        forgetPeerDialog.close()
                        toast.show(root.copy("已解除配对", "Device forgotten"))
                    }
                }
            }
        }
    }

    Dialog {
        id: incomingDialog
        modal: true; x: Math.round((root.width - width) / 2); y: Math.round((root.height - height) / 2)
        implicitWidth: 380; implicitHeight: 230; width: Math.min(root.width - 32, implicitWidth)
        padding: 24; standardButtons: Dialog.NoButton
        background: Rectangle { radius: 14; color: root.surface; border.color: root.line }
        contentItem: ColumnLayout {
            spacing: 16
            RowLayout {
                spacing: 14
                Rectangle { Layout.preferredWidth: 48; Layout.preferredHeight: 48; radius: 24; color: "#E1F6FB"; Text { anchors.centerIn: parent; text: calls.mode === "screen" ? "screen_share" : "videocam"; font.family: root.iconFamily; font.pixelSize: 25; color: root.cyanDark } }
                ColumnLayout {
                    Layout.fillWidth: true; spacing: 2
                    Label { text: t("incomingCall"); color: root.ink; font.pixelSize: 18; font.weight: Font.Bold }
                    Label { text: peerNameForIp(incomingCallIp); color: root.muted; font.pixelSize: 12 }
                }
            }
            Label { text: calls.mode === "screen" ? t("screen") : t("video"); color: root.ink }
            RowLayout {
                Layout.fillWidth: true; Item { Layout.fillWidth: true }
                ModernButton { text: t("decline"); onClicked: { calls.rejectCall(); incomingDialog.close() } }
                ModernButton { text: t("accept"); primary: true; onClicked: { calls.acceptCall(); incomingDialog.close() } }
            }
        }
    }

    Dialog {
        id: callDialog
        modal: true; x: Math.round((root.width - width) / 2); y: Math.round((root.height - height) / 2)
        implicitWidth: 1120; implicitHeight: 720
        width: Math.min(root.width - (compact ? 8 : 24), implicitWidth)
        height: Math.min(root.height - (compact ? 8 : 24), implicitHeight)
        padding: 0; closePolicy: Popup.NoAutoClose
        background: Rectangle { radius: compact ? 8 : 14; color: root.surface; border.color: root.line }
        contentItem: RowLayout {
            spacing: 0
            Rectangle {
                visible: !compact; Layout.preferredWidth: 58; Layout.fillHeight: true; color: "#F8FAFC"; border.color: root.line
                ColumnLayout {
                    anchors.fill: parent; anchors.margins: 8; spacing: 10
                    Rectangle {
                        Layout.alignment: Qt.AlignHCenter; Layout.preferredWidth: 36; Layout.preferredHeight: 36; radius: 9; color: root.navy
                        Text { anchors.centerIn: parent; text: "device_hub"; font.family: root.iconFamily; font.pixelSize: 21; color: "white" }
                    }
                    Item { Layout.preferredHeight: 14 }
                    IconAction { Layout.alignment: Qt.AlignHCenter; glyph: "devices" }
                    IconAction { Layout.alignment: Qt.AlignHCenter; glyph: "chat" }
                    IconAction { Layout.alignment: Qt.AlignHCenter; glyph: "videocam"; iconColor: root.cyanDark; hoverColor: "#DFF4F9" }
                    Item { Layout.fillHeight: true }
                    IconAction { Layout.alignment: Qt.AlignHCenter; glyph: "settings"; onClicked: settingsDrawer.open() }
                }
            }
            Rectangle {
                Layout.fillWidth: true; Layout.fillHeight: true; color: root.canvas
                ColumnLayout {
                    anchors.fill: parent; anchors.margins: compact ? 8 : 18; spacing: 10
                    Rectangle {
                        Layout.fillWidth: true; Layout.preferredHeight: 40; radius: 7; color: root.surface; border.color: root.line
                        RowLayout {
                            anchors.fill: parent; anchors.leftMargin: 12; anchors.rightMargin: 12
                            Text { text: calls.mode === "screen" ? "screen_share" : "videocam"; font.family: root.iconFamily; font.pixelSize: 17; color: root.cyanDark }
                            Label { Layout.fillWidth: true; text: calls.mode === "screen" ? root.copy("正在共享屏幕", "Screen sharing") : root.copy("与 " + targetName + " 视频通话", "Video call with " + targetName); color: root.ink; font.pixelSize: 12; font.weight: Font.DemiBold }
                            Label { text: calls.state === "connected" ? root.copy("已连接", "Connected") : root.copy("连接中", "Connecting"); color: root.cyanDark; font.pixelSize: 10 }
                        }
                    }
                    Rectangle {
                        Layout.fillWidth: true; Layout.fillHeight: true; radius: 8; color: "#0A1F33"; border.color: "#183A55"; clip: true
                        Image { anchors.fill: parent; anchors.margins: 1; fillMode: Image.PreserveAspectFit; cache: false; source: "image://call/remote?" + calls.remoteFrameRevision }
                        ColumnLayout {
                            anchors.centerIn: parent; visible: calls.state === "calling"; spacing: 10
                            BusyIndicator { Layout.alignment: Qt.AlignHCenter; running: true }
                            Label { text: root.copy("正在等待对方…", "Waiting for the other device…"); color: "white"; font.pixelSize: 15 }
                        }
                        Rectangle {
                            visible: !calls.videoMuted; width: compact ? 120 : 174; height: compact ? 82 : 112
                            anchors.right: parent.right; anchors.top: parent.top; anchors.margins: 14
                            radius: 8; color: "#172E43"; border.color: "#66849B"; clip: true
                            Image { anchors.fill: parent; anchors.margins: 1; fillMode: Image.PreserveAspectFit; cache: false; source: "image://call/local?" + calls.localFrameRevision }
                            Label { anchors.left: parent.left; anchors.bottom: parent.bottom; anchors.margins: 7; text: root.copy("你", "You"); color: "white"; font.pixelSize: 9 }
                        }
                        Rectangle {
                            width: compact ? Math.min(parent.width - 20, 330) : 420; height: 64; radius: 32
                            color: "#F8FBFD"; anchors.horizontalCenter: parent.horizontalCenter; anchors.bottom: parent.bottom; anchors.bottomMargin: 16
                            border.color: root.line
                            RowLayout {
                                anchors.centerIn: parent; spacing: compact ? 3 : 8
                                IconAction { glyph: calls.audioMuted ? "mic_off" : "mic"; hoverColor: "#E4EDF3"; onClicked: calls.audioMuted = !calls.audioMuted }
                                IconAction { glyph: calls.videoMuted ? "videocam_off" : "videocam"; hoverColor: "#E4EDF3"; onClicked: calls.videoMuted = !calls.videoMuted }
                                Rectangle { Layout.preferredWidth: 1; Layout.preferredHeight: 26; color: root.line }
                                IconAction { glyph: "screen_share"; iconColor: calls.mode === "screen" ? root.cyanDark : root.ink; hoverColor: "#DDF5FA"; onClicked: calls.switchMode(calls.mode === "screen" ? "camera" : "screen") }
                                IconAction { glyph: "more_vert"; hoverColor: "#E4EDF3" }
                                ModernButton {
                                    id: hangup; Layout.preferredWidth: compact ? 70 : 108; Layout.preferredHeight: 42
                                    glyph: "call_end"; primary: true
                                    text: compact ? "" : t("hangup"); onClicked: calls.hangup()
                                }
                            }
                        }
                    }
                }
            }
            Rectangle {
                visible: !compact; Layout.preferredWidth: 230; Layout.fillHeight: true; color: root.surface; border.color: root.line
                ColumnLayout {
                    anchors.fill: parent; anchors.margins: 14; spacing: 12
                    RowLayout {
                        Layout.fillWidth: true
                        Label { text: root.copy("会话信息", "Session info"); color: root.ink; font.weight: Font.DemiBold }
                        Item { Layout.fillWidth: true }
                        Rectangle {
                            Layout.preferredWidth: 68; Layout.preferredHeight: 24; radius: 7; color: "#DDF7FC"
                            Label { anchors.centerIn: parent; text: calls.state === "connected" ? root.copy("已连接", "LIVE") : "..."; color: root.cyanDark; font.pixelSize: 9; font.weight: Font.Bold }
                        }
                    }
                    Rectangle { Layout.fillWidth: true; Layout.preferredHeight: 1; color: root.line }
                    Label { text: root.copy("参与者 (2)", "PARTICIPANTS (2)"); color: root.muted; font.pixelSize: 9; font.weight: Font.Bold; font.letterSpacing: 1 }
                    Repeater {
                        model: [{name:root.copy("你","You"), sub:root.copy("本机","This device"), initial:root.copy("我","ME"), local:true},
                                {name:targetName || root.copy("对方设备","Peer"), sub:calls.mode === "screen" ? t("screen") : t("video"), initial:targetName ? targetName.charAt(0).toUpperCase() : "P", local:false}]
                        delegate: Rectangle {
                            required property var modelData
                            Layout.fillWidth: true; Layout.preferredHeight: 58; radius: 8
                            color: modelData.local ? "#F6F9FB" : "#EAF7FB"; border.color: modelData.local ? "transparent" : "#BFE9F3"
                            RowLayout {
                                anchors.fill: parent; anchors.margins: 9
                                Rectangle { Layout.preferredWidth: 34; Layout.preferredHeight: 34; radius: 17; color: modelData.local ? "#E3ECF3" : root.navy; Label { anchors.centerIn: parent; text: modelData.initial; color: modelData.local ? root.ink : "white"; font.pixelSize: 9 } }
                                ColumnLayout {
                                    Layout.fillWidth: true; spacing: 1
                                    Label { text: modelData.name; color: root.ink; font.pixelSize: 12; elide: Text.ElideRight; Layout.fillWidth: true }
                                    Label { text: modelData.sub; color: modelData.local ? root.muted : root.cyanDark; font.pixelSize: 9 }
                                }
                                Text { text: modelData.local && calls.audioMuted ? "mic_off" : "volume_up"; font.family: root.iconFamily; font.pixelSize: 16; color: modelData.local && calls.audioMuted ? "#A6B3BD" : root.cyanDark }
                            }
                        }
                    }
                    Item { Layout.fillHeight: true }
                    Label { Layout.fillWidth: true; wrapMode: Text.Wrap; text: root.copy("局域网点对点传输，不经过云端。", "Peer-to-peer on your LAN. No cloud relay."); color: root.muted; font.pixelSize: 10 }
                }
            }
        }
    }

    Rectangle {
        id: toast
        property string message: ""
        z: 999; anchors.horizontalCenter: parent.horizontalCenter; anchors.bottom: parent.bottom; anchors.bottomMargin: 94
        width: Math.min(root.width - 32, toastLabel.implicitWidth + 42); height: 42; radius: 10; color: root.navy; opacity: 0
        Label { id: toastLabel; anchors.centerIn: parent; text: toast.message; color: "white"; font.pixelSize: 12 }
        SequentialAnimation {
            id: toastAnimation
            NumberAnimation { target: toast; property: "opacity"; to: 1; duration: 140 }
            PauseAnimation { duration: 2200 }
            NumberAnimation { target: toast; property: "opacity"; to: 0; duration: 220 }
        }
        function show(message) { toast.message = message; toastAnimation.restart() }
    }
    Component.onCompleted: {
        transfer.saveDirectory = app.downloadDirectory
        transfer.restoreTransfers()
        if (isMobile) visibility = Window.Maximized
    }
}

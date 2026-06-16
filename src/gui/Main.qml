import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import QtQuick.Dialogs

ApplicationWindow {
    id: window
    width: 960
    height: 780
    minimumWidth: 760
    minimumHeight: 640
    visible: true
    title: "bridge"
    color: "#f6f7f9"
    font.family: "Sans Serif"
    property bool receiving: false

    component Copy : Label {
        color: "#687082"
        font.pixelSize: 13
        textFormat: Text.PlainText
        wrapMode: Text.WordWrap
    }
    component Action : Button {
        id: action
        property bool primary: false
        implicitHeight: 40
        leftPadding: 16; rightPadding: 16
        hoverEnabled: true
        font.pixelSize: 13; font.weight: Font.DemiBold
        contentItem: Text {
            text: action.text; font: action.font
            color: !action.enabled ? "#9299a7" : action.primary ? "white" : "#303849"
            horizontalAlignment: Text.AlignHCenter; verticalAlignment: Text.AlignVCenter
            textFormat: Text.PlainText
        }
        background: Rectangle {
            radius: 8
            color: !action.enabled ? "#eef0f4" : action.primary ? (action.down ? "#403bc4" : action.hovered ? "#605beb" : "#514bd7") : action.hovered ? "#f0f2f6" : "white"
            border.width: action.activeFocus ? 2 : 1
            border.color: action.activeFocus ? "#9a96ee" : action.primary ? "transparent" : "#dfe3ea"
        }
    }
    component Field : TextField {
        id: field
        implicitHeight: 40
        leftPadding: 12; rightPadding: 12
        color: "#303849"; placeholderTextColor: "#9299a7"
        font.pixelSize: 13
        selectByMouse: true
        background: Rectangle {
            radius: 8; color: field.enabled ? "white" : "#f0f2f5"
            border.color: field.activeFocus ? "#837de5" : "#dfe3ea"
        }
    }
    component Panel : Rectangle {
        radius: 12; color: "white"; border.color: "#e1e5ec"
    }

    FileDialog {
        id: filePicker
        title: "Choose file"
        fileMode: FileDialog.OpenFile
        options: FileDialog.DontResolveSymlinks
        onAccepted: sessionModel.selectSource(selectedFile)
    }
    FolderDialog {
        id: folderPicker
        title: "Choose folder"
        options: FolderDialog.DontResolveSymlinks
        onAccepted: sessionModel.selectSource(selectedFolder)
    }
    FolderDialog {
        id: destinationPicker
        title: "Choose destination"
        options: FolderDialog.DontResolveSymlinks
        onAccepted: sessionModel.selectDestination(selectedFolder)
    }

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 28
        spacing: 20
        RowLayout {
            Layout.fillWidth: true
            Label { text: "bridge"; color: "#222936"; font.pixelSize: 23; font.weight: Font.DemiBold; Layout.fillWidth: true }
            Action { text: "Send"; primary: !window.receiving; enabled: !sessionModel.busy; onClicked: window.receiving = false }
            Action { text: "Receive"; primary: window.receiving; enabled: !sessionModel.busy; onClicked: window.receiving = true }
        }
        ScrollView {
            id: scroll
            Layout.fillWidth: true
            Layout.fillHeight: true
            contentWidth: availableWidth
            clip: true
            ScrollBar.horizontal.policy: ScrollBar.AlwaysOff
            ColumnLayout {
                width: scroll.availableWidth
                spacing: 16
                Panel {
                    visible: !window.receiving
                    Layout.fillWidth: true
                    implicitHeight: 190
                    color: dropZone.containsDrag ? "#f0efff" : "white"
                    border.color: dropZone.containsDrag ? "#837de5" : "#e1e5ec"
                    DropArea {
                        id: dropZone
                        objectName: "sourceDropArea"
                        anchors.fill: parent
                        enabled: !sessionModel.busy && sessionModel.transferSupported
                        onEntered: function(drag) { drag.accepted = drag.hasUrls && !sessionModel.busy }
                        onDropped: function(drop) {
                            if (drop.hasUrls && sessionModel.dropUrls(drop.urls)) drop.acceptProposedAction()
                            else drop.accepted = false
                        }
                    }
                    ColumnLayout {
                        anchors.centerIn: parent
                        width: parent.width - 40
                        spacing: 13
                        Label {
                            text: sessionModel.selectedSource.length ? sessionModel.selectedName : "Drop a file or folder"
                            textFormat: Text.PlainText
                            elide: Text.ElideMiddle
                            color: "#303849"; font.pixelSize: 18; font.weight: Font.DemiBold
                            Layout.maximumWidth: parent.width
                            Layout.alignment: Qt.AlignHCenter
                        }
                        Copy {
                            visible: sessionModel.selectedSource.length > 0
                            text: sessionModel.selectedSource
                            Layout.fillWidth: true
                            horizontalAlignment: Text.AlignHCenter
                            elide: Text.ElideMiddle; wrapMode: Text.NoWrap
                        }
                        RowLayout {
                            Layout.alignment: Qt.AlignHCenter
                            spacing: 10
                            Action { objectName: "chooseFolderButton"; text: "Choose folder"; enabled: !sessionModel.busy && sessionModel.transferSupported; onClicked: folderPicker.open() }
                            Action { text: "Choose file"; enabled: !sessionModel.busy && sessionModel.transferSupported; onClicked: filePicker.open() }
                        }
                    }
                }
                Panel {
                    visible: !window.receiving
                    Layout.fillWidth: true
                    implicitHeight: sendContent.implicitHeight + 40
                    ColumnLayout {
                        id: sendContent
                        anchors.fill: parent; anchors.margins: 20
                        spacing: 12
                        Copy { text: "Receiver"; color: "#303849"; font.weight: Font.DemiBold }
                        RowLayout {
                            Layout.fillWidth: true
                            spacing: 10
                            Field { id: peer; text: "127.0.0.1"; placeholderText: "IP address"; Accessible.name: "Receiver IP address"; Layout.fillWidth: true; enabled: !sessionModel.busy }
                            Field { id: peerPort; placeholderText: "Port"; Accessible.name: "Receiver port"; Layout.preferredWidth: 90; enabled: !sessionModel.busy }
                            Action { text: "Send"; primary: true; enabled: !sessionModel.busy && sessionModel.transferSupported && sessionModel.selectedSource.length > 0 && peerPort.text.length > 0; onClicked: sessionModel.sendSelected(peer.text, peerPort.text, resume.checked) }
                        }
                    }
                }
                Panel {
                    visible: window.receiving
                    Layout.fillWidth: true
                    implicitHeight: receiveContent.implicitHeight + 40
                    ColumnLayout {
                        id: receiveContent
                        anchors.fill: parent; anchors.margins: 20
                        spacing: 12
                        Copy { text: "Destination"; color: "#303849"; font.weight: Font.DemiBold }
                        RowLayout {
                            Layout.fillWidth: true
                            Copy { text: sessionModel.destination.length ? sessionModel.destination : "No folder selected"; Layout.fillWidth: true; elide: Text.ElideMiddle; wrapMode: Text.NoWrap }
                            Action { text: "Choose folder"; enabled: !sessionModel.busy && sessionModel.transferSupported; onClicked: destinationPicker.open() }
                        }
                        Copy { text: "Listen address"; color: "#303849"; font.weight: Font.DemiBold }
                        RowLayout {
                            Layout.fillWidth: true
                            ComboBox {
                                id: localAddress
                                model: sessionModel.localAddresses
                                Layout.fillWidth: true
                                enabled: !sessionModel.busy
                                implicitHeight: 40
                                font.pixelSize: 13
                                indicator: Text {
                                    x: parent.width - width - 12
                                    y: (parent.height - height) / 2
                                    text: "▾"; color: "#687082"
                                }
                                background: Rectangle {
                                    radius: 8; color: localAddress.enabled ? "white" : "#f0f2f5"
                                    border.color: localAddress.activeFocus ? "#837de5" : "#dfe3ea"
                                }
                            }
                            Field { id: receivePort; text: "0"; placeholderText: "Port"; Layout.preferredWidth: 90; enabled: !sessionModel.busy; Accessible.name: "Receive port, zero chooses an available port" }
                            Action { text: "Start receiving"; primary: true; enabled: !sessionModel.busy && sessionModel.transferSupported && sessionModel.destination.length > 0; onClicked: sessionModel.receive(localAddress.currentText, receivePort.text, sessionModel.destination, resume.checked) }
                        }
                        Copy { visible: sessionModel.endpoint.length > 0 && sessionModel.busy; text: "Address: " + sessionModel.endpoint; Layout.fillWidth: true }
                    }
                }
                Panel {
                    visible: sessionModel.canConfirm
                    Layout.fillWidth: true
                    implicitHeight: pairingContent.implicitHeight + 40
                    ColumnLayout {
                        id: pairingContent
                        anchors.fill: parent; anchors.margins: 20
                        spacing: 12
                        Label { text: "Confirm peer"; color: "#303849"; font.pixelSize: 17; font.weight: Font.DemiBold }
                        Copy { text: "Compare all 64 fingerprint digits on both computers. Confirm only if they match."; Layout.fillWidth: true }
                        TextArea {
                            text: sessionModel.fingerprint; textFormat: TextEdit.PlainText
                            readOnly: true; selectByMouse: true; wrapMode: TextEdit.WrapAnywhere
                            font.family: "monospace"; font.pixelSize: 15; color: "#303849"
                            Layout.fillWidth: true
                            background: Rectangle { radius: 8; color: "#f3f4f7" }
                        }
                        RowLayout {
                            Action { text: "Confirm match"; primary: true; onClicked: sessionModel.confirm() }
                            Action { text: "Reject"; onClicked: sessionModel.reject() }
                        }
                    }
                }
                Panel {
                    Layout.fillWidth: true
                    implicitHeight: transferContent.implicitHeight + 40
                    ColumnLayout {
                        id: transferContent
                        anchors.fill: parent; anchors.margins: 20
                        spacing: 12
                        RowLayout {
                            Layout.fillWidth: true
                            Label { text: sessionModel.phaseTitle; color: "#303849"; font.pixelSize: 17; font.weight: Font.DemiBold; Layout.fillWidth: true }
                            Copy { visible: sessionModel.filename.length > 0; text: Math.round(sessionModel.progress * 100) + "%" }
                        }
                        Copy { visible: sessionModel.busy; text: sessionModel.authenticated ? "Peer confirmed · TLS encrypted" : "Peer not yet confirmed"; Layout.fillWidth: true }
                        Copy { text: sessionModel.status; Layout.fillWidth: true }
                        Copy { visible: sessionModel.filename.length > 0; text: (sessionModel.offeredFolder ? "Folder: " : "File: ") + sessionModel.filename; color: "#303849"; Layout.fillWidth: true; elide: Text.ElideMiddle; wrapMode: Text.NoWrap }
                        ProgressBar {
                            id: progressBar
                            value: sessionModel.progress; Layout.fillWidth: true; visible: sessionModel.filename.length > 0
                            contentItem: Item { implicitHeight: 6; Rectangle { width: progressBar.visualPosition * parent.width; height: parent.height; radius: 3; color: "#514bd7" } }
                            background: Rectangle { implicitHeight: 6; radius: 3; color: "#eceef3" }
                        }
                        Copy { visible: sessionModel.checkpoint.length > 0; text: sessionModel.checkpoint; font.pixelSize: 12; Layout.fillWidth: true }
                        Copy { visible: window.receiving && sessionModel.canAccept; text: "Destination: " + sessionModel.destination; Layout.fillWidth: true }
                        RowLayout {
                            visible: sessionModel.canAccept || sessionModel.busy
                            Action { text: "Accept"; primary: true; visible: sessionModel.canAccept; onClicked: sessionModel.acceptFile() }
                            Action { text: "Pause"; objectName: "pauseButton"; enabled: sessionModel.canPause; visible: !sessionModel.canAccept; onClicked: sessionModel.pause() }
                            Action { text: "Continue"; objectName: "continueButton"; primary: true; enabled: sessionModel.canContinue; visible: sessionModel.canContinue; onClicked: sessionModel.continueTransfer() }
                            Item { Layout.fillWidth: true }
                            Action { text: window.receiving ? "Stop receiving" : "Cancel"; onClicked: sessionModel.cancel() }
                        }
                    }
                }
                CheckBox {
                    id: resume
                    text: "Resume interrupted transfer"
                    enabled: !sessionModel.busy
                    font.pixelSize: 13
                    indicator: Rectangle {
                        implicitWidth: 18; implicitHeight: 18
                        x: resume.leftPadding; y: (resume.height - height) / 2
                        radius: 4
                        border.color: resume.checked ? "#514bd7" : "#b9c0cb"
                        color: resume.checked ? "#514bd7" : "white"
                        Rectangle {
                            anchors.centerIn: parent
                            width: 8; height: 8; radius: 2
                            color: "white"; visible: resume.checked
                        }
                    }
                }
            }
        }
    }
    onClosing: sessionModel.cancel()
}

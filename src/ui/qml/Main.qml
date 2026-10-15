import QtQuick
import QtQuick.Controls
import BlendNet.Ui

ApplicationWindow {
    id: root
    width: 1100
    height: 700
    visible: true
    title: qsTr("BlendNet")

    AppController { id: app }

    Label {
        anchors.centerIn: parent
        text: qsTr("%1 %2 : fenêtre vide (P1)").arg(app.appName).arg(app.version)
        font.pixelSize: 20
    }
}

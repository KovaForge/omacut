import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts

// Where upload hosts are listed, added, edited and removed. The forms are
// generated from each provider's fields (see upload::Provider).
Rectangle {
    id: panel
    color: "#000000cc"

    // The upload::Hosts to manage.
    property var hosts: null
    // Where .sxcu files can be dropped, for the hint under the list.
    property string uploadersDir: ""
    signal closed()

    // "list", "pick" (a kind of host to add) or "edit".
    property string mode: "list"
    property string editId: ""
    property string editProvider: ""
    property string editProviderName: ""
    property string authorizeLabel: ""
    property var editFields: []
    // The form's values by field key; edited in place, read on save.
    property var editValues: ({})
    property var secretsSet: []
    // Bumped when values change from outside the fields (a sign-in), so
    // they re-read what they show.
    property int formRevision: 0
    property string formError: ""
    property string authorizeMessage: ""
    property string pendingRemove: ""

    function open() {
        mode = "list";
        pendingRemove = "";
        visible = true;
    }
    function close() {
        if (hosts) {
            hosts.cancelAuthorize();
            hosts.discardAuthorization();
        }
        visible = false;
        closed();
    }
    // Escape: out of a form or picker to the list, then closed.
    function back() {
        if (mode === "list") {
            close();
            return;
        }
        hosts.cancelAuthorize();
        hosts.discardAuthorization();
        mode = "list";
    }

    function defaultFor(field) {
        if (field.defaultValue !== undefined && field.defaultValue !== null)
            return field.defaultValue;
        if (field.type === "toggle")
            return false;
        if (field.type === "number")
            return 0;
        if (field.type === "hosts")
            return [];
        if (field.type === "choice")
            return field.choices.length > 0 ? field.choices[0] : "";
        return "";
    }
    function beginForm(id, providerId, name, values) {
        hosts.discardAuthorization();
        var provider = null;
        for (var i = 0; i < hosts.providers.length; ++i) {
            if (hosts.providers[i].id === providerId)
                provider = hosts.providers[i];
        }
        editId = id;
        editProvider = providerId;
        editProviderName = provider ? provider.name : providerId;
        authorizeLabel = provider ? provider.authorizeLabel : "";
        editFields = hosts.fields(providerId);
        var v = {};
        for (var f = 0; f < editFields.length; ++f) {
            var field = editFields[f];
            v[field.key] = values[field.key] !== undefined ? values[field.key] : defaultFor(field);
        }
        editValues = v;
        secretsSet = values.secretsSet || [];
        nameField.text = name;
        formError = "";
        authorizeMessage = "";
        mode = "edit";
        formRevision++;
    }
    function startEdit(host) {
        beginForm(host.id, host.provider, host.name, hosts.values(host.id));
    }
    function setValue(key, value) {
        editValues[key] = value;
    }
    function save() {
        var id = hosts.save(editId, editProvider, nameField.text, editValues);
        if (id === "") {
            formError = hosts.lastError();
            return;
        }
        mode = "list";
    }

    Connections {
        target: panel.hosts
        function onAuthorizeStatus(message) {
            panel.authorizeMessage = message;
        }
        function onAuthorizeFinished(error, summary, formValues) {
            if (error !== "") {
                panel.authorizeMessage = error;
                return;
            }
            for (var key in formValues) {
                if (key !== "secretsSet")
                    panel.editValues[key] = formValues[key];
            }
            panel.secretsSet = panel.secretsSet.concat(formValues.secretsSet || []);
            panel.authorizeMessage = summary + " — save to keep it.";
            panel.formRevision++;
        }
    }

    MouseArea {
        anchors.fill: parent
        onClicked: panel.close()
    }

    Rectangle {
        id: card
        anchors.centerIn: parent
        width: Math.min(parent.width - 48, 520)
        height: Math.min(parent.height - 48, content.implicitHeight + 48)
        radius: 12
        color: "#1c1c1e"

        MouseArea {
            anchors.fill: parent
        }

        ScrollView {
            id: scroller
            anchors.fill: parent
            anchors.margins: 24
            contentWidth: availableWidth
            clip: true

            ColumnLayout {
                id: content
                width: scroller.availableWidth
                spacing: 10

                // --- the list of hosts ---
                ColumnLayout {
                    visible: panel.mode === "list"
                    Layout.fillWidth: true
                    spacing: 6

                    Label {
                        text: "Upload hosts"
                        color: "white"
                        font.pixelSize: 16
                        font.weight: Font.DemiBold
                        bottomPadding: 4
                    }

                    Repeater {
                        model: panel.hosts ? panel.hosts.list : []
                        delegate: Rectangle {
                            required property var modelData
                            Layout.fillWidth: true
                            implicitHeight: 48
                            radius: 8
                            color: "#242427"

                            ColumnLayout {
                                anchors.left: parent.left
                                anchors.right: hostButtons.left
                                anchors.verticalCenter: parent.verticalCenter
                                anchors.leftMargin: 12
                                anchors.rightMargin: 8
                                spacing: 0
                                Label {
                                    Layout.fillWidth: true
                                    text: modelData.name
                                    color: "white"
                                    font.pixelSize: 13
                                    elide: Text.ElideRight
                                }
                                Label {
                                    Layout.fillWidth: true
                                    text: modelData.readOnly
                                        ? (modelData.id.indexOf("builtin:") === 0 ? "Built in" : ".sxcu file")
                                        : modelData.providerName
                                    color: "#8a8a90"
                                    font.pixelSize: 11
                                    elide: Text.ElideRight
                                }
                            }
                            Row {
                                id: hostButtons
                                anchors.right: parent.right
                                anchors.rightMargin: 8
                                anchors.verticalCenter: parent.verticalCenter
                                spacing: 6
                                visible: !modelData.readOnly
                                DialogButton {
                                    text: "Edit"
                                    onClicked: panel.startEdit(modelData)
                                }
                                DialogButton {
                                    text: panel.pendingRemove === modelData.id ? "Really remove" : "Remove"
                                    onClicked: {
                                        if (panel.pendingRemove === modelData.id) {
                                            panel.pendingRemove = "";
                                            panel.hosts.remove(modelData.id);
                                        } else {
                                            panel.pendingRemove = modelData.id;
                                        }
                                    }
                                }
                            }
                        }
                    }

                    Label {
                        Layout.fillWidth: true
                        topPadding: 4
                        text: "ShareX .sxcu files in " + panel.uploadersDir + " are listed too."
                        color: "#7a7a80"
                        font.pixelSize: 11
                        wrapMode: Text.WrapAnywhere
                    }

                    RowLayout {
                        Layout.fillWidth: true
                        Layout.topMargin: 8
                        Item { Layout.fillWidth: true }
                        DialogButton {
                            text: "Done"
                            onClicked: panel.close()
                        }
                        DialogButton {
                            text: "Add host"
                            primary: true
                            onClicked: panel.mode = "pick"
                        }
                    }
                }

                // --- picking a kind of host ---
                ColumnLayout {
                    visible: panel.mode === "pick"
                    Layout.fillWidth: true
                    spacing: 6

                    Label {
                        text: "Add a host"
                        color: "white"
                        font.pixelSize: 16
                        font.weight: Font.DemiBold
                        bottomPadding: 4
                    }

                    Repeater {
                        model: panel.hosts ? panel.hosts.providers : []
                        delegate: Rectangle {
                            required property var modelData
                            objectName: "provider_" + modelData.id
                            Layout.fillWidth: true
                            implicitHeight: providerText.implicitHeight + 20
                            radius: 8
                            color: providerHover.hovered ? "#2c2c2f" : "#242427"

                            ColumnLayout {
                                id: providerText
                                anchors.left: parent.left
                                anchors.right: parent.right
                                anchors.verticalCenter: parent.verticalCenter
                                anchors.margins: 12
                                spacing: 2
                                Label {
                                    text: modelData.name
                                    color: "white"
                                    font.pixelSize: 13
                                    font.weight: Font.DemiBold
                                }
                                Label {
                                    Layout.fillWidth: true
                                    text: modelData.description
                                    color: "#b8b8bc"
                                    font.pixelSize: 12
                                    wrapMode: Text.WordWrap
                                }
                            }
                            HoverHandler {
                                id: providerHover
                                cursorShape: Qt.PointingHandCursor
                            }
                            TapHandler {
                                onTapped: panel.beginForm("", modelData.id, modelData.name, {})
                            }
                        }
                    }

                    RowLayout {
                        Layout.fillWidth: true
                        Layout.topMargin: 8
                        Item { Layout.fillWidth: true }
                        DialogButton {
                            text: "Back"
                            onClicked: panel.mode = "list"
                        }
                    }
                }

                // --- a host's settings ---
                ColumnLayout {
                    visible: panel.mode === "edit"
                    Layout.fillWidth: true
                    spacing: 4

                    Label {
                        text: (panel.editId === "" ? "New " : "") + panel.editProviderName + " host"
                        color: "white"
                        font.pixelSize: 16
                        font.weight: Font.DemiBold
                        bottomPadding: 4
                    }

                    Label {
                        text: "Name"
                        color: "#b8b8bc"
                        font.pixelSize: 12
                    }
                    TextField {
                        id: nameField
                        objectName: "field_name"
                        Layout.fillWidth: true
                        implicitHeight: 40
                        font.pixelSize: 13
                        selectByMouse: true
                    }

                    Repeater {
                        model: panel.editFields
                        delegate: ColumnLayout {
                            id: fieldColumn
                            required property var modelData
                            readonly property var field: modelData
                            Layout.fillWidth: true
                            Layout.topMargin: 6
                            spacing: 2

                            Label {
                                visible: fieldColumn.field.type !== "toggle"
                                text: fieldColumn.field.label + (fieldColumn.field.required ? "" : " (optional)")
                                color: "#b8b8bc"
                                font.pixelSize: 12
                            }

                            TextField {
                                id: textInput
                                objectName: "field_" + fieldColumn.field.key
                                visible: ["text", "secret", "number"].indexOf(fieldColumn.field.type) >= 0
                                Layout.fillWidth: true
                                implicitHeight: 40
                                font.pixelSize: 13
                                selectByMouse: true
                                echoMode: fieldColumn.field.type === "secret" ? TextInput.Password : TextInput.Normal
                                inputMethodHints: fieldColumn.field.type === "number" ? Qt.ImhDigitsOnly : Qt.ImhNone
                                validator: fieldColumn.field.type === "number" ? numberValidator : null
                                placeholderText: fieldColumn.field.type === "secret"
                                    && panel.secretsSet.indexOf(fieldColumn.field.key) >= 0
                                    ? "Stored — leave blank to keep it"
                                    : fieldColumn.field.placeholder
                                function load() {
                                    if (!visible)
                                        return;
                                    var value = panel.editValues[fieldColumn.field.key];
                                    text = fieldColumn.field.type === "secret" || value === undefined || value === null
                                        ? "" : String(value);
                                }
                                onTextEdited: panel.setValue(fieldColumn.field.key,
                                    fieldColumn.field.type === "number" ? (parseInt(text) || 0) : text)
                                Component.onCompleted: load()
                                Connections {
                                    target: panel
                                    function onFormRevisionChanged() { textInput.load(); }
                                }
                            }
                            IntValidator {
                                id: numberValidator
                                bottom: 0
                            }

                            ScrollView {
                                visible: fieldColumn.field.type === "multiline"
                                Layout.fillWidth: true
                                Layout.preferredHeight: 140
                                TextArea {
                                    id: areaInput
                                    objectName: "field_" + fieldColumn.field.key
                                    wrapMode: TextEdit.WrapAnywhere
                                    selectByMouse: true
                                    font.family: "monospace"
                                    font.pixelSize: 12
                                    placeholderText: fieldColumn.field.placeholder
                                    function load() {
                                        if (fieldColumn.field.type === "multiline")
                                            text = panel.editValues[fieldColumn.field.key] || "";
                                    }
                                    onTextChanged: {
                                        if (fieldColumn.field.type === "multiline" && activeFocus)
                                            panel.setValue(fieldColumn.field.key, text);
                                    }
                                    Component.onCompleted: load()
                                    Connections {
                                        target: panel
                                        function onFormRevisionChanged() { areaInput.load(); }
                                    }
                                }
                            }

                            CheckBox {
                                id: toggleInput
                                objectName: "field_" + fieldColumn.field.key
                                visible: fieldColumn.field.type === "toggle"
                                text: fieldColumn.field.label
                                leftPadding: 0
                                function load() {
                                    if (fieldColumn.field.type === "toggle")
                                        checked = !!panel.editValues[fieldColumn.field.key];
                                }
                                onToggled: panel.setValue(fieldColumn.field.key, checked)
                                Component.onCompleted: load()
                                Connections {
                                    target: panel
                                    function onFormRevisionChanged() { toggleInput.load(); }
                                }
                            }

                            ComboBox {
                                id: choiceInput
                                objectName: "field_" + fieldColumn.field.key
                                visible: fieldColumn.field.type === "choice"
                                Layout.fillWidth: true
                                implicitHeight: 40
                                font.pixelSize: 13
                                model: fieldColumn.field.choices
                                function load() {
                                    if (fieldColumn.field.type === "choice")
                                        currentIndex = Math.max(0, fieldColumn.field.choices.indexOf(
                                            panel.editValues[fieldColumn.field.key]));
                                }
                                onActivated: (index) => panel.setValue(fieldColumn.field.key,
                                                                       fieldColumn.field.choices[index])
                                Component.onCompleted: load()
                                Connections {
                                    target: panel
                                    function onFormRevisionChanged() { choiceInput.load(); }
                                }
                            }

                            // An ordered pick of other hosts: ticked ones first,
                            // in the order they'll be tried.
                            ColumnLayout {
                                id: hostsInput
                                visible: fieldColumn.field.type === "hosts"
                                Layout.fillWidth: true
                                spacing: 0
                                property var chosen: []
                                readonly property var candidates: {
                                    var all = panel.hosts ? panel.hosts.list : [];
                                    var others = [];
                                    for (var i = 0; i < all.length; ++i) {
                                        if (all[i].provider !== "auto" && all[i].id !== panel.editId)
                                            others.push(all[i]);
                                    }
                                    var ordered = [];
                                    for (var c = 0; c < chosen.length; ++c) {
                                        for (var j = 0; j < others.length; ++j) {
                                            if (others[j].id === chosen[c])
                                                ordered.push(others[j]);
                                        }
                                    }
                                    for (var k = 0; k < others.length; ++k) {
                                        if (chosen.indexOf(others[k].id) < 0)
                                            ordered.push(others[k]);
                                    }
                                    return ordered;
                                }
                                function load() {
                                    if (fieldColumn.field.type === "hosts")
                                        chosen = (panel.editValues[fieldColumn.field.key] || []).slice();
                                }
                                function store(list) {
                                    chosen = list;
                                    panel.setValue(fieldColumn.field.key, list.slice());
                                }
                                Component.onCompleted: load()
                                Connections {
                                    target: panel
                                    function onFormRevisionChanged() { hostsInput.load(); }
                                }

                                Repeater {
                                    model: hostsInput.visible ? hostsInput.candidates : []
                                    delegate: RowLayout {
                                        required property var modelData
                                        required property int index
                                        readonly property int order: hostsInput.chosen.indexOf(modelData.id)
                                        Layout.fillWidth: true
                                        CheckBox {
                                            objectName: "choose_" + modelData.id
                                            Layout.fillWidth: true
                                            leftPadding: 0
                                            checked: order >= 0
                                            text: (order >= 0 ? (order + 1) + ". " : "") + modelData.name
                                            onToggled: {
                                                var list = hostsInput.chosen.slice();
                                                if (checked)
                                                    list.push(modelData.id);
                                                else
                                                    list.splice(list.indexOf(modelData.id), 1);
                                                hostsInput.store(list);
                                            }
                                        }
                                        DialogButton {
                                            visible: order > 0
                                            text: "Earlier"
                                            onClicked: {
                                                var list = hostsInput.chosen.slice();
                                                list.splice(order, 1);
                                                list.splice(order - 1, 0, modelData.id);
                                                hostsInput.store(list);
                                            }
                                        }
                                    }
                                }
                            }

                            Label {
                                visible: fieldColumn.field.help !== ""
                                Layout.fillWidth: true
                                text: fieldColumn.field.help
                                color: "#7a7a80"
                                font.pixelSize: 11
                                wrapMode: Text.Wrap
                            }
                        }
                    }

                    RowLayout {
                        visible: panel.authorizeLabel !== ""
                        Layout.fillWidth: true
                        Layout.topMargin: 10
                        spacing: 10
                        DialogButton {
                            text: panel.hosts && panel.hosts.authorizing ? "Cancel sign-in" : panel.authorizeLabel
                            onClicked: {
                                if (panel.hosts.authorizing) {
                                    panel.hosts.cancelAuthorize();
                                    return;
                                }
                                panel.authorizeMessage = "";
                                panel.hosts.authorize(panel.editId, panel.editProvider, panel.editValues);
                            }
                        }
                        Label {
                            Layout.fillWidth: true
                            text: panel.authorizeMessage
                            color: "#d6d6da"
                            font.pixelSize: 12
                            wrapMode: Text.Wrap
                        }
                    }

                    Label {
                        visible: panel.formError !== ""
                        Layout.fillWidth: true
                        Layout.topMargin: 8
                        text: panel.formError
                        color: Material.accent
                        font.pixelSize: 13
                        wrapMode: Text.Wrap
                    }

                    RowLayout {
                        Layout.fillWidth: true
                        Layout.topMargin: 12
                        Item { Layout.fillWidth: true }
                        DialogButton {
                            text: "Cancel"
                            onClicked: panel.back()
                        }
                        DialogButton {
                            text: "Save"
                            primary: true
                            enabled: !(panel.hosts && panel.hosts.authorizing)
                            onClicked: panel.save()
                        }
                    }
                }
            }
        }
    }
}

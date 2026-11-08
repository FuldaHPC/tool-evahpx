import QtQuick
import QtQuick.Controls

// What the worker threads of HPX did: one row per worker thread (and locality), a bar for every time a task ran.
// The colour is the name of the task. Wheel with Ctrl or pinch: zoom, wheel: scroll, drag: move, double click: whole run.
// Hover a bar for the task, click it to see where it came from (its parent, cyan) and what it created (its children, yellow).
Item {
    id: root
    property var file: null      // TaskFile
    property var win: null       // the window, for fmtUs()

    // the part of the run that is shown, absolute microseconds
    property real viewA: file ? file.tmin : 0
    property real viewB: file ? file.tmax : 1
    property real scrollY: 0
    property var legend: file && file.loaded ? file.topNames(10) : []
    property string selTask: ""
    property var selSlices: []
    property var selRel: ({})
    property var hoverSlice: ({})
    property real mouseX: 0
    property real mouseY: 0

    readonly property int labelW: 70
    readonly property int axisH: 30
    readonly property int legendH: 80
    readonly property int nRows: file && file.loaded ? file.rowLabels.length : 0
    readonly property real plotW: Math.max(width - labelW - 14, 10)
    readonly property real plotH: Math.max(height - axisH - legendH, 10)
    readonly property real rowH: Math.max(8, Math.min(26, Math.floor(plotH / Math.max(nRows, 1))))
    readonly property real usPerPx: (viewB - viewA) / plotW
    readonly property real fullSpan: file ? Math.max(file.tmax - file.tmin, 1) : 1

    onFileChanged: resetView()
    Connections { target: root.file; function onChanged() { root.resetView(); } }
    onViewAChanged: canvas.requestPaint()
    onViewBChanged: canvas.requestPaint()
    onScrollYChanged: canvas.requestPaint()
    onSelTaskChanged: canvas.requestPaint()
    onWidthChanged: canvas.requestPaint()
    onHeightChanged: canvas.requestPaint()

    function resetView() {
        if (!file || !file.loaded)
            return;
        const pad = fullSpan * 0.01;
        viewA = file.tmin - pad;
        viewB = file.tmax + pad;
        scrollY = 0;
        selTask = "";
        selSlices = [];
        selRel = ({});
        legend = file.topNames(10);
    }
    function clampView() {
        const span = Math.min(viewB - viewA, fullSpan * 1.05);
        let a = viewA;
        a = Math.max(file.tmin - fullSpan * 0.02, Math.min(a, file.tmax + fullSpan * 0.02 - span));
        viewA = a;
        viewB = a + span;
    }
    function zoom(factor, atX) {            // factor < 1 zooms in; atX: x in the plot
        const t = viewA + (atX - labelW) * usPerPx;
        const span = Math.max(2, Math.min((viewB - viewA) * factor, fullSpan * 1.05));
        const f = (atX - labelW) / plotW;
        viewA = t - f * span;
        viewB = viewA + span;
        clampView();
    }
    function maxScroll() { return Math.max(0, nRows * rowH - plotH); }

    function colorOf(idx) {
        if (idx < 0)
            return "#9a9a9a";
        const n = file.names[idx];
        if (n === "<unnamed task>")
            return "#707070";
        if (n === "background_work")
            return "#4a4a4a";
        return Qt.hsla((idx * 0.381966) % 1, 0.6, 0.55, 1);
    }
    function niceStep(range, maxTicks) {
        const raw = range / maxTicks, p = Math.pow(10, Math.floor(Math.log(raw) / Math.LN10)), f = raw / p;
        return (f <= 1 ? 1 : f <= 2 ? 2 : f <= 5 ? 5 : 10) * p;
    }
    function rowAt(y) { return Math.floor((y - axisH + scrollY) / rowH); }
    function xOf(t) { return labelW + (t - viewA) / usPerPx; }
    function yOf(row) { return axisH + row * rowH - scrollY; }
    function fmt(v) { return win ? win.fmtUs(v) : v.toFixed(1) + " us"; }

    function select(task) {
        selTask = task;
        if (task === "") {
            selSlices = [];
            selRel = ({});
            return;
        }
        selSlices = file.slicesOf(task);
        selRel = file.relatives(task);
    }

    // a run without task data: nothing to show here
    Rectangle {
        anchors.fill: parent
        z: 10
        visible: !root.file || !root.file.loaded
        color: "#2b2b2b"
        border.color: "#222222"
        Text {
            anchors.centerIn: parent
            width: Math.min(parent.width - 40, 560)
            horizontalAlignment: Text.AlignHCenter
            wrapMode: Text.WordWrap
            color: "#bbbbbb"; font.pixelSize: 14; lineHeight: 1.2
            text: "No task data for this run.\nThe threads of HPX are only recorded when the program runs with eduhpx-run --tasks (HPX built with install-hpx.sh --with-task-trace)."
        }
    }

    Canvas {
        id: canvas
        anchors.fill: parent
        renderTarget: Canvas.Image

        onPaint: {
            const ctx = getContext("2d");
            ctx.reset();
            ctx.fillStyle = "#2b2b2b";
            ctx.fillRect(0, 0, width, height);
            if (!root.file || !root.file.loaded)
                return;
            ctx.font = "10px sans-serif";
            const first = Math.max(0, root.rowAt(root.axisH)), last = Math.min(root.nRows - 1, root.rowAt(root.axisH + root.plotH));
            const upp = root.usPerPx;
            // the rows
            ctx.save();
            ctx.beginPath();
            ctx.rect(root.labelW, root.axisH, root.plotW, root.plotH);
            ctx.clip();
            for (let r = first; r <= last; ++r) {
                const y = root.yOf(r);
                ctx.fillStyle = r % 2 ? "#303030" : "#2b2b2b";
                ctx.fillRect(root.labelW, y, root.plotW, root.rowH);
                const sp = root.file.spans(r, root.viewA, root.viewB, upp);
                for (let k = 0; k < sp.length; k += 6) {
                    const x0 = root.xOf(sp[k]), w = Math.max(1, (sp[k + 1] - sp[k]) / upp);
                    ctx.fillStyle = root.colorOf(sp[k + 2]);
                    // many small tasks merged: the more of the time they fill, the stronger the colour
                    ctx.globalAlpha = sp[k + 3] > 1 ? 0.3 + 0.7 * sp[k + 4] : 1;
                    ctx.fillRect(x0, y + 1, w, root.rowH - 2);
                }
                ctx.globalAlpha = 1;
            }
            // the task that was clicked, its parent and its children
            if (root.selTask !== "") {
                const mark = (sl, color, width) => {
                    ctx.strokeStyle = color;
                    ctx.lineWidth = width;
                    for (const s of sl)
                        ctx.strokeRect(root.xOf(s.t0) - 1, root.yOf(s.row), Math.max(3, (s.t1 - s.t0) / upp + 2), root.rowH);
                };
                const first = root.selSlices.length > 0 ? root.selSlices[0] : null;
                if (first && root.selRel.parent) {
                    const ps = root.file.slicesOf(root.selRel.parent);
                    mark(ps, "#4fc3f7", 2);
                    if (ps.length > 0) {                      // from the parent to the task
                        ctx.strokeStyle = "#4fc3f7";
                        ctx.lineWidth = 1.5;
                        ctx.beginPath();
                        ctx.moveTo(root.xOf(ps[0].t0), root.yOf(ps[0].row) + root.rowH / 2);
                        ctx.lineTo(root.xOf(first.t0), root.yOf(first.row) + root.rowH / 2);
                        ctx.stroke();
                    }
                }
                mark(root.selSlices, "#ffffff", 2);
                if (first && root.selRel.children) {
                    ctx.lineWidth = 1;
                    for (const c of root.selRel.children) {
                        const cs = root.file.slicesOf(c.task);
                        if (cs.length === 0)
                            continue;
                        mark([cs[0]], "#ffd600", 1.5);
                        ctx.strokeStyle = "rgba(255,214,0,0.7)";
                        ctx.beginPath();
                        ctx.moveTo(root.xOf(first.t1), root.yOf(first.row) + root.rowH / 2);
                        ctx.lineTo(root.xOf(cs[0].t0), root.yOf(cs[0].row) + root.rowH / 2);
                        ctx.stroke();
                    }
                }
            }
            ctx.restore();
            // the labels of the rows
            ctx.fillStyle = "#bbbbbb";
            ctx.textAlign = "left";
            for (let r = first; r <= last; ++r) {
                if (root.rowH >= 11)
                    ctx.fillText(root.file.rowLabels[r], 6, root.yOf(r) + root.rowH / 2 + 3.5);
            }
            // the time axis
            ctx.fillStyle = "#363636";
            ctx.fillRect(0, 0, width, root.axisH);
            const step = root.niceStep(root.viewB - root.viewA, Math.max(4, root.plotW / 90));
            const t0 = root.file.tmin;
            ctx.strokeStyle = "#555555";
            ctx.fillStyle = "#cccccc";
            ctx.textAlign = "center";
            ctx.lineWidth = 1;
            for (let t = Math.ceil((root.viewA - t0) / step) * step; t0 + t <= root.viewB; t += step) {
                const x = root.xOf(t0 + t);
                ctx.beginPath();
                ctx.moveTo(x + 0.5, root.axisH - 6);
                ctx.lineTo(x + 0.5, root.axisH);
                ctx.stroke();
                ctx.fillText(root.fmt(t), x, root.axisH - 12);
            }
        }
    }

    // mouse: hover, click, drag, wheel
    MouseArea {
        id: area
        x: 0; y: 0; width: parent.width; height: root.axisH + root.plotH
        hoverEnabled: true
        property real lastX: 0
        property real lastY: 0
        property bool moved: false
        onPressed: (m) => { lastX = m.x; lastY = m.y; moved = false; }
        onPositionChanged: (m) => {
            root.mouseX = m.x;
            root.mouseY = m.y;
            if (pressed) {
                moved = true;
                const dt = (lastX - m.x) * root.usPerPx;
                root.viewA += dt;
                root.viewB += dt;
                root.clampView();
                root.scrollY = Math.max(0, Math.min(root.scrollY + (lastY - m.y), root.maxScroll()));
                lastX = m.x;
                lastY = m.y;
                return;
            }
            const row = root.rowAt(m.y);
            if (m.x >= root.labelW && m.y >= root.axisH && row >= 0 && row < root.nRows)
                root.hoverSlice = root.file.sliceAt(row, root.viewA + (m.x - root.labelW) * root.usPerPx, root.usPerPx * 3);
            else
                root.hoverSlice = ({});
        }
        onExited: root.hoverSlice = ({})
        onClicked: (m) => {
            if (moved)
                return;
            root.select(root.hoverSlice.task !== undefined ? root.hoverSlice.task : "");
        }
        onDoubleClicked: root.resetView()
        onWheel: (w) => {
            const ctrl = (w.modifiers & (Qt.ControlModifier | Qt.MetaModifier)) !== 0;
            if (ctrl || Math.abs(w.angleDelta.x) > Math.abs(w.angleDelta.y) * 2) {
                if (ctrl)
                    root.zoom(Math.pow(0.9985, w.angleDelta.y !== 0 ? w.angleDelta.y : w.pixelDelta.y * 3), w.x);
                else {
                    const dt = -w.angleDelta.x * root.usPerPx * 0.5;
                    root.viewA += dt;
                    root.viewB += dt;
                    root.clampView();
                }
            } else {
                root.scrollY = Math.max(0, Math.min(root.scrollY - (w.pixelDelta.y !== 0 ? w.pixelDelta.y : w.angleDelta.y * 0.5), root.maxScroll()));
            }
        }
        PinchHandler {
            id: pinch
            target: null
            property real start: 1
            onActiveChanged: if (active) start = root.viewB - root.viewA
            onActiveScaleChanged: if (active) {
                const span = Math.max(2, Math.min(start / Math.max(activeScale, 0.05), root.fullSpan * 1.05));
                const c = (root.viewA + root.viewB) / 2;
                root.viewA = c - span / 2;
                root.viewB = c + span / 2;
                root.clampView();
            }
        }
    }

    // zoom buttons
    Row {
        anchors { right: parent.right; rightMargin: 16; bottom: parent.bottom; bottomMargin: 8 }
        spacing: 4
        z: 3
        Repeater {
            model: [{t: "+", f: 0.5}, {t: "−", f: 2}, {t: "all", f: 0}]
            delegate: Rectangle {
                required property var modelData
                width: modelData.t === "all" ? 34 : 24; height: 22; radius: 4
                color: "#444444"; border.color: "#6a6a6a"
                Text { anchors.centerIn: parent; text: parent.modelData.t; color: "white"; font.pixelSize: 12 }
                MouseArea {
                    anchors.fill: parent
                    cursorShape: Qt.PointingHandCursor
                    onClicked: parent.modelData.f === 0 ? root.resetView() : root.zoom(parent.modelData.f, root.labelW + root.plotW / 2)
                }
            }
        }
    }

    // how to read the picture
    Text {
        x: root.labelW; y: root.axisH + root.plotH + 5
        width: root.plotW
        color: "#bbbbbb"; font.pixelSize: 11; wrapMode: Text.WordWrap
        text: "One row = one worker thread (a core that runs HPX tasks), time runs to the right. A bar = a task that ran there, the colour is its name (below). "
            + "Empty = the thread was idle. Scroll = zoom, drag = move, hover = details, click a bar = its parent (cyan) and children (yellow)."
    }

    // the names of the tasks that take the most time
    Flow {
        x: root.labelW; y: root.axisH + root.plotH + 40
        width: root.plotW - 110
        height: root.legendH - 42
        clip: true
        spacing: 12
        Repeater {
            model: root.legend
            delegate: Row {
                required property var modelData
                spacing: 5
                Rectangle { width: 14; height: 10; radius: 2; color: root.colorOf(modelData.index); anchors.verticalCenter: parent.verticalCenter }
                Text {
                    color: "#dddddd"; font.pixelSize: 11
                    text: modelData.name + "  " + root.fmt(modelData.total) + " (" + modelData.count + ")"
                    elide: Text.ElideRight
                    width: Math.min(implicitWidth, 300)
                }
            }
        }
    }

    // what is under the mouse, and the task that was clicked: a solid card
    Rectangle {
        id: card
        readonly property bool hovering: root.hoverSlice.task !== undefined
        visible: hovering || root.selTask !== ""
        z: 4
        x: hovering ? (root.mouseX + 18 + width > root.width - 8 ? root.mouseX - 18 - width : root.mouseX + 18) : root.labelW + 8
        y: hovering ? (root.mouseY + 18 + height > root.height - 8 ? root.mouseY - 18 - height : root.mouseY + 18) : root.axisH + 8
        width: cardText.width + 24
        height: cardText.height + 18
        radius: 8
        color: "#262626"
        border.color: "#555555"
        Text {
            id: cardText
            x: 12; y: 9
            color: "white"; font.pixelSize: 13; lineHeight: 1.15
            text: {
                const s = card.hovering ? root.hoverSlice : null;
                if (s) {
                    const wait = s.created >= 0 ? "\nwaited " + root.fmt(Math.max(0, s.t0 - s.created)) + " after it was created" : "";
                    return s.name + "\nruns " + root.fmt(s.t1 - s.t0) + ", ends: " + s.end + "\nat +" + root.fmt(s.t0 - root.file.tmin)
                        + wait + (s.parent ? "\nmade by " + (root.file.nameOfTask(s.parent) || "?") : "")
                        + (s.parent ? "\n(click: its parent and children)" : "");
                }
                if (root.selTask === "" || root.selSlices.length === 0)
                    return "";
                const first = root.selSlices[0];
                return "selected: " + root.file.nameOfTask(root.selTask) + "\n" + root.selSlices.length + (root.selSlices.length === 1 ? " slice" : " slices")
                    + ", first at +" + root.fmt(first.t0 - root.file.tmin)
                    + "\nmade by " + (root.selRel.parent ? (root.selRel.parentName || "?") + "  (cyan)" : "nobody (a root task)")
                    + "\ncreated " + root.selRel.childCount + (root.selRel.childCount === 1 ? " task" : " tasks") + (root.selRel.childCount > 0 ? "  (yellow)" : "")
                    + "\n(click an empty place: unselect)";
            }
        }
    }
}

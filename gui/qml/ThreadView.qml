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
    property bool showChain: false        // the chain of tasks that decided how long the run took
    property var chain: ({})
    property var findings: []             // what the data say about idle threads and why (TaskFile.findings())
    property bool showFindings: false
    property var phases: []                // the named and the unnamed stretches of the run (TaskFile.phases())
    property string scopeKind: "all"       // what the findings are about: "all" (whole run), "phase", "view" (the part that is shown)
    property int scopeIndex: -1
    readonly property real scopeA: scopeKind === "phase" && scopeIndex >= 0 && scopeIndex < phases.length ? phases[scopeIndex].from : scopeKind === "view" ? viewA : (file ? file.tmin : 0)
    readonly property real scopeB: scopeKind === "phase" && scopeIndex >= 0 && scopeIndex < phases.length ? phases[scopeIndex].to : scopeKind === "view" ? viewB : (file ? file.tmax : 1)
    readonly property var scopes: {
        const l = [{label: "Whole run", kind: "all", idx: -1}];
        for (let i = 0; i < phases.length; ++i)
            l.push({label: (phases[i].named ? "named " : "unnamed ") + "+" + fmt(phases[i].from - file.tmin) + " – +" + fmt(phases[i].to - file.tmin), kind: "phase", idx: i});
        l.push({label: "Visible part", kind: "view", idx: -1});
        return l;
    }
    property string panelMode: "findings"   // what the panel shows: "findings", "names" (a table of the task names) or "task" (the task that was clicked)
    property var nameStats: []
    property int hiName: -1                  // the name that is marked in the rows (the others get dim), -1: none
    property string sortKey: "total"
    property bool sortDesc: true
    property string histKind: "run"          // what the histogram shows: "run", "wait" or "pause"
    property var taskDetail: ({})
    readonly property var sortedStats: {
        const l = nameStats.slice(), d = sortDesc ? -1 : 1, k = sortKey;
        l.sort((a, b) => k === "name" ? d * a.name.localeCompare(b.name) : d * ((a[k] || 0) - (b[k] || 0)));
        return l;
    }
    property int openFinding: -1           // the finding whose details are open
    readonly property real panelW: !showFindings ? 0 : panelMode === "names" ? Math.min(600, Math.max(440, width * 0.42)) : Math.min(440, Math.max(300, width * 0.3))
    property bool showInternal: false     // the tasks of HPX itself (timers, AGAS, start and stop)
    property string selTask: ""
    property var selSlices: []
    property var selRel: ({})
    property var hoverSlice: ({})
    property real mouseX: 0
    property real mouseY: 0

    readonly property int labelW: 70
    readonly property int stripH: 40          // how busy the threads were, over time
    readonly property int axisH: 30 + stripH  // the time axis and the strip: the rows start below
    readonly property int legendH: showChain ? 140 : 104
    readonly property int nRows: file && file.loaded ? file.rowLabels.length : 0
    readonly property real plotW: Math.max(width - labelW - 14 - (showFindings ? panelW + 8 : 0), 10)
    readonly property real plotH: Math.max(height - axisH - legendH, 10)
    readonly property real rowH: Math.max(8, Math.min(26, Math.floor(plotH / Math.max(nRows, 1))))
    readonly property real usPerPx: (viewB - viewA) / plotW
    readonly property real fullSpan: file ? Math.max(file.tmax - file.tmin, 1) : 1

    onFileChanged: { resetView(); fileLoaded(); }
    Connections {
        target: root.file
        function onChanged() { root.resetView(); root.fileLoaded(); }
        function onFilterChanged() { root.legend = root.file.topNames(10); root.updateChain(); root.updateFindings(); canvas.requestPaint(); }
    }
    onShowFindingsChanged: { updateFindings(); canvas.requestPaint(); }
    // a run was loaded: the findings are about the whole run (a program often measures several parts: the phases are chips to look at one)
    function fileLoaded() {
        phases = file && file.loaded ? file.phases() : [];
        scopeKind = "all";
        scopeIndex = -1;
        showFindings = file && file.loaded && file.sliceCount < 2000000;
        updateFindings();
    }
    function updateFindings() {
        findings = showFindings && file && file.loaded && panelMode === "findings" ? file.findingsIn(scopeA, scopeB) : findings;
        nameStats = showFindings && file && file.loaded && panelMode === "names" ? file.nameStats(scopeA, scopeB) : nameStats;
        openFinding = -1;
        canvas.requestPaint();
        hist.requestPaint();
    }
    onPanelModeChanged: {
        if (panelMode === "findings")
            findings = file && file.loaded ? file.findingsIn(scopeA, scopeB) : [];
        else if (panelMode === "names")
            nameStats = file && file.loaded ? file.nameStats(scopeA, scopeB) : [];
        canvas.requestPaint();
        hist.requestPaint();
    }
    onHiNameChanged: { canvas.requestPaint(); hist.requestPaint(); }
    onHistKindChanged: hist.requestPaint()
    function setScope(kind, idx) {
        scopeKind = kind;
        scopeIndex = idx;
        if (kind === "phase") {                 // look at the phase
            const p = phases[idx], pad = (p.to - p.from) * 0.02;
            viewA = p.from - pad;
            viewB = p.to + pad;
            clampView();
        }
        updateFindings();
    }
    Timer { id: viewTimer; interval: 300; onTriggered: root.updateFindings() }
    // look at the part of the run that a finding is about
    function showFinding(f) {
        if (f.kind === "chain") {
            showChain = true;
            return;
        }
        const pad = Math.max((f.to - f.from) * 0.05, 10);
        viewA = f.from - pad;
        viewB = f.to + pad;
        clampView();
    }
    onShowInternalChanged: if (file) file.hideInternal = !showInternal
    onShowChainChanged: updateChain()
    function updateChain() {
        chain = showChain && file && file.loaded ? file.criticalChain() : ({});
        if (chain.count) {                  // look at the chain
            const pad = Math.max((chain.end - chain.start) * 0.04, 10);
            viewA = chain.start - pad;
            viewB = chain.end + pad;
            clampView();
        }
        canvas.requestPaint();
    }
    onViewAChanged: { canvas.requestPaint(); if (scopeKind === "view" && showFindings) viewTimer.restart(); }
    onViewBChanged: { canvas.requestPaint(); if (scopeKind === "view" && showFindings) viewTimer.restart(); }
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
        updateChain();
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
    function short(v) {       // three digits: 47.0 us, 6.31 ms, 32.7 s
        return v >= 1e6 ? (v / 1e6).toPrecision(3) + " s" : v >= 1e3 ? (v / 1e3).toPrecision(3) + " ms" : v.toPrecision(3) + " us";
    }
    function fmt(v) { return win ? win.fmtUs(v) : v.toFixed(1) + " us"; }

    function select(task) {
        selTask = task;
        taskDetail = task === "" || !file ? ({}) : file.taskInfo(task);
        if (task !== "" && showFindings)
            panelMode = "task";
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
                    ctx.globalAlpha = (sp[k + 3] > 1 ? 0.3 + 0.7 * sp[k + 4] : 1) * (root.hiName >= 0 && sp[k + 2] !== root.hiName ? (sp[k + 5] ? 0.5 : 0.15) : 1);
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
            // the chain that decided the run time: its slices, and a line from one to the next
            if (root.chain.slices && root.chain.slices.length > 0) {
                const cs = root.chain.slices;
                ctx.strokeStyle = "#ff9800";
                ctx.lineWidth = 1.5;
                ctx.beginPath();
                for (let k = 0; k < cs.length; k += 3) {
                    const y = root.yOf(cs[k]) + root.rowH / 2;
                    k === 0 ? ctx.moveTo(root.xOf(cs[k + 1]), y) : ctx.lineTo(root.xOf(cs[k + 1]), y);
                    ctx.lineTo(root.xOf(cs[k + 2]), y);
                }
                ctx.stroke();
                ctx.lineWidth = 2;
                for (let k = 0; k < cs.length; k += 3)
                    ctx.strokeRect(root.xOf(cs[k + 1]) - 0.5, root.yOf(cs[k]), Math.max(2, (cs[k + 2] - cs[k + 1]) / upp + 1), root.rowH);
                // before every task of the chain: yellow = it was not made yet, red = it was made and waited for a thread
                const lk = root.chain.links || [];
                for (let k = 0; k + 3 < lk.length; k += 4) {
                    if (lk[k + 1] < 0 || lk[k + 2] <= lk[k])
                        continue;
                    const m = Math.min(Math.max(lk[k + 1], lk[k]), lk[k + 2]), y = root.yOf(lk[k + 3]) + root.rowH * 0.3;
                    ctx.fillStyle = "rgba(255,214,0,0.9)";
                    ctx.fillRect(root.xOf(lk[k]), y, Math.max(1, (m - lk[k]) / upp), root.rowH * 0.4);
                    ctx.fillStyle = "rgba(255,70,70,0.95)";
                    ctx.fillRect(root.xOf(m), y, Math.max(1, (lk[k + 2] - m) / upp), root.rowH * 0.4);
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
                ctx.moveTo(x + 0.5, 24);
                ctx.lineTo(x + 0.5, 30);
                ctx.stroke();
                ctx.fillText(root.fmt(t), x, 18);
            }
            // how busy the threads were: the share of the worker threads that ran a task, over time (orange: less than 70 %)
            ctx.fillStyle = "#2f2f2f";
            ctx.fillRect(0, 30, width, root.stripH);
            const bins = Math.max(10, Math.floor(root.plotW / 3));
            let util = [];
            let nloc = 0;
            for (let l = 0; l < root.file.localities; ++l)
                if (root.file.hasLocality(l)) {
                    const u = root.file.utilization(l, root.viewA, root.viewB, bins);
                    for (let k = 0; k < u.length; ++k)
                        util[k] = (util[k] || 0) + u[k];
                    ++nloc;
                }
            const sh = root.stripH - 8, sy = 30 + root.stripH - 3;
            for (let k = 0; k < util.length; ++k) {
                const v = util[k] / Math.max(nloc, 1);
                ctx.fillStyle = v < 0.7 ? "#ff9800" : "#4f6fa8";
                ctx.fillRect(root.labelW + k * root.plotW / bins, sy - v * sh, Math.ceil(root.plotW / bins), v * sh);
            }
            ctx.strokeStyle = "#666666";
            ctx.setLineDash([3, 3]);
            ctx.beginPath();
            ctx.moveTo(root.labelW, sy - sh + 0.5);
            ctx.lineTo(root.labelW + root.plotW, sy - sh + 0.5);
            ctx.stroke();
            ctx.setLineDash([]);
            ctx.fillStyle = "#aaaaaa";
            ctx.textAlign = "left";
            ctx.fillText("threads busy", 6, 30 + 33);
            ctx.fillText("100 % ┄", root.labelW - 44, sy - sh + 4);
            if (root.showFindings && root.scopeKind !== "view") {          // the part of the run that the findings are about
                const xa = Math.max(root.labelW, root.xOf(root.scopeA)), xb = Math.min(root.labelW + root.plotW, root.xOf(root.scopeB));
                if (xb > xa) {
                    ctx.fillStyle = "#3fae80";
                    ctx.fillRect(xa, 0, xb - xa, 3);
                }
            }
        }
    }

    // mouse: hover, click, drag, wheel
    MouseArea {
        id: area
        x: 0; y: 0; width: parent.width - (root.showFindings ? root.panelW + 8 : 0); height: root.axisH + root.plotH
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

    // what is shown: the tasks of HPX itself, the chain that decided the run time
    Row {
        anchors { right: parent.right; rightMargin: 116; bottom: parent.bottom; bottomMargin: 8 }
        spacing: 6
        z: 3
        Repeater {
            model: [{t: "Findings", k: "f"}, {t: "HPX internals", k: "i"}, {t: "Critical chain", k: "c"}]
            delegate: Rectangle {
                required property var modelData
                readonly property bool on: modelData.k === "i" ? root.showInternal : modelData.k === "f" ? root.showFindings : root.showChain
                width: chipText.implicitWidth + 20; height: 22; radius: 11
                color: on ? (modelData.k === "c" ? "#b26a00" : modelData.k === "f" ? "#2e7d5b" : "#4a6a8a") : "#444444"
                border.color: on ? "#ffffff" : "#6a6a6a"
                Text { id: chipText; anchors.centerIn: parent; text: parent.modelData.t; color: "white"; font.pixelSize: 12 }
                MouseArea {
                    anchors.fill: parent
                    cursorShape: Qt.PointingHandCursor
                    onClicked: parent.modelData.k === "i" ? root.showInternal = !root.showInternal : parent.modelData.k === "f" ? root.showFindings = !root.showFindings : root.showChain = !root.showChain
                }
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

    // the chain that decided the run time, in words
    Text {
        x: root.labelW; y: root.axisH + root.plotH + 4
        width: root.plotW
        visible: root.showChain
        color: "#ffb74d"; font.pixelSize: 12; font.bold: true; wrapMode: Text.WordWrap
        text: {
            const c = root.chain;
            if (!c.count)
                return "Critical chain: none. No tasks of this run start one another in a chain (the tasks of HPX itself and the driver hpx_main are not counted).";
            const parts = (c.byName || []).map(n => n.name + " " + root.fmt(n.us));
            return "Critical chain: " + c.count + " tasks, each made by the one before. From its first to its last task " + root.fmt(c.spanUs)
                + " pass, the tasks of the chain run " + root.fmt(c.runUs) + " (" + Math.round(100 * c.runUs / Math.max(1, c.spanUs))
                + " %), the rest is waiting. Most time: " + parts.join(", ")
                + ". Yellow strip = the task was not made yet, red = made, waiting for a thread.";
        }
    }

    // how to read the picture
    Text {
        x: root.labelW; y: root.axisH + root.plotH + (root.showChain ? 70 : 5)
        width: root.plotW
        color: "#bbbbbb"; font.pixelSize: 11; wrapMode: Text.WordWrap
        text: "One row = one worker thread (a core that runs HPX tasks), time runs to the right. A bar = a task that ran there, the colour is its name (below). "
            + "Empty = the thread was idle. Scroll = zoom, drag = move, hover = details, click a bar = its parent (cyan) and children (yellow)."
    }

    // the names of the tasks that take the most time
    Flow {
        x: root.labelW; y: root.axisH + root.plotH + (root.showChain ? 104 : 40)
        width: root.plotW - 300
        height: root.legendH - (root.showChain ? 106 : 42)
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

    // what the data say: parts of the run with idle threads and what is behind them, click one to look at it
    Rectangle {
        visible: root.showFindings && root.file && root.file.loaded
        z: 5
        x: root.width - root.panelW - 8; y: root.axisH
        width: root.panelW; height: root.plotH
        color: "#262626"; border.color: "#555555"; radius: 6
        clip: true
        Row {
            id: tabs
            x: 12; y: 8
            spacing: 6
            Repeater {
                model: [{t: root.findings.length > 0 ? "Findings (" + root.findings.length + ")" : "Findings", k: "findings"}, {t: "Names", k: "names"}, {t: "Task", k: "task"}]
                delegate: Rectangle {
                    required property var modelData
                    readonly property bool on: root.panelMode === modelData.k
                    width: tabText.implicitWidth + 20; height: 24; radius: 12
                    color: on ? "#2e7d5b" : "#444444"
                    border.color: on ? "#ffffff" : "#6a6a6a"
                    Text { id: tabText; anchors.centerIn: parent; text: parent.modelData.t; color: "white"; font.pixelSize: 12; font.bold: parent.on }
                    MouseArea {
                        anchors.fill: parent
                        cursorShape: Qt.PointingHandCursor
                        onClicked: root.panelMode = parent.modelData.k
                    }
                }
            }
        }
        readonly property real contentTop: root.panelMode === "task" ? tabs.y + tabs.height + 8 : scopeRow.y + scopeRow.height + 6
        Flow {
            id: scopeRow
            visible: root.panelMode !== "task"
            x: 12; y: 40; width: parent.width - 24
            spacing: 5
            Repeater {
                model: root.scopes
                delegate: Rectangle {
                    required property var modelData
                    readonly property bool on: root.scopeKind === modelData.kind && root.scopeIndex === modelData.idx
                    width: scopeText.implicitWidth + 16; height: 20; radius: 10
                    color: on ? "#2e7d5b" : "#444444"
                    border.color: on ? "#ffffff" : "#6a6a6a"
                    Text { id: scopeText; anchors.centerIn: parent; text: parent.modelData.label; color: "white"; font.pixelSize: 11 }
                    MouseArea {
                        anchors.fill: parent
                        cursorShape: Qt.PointingHandCursor
                        onClicked: root.setScope(parent.modelData.kind, parent.modelData.idx)
                    }
                }
            }
        }
        Text {
            visible: root.panelMode === "findings" && root.findings.length === 0
            x: 12; y: scopeRow.y + scopeRow.height + 8; width: parent.width - 24
            color: "#bbbbbb"; font.pixelSize: 12; wrapMode: Text.WordWrap
            text: "Nothing stands out: no longer part of the run with many idle threads, no very small tasks, and the critical chain is not waiting. (Only what the task data show is checked.)"
        }
        ListView {
            visible: root.panelMode === "findings"
            x: 6; y: scopeRow.y + scopeRow.height + 6; width: parent.width - 12; height: parent.height - y - 6
            spacing: 6
            clip: true
            model: root.findings
            boundsBehavior: Flickable.StopAtBounds
            ScrollBar.vertical: ScrollBar {}
            delegate: Rectangle {
                id: item
                required property var modelData
                required property int index
                readonly property bool open: root.openFinding === index
                width: ListView.view.width - 10
                height: body.implicitHeight + 16
                radius: 6
                color: hit.containsMouse ? "#383838" : "#303030"
                border.color: modelData.level === "warn" ? "#b26a00" : "#4a6a8a"
                Column {
                    id: body
                    x: 8; y: 8; width: parent.width - 16
                    spacing: 4
                    Text {
                        width: parent.width; wrapMode: Text.WordWrap
                        color: item.modelData.level === "warn" ? "#ffb74d" : "#90caf9"; font.pixelSize: 12; font.bold: true
                        text: item.modelData.title
                    }
                    Text {
                        width: parent.width; wrapMode: Text.WordWrap
                        color: "#ffffff"; font.pixelSize: 12
                        text: item.modelData.brief
                    }
                    Text {
                        visible: item.open
                        width: parent.width; wrapMode: Text.WordWrap
                        color: "#cccccc"; font.pixelSize: 11; lineHeight: 1.1
                        text: item.modelData.text
                    }
                    Text {
                        color: "#8a8a8a"; font.pixelSize: 10
                        text: item.open ? "click: close the details" : "click: show this part and the details"
                    }
                }
                MouseArea {
                    id: hit
                    anchors.fill: parent
                    hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    onClicked: { root.openFinding = item.open ? -1 : item.index; root.showFinding(item.modelData); }
                }
            }
        }

        // the table of the task names in the part of the run: click a name to mark it in the rows, click a head to sort
        Item {
            id: namesPage
            visible: root.panelMode === "names"
            x: 6; y: parent.contentTop; width: parent.width - 12; height: parent.height - y - 6
            readonly property var cols: [{k: "count", t: "n", w: 52}, {k: "total", t: "total", w: 68}, {k: "p50", t: "median", w: 64}, {k: "p90", t: "p90", w: 64}, {k: "waitP50", t: "wait", w: 64}, {k: "pauses", t: "pauses", w: 54}]
            readonly property real nameW: Math.max(90, width - 12 - cols.reduce((a, c) => a + c.w, 0))
            Row {
                id: head
                x: 6; height: 20
                Text {
                    width: namesPage.nameW; height: parent.height; verticalAlignment: Text.AlignVCenter
                    color: root.sortKey === "name" ? "#ffffff" : "#aaaaaa"; font.pixelSize: 11; font.bold: true
                    text: "name" + (root.sortKey === "name" ? (root.sortDesc ? " ▼" : " ▲") : "")
                    MouseArea { anchors.fill: parent; cursorShape: Qt.PointingHandCursor; onClicked: { root.sortDesc = root.sortKey === "name" ? !root.sortDesc : false; root.sortKey = "name"; } }
                }
                Repeater {
                    model: namesPage.cols
                    delegate: Text {
                        required property var modelData
                        width: modelData.w; height: 20; horizontalAlignment: Text.AlignRight; verticalAlignment: Text.AlignVCenter
                        color: root.sortKey === modelData.k ? "#ffffff" : "#aaaaaa"; font.pixelSize: 11; font.bold: true
                        text: modelData.t + (root.sortKey === modelData.k ? (root.sortDesc ? " ▼" : " ▲") : "")
                        MouseArea { anchors.fill: parent; cursorShape: Qt.PointingHandCursor; onClicked: { root.sortDesc = root.sortKey === parent.modelData.k ? !root.sortDesc : true; root.sortKey = parent.modelData.k; } }
                    }
                }
            }
            ListView {
                x: 0; y: head.height + 2; width: parent.width; height: parent.height - y - (root.hiName >= 0 ? 176 : 0)
                clip: true
                model: root.sortedStats
                boundsBehavior: Flickable.StopAtBounds
                ScrollBar.vertical: ScrollBar {}
                delegate: Rectangle {
                    id: nrow
                    required property var modelData
                    width: ListView.view.width - 10; height: 22
                    color: root.hiName === modelData.index ? "#3b4d68" : (nhit.containsMouse ? "#383838" : "transparent")
                    Row {
                        x: 6; anchors.verticalCenter: parent.verticalCenter
                        Item {
                            width: namesPage.nameW; height: 22
                            Rectangle { id: sw; x: 0; y: 6; width: 10; height: 10; radius: 2; color: root.colorOf(nrow.modelData.index) }
                            Text { x: 16; width: parent.width - 18; height: 22; verticalAlignment: Text.AlignVCenter; elide: Text.ElideRight; color: "#eeeeee"; font.pixelSize: 11; text: nrow.modelData.name }
                        }
                        Repeater {
                            model: namesPage.cols
                            delegate: Text {
                                required property var modelData
                                width: modelData.w; height: 22; horizontalAlignment: Text.AlignRight; verticalAlignment: Text.AlignVCenter
                                color: "#cccccc"; font.pixelSize: 11
                                text: {
                                    const v = nrow.modelData[modelData.k];
                                    return modelData.k === "count" || modelData.k === "pauses" ? String(v) : root.short(v);
                                }
                            }
                        }
                    }
                    MouseArea {
                        id: nhit
                        anchors.fill: parent
                        hoverEnabled: true
                        cursorShape: Qt.PointingHandCursor
                        onClicked: root.hiName = root.hiName === modelData.index ? -1 : modelData.index
                    }
                }
            }
            // how the durations of the marked name are spread (bar height: log of the count)
            Item {
                visible: root.hiName >= 0
                x: 0; y: parent.height - 170; width: parent.width; height: 170
                Row {
                    x: 6; y: 2; spacing: 5
                    Repeater {
                        model: [{k: "run", t: "run time"}, {k: "wait", t: "wait for a thread"}, {k: "pause", t: "pauses"}]
                        delegate: Rectangle {
                            required property var modelData
                            readonly property bool on: root.histKind === modelData.k
                            width: ht.implicitWidth + 14; height: 20; radius: 10
                            color: on ? "#2e7d5b" : "#444444"; border.color: on ? "#ffffff" : "#6a6a6a"
                            Text { id: ht; anchors.centerIn: parent; text: parent.modelData.t; color: "white"; font.pixelSize: 11 }
                            MouseArea { anchors.fill: parent; cursorShape: Qt.PointingHandCursor; onClicked: root.histKind = parent.modelData.k }
                        }
                    }
                }
                Text {
                    x: 6; y: 26; width: parent.width - 12; wrapMode: Text.WordWrap
                    color: "#bbbbbb"; font.pixelSize: 11
                    text: {
                        const l = root.nameStats.filter(n => n.index === root.hiName);
                        if (l.length === 0)
                            return "";
                        const n = l[0];
                        if (root.histKind === "run")
                            return n.name + ": " + n.count + " slices, median " + root.fmt(n.p50) + ", p90 " + root.fmt(n.p90) + ", longest " + root.fmt(n.max);
                        if (root.histKind === "wait")
                            return n.name + ": from made to started: median " + root.fmt(n.waitP50) + ", p90 " + root.fmt(n.waitP90);
                        return n.pauses === 0 ? n.name + ": never paused" : n.name + ": " + n.pauses + " pauses, median " + root.fmt(n.pauseP50) + ", longest " + root.fmt(n.pauseMax) + ", " + n.moved + " went on on another thread";
                    }
                }
                Canvas {
                    id: hist
                    x: 0; y: 56; width: parent.width; height: parent.height - 56
                    renderTarget: Canvas.Image
                    onPaint: {
                        const ctx = getContext("2d");
                        ctx.reset();
                        ctx.fillStyle = "#262626";
                        ctx.fillRect(0, 0, width, height);
                        if (root.hiName < 0 || !root.file || !root.file.loaded)
                            return;
                        const c = root.file.durationHistogram(root.hiName, root.scopeA, root.scopeB, root.histKind);
                        let mx = 1;
                        for (const v of c)
                            mx = Math.max(mx, v);
                        const bw = (width - 12) / c.length, base = height - 18, full = base - 4;
                        ctx.fillStyle = root.colorOf(root.hiName);
                        for (let k = 0; k < c.length; ++k)
                            if (c[k] > 0)
                                ctx.fillRect(6 + k * bw, base - Math.max(2, full * Math.log(1 + c[k]) / Math.log(1 + mx)), bw - 1, Math.max(2, full * Math.log(1 + c[k]) / Math.log(1 + mx)));
                        ctx.strokeStyle = "#666666";
                        ctx.beginPath();
                        ctx.moveTo(6, base + 0.5);
                        ctx.lineTo(width - 6, base + 0.5);
                        ctx.stroke();
                        ctx.fillStyle = "#aaaaaa";
                        ctx.font = "10px sans-serif";
                        ctx.textAlign = "center";
                        const labels = ["1 us", "10 us", "100 us", "1 ms", "10 ms", "100 ms", "1 s"];
                        for (let d = 0; d < labels.length; ++d)
                            ctx.fillText(labels[d], Math.min(width - 22, Math.max(18, 6 + d * 4 * bw)), height - 4);
                    }
                }
            }
        }

        // the task that was clicked: its life
        Flickable {
            id: taskPage
            visible: root.panelMode === "task"
            x: 6; y: parent.contentTop; width: parent.width - 12; height: parent.height - y - 6
            contentHeight: taskCol.height + 12
            clip: true
            boundsBehavior: Flickable.StopAtBounds
            ScrollBar.vertical: ScrollBar {}
            Column {
                id: taskCol
                x: 6; width: taskPage.width - 22
                spacing: 6
                Text {
                    visible: root.taskDetail.name === undefined
                    width: parent.width; wrapMode: Text.WordWrap; color: "#bbbbbb"; font.pixelSize: 12; lineHeight: 1.1
                    text: "Click a bar in the rows to see one task: when it was made, how long it waited for a thread, how long it ran, where it was paused and on which thread it went on."
                }
                Text {
                    visible: root.taskDetail.name !== undefined
                    width: parent.width; wrapMode: Text.WordWrap; color: root.colorOf(root.taskDetail.nameIndex !== undefined ? root.taskDetail.nameIndex : -1); font.pixelSize: 14; font.bold: true
                    text: root.taskDetail.name !== undefined ? root.taskDetail.name : ""
                }
                Text {
                    visible: root.taskDetail.name !== undefined
                    width: parent.width; wrapMode: Text.WordWrap; color: "#dddddd"; font.pixelSize: 12; lineHeight: 1.2
                    text: {
                        const t = root.taskDetail;
                        if (t.name === undefined)
                            return "";
                        const rel = v => "+" + root.fmt(v - root.file.tmin);
                        let s = "made " + (t.created >= 0 ? rel(t.created) : "(not known)") + ", began " + rel(t.start) + (t.wait >= 0 ? " (waited " + root.fmt(t.wait) + " for a thread)" : "") + "\n"
                            + "ran " + root.fmt(t.run) + " in " + t.slices + (t.slices === 1 ? " slice" : " slices") + ", ended " + rel(t.end) + "\n";
                        s += t.pauses.length === 0 ? "never paused" : "paused " + t.pauses.length + (t.pauses.length === 1 ? " time" : " times") + ", " + root.fmt(t.pauseTotal) + " in all, went on on another thread " + t.moved + " times";
                        s += "\nmade by " + (t.parent ? t.parentName : "nobody (a root task)") + ", made " + t.childCount + (t.childCount === 1 ? " task" : " tasks");
                        return s;
                    }
                }
                Repeater {
                    model: root.taskDetail.pauses !== undefined ? root.taskDetail.pauses : []
                    delegate: Rectangle {
                        id: prow
                        required property var modelData
                        width: taskCol.width; height: ptext.implicitHeight + 12; radius: 5
                        color: phit.containsMouse ? "#383838" : "#303030"; border.color: "#555555"
                        Text {
                            id: ptext
                            x: 8; y: 6; width: parent.width - 16; wrapMode: Text.WordWrap; color: "#dddddd"; font.pixelSize: 11
                            text: {
                                const m = prow.modelData, lab = r => root.file.rowLabels[r] || "?";
                                let s = m.why + " for " + root.fmt(m.dur) + " at +" + root.fmt(m.from - root.file.tmin) + ": " + lab(m.rowFrom) + (m.rowFrom === m.rowTo ? ", went on there" : " → " + lab(m.rowTo));
                                if (m.wake)
                                    s += "\nwoken by a value set on " + lab(m.wake.from);
                                return s;
                            }
                        }
                        MouseArea {
                            id: phit
                            anchors.fill: parent
                            hoverEnabled: true
                            cursorShape: Qt.PointingHandCursor
                            onClicked: {          // look at the pause
                                const pad = Math.max(prow.modelData.dur * 2, 500);
                                root.viewA = prow.modelData.from - pad;
                                root.viewB = prow.modelData.to + pad;
                                root.clampView();
                            }
                        }
                    }
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

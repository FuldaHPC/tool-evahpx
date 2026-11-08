import QtQuick
import QtQuick.Controls

// Who exchanged data with whom in the current group, between and inside localities.
// Off the diagonal: the parcels from the locality of the row to the locality of the column (blue, the more bytes the stronger).
// On the diagonal: the values that tasks handed to other tasks inside that locality (green, the more the stronger), from the task
// data of the run. A click on a diagonal cell shows who handed values to whom among the worker threads of that locality.
Item {
    id: root
    property var win: null       // the window: rows of the group, fmtSize()
    property var file: null      // TaskFile (can be empty)

    property var locs: []
    property var parcels: ({})   // "src,dst" -> {n, bytes}
    property var hand: ({})      // loc -> {n, cross}
    property real maxBytes: 1
    property real maxHand: 1
    property int selLoc: -1
    property var detail: ({})    // the matrix of the worker threads of selLoc
    property int hoverI: -1
    property int hoverJ: -1
    property real mouseX: 0
    property real mouseY: 0

    readonly property bool hasTasks: file !== null && file.loaded
    // the time of the group; without parcels (a run of the threads only) the whole run
    readonly property real gStart: win && win.currentGroup ? win.currentGroup.start : (hasTasks ? file.tmin : 0)
    readonly property real gEnd: win && win.currentGroup ? win.currentGroup.end : (hasTasks ? file.tmax : 0)
    readonly property int leftM: 78
    readonly property int topM: 64
    readonly property int bottomM: 70
    readonly property real mainW: selLoc >= 0 ? width * 0.52 : width
    readonly property real cell: Math.max(6, Math.min(48, Math.min(mainW - leftM - 24, height - topM - bottomM) / Math.max(locs.length, 1)))

    function rebuild() {
        if (!win)
            return;
        const set = {};
        for (const l of win.laneInfo)
            set[l.loc] = true;
        if (hasTasks && win.laneInfo.length === 0)          // no parcels: the localities of the task data
            for (const label of file.rowLabels)
                set[parseInt(label.substring(1))] = true;
        const ls = Object.keys(set).map(x => parseInt(x)).sort((a, b) => a - b);
        const p = {};
        let mb = 1;
        for (const r of win.rows) {
            const k = r.src + "," + r.dst;
            const e = p[k] || (p[k] = {n: 0, bytes: 0});
            e.n += 1;
            e.bytes += r.size;
            if (r.src !== r.dst)
                mb = Math.max(mb, e.bytes);
        }
        const h = {};
        let mh = 1;
        if (hasTasks) {
            const t = file.handoverTotals(gStart, gEnd);
            for (let i = 0; i + 2 < t.length; i += 3) {
                h[t[i]] = {n: t[i + 1], cross: t[i + 2]};
                mh = Math.max(mh, t[i + 1]);
            }
        }
        locs = ls;
        parcels = p;
        hand = h;
        maxBytes = mb;
        maxHand = mh;
        if (selLoc >= 0)
            select(selLoc);
        canvas.requestPaint();
    }
    function select(loc) {
        selLoc = loc;
        detail = loc >= 0 && hasTasks ? file.handoverMatrix(loc, gStart, gEnd) : ({});
        canvas.requestPaint();
    }
    Connections { target: root.win; function onLaneInfoChanged() { root.rebuild(); } }
    Connections { target: root.file; function onFilterChanged() { root.rebuild(); } function onChanged() { root.rebuild(); } }
    onVisibleChanged: if (visible) rebuild()
    onWidthChanged: canvas.requestPaint()
    onHeightChanged: canvas.requestPaint()
    onSelLocChanged: canvas.requestPaint()
    onHoverIChanged: canvas.requestPaint()
    onHoverJChanged: canvas.requestPaint()

    // the cell under (x, y) in the matrix of the localities
    function cellAt(x, y) {
        const j = Math.floor((x - leftM) / cell), i = Math.floor((y - topM) / cell);
        return i >= 0 && j >= 0 && i < locs.length && j < locs.length ? [i, j] : [-1, -1];
    }
    function pc(v, vmax) { return Math.sqrt(Math.max(0, v) / Math.max(vmax, 1)); }

    Canvas {
        id: canvas
        anchors.fill: parent
        renderTarget: Canvas.Image
        onPaint: {
            const ctx = getContext("2d");
            ctx.reset();
            ctx.fillStyle = "#2b2b2b";
            ctx.fillRect(0, 0, width, height);
            const n = root.locs.length;
            if (n === 0)
                return;
            const c = root.cell;
            ctx.font = "13px sans-serif";
            ctx.fillStyle = "#dddddd";
            ctx.textAlign = "left";
            ctx.fillText("Data exchange in this group, between and inside localities", root.leftM, 22);
            ctx.font = "11px sans-serif";
            ctx.fillStyle = "#999999";
            ctx.fillText("row = from, column = to", root.leftM, 40);
            // the cells
            for (let i = 0; i < n; ++i)
                for (let j = 0; j < n; ++j) {
                    const x = root.leftM + j * c, y = root.topM + i * c;
                    ctx.fillStyle = "#262626";
                    ctx.fillRect(x, y, c - 1, c - 1);
                    if (i === j) {
                        const h = root.hand[root.locs[i]];
                        if (h && h.n > 0) {
                            ctx.fillStyle = Qt.rgba(0.2, 0.75, 0.55, 0.2 + 0.8 * root.pc(h.n, root.maxHand));
                            ctx.fillRect(x, y, c - 1, c - 1);
                        }
                    } else {
                        const e = root.parcels[root.locs[i] + "," + root.locs[j]];
                        if (e) {
                            ctx.fillStyle = Qt.rgba(0.31, 0.76, 0.97, 0.2 + 0.8 * root.pc(e.bytes, root.maxBytes));
                            ctx.fillRect(x, y, c - 1, c - 1);
                        }
                    }
                }
            // the labels
            ctx.font = "10px sans-serif";
            ctx.fillStyle = "#bbbbbb";
            const every = c >= 14 ? 1 : Math.ceil(14 / c);
            ctx.textAlign = "right";
            for (let i = 0; i < n; i += every)
                ctx.fillText("L#" + root.locs[i], root.leftM - 6, root.topM + i * c + c / 2 + 3.5);
            ctx.textAlign = "center";
            for (let j = 0; j < n; j += every) {
                ctx.save();
                ctx.translate(root.leftM + j * c + c / 2, root.topM - 6);
                ctx.rotate(-Math.PI / 2);
                ctx.textAlign = "left";
                ctx.fillText("L#" + root.locs[j], 0, 3.5);
                ctx.restore();
            }
            // hover and selection
            const frame = (i, j, color, w) => { ctx.strokeStyle = color; ctx.lineWidth = w; ctx.strokeRect(root.leftM + j * c - 0.5, root.topM + i * c - 0.5, c, c); };
            if (root.selLoc >= 0) {
                const k = root.locs.indexOf(root.selLoc);
                if (k >= 0)
                    frame(k, k, "#ffffff", 2);
            }
            if (root.hoverI >= 0)
                frame(root.hoverI, root.hoverJ, "#ffd600", 1.5);

            // below: what the colours mean
            const ly = root.topM + n * c + 22;
            ctx.textAlign = "left";
            ctx.font = "12px sans-serif";
            ctx.fillStyle = "#4fc3f7";
            ctx.fillRect(root.leftM, ly - 9, 14, 10);
            ctx.fillStyle = "#dddddd";
            ctx.fillText("parcels between localities (bytes)", root.leftM + 20, ly);
            ctx.fillStyle = "#33bf8c";
            ctx.fillRect(root.leftM, ly + 10, 14, 10);
            ctx.fillStyle = "#dddddd";
            ctx.fillText(root.hasTasks ? "values handed from one task to another inside a locality (number; click a green cell)"
                                       : "values handed between tasks inside a locality: no task data for this run", root.leftM + 20, ly + 19);

            // the worker threads of the chosen locality
            if (root.selLoc >= 0) {
                const x0 = root.mainW + 20, w = root.workers();
                ctx.fillStyle = "#dddddd";
                ctx.font = "13px sans-serif";
                ctx.fillText("Inside L#" + root.selLoc + ": worker thread to worker thread", x0, 22);
                if (w === 0) {
                    ctx.font = "12px sans-serif";
                    ctx.fillStyle = "#999999";
                    ctx.fillText("no task data for this locality", x0, 44);
                    return;
                }
                const counts = root.detail.counts;
                let total = 0, same = 0, mx = 1;
                for (let i = 0; i < w; ++i)
                    for (let j = 0; j < w; ++j) {
                        const v = counts[i * w + j];
                        total += v;
                        same += i === j ? v : 0;
                        mx = Math.max(mx, v);
                    }
                ctx.font = "11px sans-serif";
                ctx.fillStyle = "#999999";
                ctx.fillText(total + " values handed over, " + (total > 0 ? Math.round(100 * (total - same) / total) : 0) + " % to another thread", x0, 40);
                const lm = 36, cw = Math.max(6, Math.min(24, Math.min(width - x0 - lm - 20, height - root.topM - root.bottomM) / w));
                for (let i = 0; i < w; ++i)
                    for (let j = 0; j < w; ++j) {
                        const v = counts[i * w + j];
                        ctx.fillStyle = "#262626";
                        ctx.fillRect(x0 + lm + j * cw, root.topM + i * cw, cw - 1, cw - 1);
                        if (v > 0) {
                            ctx.fillStyle = Qt.rgba(0.2, 0.75, 0.55, 0.2 + 0.8 * Math.sqrt(v / mx));
                            ctx.fillRect(x0 + lm + j * cw, root.topM + i * cw, cw - 1, cw - 1);
                        }
                    }
                ctx.font = "10px sans-serif";
                ctx.fillStyle = "#bbbbbb";
                const ev = cw >= 12 ? 1 : Math.ceil(12 / cw);
                ctx.textAlign = "right";
                for (let i = 0; i < w; i += ev)
                    ctx.fillText("w" + root.detail.workers[i], x0 + lm - 5, root.topM + i * cw + cw / 2 + 3.5);
                ctx.textAlign = "left";
                for (let j = 0; j < w; j += ev) {
                    ctx.save();
                    ctx.translate(x0 + lm + j * cw + cw / 2, root.topM - 5);
                    ctx.rotate(-Math.PI / 2);
                    ctx.fillText("w" + root.detail.workers[j], 0, 3.5);
                    ctx.restore();
                }
            }
        }
    }
    function workers() { return detail.workers ? detail.workers.length : 0; }

    MouseArea {
        anchors.fill: parent
        hoverEnabled: true
        onPositionChanged: (m) => {
            root.mouseX = m.x;
            root.mouseY = m.y;
            const [i, j] = root.cellAt(m.x, m.y);
            root.hoverI = i;
            root.hoverJ = j;
        }
        onExited: { root.hoverI = -1; root.hoverJ = -1; }
        onClicked: (m) => {
            const [i, j] = root.cellAt(m.x, m.y);
            root.select(i >= 0 && i === j ? root.locs[i] : -1);
        }
    }

    // what is under the mouse
    Rectangle {
        visible: root.hoverI >= 0
        z: 4
        x: Math.min(root.mouseX + 16, root.width - width - 8)
        y: Math.min(root.mouseY + 16, root.height - height - 8)
        width: tipText.width + 24
        height: tipText.height + 18
        radius: 8
        color: "#262626"
        border.color: "#555555"
        Text {
            id: tipText
            x: 12; y: 9
            color: "white"; font.pixelSize: 13; lineHeight: 1.15
            text: {
                const i = root.hoverI, j = root.hoverJ;
                if (i < 0)
                    return "";
                const a = root.locs[i], b = root.locs[j];
                if (i === j) {
                    const h = root.hand[a];
                    return "inside L#" + a + "\n" + (h ? h.n + " values handed from task to task, " + Math.round(100 * h.cross / Math.max(1, h.n)) + " % to another worker thread"
                                                     : (root.hasTasks ? "no hand-overs recorded" : "no task data for this run")) + (h ? "\n(click: which threads)" : "");
                }
                const e = root.parcels[a + "," + b];
                return "L#" + a + " → L#" + b + "\n" + (e ? e.n + (e.n === 1 ? " parcel, " : " parcels, ") + root.win.fmtSize(e.bytes) : "no parcels");
            }
        }
    }
}

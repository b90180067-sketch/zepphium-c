/* =============================================================================
   Zephium · new tab
   Loads ascii_core.wasm (C17), paints its cell buffer as coloured monospace
   glyphs, and wires the omnibox / scene switch / clock / shortcuts.
   ========================================================================== */
(() => {
    'use strict';

    const FONT = '"Paper Mono", "Cascadia Mono", Consolas, "Segoe UI", monospace';
    const STORE_KEY = 'zephium.newtab.scene';
    const MAX_COLS = 300;
    const MAX_ROWS = 120;
    const FRAME_MS = 33;

    const SCENES = [
        {
            no: '01',
            name: 'Vịnh trăng',
            accent: '#7ce8c4',
            accentRgb: '124, 232, 196',
            bg0: '#03070d', bg1: '#06111c', bg2: '#071a24', bg3: '#03090e',
            glow: 'rgba(244, 231, 195, 0.15)',
            glowX: '72%', glowY: '20%', horizonY: '62%',
            horizonGlow: 'rgba(55, 140, 150, 0.10)',
            waterGlow: 'rgba(30, 96, 112, 0.30)',
            palette: [
                'rgba(0, 0, 0, 0)',   /* none */
                '#6f92b0',            /* far ridge */
                '#4f7492',            /* mid ridge */
                '#35536f',            /* near ridge */
                '#142334',            /* dark */
                '#c3d7e6',            /* star */
                '#fff8e6',            /* star bright */
                '#ffeec2',            /* moon */
                '#cbb98e',            /* moon dim */
                '#55708a',            /* cloud */
                '#06141e',            /* water */
                '#145064',            /* water mid */
                '#c6fff0',            /* glint */
                '#294634',            /* land */
                '#f5d796',            /* accent */
                '#8ff7da',            /* accent 2 */
                '#0b1824',            /* far fill */
                '#07121c',            /* mid fill */
                '#040b12'             /* near fill */
            ]
        },
        {
            no: '02',
            name: 'Bình minh',
            accent: '#ffb36f',
            accentRgb: '255, 179, 111',
            bg0: '#100a17', bg1: '#241326', bg2: '#3a2030', bg3: '#120b13',
            glow: 'rgba(255, 178, 112, 0.34)',
            glowX: '69%', glowY: '38%', horizonY: '66%',
            horizonGlow: 'rgba(255, 130, 80, 0.22)',
            waterGlow: 'rgba(110, 48, 62, 0.34)',
            palette: [
                'rgba(0, 0, 0, 0)',
                '#b2839c',
                '#815676',
                '#563662',
                '#1d1424',
                '#ffe0b5',
                '#fff7e6',
                '#ffe0a3',
                '#e0a17e',
                '#a7636c',
                '#1a0f1e',
                '#4a2638',
                '#ffe3a8',
                '#4a2a28',
                '#ffb166',
                '#ff8b75',
                '#2a1928',
                '#1b0f1e',
                '#0e0712'
            ]
        },
        {
            no: '03',
            name: 'Cực quang',
            accent: '#7af0c4',
            accentRgb: '122, 240, 196',
            bg0: '#030a12', bg1: '#061923', bg2: '#08232d', bg3: '#02080d',
            glow: 'rgba(106, 238, 190, 0.13)',
            glowX: '48%', glowY: '22%', horizonY: '68%',
            horizonGlow: 'rgba(60, 220, 180, 0.10)',
            waterGlow: 'rgba(22, 78, 88, 0.30)',
            palette: [
                'rgba(0, 0, 0, 0)',
                '#3b7484',
                '#275766',
                '#163843',
                '#091b22',
                '#d2f0fa',
                '#ffffff',
                '#e5f7ff',
                '#95bcc9',
                '#245662',
                '#04141b',
                '#0c3a45',
                '#a6ffe0',
                '#123028',
                '#86f7cf',
                '#c0a6ff',
                '#0b2029',
                '#07151c',
                '#040c11'
            ]
        }
    ];

    const QUICK = [
        { label: 'DuckDuckGo', host: 'duckduckgo.com', url: 'https://duckduckgo.com', icon: 'i-search' },
        { label: 'GitHub', host: 'github.com', url: 'https://github.com', icon: 'i-code' },
        { label: 'Wikipedia', host: 'wikipedia.org', url: 'https://vi.wikipedia.org', icon: 'i-book' },
        { label: 'YouTube', host: 'youtube.com', url: 'https://www.youtube.com', icon: 'i-play' },
        { label: 'Gmail', host: 'mail.google.com', url: 'https://mail.google.com', icon: 'i-mail' },
        { label: 'VnExpress', host: 'vnexpress.net', url: 'https://vnexpress.net', icon: 'i-news' }
    ];

    const CHAR = Array.from({ length: 128 }, (_, i) => String.fromCharCode(i));

    const el = {
        canvas: document.getElementById('art'),
        clock: document.getElementById('clock'),
        date: document.getElementById('date'),
        form: document.getElementById('search-form'),
        input: document.getElementById('omnibox'),
        suggestions: document.getElementById('suggestions'),
        quick: document.getElementById('quick'),
        coreStatus: document.getElementById('core-status'),
        gridMeta: document.getElementById('grid-meta'),
        sceneMeta: document.getElementById('scene-meta'),
        toast: document.getElementById('toast')
    };

    const ctx = el.canvas.getContext('2d', { alpha: true });
    const reduceMotion = window.matchMedia('(prefers-reduced-motion: reduce)').matches;

    const state = {
        exports: null,
        cells: null,
        cols: 0,
        rows: 0,
        cellW: 8,
        cellH: 15,
        fontPx: 12,
        offX: 0,
        offY: 0,
        aspect: 1.7,
        scene: 0,
        started: 0,
        lastDraw: 0,
        fps: 0,
        frames: 0,
        fpsAt: 0,
        running: false,
        wasmBytes: 0
    };

    let suggestions = [];
    let activeIndex = -1;
    let toastTimer = 0;
    let resizeTimer = 0;
    let rafId = 0;

    /* ---------------------------------------------------------------- utils */

    const clamp = (v, lo, hi) => Math.min(hi, Math.max(lo, v));

    function storeGet(key) {
        try { return window.localStorage.getItem(key); } catch (_) { return null; }
    }

    function storeSet(key, value) {
        try { window.localStorage.setItem(key, value); } catch (_) { /* file:// or private mode */ }
    }

    function hostOf(url) {
        try {
            const host = new URL(url).hostname.replace(/^www\./, '');
            return host || 'liên kết';
        } catch (_) {
            return 'liên kết';
        }
    }

    function truncate(s, n) {
        return s.length > n ? s.slice(0, n - 1) + '…' : s;
    }

    function toast(message) {
        el.toast.textContent = message;
        el.toast.classList.add('is-visible');
        clearTimeout(toastTimer);
        toastTimer = window.setTimeout(() => el.toast.classList.remove('is-visible'), 2200);
    }

    function capitalize(s) {
        return s ? s.charAt(0).toUpperCase() + s.slice(1) : s;
    }

    /* ------------------------------------------------------------- the clock */

    const timeFmt = new Intl.DateTimeFormat('vi-VN', {
        hour: '2-digit', minute: '2-digit', hourCycle: 'h23'
    });
    const dateFmt = new Intl.DateTimeFormat('vi-VN', {
        weekday: 'long', day: '2-digit', month: 'long'
    });

    function tickClock() {
        const now = new Date();
        el.clock.textContent = timeFmt.format(now);
        el.clock.dateTime = now.toISOString();
        el.date.textContent = capitalize(dateFmt.format(now));
    }

    /* ----------------------------------------------------------- wasm loading */

    function base64ToBytes(b64) {
        const raw = atob(b64);
        const out = new Uint8Array(raw.length);
        for (let i = 0; i < raw.length; i++) out[i] = raw.charCodeAt(i);
        return out;
    }

    async function loadCore() {
        let bytes = null;
        try {
            const response = await fetch('ascii_core.wasm', { cache: 'no-cache' });
            if (response.ok) bytes = new Uint8Array(await response.arrayBuffer());
        } catch (_) { /* file:// has no fetch; fall through to the embedded copy */ }

        if (!bytes && window.ASCII_CORE_WASM_BASE64) {
            bytes = base64ToBytes(window.ASCII_CORE_WASM_BASE64);
        }
        if (!bytes) throw new Error('ASCII core is missing');

        const { instance } = await WebAssembly.instantiate(bytes, {});
        state.wasmBytes = bytes.length;
        return instance.exports;
    }

    /* ------------------------------------------------------------- layout */

    async function waitForFont() {
        if (!document.fonts || !document.fonts.load) return;
        try {
            await document.fonts.load('500 16px "Paper Mono"');
            await document.fonts.ready;
        } catch (_) { /* fall back to system monospace */ }
    }

    function layout() {
        if (!state.exports) return;

        const width = window.innerWidth;
        const height = window.innerHeight;
        const dpr = clamp(window.devicePixelRatio || 1, 1, 1.5);

        el.canvas.width = Math.max(1, Math.round(width * dpr));
        el.canvas.height = Math.max(1, Math.round(height * dpr));
        el.canvas.style.width = width + 'px';
        el.canvas.style.height = height + 'px';
        ctx.setTransform(dpr, 0, 0, dpr, 0, 0);

        /* About 52 rows on any window, like a terminal landscape. */
        let cellH = height / 52;
        let fontPx = cellH / 1.24;
        fontPx = clamp(fontPx, 9.5, 20);
        cellH = fontPx * 1.24;

        ctx.font = '500 ' + fontPx.toFixed(2) + 'px ' + FONT;
        const measured = ctx.measureText('M').width;
        let cellW = measured > 1 ? measured : fontPx * 0.6;
        cellW = Math.max(5.4, cellW);

        let cols = Math.floor(width / cellW);
        let rows = Math.floor(height / cellH);
        cols = clamp(cols, 24, MAX_COLS);
        rows = clamp(rows, 16, MAX_ROWS);

        state.cellW = cellW;
        state.cellH = cellH;
        state.fontPx = fontPx;
        state.cols = cols;
        state.rows = rows;
        state.offX = Math.round((width - cols * cellW) / 2);
        state.offY = Math.round((height - rows * cellH) / 2);
        state.aspect = (cols * cellW) / (rows * cellH);

        const ptr = state.exports.ascii_cells();
        state.cells = new Uint16Array(state.exports.memory.buffer, ptr, cols * rows);

        if (reduceMotion) {
            drawAt(9);
        } else {
            forceDraw();
        }
        updateGridMeta();
    }

    /* --------------------------------------------------------------- drawing */

    function drawAt(t) {
        if (!state.exports || !state.cells) return;
        if (document.body.dataset.theme === 'aero') return;
        state.exports.ascii_render(t, state.scene, state.cols, state.rows, state.aspect);
        paint();
    }

    function forceDraw() {
        drawAt(reduceMotion ? 9 : (performance.now() - state.started) / 1000);
    }

    function paint() {
        const palette = SCENES[state.scene].palette;
        const cells = state.cells;
        const cols = state.cols;
        const rows = state.rows;
        const cellW = state.cellW;
        const cellH = state.cellH;
        const baseX = state.offX;
        const baseY = state.offY;
        const charAdvance = cellW;

        ctx.clearRect(0, 0, el.canvas.width, el.canvas.height);
        ctx.font = '500 ' + state.fontPx.toFixed(2) + 'px ' + FONT;
        ctx.textAlign = 'left';
        ctx.textBaseline = 'middle';
        ctx.globalAlpha = 0.96;

        for (let y = 0; y < rows; y++) {
            const rowStart = y * cols;
            let run = '';
            let runX = 0;
            let runColor = -1;
            const py = baseY + y * cellH + cellH * 0.54;

            for (let x = 0; x < cols; x++) {
                const cell = cells[rowStart + x];
                const code = cell & 0xff;
                if (code === 32) {
                    if (run) {
                        ctx.fillStyle = palette[runColor] || palette[1];
                        ctx.fillText(run, baseX + runX * charAdvance, py);
                        run = '';
                    }
                    continue;
                }
                const color = (cell >>> 8) & 0x1f;
                if (color !== runColor) {
                    if (run) {
                        ctx.fillStyle = palette[runColor] || palette[1];
                        ctx.fillText(run, baseX + runX * charAdvance, py);
                        run = '';
                    }
                    runX = x;
                    runColor = color;
                }
                run += code < 128 ? CHAR[code] : ' ';
            }
            if (run) {
                ctx.fillStyle = palette[runColor] || palette[1];
                ctx.fillText(run, baseX + runX * charAdvance, py);
            }
        }
        ctx.globalAlpha = 1;
    }

    function updateGridMeta() {
        if (!el.gridMeta) return;
        const fps = state.fps > 0 ? ' · ' + state.fps + ' fps' : '';
        el.gridMeta.textContent = state.cols + ' x ' + state.rows + ' ô' + fps;
    }

    function loop(now) {
        if (!state.running) return;
        rafId = window.requestAnimationFrame(loop);
        if (document.hidden) return;
        if (now - state.lastDraw < FRAME_MS) return;
        state.lastDraw = now;

        const t = (now - state.started) / 1000;
        state.exports.ascii_render(t, state.scene, state.cols, state.rows, state.aspect);
        paint();

        state.frames++;
        if (now - state.fpsAt > 800) {
            state.fps = Math.round((state.frames * 1000) / (now - state.fpsAt));
            state.frames = 0;
            state.fpsAt = now;
            updateGridMeta();
        }
    }

    /* ---------------------------------------------------------------- theme */
    /* Called by the shell with "aero" or "dark" through ExecuteScript. It only
     * flips data-theme and stops/resumes the render loop; it never navigates and
     * never posts a message back to the shell. */
    function zephiumSetTheme(theme) {
        if (theme !== 'aero' && theme !== 'dark') return;
        document.body.dataset.theme = theme;

        if (theme === 'aero') {
            state.running = false;
            if (rafId) {
                window.cancelAnimationFrame(rafId);
                rafId = 0;
            }
            return;
        }

        if (reduceMotion) {
            if (state.exports) drawAt(9);
            return;
        }
        if (state.exports && !state.running) {
            state.running = true;
            state.lastDraw = 0;
            rafId = window.requestAnimationFrame(loop);
        }
    }

    window.zephiumSetTheme = zephiumSetTheme;

    /* --------------------------------------------------------------- scenes */

    function currentSceneFromStore() {
        const preview = Number(new URLSearchParams(window.location.search).get('scene'));
        if (preview >= 1 && preview <= 3) return preview - 1;
        const raw = storeGet(STORE_KEY);
        if (raw !== null) {
            const saved = Number(raw);
            if (saved === 0 || saved === 1 || saved === 2) return saved;
        }
        const hour = new Date().getHours();
        return hour >= 6 && hour < 17 ? 1 : 0;
    }

    function applyScene(id, persist = true) {
        if (id < 0 || id >= SCENES.length) return;
        const scene = SCENES[id];
        state.scene = id;

        const root = document.documentElement.style;
        root.setProperty('--accent', scene.accent);
        root.setProperty('--accent-rgb', scene.accentRgb);
        root.setProperty('--bg0', scene.bg0);
        root.setProperty('--bg1', scene.bg1);
        root.setProperty('--bg2', scene.bg2);
        root.setProperty('--bg3', scene.bg3);
        root.setProperty('--glow', scene.glow);
        root.setProperty('--glow-x', scene.glowX);
        root.setProperty('--glow-y', scene.glowY);
        root.setProperty('--horizon-y', scene.horizonY);
        root.setProperty('--horizon-glow', scene.horizonGlow);
        root.setProperty('--water-glow', scene.waterGlow);

        document.body.dataset.scene = String(id);
        document.querySelectorAll('.scene').forEach((button) => {
            button.setAttribute('aria-pressed', String(Number(button.dataset.scene) === id));
        });
        el.sceneMeta.textContent = 'Cảnh ' + scene.no + ' — ' + scene.name;
        if (persist) storeSet(STORE_KEY, String(id));

        document.body.classList.add('scene-changing');
        window.setTimeout(() => document.body.classList.remove('scene-changing'), 220);
        forceDraw();
    }

    /* -------------------------------------------------------------- omnibox */

    function looksLikeUrl(value) {
        if (/^https?:\/\//i.test(value)) return true;
        if (/^(localhost|127\.0\.0\.1|\[::1\])(:\d+)?([/?#]|$)/i.test(value)) return true;
        if (/\s/.test(value)) return false;
        return /^[^\s/?#]+\.[a-z]{2,}(:\d+)?([/?#]|$)/i.test(value);
    }

    function normalizeUrl(value) {
        return /^https?:\/\//i.test(value) ? value : 'https://' + value;
    }

    function resolveInput(value) {
        let q = value.trim();
        if (!q) return null;
        if (q.startsWith('?')) {
            q = q.slice(1).trim();
            return q ? 'https://duckduckgo.com/?q=' + encodeURIComponent(q) : null;
        }
        if (looksLikeUrl(q)) return normalizeUrl(q);
        return 'https://duckduckgo.com/?q=' + encodeURIComponent(q);
    }

    function handleSceneCommand(value) {
        const match = value.trim().match(/^:(scene|canh)(?:\s+([1-3]))?$/i);
        if (!match) return false;
        const id = match[2] ? Number(match[2]) - 1 : (state.scene + 1) % SCENES.length;
        applyScene(id);
        toast('Đã đổi sang ' + SCENES[id].name);
        return true;
    }

    function openTarget(raw, label) {
        const value = String(raw || '').trim();
        if (!value) return;
        if (handleSceneCommand(value)) {
            el.input.value = '';
            closeSuggestions();
            return;
        }

        const url = resolveInput(value);
        if (!url) return;
        const search = url.startsWith('https://duckduckgo.com/?q=');
        const where = search ? 'DuckDuckGo' : (label || hostOf(url));

        el.form.classList.add('is-launching');
        window.setTimeout(() => el.form.classList.remove('is-launching'), 460);
        toast(search ? 'Đang tìm “' + truncate(value, 46) + '”…' : 'Đang mở ' + where + '…');

        const bridge = window.chrome && window.chrome.webview &&
            typeof window.chrome.webview.postMessage === 'function';
        if (bridge) {
            window.chrome.webview.postMessage('navigate|' + url);
        } else {
            window.location.href = url;
        }
    }

    function buildSuggestions(query) {
        const q = query.trim();
        if (!q) return [];
        const items = [];
        const lower = q.toLowerCase();

        if (looksLikeUrl(q)) {
            items.push({
                kind: 'url',
                label: 'Mở ' + normalizeUrl(q),
                meta: 'Địa chỉ web',
                value: q,
                icon: 'i-globe'
            });
        }

        items.push({
            kind: 'search',
            label: 'Tìm “' + truncate(q, 56) + '”',
            meta: 'DuckDuckGo',
            value: q,
            icon: 'i-search'
        });

        for (const quick of QUICK) {
            if (quick.label.toLowerCase().includes(lower) || quick.host.includes(lower)) {
                items.push({
                    kind: 'quick',
                    label: quick.label,
                    meta: quick.host,
                    value: quick.url,
                    icon: quick.icon
                });
            }
        }

        return items.slice(0, 6);
    }

    function renderSuggestions() {
        el.suggestions.textContent = '';
        suggestions = buildSuggestions(el.input.value);

        if (!suggestions.length) {
            closeSuggestions();
            return;
        }

        activeIndex = suggestions.length ? 0 : -1;
        suggestions.forEach((item, index) => {
            const button = document.createElement('button');
            button.type = 'button';
            button.className = 'suggestion' + (index === activeIndex ? ' is-active' : '');
            button.dataset.index = String(index);
            button.setAttribute('role', 'option');
            button.setAttribute('aria-selected', String(index === activeIndex));

            const icon = document.createElement('span');
            icon.className = 's-icon';
            icon.innerHTML = '<svg><use href="#' + item.icon + '"></use></svg>';

            const text = document.createElement('span');
            text.className = 's-text';
            const label = document.createElement('span');
            label.className = 's-label';
            label.textContent = item.label;
            const meta = document.createElement('span');
            meta.className = 's-meta';
            meta.textContent = item.meta;
            text.append(label, meta);

            const enter = document.createElement('span');
            enter.className = 's-enter';
            enter.textContent = '↵';

            button.append(icon, text, enter);
            button.addEventListener('mousedown', (event) => event.preventDefault());
            button.addEventListener('click', () => chooseSuggestion(index));
            el.suggestions.append(button);
        });

        el.suggestions.hidden = false;
        el.input.setAttribute('aria-expanded', 'true');
    }

    function closeSuggestions() {
        el.suggestions.hidden = true;
        el.suggestions.textContent = '';
        el.input.setAttribute('aria-expanded', 'false');
        suggestions = [];
        activeIndex = -1;
    }

    function highlightSuggestion(index) {
        if (!suggestions.length) return;
        activeIndex = (index + suggestions.length) % suggestions.length;
        Array.from(el.suggestions.children).forEach((child, i) => {
            child.classList.toggle('is-active', i === activeIndex);
            child.setAttribute('aria-selected', String(i === activeIndex));
        });
        const active = el.suggestions.children[activeIndex];
        if (active) active.scrollIntoView({ block: 'nearest' });
    }

    function chooseSuggestion(index) {
        const item = suggestions[index];
        if (!item) {
            openTarget(el.input.value);
            return;
        }
        if (item.kind === 'search' || item.kind === 'url') {
            closeSuggestions();
            openTarget(item.value);
        } else {
            closeSuggestions();
            openTarget(item.value, item.label);
        }
    }

    function onSubmit(event) {
        event.preventDefault();
        if (suggestions.length && activeIndex >= 0) {
            chooseSuggestion(activeIndex);
        } else {
            openTarget(el.input.value);
        }
    }

    /* ---------------------------------------------------------------- wiring */

    function wireUI() {
        document.querySelectorAll('.scene').forEach((button) => {
            button.addEventListener('click', () => applyScene(Number(button.dataset.scene)));
        });

        el.quick.querySelectorAll('.quick-link').forEach((button) => {
            button.addEventListener('click', () => openTarget(button.dataset.url, button.title || button.textContent.trim()));
        });

        el.form.addEventListener('submit', onSubmit);
        el.input.addEventListener('input', renderSuggestions);
        el.input.addEventListener('focus', () => {
            if (el.input.value.trim()) renderSuggestions();
        });
        el.input.addEventListener('keydown', (event) => {
            if (event.key === 'ArrowDown') {
                event.preventDefault();
                if (!suggestions.length) renderSuggestions();
                else highlightSuggestion(activeIndex + 1);
            } else if (event.key === 'ArrowUp') {
                event.preventDefault();
                if (suggestions.length) highlightSuggestion(activeIndex - 1);
            } else if (event.key === 'Escape') {
                event.preventDefault();
                closeSuggestions();
                el.input.blur();
            }
        });
        el.input.addEventListener('blur', () => {
            window.setTimeout(() => {
                if (document.activeElement !== el.input) closeSuggestions();
            }, 120);
        });

        window.addEventListener('keydown', (event) => {
            const tag = (event.target && event.target.tagName) || '';
            const typing = tag === 'INPUT' || tag === 'TEXTAREA';
            if ((event.key === '/' || ((event.ctrlKey || event.metaKey) && event.key.toLowerCase() === 'k')) && !typing) {
                event.preventDefault();
                el.input.focus();
                el.input.select();
            } else if ((event.ctrlKey || event.metaKey) && event.key.toLowerCase() === 'l') {
                event.preventDefault();
                el.input.focus();
                el.input.select();
            }
        });

        window.addEventListener('resize', () => {
            window.clearTimeout(resizeTimer);
            resizeTimer = window.setTimeout(layout, 120);
        });

        document.addEventListener('visibilitychange', () => {
            if (!document.hidden && !reduceMotion && state.exports) {
                state.lastDraw = 0;
            }
        });

        el.clock.parentElement.addEventListener('click', () => toast(new Date().toLocaleString('vi-VN')));
        document.getElementById('brand').addEventListener('click', (event) => {
            event.preventDefault();
            el.input.focus();
            closeSuggestions();
        });
    }

    /* ----------------------------------------------------------------- boot */

    async function boot() {
        wireUI();
        tickClock();
        window.setInterval(tickClock, 1000);

        applyScene(currentSceneFromStore(), false);
        await waitForFont();

        try {
            state.exports = await loadCore();
            el.coreStatus.textContent = 'C17 core / wasm ' + Math.round(state.wasmBytes / 1024) + ' KB';
        } catch (error) {
            el.coreStatus.textContent = 'C17 core / lỗi tải';
            toast('Không tải được ASCII core (' + (error && error.message ? error.message : 'wasm') + ')');
            return;
        }

        state.started = performance.now();
        state.fpsAt = state.started;
        layout();

        /* A new tab should be ready to type into, like the address bar. */
        window.setTimeout(() => {
            try {
                el.input.focus({ preventScroll: true });
            } catch (_) {
                el.input.focus();
            }
        }, 260);

        if (reduceMotion) {
            drawAt(9);
        } else if (document.body.dataset.theme !== 'aero') {
            state.running = true;
            rafId = window.requestAnimationFrame(loop);
        }
    }

    boot();
})();

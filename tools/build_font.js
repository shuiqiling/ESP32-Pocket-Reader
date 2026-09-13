// Build a custom LVGL font that covers all characters currently present in spiffs_data.
// Usage:
//   cd D:\lvgl
//   npm install lv_font_conv@1.5.3
//   node tools/build_font.js
const fs = require('fs');
const path = require('path');
const convert = require('../node_modules/lv_font_conv/lib/convert');

const root = path.resolve(__dirname, '..');
const spiffsDir = path.join(root, 'spiffs_data');

function collectSymbols() {
    const chars = new Set();
    // Always include ASCII printable characters.
    for (let cp = 0x20; cp <= 0x7E; cp++) {
        chars.add(String.fromCodePoint(cp));
    }

    // Cover GB2312 (Simplified Chinese) plus the common Big5 block used by the
    // search result template. This is substantially smaller than the entire
    // 20k CJK block and still fits the 4MB device's application partition.
    const gb = new TextDecoder('gb18030');
    for (let high = 0xB0; high <= 0xF7; high++) {
        for (let low = 0xA1; low <= 0xFE; low++) {
            const ch = gb.decode(Uint8Array.from([high, low]));
            if (ch && ch !== '\uFFFD') chars.add(ch);
        }
    }
    const big5 = new TextDecoder('big5');
    for (let high = 0xA4; high <= 0xC6; high++) {
        for (let low = 0x40; low <= 0xFE; low++) {
            if (low === 0x7F || (low > 0x7E && low < 0xA1)) continue;
            const ch = big5.decode(Uint8Array.from([high, low]));
            if (ch && ch !== '\uFFFD') chars.add(ch);
        }
    }

    // Fixed UI/status strings used by the WiFi setup + reader screens.
    // Extracted from every Chinese string literal in main/*.c so on-screen
    // progress/error texts never render as tofu (e.g. "阅").
    const uiFixed = '《》上下不中为任保停入内写击分创功务动原取可号名后启和器在地大失存完容密小已建录成或截挂据接放效数文斗断无有未本析查检止正没法点留目知码称稍空章第罗置节获行解设试说请读败账超足载输过进连重长闲阅陆限页，';
    for (const ch of uiFixed) {
        chars.add(ch);
    }

    // Keep UI glyph coverage in sync automatically. Only inspect C string
    // literals (not comments) so changing an on-screen message cannot produce
    // missing-glyph squares on the device.
    const mainDir = path.join(root, 'main');
    const sourceFiles = fs.readdirSync(mainDir)
        .filter((f) => /\.(c|h)$/i.test(f));
    for (const file of sourceFiles) {
        const source = fs.readFileSync(path.join(mainDir, file), 'utf8');
        const literals = source.match(/"(?:\\.|[^"\\])*"/g) || [];
        for (const literal of literals) {
            for (const ch of literal) {
                if (ch.codePointAt(0) > 0x7E) {
                    chars.add(ch);
                }
            }
        }
    }

    const files = fs.readdirSync(spiffsDir).filter((f) => f.toLowerCase().endsWith('.txt'));
    for (const file of files) {
        const text = fs.readFileSync(path.join(spiffsDir, file), 'utf8');
        for (const ch of text) {
            if (ch !== '\n' && ch !== '\r') {
                chars.add(ch);
            }
        }
    }
    return Array.from(chars).join('');
}

// The generated font embeds glyph outlines, so the source font must be one we
// are allowed to redistribute. SimHei / Microsoft YaHei ship with Windows and
// their licences forbid redistribution -- do not use them for a public build.
// Noto Sans SC is the Google Fonts release of Source Han Sans (思源黑体); both
// are SIL Open Font License 1.1, which permits redistribution. Medium weight is
// preferred because 1bpp rendering at 16 px drops strokes from lighter weights.
const FONT_CANDIDATES = [
    process.env.FONT_SOURCE,
    'C:/Windows/Fonts/Noto Sans SC Medium (TrueType).otf',
    'C:/Windows/Fonts/Noto Sans SC (TrueType).otf',
    'C:/Windows/Fonts/NotoSansSC-VF.ttf',
    '/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc',
    '/usr/share/fonts/truetype/noto/NotoSansCJK-Regular.ttc',
].filter(Boolean);

function resolveFontSource()
{
    for (const candidate of FONT_CANDIDATES) {
        if (fs.existsSync(candidate)) {
            return candidate;
        }
    }
    console.error('No usable source font found. Looked for:');
    for (const candidate of FONT_CANDIDATES) {
        console.error(`  ${candidate}`);
    }
    console.error('\nSet FONT_SOURCE to an OFL-licensed CJK font (Noto Sans SC,');
    console.error('Source Han Sans, ...) and run again. Do not use simhei.ttf or');
    console.error('msyh.ttc -- their licences forbid redistributing derived glyphs.');
    process.exit(1);
}

async function main() {
    if (!fs.existsSync(path.join(root, 'node_modules', 'lv_font_conv'))) {
        console.error('lv_font_conv is not installed. Run: npm install lv_font_conv@1.5.3');
        process.exit(1);
    }

    const symbols = collectSymbols();
    console.log(`Symbol count: ${Array.from(symbols).length}`);
    const fontSource = resolveFontSource();
    console.log(`Font source: ${fontSource}`);
    const output = path.join(root, 'main', 'fonts', 'novel_font_16.c');

    const args = {
        opts_string: 'custom font',
        size: 16,
        bpp: 1,
        lcd: false,
        lcd_v: false,
        use_color_info: false,
        format: 'lvgl',
        output,
        lv_font_name: 'novel_font_16',
        lv_include: 'lvgl.h',
        no_kerning: true,
        no_compress: true,
        no_prefilter: true,
        font: [
            {
                source_path: fontSource,
                source_bin: fs.readFileSync(fontSource),
                ranges: [{ symbols }]
            }
        ]
    };

    const files = await convert(args);
    for (const [filePath, data] of Object.entries(files)) {
        fs.mkdirSync(path.dirname(filePath), { recursive: true });
        let outputData = Buffer.isBuffer(data) ? data.toString('utf8') : String(data);
        if (path.resolve(filePath) === path.resolve(output)) {
            const marker = '.fallback = NULL,';
            if (!outputData.includes(marker)) {
                throw new Error('Generated font has no fallback field to patch');
            }
            outputData = outputData.replace(marker,
                '.fallback = &lv_font_montserrat_16,');
        }
        fs.writeFileSync(filePath, outputData);
        console.log(`Wrote ${filePath} (${Buffer.byteLength(outputData)} bytes)`);
    }
}

main().catch((err) => {
    console.error(err);
    process.exit(1);
});

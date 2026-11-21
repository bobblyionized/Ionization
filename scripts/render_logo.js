// Rasterize assets/logo.svg (and logo-small.svg for tiny sizes) to PNGs, then pack app.ico.
// Usage: node scripts/render_logo.js <path-to-node_modules-containing-@resvg/resvg-js>
const fs = require("fs");
const path = require("path");
const modules = process.argv[2] || "node_modules";
const { Resvg } = require(path.join(modules, "@resvg", "resvg-js"));

const root = path.join(__dirname, "..");
const big = fs.readFileSync(path.join(root, "assets", "logo.svg"));
const small = fs.readFileSync(path.join(root, "assets", "logo-small.svg"));

function render(svg, size) {
  const r = new Resvg(svg, { fitTo: { mode: "width", value: size }, background: "rgba(0,0,0,0)" });
  return r.render().asPng();
}

const sizes = [16, 20, 24, 32, 40, 48, 64, 128, 256];
const images = sizes.map((s) => ({ size: s, png: render(s <= 32 ? small : big, s) }));
fs.writeFileSync(path.join(root, "assets", "app.png"), render(big, 512));
fs.writeFileSync(path.join(root, "ui", "logo.png"), render(big, 128));

// ICO container with PNG-compressed entries.
const header = Buffer.alloc(6);
header.writeUInt16LE(0, 0); header.writeUInt16LE(1, 2); header.writeUInt16LE(images.length, 4);
const entries = [];
let offset = 6 + 16 * images.length;
for (const im of images) {
  const e = Buffer.alloc(16);
  e.writeUInt8(im.size % 256, 0); e.writeUInt8(im.size % 256, 1); e.writeUInt8(0, 2); e.writeUInt8(0, 3);
  e.writeUInt16LE(1, 4); e.writeUInt16LE(32, 6); e.writeUInt32LE(im.png.length, 8); e.writeUInt32LE(offset, 12);
  offset += im.png.length;
  entries.push(e);
}
fs.writeFileSync(path.join(root, "assets", "app.ico"), Buffer.concat([header, ...entries, ...images.map((i) => i.png)]));
console.log("wrote assets/app.ico, assets/app.png, ui/logo.png");

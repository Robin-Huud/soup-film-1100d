// Makes a Magic Lantern RBF bitmap font from any font macOS can draw.
//
//   swift mkrbf.swift "Avenir Next Demi Bold" 24 SOUP24.RBF
//
// RBF is CHDK's format: a header, a width per character and a 1-bit
// bitmap per character, rows LSB-first, padded to a whole byte. The
// height given is the cell height in pixels; glyphs are thresholded
// from CoreText's antialiased render, since the BMP overlay has no alpha.
import CoreText
import CoreGraphics
import Foundation

let args = CommandLine.arguments
guard args.count >= 4, let cell = Int(args[2]) else {
    print("usage: mkrbf <font name> <cell height px> <out.rbf> [threshold 0-255]"); exit(2)
}
let threshold = args.count > 4 ? UInt8(args[4])! : 110
let first = 32, last = 126

// size the font so ascent + descent fills the cell
var font = CTFontCreateWithName(args[1] as CFString, 100, nil)
let scale = CGFloat(cell) / (CTFontGetAscent(font) + CTFontGetDescent(font))
font = CTFontCreateWithName(args[1] as CFString, 100 * scale, nil)
let ascent = CTFontGetAscent(font), descent = CTFontGetDescent(font)
print("using \(CTFontCopyFullName(font)), \(String(format: "%.1f", 100 * scale)) pt")

var advances = [Int](), bitmaps = [[UInt8]]()
for c in first...last {
    var ch = UniChar(c), g = CGGlyph(0)
    CTFontGetGlyphsForCharacters(font, &ch, &g, 1)
    var adv = CGSize.zero
    CTFontGetAdvancesForGlyphs(font, .horizontal, &g, &adv, 1)
    advances.append(max(1, Int(adv.width.rounded())))
    bitmaps.append([])
}
let maxW = advances.max()!
let rowBytes = (maxW + 7) / 8, padW = rowBytes * 8

for (i, c) in (first...last).enumerated() {
    var gray = [UInt8](repeating: 0, count: padW * cell)
    let ctx = CGContext(data: &gray, width: padW, height: cell, bitsPerComponent: 8, bytesPerRow: padW,
                        space: CGColorSpaceCreateDeviceGray(), bitmapInfo: CGImageAlphaInfo.none.rawValue)!
    ctx.setFillColor(gray: 1, alpha: 1)
    var ch = UniChar(c), g = CGGlyph(0)
    CTFontGetGlyphsForCharacters(font, &ch, &g, 1)
    var pos = CGPoint(x: 0, y: descent)
    CTFontDrawGlyphs(font, &g, &pos, 1, ctx)
    var bits = [UInt8](repeating: 0, count: rowBytes * cell)
    for y in 0..<cell {
        for x in 0..<advances[i] where gray[y * padW + x] > threshold {
            bits[y * rowBytes + x / 8] |= UInt8(1 << (x % 8))   // CG rows run top to bottom here
        }
    }
    bitmaps[i] = bits
}

var out = Data()
func i32(_ v: Int) { var x = Int32(v); out.append(Data(bytes: &x, count: 4)) }
let count = last - first + 1, charSize = rowBytes * cell
let headerSize = 8 + 64 + 11 * 4
i32(0x0DF00EE0); i32(3)
var name = [UInt8](repeating: 0, count: 64)
for (i, b) in args[1].utf8.prefix(63).enumerated() { name[i] = b }
out.append(contentsOf: name)
i32(charSize); i32(cell); i32(cell); i32(maxW); i32(first); i32(last); i32(0)
i32(headerSize); i32(headerSize + count); i32(Int(descent.rounded())); i32(0)
out.append(contentsOf: advances.map { UInt8($0) })
for b in bitmaps { out.append(contentsOf: b) }
try! out.write(to: URL(fileURLWithPath: args[3]))
print("\(args[3]): \(cell) px, widest \(maxW), \(out.count) bytes")
_ = ascent

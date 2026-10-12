// Renders Kite's icon (bolt.shield on a rounded square) to PNGs and packs
// them into Windows .ico files. Usage: swift tools/make_icons.swift <outdir>
import AppKit

let out = CommandLine.arguments.count > 1 ? CommandLine.arguments[1] : "."
let sizes = [16, 20, 24, 32, 40, 48, 64, 256]

func render(_ size: Int, bg: NSColor, fullBleed: Bool = false, macGrid: Bool = false) -> Data {
    let boltColor = bg
    let rep = NSBitmapImageRep(bitmapDataPlanes: nil, pixelsWide: size, pixelsHigh: size,
                               bitsPerSample: 8, samplesPerPixel: 4, hasAlpha: true, isPlanar: false,
                               colorSpaceName: .deviceRGB, bytesPerRow: 0, bitsPerPixel: 0)!
    NSGraphicsContext.saveGraphicsState()
    NSGraphicsContext.current = NSGraphicsContext(bitmapImageRep: rep)
    let s = CGFloat(size)
    let rect = NSRect(x: 0, y: 0, width: s, height: s)
    // small sizes use almost the full square so the glyph stays legible
    // macOS icons sit on an 824/1024 grid with transparent margins
    let inset = macGrid ? s * 0.098 : (size <= 24 || fullBleed ? 0 : s * 0.06)
    let box = rect.insetBy(dx: inset, dy: inset)
    // iOS masks the corners itself and wants an opaque square
    let radius = fullBleed ? 0 : box.width * (macGrid ? 0.225 : 0.24)
    let path = NSBezierPath(roundedRect: box, xRadius: radius, yRadius: radius)
    let gradient = NSGradient(starting: bg.blended(withFraction: 0.18, of: .white)!, ending: bg)!
    gradient.draw(in: path, angle: -90)

    let cfg = NSImage.SymbolConfiguration(pointSize: s * 0.62, weight: .bold)
        .applying(.init(paletteColors: [boltColor, .white]))
    if let sym = NSImage(systemSymbolName: "bolt.shield.fill", accessibilityDescription: nil)?
        .withSymbolConfiguration(cfg) {
        let sz = sym.size
        let scale = min(box.width * 0.70 / sz.width, box.height * 0.70 / sz.height)
        let w = sz.width * scale, h = sz.height * scale
        sym.draw(in: NSRect(x: (s - w) / 2, y: (s - h) / 2, width: w, height: h))
    }
    NSGraphicsContext.restoreGraphicsState()
    return rep.representation(using: .png, properties: [:])!
}

func ico(_ pngs: [(Int, Data)]) -> Data {
    var d = Data()
    func u16(_ v: Int) { var x = UInt16(v).littleEndian; d.append(Data(bytes: &x, count: 2)) }
    func u32(_ v: Int) { var x = UInt32(v).littleEndian; d.append(Data(bytes: &x, count: 4)) }
    u16(0); u16(1); u16(pngs.count)
    var offset = 6 + 16 * pngs.count
    for (size, png) in pngs {
        d.append(UInt8(size >= 256 ? 0 : size)); d.append(UInt8(size >= 256 ? 0 : size))
        d.append(0); d.append(0)
        u16(1); u16(32); u32(png.count); u32(offset)
        offset += png.count
    }
    for (_, png) in pngs { d.append(png) }
    return d
}

let green = NSColor(srgbRed: 0.09, green: 0.64, blue: 0.29, alpha: 1)
let gray = NSColor(srgbRed: 0.42, green: 0.45, blue: 0.50, alpha: 1)
for (name, color) in [("kite", green), ("kite-off", gray)] {
    let pngs = sizes.map { ($0, render($0, bg: color)) }
    try! ico(pngs).write(to: URL(fileURLWithPath: "\(out)/\(name).ico"))
    try! pngs.last!.1.write(to: URL(fileURLWithPath: "\(out)/\(name)-256.png"))
}
if CommandLine.arguments.count > 3 {
    try! render(1024, bg: green, macGrid: true).write(to: URL(fileURLWithPath: CommandLine.arguments[3]))
}
if CommandLine.arguments.count > 2 {
    try! render(1024, bg: green, fullBleed: true).write(to: URL(fileURLWithPath: CommandLine.arguments[2]))
}
print("ok")

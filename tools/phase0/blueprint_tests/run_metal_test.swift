// usage: swift run_metal_test.swift <test_data_dir> <sog_golden_test.metallib>
// Dispatches sog_golden_test over every splat on the default GPU and reports the worst error per field
// against the float64 reference written by export_test_data.py. Exits non-zero if a tolerance fails.
import Foundation
import Metal

let args = CommandLine.arguments
let dir = URL(fileURLWithPath: args[1])
func load(_ name: String) -> Data { try! Data(contentsOf: dir.appendingPathComponent(name)) }

guard let device = MTLCreateSystemDefaultDevice() else { fatalError("no Metal device") }
let library = try device.makeLibrary(URL: URL(fileURLWithPath: args[2]))
let pipeline = try device.makeComputePipelineState(function: library.makeFunction(name: "sog_golden_test")!)

let records = load("records.bin")
let n = records.count / 20
func buffer(_ d: Data) -> MTLBuffer {
    d.withUnsafeBytes { device.makeBuffer(bytes: $0.baseAddress!, length: d.count, options: .storageModeShared)! }
}
let out = device.makeBuffer(length: n * 24 * MemoryLayout<Float>.size, options: .storageModeShared)!

let queue = device.makeCommandQueue()!
let cmd = queue.makeCommandBuffer()!
let enc = cmd.makeComputeCommandEncoder()!
enc.setComputePipelineState(pipeline)
enc.setBuffer(buffer(records), offset: 0, index: 0)
enc.setBuffer(buffer(load("constants.bin")), offset: 0, index: 1)
enc.setBuffer(buffer(load("scale_lut.bin")), offset: 0, index: 2)
enc.setBuffer(buffer(load("dc_lut.bin")), offset: 0, index: 3)
enc.setBuffer(buffer(load("palette_half4.bin")), offset: 0, index: 4)
enc.setBuffer(buffer(load("dir.bin")), offset: 0, index: 5)
enc.setBuffer(out, offset: 0, index: 6)
enc.dispatchThreads(MTLSize(width: n, height: 1, depth: 1),
                    threadsPerThreadgroup: MTLSize(width: min(256, pipeline.maxTotalThreadsPerThreadgroup), height: 1, depth: 1))
enc.endEncoding()
cmd.commit()
cmd.waitUntilCompleted()
if let error = cmd.error { fatalError("GPU error: \(error)") }

let got = out.contents().bindMemory(to: Float.self, capacity: n * 24)
let expected = load("expected.bin")

// name, first column, width, tolerance, relative (divide by max(|ref|, floor))
let fields: [(String, Int, Int, Double, Bool, Double)] = [
    ("position (m)", 0, 3, 2e-5, false, 0), ("quat (w,x,y,z)", 3, 4, 1e-5, false, 0),
    ("scale", 7, 3, 1e-5, true, 1e-12), ("opacity", 10, 1, 1e-6, false, 0),
    ("color_dc", 11, 3, 1e-5, false, 0), ("color_sh3 (half palette)", 14, 3, 5e-3, false, 0),
    ("label", 17, 1, 0, false, 0), ("covariance local (cm^2)", 18, 6, 1e-4, true, 1e-9),
]
var failed = false
var report: [String: Any] = ["splats": n, "gpu": device.name,
                             "gpu_ms": (cmd.gpuEndTime - cmd.gpuStartTime) * 1000]
expected.withUnsafeBytes { raw in
    let ref = raw.bindMemory(to: Float.self)
    for (name, col, width, tol, relative, floor) in fields {
        var worst = 0.0
        for i in 0..<n {
            var denom = 1.0
            if relative {
                // relative to the largest magnitude in this splat's field (e.g. the covariance diagonal)
                var m = floor
                for c in 0..<width { m = max(m, abs(Double(ref[i * 24 + col + c]))) }
                denom = m
            }
            for c in 0..<width {
                let e = abs(Double(got[i * 24 + col + c]) - Double(ref[i * 24 + col + c])) / denom
                if e > worst { worst = e }
            }
        }
        report[name] = ["max_error": worst, "tolerance": tol, "relative": relative, "pass": worst <= tol]
        if worst > tol { failed = true }
    }
}
let json = try JSONSerialization.data(withJSONObject: report, options: [.prettyPrinted, .sortedKeys])
print(String(data: json, encoding: .utf8)!)
exit(failed ? 1 : 0)

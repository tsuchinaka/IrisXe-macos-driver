// IrisXe Manager - a small window for installing and repairing the Iris Xe driver.
// The work is done by irisxe-manager.sh (bundled in the app's Resources); this file is only the window.
// Built by manager/build.sh (GitHub Actions puts "IrisXe Manager.app" into the build artifact's out/ folder).
import SwiftUI
import AppKit

struct Part: Identifiable {
    let id: String          // the script's name for it
    let title: String
    let note: String
    var path = ""
    var date = ""
    var chosen = false
}

struct StatusRow: Identifiable {
    let id = UUID()
    let name: String
    let value: String
    let extra: String
    var warn: Bool {
        let v = value.lowercased()
        return v.contains("not installed") || v.contains("locked") || v.contains("parked") || v.contains("missing")
            || v.hasPrefix("off") || v.contains("verbose")
    }
}

final class Model: ObservableObject {
    @Published var folder = ""
    @Published var parts: [Part] = [
        Part(id: "gpu", title: "Display driver (NMXGPU.kext)", note: "screen, brightness, sleep - restart needed"),
        Part(id: "accel", title: "Accelerator (NMXAccel.kext)", note: "the right one for this macOS is picked - restart needed"),
        Part(id: "agdc", title: "Display helper (NVRMAGDC.kext)", note: "restart needed"),
        Part(id: "plugin", title: "Metal plugin - update (quiet)", note: "replaces only the plugin program"),
        Part(id: "fullplugin", title: "Metal plugin - full (Mesa build)", note: "first install on a new macOS, includes the shader reader"),
        Part(id: "xlate", title: "Shader translator", note: "libnvmtl_translate.dylib"),
        Part(id: "autoarm", title: "Acceleration at every boot (autoarm)", note: "comes with this app"),
    ]
    @Published var status: [StatusRow] = []
    @Published var log = ""
    @Published var busy = false

    let script: String = Bundle.main.path(forResource: "irisxe-manager", ofType: "sh") ?? ""

    init() {
        // when the app sits inside a downloaded build (out/IrisXe Manager.app), that build is the default source
        let here = Bundle.main.bundleURL.deletingLastPathComponent()
        if FileManager.default.fileExists(atPath: here.appendingPathComponent("NMXGPU.kext").path) {
            folder = here.deletingLastPathComponent().path
        }
        refresh()
        if !folder.isEmpty { scan() }
    }

    // ---- running the script ----
    private func shellQuote(_ s: String) -> String { "'" + s.replacingOccurrences(of: "'", with: "'\\''") + "'" }

    func run(_ args: [String], admin: Bool, done: @escaping (String) -> Void) {
        let cmd = (["/bin/bash", script] + args).map(shellQuote).joined(separator: " ") + " 2>&1"
        DispatchQueue.global(qos: .userInitiated).async {
            let p = Process()
            if admin {
                // osascript shows macOS's own password prompt; the command then runs as root
                let asCmd = cmd.replacingOccurrences(of: "\\", with: "\\\\").replacingOccurrences(of: "\"", with: "\\\"")
                p.executableURL = URL(fileURLWithPath: "/usr/bin/osascript")
                p.arguments = ["-e", "do shell script \"\(asCmd)\" with administrator privileges"]
            } else {
                p.executableURL = URL(fileURLWithPath: "/bin/bash")
                p.arguments = ["-c", cmd]
            }
            let pipe = Pipe()
            p.standardOutput = pipe
            p.standardError = pipe
            var out = ""
            do {
                try p.run()
                let data = pipe.fileHandleForReading.readDataToEndOfFile()
                p.waitUntilExit()
                out = String(data: data, encoding: .utf8) ?? ""
                if admin && p.terminationStatus != 0 && out.contains("-128") { out = "cancelled (no password given)\n" }
            } catch {
                out = "could not run the helper: \(error.localizedDescription)\n"
            }
            DispatchQueue.main.async { done(out) }
        }
    }

    func appendLog(_ s: String) {
        let stamp = DateFormatter.localizedString(from: Date(), dateStyle: .none, timeStyle: .medium)
        log += "[\(stamp)]\n" + s + (s.hasSuffix("\n") ? "" : "\n")
    }

    // ---- actions ----
    func refresh() {
        run(["status"], admin: false) { out in
            self.status = out.split(separator: "\n").compactMap { line in
                let f = line.split(separator: "|", omittingEmptySubsequences: false).map(String.init)
                guard f.count >= 2 else { return nil }
                return StatusRow(name: f[0], value: f[1], extra: f.count > 2 ? f[2] : "")
            }
        }
    }

    func chooseFolder() {
        let panel = NSOpenPanel()
        panel.canChooseDirectories = true
        panel.canChooseFiles = false
        panel.allowsMultipleSelection = false
        panel.message = "Choose the unzipped build you downloaded from GitHub (the folder that contains 'out')"
        if panel.runModal() == .OK, let url = panel.url {
            folder = url.path
            scan()
        }
    }

    func scan() {
        guard !folder.isEmpty else { return }
        run(["scan", folder], admin: false) { out in
            for line in out.split(separator: "\n") {
                let kv = line.split(separator: "=", maxSplits: 1, omittingEmptySubsequences: false).map(String.init)
                guard kv.count == 2, let i = self.parts.firstIndex(where: { $0.id == kv[0] }) else { continue }
                let pd = kv[1].split(separator: "|", omittingEmptySubsequences: false).map(String.init)
                self.parts[i].path = pd.first ?? ""
                self.parts[i].date = pd.count > 1 ? pd[1] : ""
                if self.parts[i].path.isEmpty { self.parts[i].chosen = false }
            }
        }
    }

    func chooseAllFound() {
        for i in parts.indices {
            // the full plugin is only for a first install; the quiet update is the everyday one
            parts[i].chosen = !parts[i].path.isEmpty && parts[i].id != "fullplugin"
        }
    }

    func install() {
        let chosen = parts.filter { $0.chosen && !$0.path.isEmpty }.map { $0.id }
        guard !chosen.isEmpty else { appendLog("Nothing chosen."); return }
        busy = true
        appendLog("Installing: " + chosen.joined(separator: ", "))
        run(["install", folder] + chosen, admin: true) { out in
            self.appendLog(out)
            self.busy = false
            self.refresh()
        }
    }

    func repair() {
        busy = true
        run(["repair"], admin: true) { out in self.appendLog(out); self.busy = false; self.refresh() }
    }

    func armNow() {
        busy = true
        run(["armnow"], admin: true) { out in self.appendLog(out); self.busy = false }
    }

    func restartMac() {
        let a = NSAlert()
        a.messageText = "Restart the Mac now?"
        a.informativeText = "Save your work first - open apps will be closed."
        a.addButton(withTitle: "Restart")
        a.addButton(withTitle: "Cancel")
        guard a.runModal() == .alertFirstButtonReturn else { return }
        let p = Process()
        p.executableURL = URL(fileURLWithPath: "/usr/bin/osascript")
        p.arguments = ["-e", "do shell script \"/sbin/shutdown -r now\" with administrator privileges"]
        try? p.run()
    }
}

struct ContentView: View {
    @StateObject private var m = Model()

    var body: some View {
        VStack(alignment: .leading, spacing: 12) {
            HStack {
                Text("Iris Xe Driver").font(.title2).bold()
                Spacer()
                if m.busy { ProgressView().controlSize(.small) }
                Button { m.refresh(); m.scan() } label: { Label("Refresh", systemImage: "arrow.clockwise") }
            }

            GroupBox("Now") {
                VStack(alignment: .leading, spacing: 4) {
                    ForEach(m.status) { row in
                        HStack(alignment: .firstTextBaseline) {
                            Text(row.name).frame(width: 90, alignment: .leading).foregroundStyle(.secondary)
                            Text(row.value).foregroundStyle(row.warn ? Color.orange : Color.primary)
                            if !row.extra.isEmpty { Text(row.extra).foregroundStyle(.secondary).lineLimit(1).truncationMode(.tail) }
                            Spacer()
                        }
                        .font(.system(.callout, design: .default))
                    }
                    if m.status.isEmpty { Text("reading...").foregroundStyle(.secondary) }
                }
                .frame(maxWidth: .infinity, alignment: .leading)
                .padding(4)
            }

            GroupBox("Install from a downloaded build") {
                VStack(alignment: .leading, spacing: 6) {
                    HStack {
                        Text(m.folder.isEmpty ? "No folder chosen" : m.folder)
                            .lineLimit(1).truncationMode(.middle).foregroundStyle(m.folder.isEmpty ? .secondary : .primary)
                        Spacer()
                        Button("Choose Folder...") { m.chooseFolder() }
                    }
                    Divider()
                    ForEach($m.parts) { $p in
                        HStack(alignment: .firstTextBaseline) {
                            Toggle(isOn: $p.chosen) {
                                VStack(alignment: .leading, spacing: 1) {
                                    Text(p.title)
                                    Text(p.path.isEmpty ? "not in this folder" : "\(p.note)  -  \(p.date)")
                                        .font(.caption).foregroundStyle(.secondary)
                                }
                            }
                            .disabled(p.path.isEmpty)
                            Spacer()
                        }
                    }
                    HStack {
                        Button("Choose Everything Found") { m.chooseAllFound() }.disabled(m.folder.isEmpty)
                        Spacer()
                        Button("Install Selected") { m.install() }
                            .keyboardShortcut(.defaultAction)
                            .disabled(m.busy || !m.parts.contains { $0.chosen })
                    }
                }
                .padding(4)
            }

            GroupBox("Fix") {
                HStack {
                    Button("Clear Safety Locks") { m.repair() }.help("After a hang: lets the display driver and acceleration start again at the next boot")
                    Button("Turn Acceleration On Now") { m.armNow() }.help("Restarts the desktop - open apps close")
                    Spacer()
                    Button("Restart Mac") { m.restartMac() }
                }
                .disabled(m.busy)
                .padding(4)
            }

            GroupBox("Log") {
                ScrollView {
                    Text(m.log.isEmpty ? "Nothing done yet." : m.log)
                        .font(.system(.caption, design: .monospaced))
                        .frame(maxWidth: .infinity, alignment: .leading)
                        .textSelection(.enabled)
                }
                .frame(minHeight: 120)
            }
        }
        .padding(16)
        .frame(minWidth: 620, minHeight: 720)
    }
}

@main
struct IrisXeManagerApp: App {
    var body: some Scene {
        WindowGroup("IrisXe Manager") { ContentView() }
            .windowResizability(.contentMinSize)
    }
}

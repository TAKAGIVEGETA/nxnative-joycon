// Mac app launcher: the settings window shown before the game starts, in the launcher's own process (launcher.c calls dm_mac_launcher before it starts the host). Its settings come from the
// app's Resources/dm/launcher.json (dm/games/settings.py with SETTINGS_PLATFORM=mac); the model and the rows are the iOS launcher's (dm/ios/Sources/LauncherSettings.swift, LauncherRow.swift),
// and so is the store: launcher.<VAR> in the app's preferences, which launcher.c reads when the window is not shown (docs/frontends.md, Saved choices). Basic settings (level basic) come
// first, the others under Advanced settings, each in the list's sections. Play puts the settings into this process's environment, which the host inherits; Quit and closing the window end the
// app without the game. "Show this window at launch" off: no window unless Option is held at launch, and launcher.c applies the saved choices. The window never takes focus by itself: an app
// opened in the background (open -g) keeps its window behind until the player switches to it
import AppKit
import QuartzCore
import SwiftUI
import IOKit.hid
import GameController

private func info(_ key: String) -> String? { Bundle.main.object(forInfoDictionaryKey: key) as? String }

/// The host's comparison of the two saves (dm/host/save_sync.c). This launcher links save_sync.o but imports no C header (pack_app.py builds it with swiftc and no bridging
/// header), so the symbol is declared here.
@_silgen_name("save_sync_report")
private func save_sync_report(_ save: UnsafePointer<CChar>, _ title: UnsafePointer<CChar>, _ folder: UnsafePointer<CChar>?, _ waitMs: Int32,
                              _ out: UnsafeMutablePointer<CChar>, _ n: Int) -> Int32

/// Cloud saves before Play: this Mac's save against the cloud's (SaveSyncReport, shared with the iPhone launcher), and the question it needs, as alerts.
/// It runs with the window's Play and, with the window off, before dmMacLauncher returns; DM_LAUNCHER=0 asks nothing (the host takes the newer save).
enum SaveCheck {
    enum Outcome { case play(String?), retry, cancel }

    /// Blocks while the cloud's file comes (SaveSyncReport.waitMs at most): call it off the main thread. nil: no report (sync is then left to the host as before)
    static func report() -> SaveSyncReport? {
        // getenv, not ProcessInfo.environment: launcher.c sets DM_SAVE and applies the env files in this process before the window
        guard let save = getenv("DM_SAVE") else { fputs("launcher: save sync: no DM_SAVE\n", stderr); return nil }
        guard let id = info("DMTitleID"), !id.isEmpty else { fputs("launcher: save sync: no DMTitleID\n", stderr); return nil }
        // the folder as launcher.c will pass it (cloud_saves_folder): DM_SAVE_SYNC from the launch environment or an env file, else the window's choice; "0" = off
        let folder = getenv("DM_SAVE_SYNC").map { String(cString: $0) } ?? (UserDefaults.standard.string(forKey: "cloudSaves.folder") ?? "")
        var buf = [CChar](repeating: 0, count: 1024)
        let r = (folder.isEmpty ? "0" : folder).withCString { save_sync_report(save, id, $0, SaveSyncReport.waitMs, &buf, buf.count) }
        let line = String(cString: buf)
        guard r == 0 else { fputs("launcher: save sync: no report on \(String(cString: save))\n", stderr); return nil }
        fputs("launcher: save sync: \(line)\n", stderr)
        return SaveSyncReport(line: line)
    }

    /// The question the report needs, as a modal alert; `cancel` names the way back ("Cancel" to the window, "Quit" without one)
    static func ask(_ r: SaveSyncReport?, cancel: String) -> Outcome {
        switch SaveSyncReport.step(r) {
        case .play(let choice): return .play(choice)
        case .choose:
            guard let r else { return .play(nil) }
            let a = NSAlert()
            a.messageText = SaveSyncReport.chooseTitle
            a.informativeText = r.chooseMessage(here: "Mac")
            a.alertStyle = .warning
            a.addButton(withTitle: r.localLabel(here: "Mac"))
            a.addButton(withTitle: r.cloudLabel)
            a.addButton(withTitle: cancel)
            a.buttons[0].keyEquivalent = ""   // no default: Return must not pick a save (the newer is not always the one to keep)
            a.buttons[2].keyEquivalent = "\u{1b}"
            switch a.runModal() {
            case .alertFirstButtonReturn: return .play(r.localChoice)
            case .alertSecondButtonReturn: return .play(r.cloudChoice)
            default: return .cancel
            }
        case .unreadable:
            let a = NSAlert()
            a.messageText = SaveSyncReport.unreadableTitle
            a.informativeText = r?.unreadableMessage(here: "Mac") ?? ""
            a.addButton(withTitle: "Play This Mac's Save")
            a.addButton(withTitle: "Try Again")
            a.addButton(withTitle: cancel)
            a.buttons[2].keyEquivalent = "\u{1b}"
            switch a.runModal() {
            case .alertFirstButtonReturn: return .play("offline")   // the mount does not wait for the cloud again
            case .alertSecondButtonReturn: return .retry
            default: return .cancel
            }
        }
    }

    static func apply(_ choice: String?) {
        guard let choice else { return }
        setenv("DM_SAVE_SYNC_CHOICE", choice, 1)
        fputs("launcher: DM_SAVE_SYNC_CHOICE=\(choice)\n", stderr)
    }

    /// With the window off: the check and its question before the game, the app in front only while it asks. false: the player quit
    static func withoutWindow() -> Bool {
        final class Box: @unchecked Sendable { let lock = NSLock(); var report: SaveSyncReport?; var waiting = false }
        let app = NSApplication.shared
        defer { app.setActivationPolicy(.prohibited) }
        while true {
            let box = Box(), sem = DispatchSemaphore(value: 0)
            DispatchQueue.global(qos: .userInitiated).async {
                let r = report()
                box.lock.lock(); box.report = r; box.lock.unlock()
                sem.signal()
                DispatchQueue.main.async { if box.waiting { app.abortModal() } }   // runs inside the waiting alert's modal loop, if there is one
            }
            if sem.wait(timeout: .now() + 0.5) == .timedOut {   // a cloud file to download: say so, and let the player go on without it
                app.setActivationPolicy(.regular); app.activate(ignoringOtherApps: true)
                let a = NSAlert()
                a.messageText = "Checking the cloud save…"
                a.informativeText = "Waiting for iCloud to deliver this game's save."
                a.addButton(withTitle: "Play Without It")
                a.addButton(withTitle: "Quit")
                box.waiting = true
                let answer = a.runModal()
                box.waiting = false
                if answer == .alertFirstButtonReturn { apply("offline"); return true }
                if answer == .alertSecondButtonReturn { return false }
                sem.wait()   // aborted: the report is in
            }
            box.lock.lock(); let r = box.report; box.lock.unlock()
            if case .play(let choice) = SaveSyncReport.step(r) { apply(choice); return true }
            app.setActivationPolicy(.regular); app.activate(ignoringOtherApps: true)
            switch ask(r, cancel: "Quit") {
            case .play(let choice): apply(choice); return true
            case .retry: continue
            case .cancel: return false
            }
        }
    }
}

struct LauncherView: View {
    @ObservedObject var m: LauncherModel
    @ObservedObject private var gyroCalibrator = GamepadGyroCalibrator.shared
    let done: (Bool) -> Void
    let fit: (CGFloat) -> Void
    @AppStorage("window.show") private var show = true
    @AppStorage("window.advanced") private var advanced = false
    @State private var checking = false
    @State private var check = 0   // which Play the running check is for: one the player went on from without it is dropped

    var body: some View {
        VStack(spacing: 0) {
            HStack(spacing: 12) {
                Image(nsImage: NSApp.applicationIconImage).resizable().frame(width: 64, height: 64).accessibilityHidden(true)
                VStack(alignment: .leading, spacing: 2) {
                    Text(info("CFBundleName") ?? "Game").font(.title2.weight(.semibold))
                    Text("Version \(info("CFBundleShortVersionString") ?? "1.0")").foregroundStyle(.secondary)
                }
                Spacer()
            }
            .padding([.horizontal, .top], 20)
            Form {
                settingSections(basic: true)
                CloudSavesSection()
                GyroCalSection(calibrator: gyroCalibrator)
                Section {
                    DisclosureGroup("Advanced settings", isExpanded: $advanced) {}
                }
                if advanced {
                    settingSections(basic: false)
                    Section {
                        Button("Reset to tested defaults") { m.reset() }
                            .disabled(!m.changed)
                    } footer: {
                        Text("Brings back all settings this game was tested with.")
                    }
                }
            }
            .formStyle(.grouped)
            .onScrollGeometryChange(for: CGFloat.self) {
                $0.contentSize.height + $0.contentInsets.top + $0.contentInsets.bottom - $0.containerSize.height
            } action: { _, extraHeight in
                fit(extraHeight)
            }
            HStack {
                Toggle("Show this window at launch", isOn: $show).toggleStyle(.checkbox)
                Spacer()
                if checking {
                    ProgressView().controlSize(.small)
                    Text("Checking the cloud save…").foregroundStyle(.secondary)
                    Button("Play Without It") { checking = false; SaveCheck.apply("offline"); done(true) }
                }
                Button("Quit") { done(false) }
                Button("Play") { start() }.keyboardShortcut(.defaultAction).disabled(checking)
            }
            .padding(20)
        }
        .frame(minWidth: 520, idealWidth: 560)
    }

    /// Play, once the cloud's save is compared with this Mac's: a save that changed on both sides is the player's pick, not the newer file's (SaveCheck)
    private func start() {
        check += 1; checking = true
        let mine = check
        DispatchQueue.global(qos: .userInitiated).async {
            let r = SaveCheck.report()
            DispatchQueue.main.async {
                guard checking, check == mine else { return }   // the player played without it
                checking = false
                switch SaveCheck.ask(r, cancel: "Cancel") {
                case .play(let choice): SaveCheck.apply(choice); done(true)
                case .retry: start()
                case .cancel: break   // back to the window, nothing chosen
                }
            }
        }
    }

    @ViewBuilder private func settingSections(basic: Bool) -> some View {
        ForEach(m.sections(basic: basic), id: \.self) { section in
            let rows = m.rows(in: section, basic: basic)
            if !rows.isEmpty {
                Section(section) {
                    ForEach(rows) { s in
                        if s.env == "DM_METAL_HUD" {
                            PerformanceHudRow(s: s, m: m)
                        } else {
                            SettingRow(s: s, m: m)
                        }
                    }
                }
            }
        }
    }
}

/// Cloud saves: a folder the save is synced through (dm/host/save_sync.h), the one the iPhone app picks in its launcher's Cloud saves section. It is kept in this app's
/// preferences as cloudSaves.folder, which launcher.c puts into DM_SAVE_SYNC before the host starts; a DM_SAVE_SYNC an env file or the launch environment set wins, and is shown here.
struct CloudSavesSection: View {
    @AppStorage("cloudSaves.folder") private var folder = ""
    private let fixed: String? = {
        let env = ProcessInfo.processInfo.environment
        let keys = ((env["DM_ENV_FILE_KEYS"] ?? "") + " " + (env["DM_ENV_LAUNCH_KEYS"] ?? "")).split(separator: " ")
        return keys.contains("DM_SAVE_SYNC") || env["DM_SAVE_SYNC"] != nil ? env["DM_SAVE_SYNC"] : nil
    }()

    var body: some View {
        Section {
            LabeledContent("Cloud saves") {
                Text(fixed.map { "\(Self.shown($0)) (env.txt)" } ?? (folder.isEmpty ? "Off" : Self.shown(folder)))
                    .foregroundStyle(.secondary).lineLimit(1).truncationMode(.head)
            }
            if fixed == nil {
                HStack {
                    Button(folder.isEmpty ? "Choose a folder…" : "Choose a different folder…", action: choose)
                    if !folder.isEmpty { Button("Turn off") { folder = "" } }
                }
            }
        } footer: {
            Text("Choose the iCloud Drive folder the iPhone app uses for this game. The save comes in when the game starts and goes out after each save; a save that is replaced is kept in the save's -backups folder.")
        }
    }

    private func choose() {
        let panel = NSOpenPanel()
        panel.canChooseDirectories = true; panel.canChooseFiles = false; panel.allowsMultipleSelection = false; panel.canCreateDirectories = true
        panel.prompt = "Use for Cloud Saves"
        panel.message = "The folder the iPhone app chose in its launcher's Cloud saves"
        panel.directoryURL = URL(fileURLWithPath: folder.isEmpty ? Self.drive : folder)
        if panel.runModal() == .OK, let url = panel.url { folder = url.path }
    }

    static let drive = NSHomeDirectory() + "/Library/Mobile Documents/com~apple~CloudDocs"   // iCloud Drive, as the Finder shows it

    /// A path under iCloud Drive as the Finder and the Files app name it: iCloud Drive › Documents › dev › Zelda; any other path as it is
    static func shown(_ path: String) -> String {
        guard path == drive || path.hasPrefix(drive + "/") else { return path }
        return (["iCloud Drive"] + path.dropFirst(drive.count).split(separator: "/").map(String.init)).joined(separator: " › ")
    }
}

enum GyroCalPhase: Equatable {
    case idle
    case countdown
    case sampling
    case done
    case moved
    case noData
}

struct GyroCalSection: View {
    @ObservedObject var calibrator: GamepadGyroCalibrator

    var body: some View {
        Section {
            VStack(alignment: .leading, spacing: 8) {
                HStack {
                    Image(systemName: "gyroscope")
                    Text("Gyroscope Calibration (Standard Controller)")
                        .font(.headline)
                    Spacer()
                    if let ctrl = calibrator.connectedControllerName {
                        Text(ctrl).font(.caption).foregroundStyle(.secondary)
                    } else {
                        Text("No motion controller connected").font(.caption).foregroundStyle(.secondary)
                    }
                }

                HStack {
                    statusView
                    Spacer()
                }

                HStack(spacing: 12) {
                    Button(calibrating ? "Calibrating..." : "Calibrate (5s countdown)") {
                        calibrator.startCalibration()
                    }
                    .disabled(calibrating)

                    if calibrator.biasDeg != nil {
                        Button("Clear Calibration", role: .destructive) {
                            calibrator.clearCalibration()
                        }
                        .disabled(calibrating)
                    }
                }
                .padding(.top, 2)
            }
            .padding(.vertical, 4)
        } header: {
            Text("Gamepad Calibration")
        } footer: {
            Text("Calibrates DualSense, DualShock 4, Switch Pro, 8BitDo, and other standard gamepads. Leave the controller completely still on a flat surface during the 5s countdown and 3s sampling. The measured bias is saved and subtracted in game to eliminate drift. Joy-Con uses its factory SPI calibration and is not affected.")
        }
    }

    private var calibrating: Bool {
        calibrator.phase == .countdown || calibrator.phase == .sampling
    }

    @ViewBuilder
    private var statusView: some View {
        switch calibrator.phase {
        case .countdown:
            Text(String(format: "Countdown: %d s (keep controller still on a flat surface…)", Int(ceil(calibrator.secondsLeft))))
                .foregroundColor(.yellow)
                .font(.callout.monospacedDigit())
        case .sampling:
            Text(String(format: "Sampling gyro bias (%.1f s remaining, do not touch)…", calibrator.secondsLeft))
                .foregroundColor(.cyan)
                .font(.callout.monospacedDigit())
        case .done:
            Text("Calibration complete and saved")
                .foregroundColor(.green)
                .font(.callout)
        case .moved:
            Text("Calibration failed: controller moved, please retry")
                .foregroundColor(.red)
                .font(.callout)
        case .noData:
            Text("Calibration failed: no gyroscope data received")
                .foregroundColor(.red)
                .font(.callout)
        case .idle:
            if let b = calibrator.biasDeg {
                let total = sqrt(b.0 * b.0 + b.1 * b.1 + b.2 * b.2)
                Text(String(format: "Calibrated (bias: %.2f°/s  [%+.2f, %+.2f, %+.2f])", total, b.0, b.1, b.2))
                    .foregroundColor(.green)
                    .font(.callout.monospacedDigit())
            } else {
                Text("Not calibrated (bias: 0.00°/s)")
                    .foregroundColor(.secondary)
                    .font(.callout)
            }
        }
    }
}

struct PerformanceHudRow: View {
    let s: Setting
    @ObservedObject var m: LauncherModel

    enum HudPreset: String, CaseIterable, Identifiable {
        case off = "0"
        case fpsOnly = "fps"
        case fpsMemory = "fps_memory"
        case full = "full"
        case custom = "custom"

        var id: String { rawValue }
        var title: String {
            switch self {
            case .off: return "Off"
            case .fpsOnly: return "FPS Only"
            case .fpsMemory: return "FPS + Memory"
            case .full: return "Full (Default)"
            case .custom: return "Custom…"
            }
        }
    }

    @AppStorage("launcher.MTL_HUD_PRESET") private var presetRaw = "0"
    @AppStorage("launcher.MTL_HUD_ELEMENTS") private var customElements = "fps"
    @AppStorage("launcher.MTL_HUD_ALIGNMENT") private var alignment = "12"
    @AppStorage("launcher.MTL_HUD_SCALE") private var scaleStr = "0.117461"

    private var locked: Bool { m.fixed.contains(s.env) || m.fixed.contains("MTL_HUD_ELEMENTS") }

    private var currentPreset: Binding<HudPreset> {
        Binding(
            get: {
                if (m.values["DM_METAL_HUD"] ?? s.def) == "0" {
                    return .off
                }
                return HudPreset(rawValue: presetRaw) ?? .fpsOnly
            },
            set: { next in
                presetRaw = next.rawValue
                switch next {
                case .off:
                    m.values["DM_METAL_HUD"] = "0"
                case .fpsOnly:
                    m.values["DM_METAL_HUD"] = "1"
                    customElements = "fps"
                    scaleStr = "0.117461"
                    alignment = "12"
                case .fpsMemory:
                    m.values["DM_METAL_HUD"] = "1"
                    customElements = "memory,fps"
                    scaleStr = "0.147233"
                    alignment = "12"
                case .full:
                    m.values["DM_METAL_HUD"] = "1"
                    customElements = "default"
                    scaleStr = "0.200000"
                case .custom:
                    m.values["DM_METAL_HUD"] = "1"
                }
                syncEnv()
            }
        )
    }

    private var scaleBinding: Binding<Double> {
        Binding(
            get: { Double(scaleStr) ?? 0.12 },
            set: {
                scaleStr = String(format: "%.6f", $0)
                syncEnv()
            }
        )
    }

    var body: some View {
        VStack(alignment: .leading, spacing: 8) {
            Picker(selection: currentPreset) {
                ForEach(HudPreset.allCases) { p in
                    Text(p.title).tag(p)
                }
            } label: {
                VStack(alignment: .leading, spacing: 4) {
                    Text("Performance HUD")
                    if locked {
                        Text(m.fromLaunch.contains(s.env) || m.fromLaunch.contains("MTL_HUD_ELEMENTS") ? "Set at launch" : "Set by an env file")
                            .font(.footnote).foregroundStyle(.secondary)
                    } else if currentPreset.wrappedValue == .off {
                        Text("Shows Apple Metal HUD metrics during gameplay.").font(.footnote).foregroundStyle(.secondary)
                    }
                }
            }
            .pickerStyle(.menu)
            .disabled(locked)

            if !locked && currentPreset.wrappedValue != .off {
                VStack(alignment: .leading, spacing: 6) {
                    HStack {
                        Text("Position").font(.subheadline)
                        Spacer()
                        Picker("", selection: $alignment) {
                            Text("Top Right").tag("12")
                            Text("Top Left").tag("9")
                            Text("Bottom Right").tag("6")
                            Text("Bottom Left").tag("3")
                        }
                        .pickerStyle(.menu)
                        .frame(width: 150)
                        .onChange(of: alignment) { _, _ in syncEnv() }
                    }

                    HStack {
                        Text("Scale").font(.subheadline)
                        Spacer()
                        Slider(value: scaleBinding, in: 0.08...0.30, step: 0.01)
                            .frame(width: 140)
                        Text(String(format: "%.2f", scaleBinding.wrappedValue))
                            .font(.caption.monospacedDigit())
                            .frame(width: 36, alignment: .trailing)
                    }

                    if currentPreset.wrappedValue == .custom {
                        HStack {
                            Text("Elements").font(.subheadline)
                            Spacer()
                            TextField("e.g. fps, memory,fps", text: $customElements)
                                .textFieldStyle(.roundedBorder)
                                .frame(width: 180)
                                .onChange(of: customElements) { _, _ in syncEnv() }
                        }
                    }
                }
                .padding(.leading, 8)
                .padding(.vertical, 4)
            }
        }
        .onAppear {
            loadInitialState()
        }
        .onChange(of: m.values["DM_METAL_HUD"]) { _, val in
            if (val ?? s.def) == "0" && presetRaw != "0" {
                presetRaw = "0"
                syncEnv()
            }
        }
    }

    private func loadInitialState() {
        guard !locked else { return }
        let env = ProcessInfo.processInfo.environment
        if let elem = env["MTL_HUD_ELEMENTS"], !elem.isEmpty {
            customElements = elem
            if elem == "fps" { presetRaw = "fps" }
            else if elem == "memory,fps" || elem == "fps,memory" { presetRaw = "fps_memory" }
            else { presetRaw = "custom" }
        }
        if let align = env["MTL_HUD_ALIGNMENT"], !align.isEmpty {
            alignment = align
        }
        if let sc = env["MTL_HUD_SCALE"], !sc.isEmpty {
            scaleStr = sc
        }
        if (m.values["DM_METAL_HUD"] ?? s.def) == "1" && presetRaw == "0" {
            presetRaw = "fps"
        }
        syncEnv()
    }

    private func syncEnv() {
        guard !locked else { return }
        let preset = currentPreset.wrappedValue
        if preset == .off {
            m.values["DM_METAL_HUD"] = "0"
            setenv("DM_METAL_HUD", "0", 1)
            setenv("MTL_HUD_ENABLED", "0", 1)
            unsetenv("MTL_HUD_ELEMENTS")
            unsetenv("MTL_HUD_ALIGNMENT")
            unsetenv("MTL_HUD_SCALE")
            UserDefaults.standard.removeObject(forKey: "launcher.MTL_HUD_ELEMENTS")
            UserDefaults.standard.removeObject(forKey: "launcher.MTL_HUD_ALIGNMENT")
            UserDefaults.standard.removeObject(forKey: "launcher.MTL_HUD_SCALE")
        } else {
            m.values["DM_METAL_HUD"] = "1"
            setenv("DM_METAL_HUD", "1", 1)
            setenv("MTL_HUD_ENABLED", "1", 1)

            let elem = (preset == .full) ? "" : customElements
            if elem.isEmpty || elem == "default" {
                unsetenv("MTL_HUD_ELEMENTS")
                UserDefaults.standard.removeObject(forKey: "launcher.MTL_HUD_ELEMENTS")
            } else {
                setenv("MTL_HUD_ELEMENTS", elem, 1)
                UserDefaults.standard.set(elem, forKey: "launcher.MTL_HUD_ELEMENTS")
            }

            setenv("MTL_HUD_ALIGNMENT", alignment, 1)
            UserDefaults.standard.set(alignment, forKey: "launcher.MTL_HUD_ALIGNMENT")

            setenv("MTL_HUD_SCALE", scaleStr, 1)
            UserDefaults.standard.set(scaleStr, forKey: "launcher.MTL_HUD_SCALE")
        }
    }
}

final class GamepadGyroCalibrator: ObservableObject {
    static let shared = GamepadGyroCalibrator()

    @Published var connectedControllerName: String?
    @Published var phase: GyroCalPhase = .idle
    @Published var secondsLeft: Double = 0
    @Published var biasDeg: (Double, Double, Double)?

    private var sampleTimer: Timer?
    private var countdownTimer: Timer?
    private var phaseStart: Date = Date()
    private var samples: [(Double, Double, Double)] = []
    private var lastSampleTime: TimeInterval = 0

    private var proManager: IOHIDManager?
    private var proBuffers: [IOHIDDevice: UnsafeMutablePointer<UInt8>] = [:]

    init() {
        loadSavedBias()
    }

    func loadSavedBias() {
        var s = UserDefaults.standard.string(forKey: "DmGyroBias") ?? UserDefaults.standard.string(forKey: "launcher.DM_GYRO_BIAS")
        if (s == nil || s!.isEmpty), let envVal = ProcessInfo.processInfo.environment["DM_GYRO_BIAS"], !envVal.isEmpty {
            s = envVal
        }
        if let s = s {
            let parts = s.split(separator: ",").compactMap { Double($0.trimmingCharacters(in: .whitespaces)) }
            if parts.count == 3 {
                biasDeg = (parts[0], parts[1], parts[2])
                setenv("DM_GYRO_BIAS", s, 1)
            }
        }
    }

    func start() {
        loadSavedBias()
        NotificationCenter.default.addObserver(self, selector: #selector(controllersChanged), name: .GCControllerDidConnect, object: nil)
        NotificationCenter.default.addObserver(self, selector: #selector(controllersChanged), name: .GCControllerDidDisconnect, object: nil)
        attachGameControllers()
        updateControllerName()
        startProManager()
    }

    func stop() {
        NotificationCenter.default.removeObserver(self)
        detachGameControllers()
        stopProManager()
        countdownTimer?.invalidate()
        countdownTimer = nil
        sampleTimer?.invalidate()
        sampleTimer = nil
    }

    @objc private func controllersChanged() {
        DispatchQueue.main.async {
            self.attachGameControllers()
            self.updateControllerName()
        }
    }

    private func attachGameControllers() {
        for c in GCController.controllers() {
            if let name = c.vendorName, name.localizedCaseInsensitiveContains("Joy-Con") {
                continue
            }
            c.handlerQueue = .main
            if let m = c.motion {
                if m.sensorsRequireManualActivation { m.sensorsActive = true }
                m.valueChangedHandler = { [weak self] motion in
                    guard let self = self, self.phase == .sampling else { return }
                    let rot = motion.rotationRate
                    if rot.x == 0 && rot.y == 0 && rot.z == 0 { return }
                    self.onSample(x: rot.x, y: rot.y, z: rot.z)
                }
            }
        }
    }

    private func detachGameControllers() {
        for c in GCController.controllers() {
            c.motion?.valueChangedHandler = nil
        }
    }

    private func updateControllerName() {
        if !proBuffers.isEmpty {
            connectedControllerName = "Nintendo Switch Pro Controller"
            return
        }
        for c in GCController.controllers() {
            if let name = c.vendorName, name.localizedCaseInsensitiveContains("Joy-Con") {
                continue
            }
            if c.motion != nil {
                connectedControllerName = c.vendorName ?? "Gamepad"
                if let m = c.motion, m.sensorsRequireManualActivation {
                    m.sensorsActive = true
                }
                return
            }
        }
        connectedControllerName = nil
    }

    private func startProManager() {
        guard proManager == nil else { return }
        let mgr = IOHIDManagerCreate(kCFAllocatorDefault, IOOptionBits(kIOHIDOptionsTypeNone))
        self.proManager = mgr
        let match: [String: Any] = [kIOHIDVendorIDKey: 0x057E, kIOHIDProductIDKey: 0x2009]
        IOHIDManagerSetDeviceMatching(mgr, match as CFDictionary)
        let ctx = Unmanaged.passUnretained(self).toOpaque()
        IOHIDManagerRegisterDeviceMatchingCallback(mgr, { ctx, result, sender, dev in
            guard let ctx = ctx, result == kIOReturnSuccess else { return }
            let self_ = Unmanaged<GamepadGyroCalibrator>.fromOpaque(ctx).takeUnretainedValue()
            self_.proMatched(dev)
        }, ctx)
        IOHIDManagerRegisterDeviceRemovalCallback(mgr, { ctx, result, sender, dev in
            guard let ctx = ctx else { return }
            let self_ = Unmanaged<GamepadGyroCalibrator>.fromOpaque(ctx).takeUnretainedValue()
            self_.proRemoved(dev)
        }, ctx)
        IOHIDManagerScheduleWithRunLoop(mgr, CFRunLoopGetMain(), CFRunLoopMode.commonModes.rawValue)
        _ = IOHIDManagerOpen(mgr, IOOptionBits(kIOHIDOptionsTypeNone))
    }

    private func stopProManager() {
        guard let mgr = proManager else { return }
        IOHIDManagerRegisterDeviceMatchingCallback(mgr, nil, nil)
        IOHIDManagerRegisterDeviceRemovalCallback(mgr, nil, nil)
        IOHIDManagerClose(mgr, IOOptionBits(kIOHIDOptionsTypeNone))
        IOHIDManagerUnscheduleFromRunLoop(mgr, CFRunLoopGetMain(), CFRunLoopMode.commonModes.rawValue)
        self.proManager = nil
        for (dev, buf) in proBuffers {
            IOHIDDeviceRegisterInputReportCallback(dev, buf, 512, nil, nil)
            buf.deallocate()
        }
        proBuffers.removeAll()
    }

    private func proMatched(_ dev: IOHIDDevice) {
        if let syn = IOHIDDeviceGetProperty(dev, "GCSyntheticDevice" as CFString), CFGetTypeID(syn) == CFBooleanGetTypeID(), CFBooleanGetValue((syn as! CFBoolean)) {
            return
        }
        let buf = UnsafeMutablePointer<UInt8>.allocate(capacity: 512)
        proBuffers[dev] = buf
        let ctx = Unmanaged.passUnretained(self).toOpaque()
        IOHIDDeviceRegisterInputReportCallback(dev, buf, 512, { ctx, result, sender, type, reportId, report, len in
            guard let ctx = ctx, result == kIOReturnSuccess, reportId == 0x30, len >= 49 else { return }
            let self_ = Unmanaged<GamepadGyroCalibrator>.fromOpaque(ctx).takeUnretainedValue()
            let rad = 0.070 * Double.pi / 180.0
            for k in 0..<3 {
                let base = 13 + 12 * k
                let gxRaw = Int16(bitPattern: UInt16(report[base + 6]) | (UInt16(report[base + 7]) << 8))
                let gyRaw = Int16(bitPattern: UInt16(report[base + 8]) | (UInt16(report[base + 9]) << 8))
                let gzRaw = Int16(bitPattern: UInt16(report[base + 10]) | (UInt16(report[base + 11]) << 8))
                let rx = -Double(gyRaw) * rad
                let ry = Double(gxRaw) * rad
                let rz = Double(gzRaw) * rad
                self_.onSample(x: rx, y: ry, z: rz)
            }
        }, ctx)
        DispatchQueue.main.async {
            self.updateControllerName()
        }
    }

    private func proRemoved(_ dev: IOHIDDevice) {
        if let buf = proBuffers.removeValue(forKey: dev) {
            IOHIDDeviceRegisterInputReportCallback(dev, buf, 512, nil, nil)
            buf.deallocate()
        }
        DispatchQueue.main.async {
            self.updateControllerName()
        }
    }

    private func onSample(x: Double, y: Double, z: Double) {
        if phase == .sampling && samples.count < 4096 {
            lastSampleTime = ProcessInfo.processInfo.systemUptime
            samples.append((x, y, z))
        }
    }

    func startCalibration() {
        attachGameControllers()
        updateControllerName()
        samples.removeAll()
        phase = .countdown
        secondsLeft = 5.0
        phaseStart = Date()

        countdownTimer?.invalidate()
        countdownTimer = Timer.scheduledTimer(withTimeInterval: 0.1, repeats: true) { [weak self] t in
            guard let self = self else { t.invalidate(); return }
            let elapsed = Date().timeIntervalSince(self.phaseStart)
            if self.phase == .countdown {
                self.secondsLeft = max(0, 5.0 - elapsed)
                if self.secondsLeft <= 0 {
                    self.phase = .sampling
                    self.secondsLeft = 3.0
                    self.phaseStart = Date()
                    self.samples.removeAll()
                    self.startActiveSamplingTimer()
                }
            } else if self.phase == .sampling {
                self.secondsLeft = max(0, 3.0 - elapsed)
                if self.secondsLeft <= 0 {
                    t.invalidate()
                    self.countdownTimer = nil
                    self.stopActiveSamplingTimer()
                    self.finishSampling()
                }
            }
        }
    }

    private func startActiveSamplingTimer() {
        attachGameControllers()
        sampleTimer?.invalidate()
        sampleTimer = Timer.scheduledTimer(withTimeInterval: 0.016, repeats: true) { [weak self] _ in
            guard let self = self, self.phase == .sampling else { return }
            if !self.proBuffers.isEmpty { return }
            if ProcessInfo.processInfo.systemUptime - self.lastSampleTime > 0.035 {
                for c in GCController.controllers() {
                    if let name = c.vendorName, name.localizedCaseInsensitiveContains("Joy-Con") {
                        continue
                    }
                    if let m = c.motion {
                        if m.sensorsRequireManualActivation && !m.sensorsActive { m.sensorsActive = true }
                        let rot = m.rotationRate
                        if rot.x == 0 && rot.y == 0 && rot.z == 0 { continue }
                        self.onSample(x: rot.x, y: rot.y, z: rot.z)
                    }
                }
            }
        }
    }

    private func stopActiveSamplingTimer() {
        sampleTimer?.invalidate()
        sampleTimer = nil
    }

    private func finishSampling() {
        let n = samples.count
        guard n >= 30 else {
            phase = .noData
            return
        }
        let xs = samples.map { $0.0 }.sorted()
        let ys = samples.map { $0.1 }.sorted()
        let zs = samples.map { $0.2 }.sorted()
        let medX = xs[n / 2], medY = ys[n / 2], medZ = zs[n / 2]

        // Relaxed scatter tolerance: Bluetooth packet jitter and resting sensor noise can scatter ~15-20 deg/s (0.35 rad/s).
        var inliers: [(Double, Double, Double)] = []
        for s in samples {
            let dist = sqrt(pow(s.0 - medX, 2) + pow(s.1 - medY, 2) + pow(s.2 - medZ, 2))
            if dist <= 0.35 {
                inliers.append(s)
            }
        }
        let outliers = n - inliers.count
        guard inliers.count >= 20 && outliers <= max(5, n / 3) else {
            phase = .moved
            return
        }
        let meanX = inliers.map { $0.0 }.reduce(0, +) / Double(inliers.count)
        let meanY = inliers.map { $0.1 }.reduce(0, +) / Double(inliers.count)
        let meanZ = inliers.map { $0.2 }.reduce(0, +) / Double(inliers.count)
        let normMean = sqrt(meanX * meanX + meanY * meanY + meanZ * meanZ)
        guard normMean <= 1.05 else {
            phase = .noData
            return
        }

        let b = (meanX * 180.0 / .pi, meanY * 180.0 / .pi, meanZ * 180.0 / .pi)
        biasDeg = b
        let val = String(format: "%.6f,%.6f,%.6f", b.0, b.1, b.2)
        UserDefaults.standard.set(val, forKey: "DmGyroBias")
        UserDefaults.standard.set(val, forKey: "launcher.DM_GYRO_BIAS")
        setenv("DM_GYRO_BIAS", val, 1)
        phase = .done
        fputs("[gyro] calibrated: bias (\(val)) deg/s\n", stderr)
    }

    func clearCalibration() {
        countdownTimer?.invalidate()
        countdownTimer = nil
        stopActiveSamplingTimer()
        biasDeg = nil
        UserDefaults.standard.removeObject(forKey: "DmGyroBias")
        UserDefaults.standard.removeObject(forKey: "launcher.DM_GYRO_BIAS")
        unsetenv("DM_GYRO_BIAS")
        phase = .idle
        fputs("[gyro] calibration cleared\n", stderr)
    }
}

private final class Launcher: NSObject, NSApplicationDelegate, NSWindowDelegate {
    let model: LauncherModel
    var window: NSWindow?
    var play = false

    init(_ model: LauncherModel) { self.model = model }

    func applicationDidFinishLaunching(_ n: Notification) {
        GamepadGyroCalibrator.shared.start()
        let name = info("CFBundleName") ?? "Game"
        let menu = NSMenu(), item = NSMenuItem(), app = NSMenu()
        app.addItem(withTitle: "Close Window", action: #selector(NSWindow.performClose(_:)), keyEquivalent: "w")
        app.addItem(withTitle: "Quit \(name)", action: #selector(NSApplication.terminate(_:)), keyEquivalent: "q")
        item.submenu = app; menu.addItem(item); NSApp.mainMenu = menu
        let w = NSWindow(contentRect: NSRect(x: 0, y: 0, width: 560, height: 660), styleMask: [.titled, .closable, .miniaturizable, .resizable], backing: .buffered, defer: false)
        w.title = name; w.isReleasedWhenClosed = false; w.delegate = self
        w.contentMinSize = NSSize(width: 520, height: 0)
        window = w
        if let screen = w.screen ?? NSScreen.main {
            w.setFrameTopLeftPoint(NSPoint(x: screen.visibleFrame.midX - w.frame.width / 2, y: screen.visibleFrame.maxY))
        }
        let host = NSHostingView(rootView: LauncherView(m: model, done: { [weak self] in self?.finish($0) }, fit: { [weak self] in self?.fit($0) }))
        host.sizingOptions = []   // the Form's measured scroll content owns the height, including after collapse
        w.contentView = host
        host.layoutSubtreeIfNeeded()
        // no activation and no ordering in front of another app's windows: LaunchServices activates an app opened from Finder or the Dock (applicationDidBecomeActive), open -g does not
        if NSApp.isActive { w.makeKeyAndOrderFront(nil) } else { w.orderBack(nil) }
    }
    func applicationDidBecomeActive(_ n: Notification) { window?.makeKeyAndOrderFront(nil) }
    func applicationShouldTerminate(_ sender: NSApplication) -> NSApplication.TerminateReply {   // ⌘Q, or a logout or restart: the app ends without the game (exit 0, as Quit)
        fputs("launcher: quit without starting the game\n", stderr)
        return .terminateNow   // not terminateCancel, which a logout or restart takes as a refusal and stops
    }
    func windowWillClose(_ n: Notification) { finish(false) }

    func fit(_ extraHeight: CGFloat) {
        guard let w = window, let screen = w.screen else { return }
        let frame = LauncherWindowLayout.fitted(w.frame, extraHeight: extraHeight, visible: screen.visibleFrame)
        if abs(frame.height - w.frame.height) > 0.5 { w.setFrame(frame, display: true) }
    }

    func finish(_ play: Bool) {
        GamepadGyroCalibrator.shared.stop()
        guard let w = window else { return }
        window = nil; self.play = play
        // The main thread blocks in launcher.c's waitpid once run() returns, so the window must leave the screen now: no fade-out (it runs on later run-loop passes),
        // and dmMacLauncher flushes the order-out (AppKit commits it at the end of a pass). Without both, the window stayed on screen, not responding, until the game ended
        w.animationBehavior = .none
        w.delegate = nil; w.orderOut(nil)
        NSApp.stop(nil)
        NSApp.postEvent(NSEvent.otherEvent(with: .applicationDefined, location: .zero, modifierFlags: [], timestamp: 0, windowNumber: 0, context: nil, subtype: 0, data1: 0, data2: 0)!, atStart: true)   // run returns at the next event
    }
}

enum LauncherWindowLayout {
    static func fitted(_ frame: NSRect, extraHeight: CGFloat, visible: NSRect) -> NSRect {
        let top = min(frame.maxY, visible.maxY)
        let height = min(frame.height + extraHeight, top - visible.minY)
        return NSRect(x: frame.minX, y: top - height, width: frame.width, height: height)
    }
}

// 1: Play (the window's settings are in the environment), 0: the player quit, 2: no window (no launcher.json, or "Show this window at launch" off): launcher.c applies the saved choices
@_cdecl("dm_mac_launcher")
public func dmMacLauncher() -> Int32 {
    guard let url = Bundle.main.resourceURL?.appendingPathComponent("dm/launcher.json"), let data = try? Data(contentsOf: url) else { return 2 }
    guard let file = try? JSONDecoder().decode(SettingsFile.self, from: data), !file.settings.isEmpty else { fputs("[RED] launcher: dm/launcher.json cannot be read; starting the game\n", stderr); return 2 }
    if (UserDefaults.standard.object(forKey: "window.show") as? Bool) == false && !NSEvent.modifierFlags.contains(.option) {
        fputs("launcher: settings window off (hold Option at launch to show it)\n", stderr)
        return SaveCheck.withoutWindow() ? 2 : 0
    }
    let model = LauncherModel(file.settings)
    let app = NSApplication.shared, launcher = Launcher(model)
    app.setActivationPolicy(.regular)
    app.delegate = launcher
    fputs("launcher: \(file.settings.count) settings shown\n", stderr)
    app.run()
    CATransaction.flush()   // finish's order-out
    app.setActivationPolicy(.prohibited)   // the host is the game's app from here on
    guard launcher.play else { fputs("launcher: quit without starting the game\n", stderr); return 0 }
    model.apply()
    return 1
}

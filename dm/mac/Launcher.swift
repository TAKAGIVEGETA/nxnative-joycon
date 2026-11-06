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

private func info(_ key: String) -> String? { Bundle.main.object(forInfoDictionaryKey: key) as? String }

struct LauncherView: View {
    @ObservedObject var m: LauncherModel
    @ObservedObject var joyConMonitor: JoyConMonitor
    let done: (Bool) -> Void
    let fit: (CGFloat) -> Void
    @AppStorage("window.show") private var show = true
    @AppStorage("window.advanced") private var advanced = false

    init(m: LauncherModel, joyConMonitor: JoyConMonitor = .shared, done: @escaping (Bool) -> Void, fit: @escaping (CGFloat) -> Void) {
        self.m = m
        self.joyConMonitor = joyConMonitor
        self.done = done
        self.fit = fit
    }

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
                JoyConSection(monitor: joyConMonitor)
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
                Button("Quit") { done(false) }
                Button("Play") { done(true) }.keyboardShortcut(.defaultAction)
            }
            .padding(20)
        }
        .frame(minWidth: 520, idealWidth: 560)
    }

    @ViewBuilder private func settingSections(basic: Bool) -> some View {
        ForEach(m.sections(basic: basic), id: \.self) { section in
            let rows = m.rows(in: section, basic: basic)
            if !rows.isEmpty {
                Section(section) {
                    ForEach(rows) { SettingRow(s: $0, m: m) }
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

/// Battery badge modeled after Eden's controller preview: outline, terminal, fill level bar, and charging symbol
struct JoyConBatteryBadge: View {
    let level: Int
    let charging: Bool
    let connected: Bool

    var percentage: Int {
        switch level {
        case 0: return 10
        case 1: return 25
        case 2: return 50
        case 3: return 75
        case 4: fallthrough
        default: return 100
        }
    }

    var levelDescription: String {
        guard connected else { return "Not Connected" }
        if charging { return "Charging" }
        switch level {
        case 0: return "Empty"
        case 1: return "Critical"
        case 2: return "Low"
        case 3: return "Medium"
        case 4: fallthrough
        default: return "Full"
        }
    }

    var fillColor: Color {
        if charging { return .yellow }
        switch level {
        case 0, 1: return .red
        case 2: return .orange
        case 3, 4: fallthrough
        default: return .green
        }
    }

    var body: some View {
        HStack(spacing: 8) {
            if connected {
                ZStack(alignment: .leading) {
                    RoundedRectangle(cornerRadius: 3)
                        .stroke(Color.secondary.opacity(0.5), lineWidth: 1.5)
                        .frame(width: 26, height: 13)
                    RoundedRectangle(cornerRadius: 1)
                        .fill(Color.secondary.opacity(0.5))
                        .frame(width: 2, height: 5)
                        .offset(x: 27)

                    let fillWidth: CGFloat = {
                        switch level {
                        case 0: return 3
                        case 1: return 6
                        case 2: return 12
                        case 3: return 18
                        case 4: fallthrough
                        default: return 23
                        }
                    }()

                    RoundedRectangle(cornerRadius: 1.5)
                        .fill(fillColor)
                        .frame(width: max(0, fillWidth), height: 9)
                        .padding(.leading, 1.5)

                    if charging {
                        Image(systemName: "bolt.fill")
                            .font(.system(size: 8, weight: .bold))
                            .foregroundColor(.primary)
                            .offset(x: 8)
                    }
                }
                .frame(width: 32, height: 14)

                Text(charging ? "⚡️ \(percentage)% (\(levelDescription))" : "\(percentage)% (\(levelDescription))")
                    .font(.callout)
                    .foregroundStyle(level <= 1 ? .red : .primary)
            } else {
                Text("Not Connected")
                    .font(.callout)
                    .foregroundStyle(.secondary)
            }
        }
    }
}

struct JoyConSection: View {
    @ObservedObject var monitor: JoyConMonitor

    var body: some View {
        Section("Controllers") {
            LabeledContent {
                JoyConBatteryBadge(level: monitor.leftBatteryLevel, charging: monitor.leftCharging, connected: monitor.leftConnected)
            } label: {
                HStack(spacing: 6) {
                    Image(systemName: "gamecontroller")
                    Text("Joy-Con (L)")
                }
            }

            LabeledContent {
                JoyConBatteryBadge(level: monitor.rightBatteryLevel, charging: monitor.rightCharging, connected: monitor.rightConnected)
            } label: {
                HStack(spacing: 6) {
                    Image(systemName: "gamecontroller")
                    Text("Joy-Con (R)")
                }
            }
        }
    }
}

final class JoyConMonitor: ObservableObject {
    static let shared = JoyConMonitor()

    @Published var leftConnected = false
    @Published var leftBatteryLevel: Int = -1
    @Published var leftCharging = false

    @Published var rightConnected = false
    @Published var rightBatteryLevel: Int = -1
    @Published var rightCharging = false

    private var manager: IOHIDManager?
    private var buffers: [IOHIDDevice: UnsafeMutablePointer<UInt8>] = [:]

    func start() {
        guard manager == nil else { return }
        let mgr = IOHIDManagerCreate(kCFAllocatorDefault, IOOptionBits(kIOHIDOptionsTypeNone))
        self.manager = mgr

        let matches: [[String: Any]] = [
            [kIOHIDVendorIDKey: 0x057E, kIOHIDProductIDKey: 0x2006],
            [kIOHIDVendorIDKey: 0x057E, kIOHIDProductIDKey: 0x2007]
        ]
        IOHIDManagerSetDeviceMatchingMultiple(mgr, matches as CFArray)

        let context = Unmanaged.passUnretained(self).toOpaque()

        IOHIDManagerRegisterDeviceMatchingCallback(mgr, { ctx, result, sender, dev in
            guard let ctx = ctx, result == kIOReturnSuccess else { return }
            let monitor = Unmanaged<JoyConMonitor>.fromOpaque(ctx).takeUnretainedValue()
            monitor.deviceMatched(dev)
        }, context)

        IOHIDManagerRegisterDeviceRemovalCallback(mgr, { ctx, result, sender, dev in
            guard let ctx = ctx else { return }
            let monitor = Unmanaged<JoyConMonitor>.fromOpaque(ctx).takeUnretainedValue()
            monitor.deviceRemoved(dev)
        }, context)

        IOHIDManagerScheduleWithRunLoop(mgr, CFRunLoopGetMain(), CFRunLoopMode.commonModes.rawValue)
        let ret = IOHIDManagerOpen(mgr, IOOptionBits(kIOHIDOptionsTypeNone))
        if ret != kIOReturnSuccess {
            fputs("[RED] launcher: cannot open HID manager for Joy-Con monitoring (\(ret))\n", stderr)
        }
    }

    func stop() {
        guard let mgr = manager else { return }
        IOHIDManagerRegisterDeviceMatchingCallback(mgr, nil, nil)
        IOHIDManagerRegisterDeviceRemovalCallback(mgr, nil, nil)
        IOHIDManagerClose(mgr, IOOptionBits(kIOHIDOptionsTypeNone))
        IOHIDManagerUnscheduleFromRunLoop(mgr, CFRunLoopGetMain(), CFRunLoopMode.commonModes.rawValue)
        self.manager = nil

        for (dev, buf) in buffers {
            IOHIDDeviceRegisterInputReportCallback(dev, buf, 256, nil, nil)
            buf.deallocate()
        }
        buffers.removeAll()
    }

    private func isLeftDevice(_ dev: IOHIDDevice) -> Bool {
        let pid = (IOHIDDeviceGetProperty(dev, kIOHIDProductIDKey as CFString) as? NSNumber)?.intValue ?? 0
        return pid == 0x2006
    }

    private func deviceMatched(_ dev: IOHIDDevice) {
        let isLeft = isLeftDevice(dev)
        DispatchQueue.main.async {
            if isLeft { self.leftConnected = true }
            else { self.rightConnected = true }
        }

        let context = Unmanaged.passUnretained(self).toOpaque()
        let buf = UnsafeMutablePointer<UInt8>.allocate(capacity: 256)
        buffers[dev] = buf

        IOHIDDeviceRegisterInputReportCallback(dev, buf, 256, { ctx, result, sender, type, reportId, report, len in
            guard let ctx = ctx, result == kIOReturnSuccess, len > 2 else { return }
            let monitor = Unmanaged<JoyConMonitor>.fromOpaque(ctx).takeUnretainedValue()
            let batt = report[2]
            let charging = ((batt >> 4) & 1) != 0
            let level = Int((batt >> 5) & 7)
            monitor.reportReceived(batt: batt, charging: charging, level: level, sender: sender)
        }, context)

        var pkt: [UInt8] = [0x01, 0x00, 0x00, 0x01, 0x40, 0x40, 0x00, 0x01, 0x40, 0x40, 0x02, 0x00]
        IOHIDDeviceSetReport(dev, kIOHIDReportTypeOutput, CFIndex(0x01), &pkt, pkt.count)
    }

    private func deviceRemoved(_ dev: IOHIDDevice) {
        let isLeft = isLeftDevice(dev)
        if let buf = buffers.removeValue(forKey: dev) {
            IOHIDDeviceRegisterInputReportCallback(dev, buf, 256, nil, nil)
            buf.deallocate()
        }
        DispatchQueue.main.async {
            if isLeft {
                self.leftConnected = false
                self.leftBatteryLevel = -1
                self.leftCharging = false
            } else {
                self.rightConnected = false
                self.rightBatteryLevel = -1
                self.rightCharging = false
            }
        }
    }

    private func reportReceived(batt: UInt8, charging: Bool, level: Int, sender: UnsafeMutableRawPointer?) {
        guard let sender = sender else { return }
        let dev = Unmanaged<IOHIDDevice>.fromOpaque(sender).takeUnretainedValue()
        let isLeft = isLeftDevice(dev)
        DispatchQueue.main.async {
            if isLeft {
                self.leftConnected = true
                self.leftBatteryLevel = level
                self.leftCharging = charging
            } else {
                self.rightConnected = true
                self.rightBatteryLevel = level
                self.rightCharging = charging
            }
        }
    }
}

private final class JoyConMenuDelegate: NSObject, NSMenuDelegate {
    func menuNeedsUpdate(_ menu: NSMenu) {
        menu.removeAllItems()
        let m = JoyConMonitor.shared
        func item(_ name: String, connected: Bool, level: Int, charging: Bool) -> NSMenuItem {
            let title: String
            if !connected {
                title = "\(name): Not Connected"
            } else {
                let desc: String
                let icon: String
                let pct: Int
                switch level {
                case 0: desc = "Empty"; pct = 10; icon = "🪫"
                case 1: desc = "Critical"; pct = 25; icon = "🪫"
                case 2: desc = "Low"; pct = 50; icon = "🪫"
                case 3: desc = "Medium"; pct = 75; icon = "🔋"
                case 4: fallthrough
                default: desc = "Full"; pct = 100; icon = "🔋"
                }
                if charging {
                    title = "\(name): ⚡️ \(pct)% (\(desc), Charging)"
                } else {
                    title = "\(name): \(icon) \(pct)% (\(desc))"
                }
            }
            let it = NSMenuItem(title: title, action: nil, keyEquivalent: "")
            it.isEnabled = false
            return it
        }
        menu.addItem(item("Joy-Con (L)", connected: m.leftConnected, level: m.leftBatteryLevel, charging: m.leftCharging))
        menu.addItem(item("Joy-Con (R)", connected: m.rightConnected, level: m.rightBatteryLevel, charging: m.rightCharging))
    }
}

private final class Launcher: NSObject, NSApplicationDelegate, NSWindowDelegate {
    let model: LauncherModel
    var window: NSWindow?
    var play = false
    private let joyConMenuDelegate = JoyConMenuDelegate()

    init(_ model: LauncherModel) { self.model = model }

    func applicationDidFinishLaunching(_ n: Notification) {
        JoyConMonitor.shared.start()
        let name = info("CFBundleName") ?? "Game"
        let menu = NSMenu(), item = NSMenuItem(), app = NSMenu()
        app.addItem(withTitle: "Close Window", action: #selector(NSWindow.performClose(_:)), keyEquivalent: "w")
        app.addItem(withTitle: "Quit \(name)", action: #selector(NSApplication.terminate(_:)), keyEquivalent: "q")
        item.submenu = app; menu.addItem(item)
        let ctrlItem = NSMenuItem(title: "Controllers", action: nil, keyEquivalent: "")
        let ctrlMenu = NSMenu(title: "Controllers")
        ctrlMenu.delegate = joyConMenuDelegate
        ctrlMenu.autoenablesItems = false
        joyConMenuDelegate.menuNeedsUpdate(ctrlMenu)
        ctrlItem.submenu = ctrlMenu
        menu.addItem(ctrlItem)
        NSApp.mainMenu = menu
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
        JoyConMonitor.shared.stop()
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
        return 2
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

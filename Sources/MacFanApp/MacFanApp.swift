import AppKit
import SwiftUI

@discardableResult
func restoreAutoWithHelper() -> Bool {
    let installed = "/Library/PrivilegedHelperTools/com.codex.macfanctl"
    let bundled = Bundle.main.path(forResource: "macfanctl", ofType: nil)
    let helperPath = FileManager.default.isExecutableFile(atPath: installed) ? installed : (bundled ?? installed)
    guard FileManager.default.isExecutableFile(atPath: helperPath) else { return false }

    let process = Process()
    process.executableURL = URL(fileURLWithPath: helperPath)
    process.arguments = ["auto"]
    process.standardOutput = Pipe()
    process.standardError = Pipe()
    do {
        try process.run()
        process.waitUntilExit()
        return process.terminationStatus == 0
    } catch {
        return false
    }
}

struct FanInfo: Codable, Identifiable {
    let index: Int
    let name: String
    let manual: Bool
    let actualRpm: Double?
    let minRpm: Double?
    let maxRpm: Double?
    let targetRpm: Double?

    var id: Int { index }
}

struct ParameterInfo: Codable, Identifiable {
    let key: String
    let type: String
    let size: Int
    let category: String
    let value: String
    let raw: String
    let number: Double?

    var id: String { "\(category)-\(key)-\(raw)" }
}

struct SMCInfo: Codable {
    let opened: Bool
    let service: String
    let openResult: String
    let error: String?
}

struct SystemInfo: Codable {
    let model: String
    let machine: String
    let osVersion: String
}

struct HardwareProfile: Codable {
    let modelIdentifier: String
    let architecture: String
    let platform: String
    let controlStrategy: String
    let supportedControl: Bool
    let fanCount: Int
    let modeKeyCount: Int
    let legacyFSAvailable: Bool
    let ftstAvailable: Bool
    let fanKeyLimit: Int
}

struct HelperStatus: Codable {
    let ok: Bool
    let command: String
    let effectiveUserId: Int
    let system: SystemInfo
    let smc: SMCInfo
    let hardwareProfile: HardwareProfile?
    let fans: [FanInfo]
    let parameters: [ParameterInfo]
    let errors: [String]
}

enum ParameterFilter: String, CaseIterable, Identifiable {
    case all = "全部"
    case fan = "风扇"
    case temperature = "温度"
    case other = "其他"

    var id: String { rawValue }

    func accepts(_ parameter: ParameterInfo) -> Bool {
        switch self {
        case .all:
            return true
        case .fan:
            return parameter.category == "fan"
        case .temperature:
            return parameter.category == "temperature"
        case .other:
            return parameter.category == "other"
        }
    }
}

@MainActor
final class FanControllerModel: ObservableObject {
    @Published var status: HelperStatus?
    @Published var sliderValue: Double = 0
    @Published var isRefreshing = false
    @Published var isApplying = false
    @Published var message = "准备刷新"
    @Published var helperPath = FanControllerModel.resolveHelperPath()
    @Published var actionErrors: [String] = []

    private var timer: Timer?
    private var userIsEditingSlider = false

    var modeText: String {
        if sliderValue <= 0.5 {
            return "AUTO"
        }
        return "\(Int(sliderValue.rounded()))%"
    }

    var canApply: Bool {
        FileManager.default.isExecutableFile(atPath: helperPath)
    }

    var canControl: Bool {
        status?.hardwareProfile?.supportedControl ?? true
    }

    var visibleErrors: [String] {
        var combined = actionErrors
        for error in status?.errors ?? [] where !combined.contains(error) {
            combined.append(error)
        }
        return combined
    }

    var hasProblem: Bool {
        !(status?.ok ?? false) || !visibleErrors.isEmpty
    }

    var strategyLine: String {
        guard let profile = status?.hardwareProfile else { return "等待硬件探测" }
        let writable = profile.supportedControl ? "支持控制" : "仅监测"
        let ftst = profile.ftstAvailable ? " · Ftst" : ""
        let fs = profile.legacyFSAvailable ? " · FS!" : ""
        return "\(profile.platform) · \(profile.controlStrategy) · \(writable)\(ftst)\(fs)"
    }

    var targetPreview: String {
        guard sliderValue > 0.5,
              let fan = status?.fans.first(where: { $0.minRpm != nil && $0.maxRpm != nil }),
              let min = fan.minRpm,
              let max = fan.maxRpm,
              max > min else {
            return sliderValue <= 0.5 ? "AUTO" : "等待 min/max"
        }
        let rpm = min + (max - min) * (sliderValue / 100)
        return "\(Int(rpm.rounded())) RPM"
    }

    static func resolveHelperPath() -> String {
        let installed = "/Library/PrivilegedHelperTools/com.codex.macfanctl"
        if FileManager.default.isExecutableFile(atPath: installed) {
            return installed
        }
        if let bundled = Bundle.main.path(forResource: "macfanctl", ofType: nil),
           FileManager.default.isExecutableFile(atPath: bundled) {
            return bundled
        }
        return installed
    }

    func start() {
        refresh()
        timer?.invalidate()
        timer = Timer.scheduledTimer(withTimeInterval: 2.0, repeats: true) { [weak self] _ in
            Task { @MainActor in
                self?.refresh(silent: true)
            }
        }
    }

    func stop() {
        timer?.invalidate()
        timer = nil
    }

    func setEditing(_ editing: Bool) {
        userIsEditingSlider = editing
        if !editing {
            applySlider()
        }
    }

    func refresh(silent: Bool = false) {
        guard !isRefreshing else { return }
        isRefreshing = true
        if !silent {
            message = "正在读取"
        }

        runHelper(arguments: ["status"]) { [weak self] result in
            guard let self else { return }
            self.isRefreshing = false
            switch result {
            case .success(let helperStatus):
                self.status = helperStatus
                self.helperPath = Self.resolveHelperPath()
                if !self.userIsEditingSlider && !self.isApplying && self.actionErrors.isEmpty {
                    self.sliderValue = Self.sliderValue(from: helperStatus)
                }
                if !silent {
                    self.message = helperStatus.errors.isEmpty ? "已刷新" : "读取完成，有诊断信息"
                }
            case .failure(let error):
                self.message = error.localizedDescription
                self.actionErrors = [error.localizedDescription]
            }
        }
    }

    func applySlider() {
        guard !isApplying else { return }
        isApplying = true
        let percent = Int(sliderValue.rounded())
        message = percent == 0 ? "正在切回 AUTO" : "正在设置 \(percent)%"
        runHelper(arguments: ["set-percent", "\(percent)"]) { [weak self] result in
            guard let self else { return }
            self.isApplying = false
            switch result {
            case .success(let helperStatus):
                self.status = helperStatus
                if helperStatus.ok && helperStatus.errors.isEmpty {
                    self.actionErrors = []
                    self.sliderValue = Self.sliderValue(from: helperStatus)
                    self.message = "已应用"
                } else {
                    self.actionErrors = helperStatus.errors.isEmpty ? ["helper 返回失败，但没有附带错误文本。"] : helperStatus.errors
                    self.message = "应用失败，查看诊断"
                }
            case .failure(let error):
                self.message = error.localizedDescription
                self.actionErrors = [error.localizedDescription]
            }
        }
    }

    func setAuto() {
        sliderValue = 0
        applySlider()
    }

    func setMaximum() {
        sliderValue = 100
        applySlider()
    }

    func clearDiagnostics() {
        actionErrors = []
        message = "已清除诊断"
        refresh(silent: true)
    }

    private static func sliderValue(from status: HelperStatus) -> Double {
        let manualFans = status.fans.filter(\.manual)
        guard !manualFans.isEmpty else { return 0 }
        guard let fan = manualFans.first(where: { $0.targetRpm != nil && $0.minRpm != nil && $0.maxRpm != nil }),
              let target = fan.targetRpm,
              let minRpm = fan.minRpm,
              let maxRpm = fan.maxRpm,
              maxRpm > minRpm else {
            return 100
        }
        return Swift.min(Swift.max(((target - minRpm) / (maxRpm - minRpm)) * 100, 1), 100)
    }

    private func runHelper(arguments: [String], completion: @escaping (Result<HelperStatus, Error>) -> Void) {
        let helper = helperPath
        DispatchQueue.global(qos: .userInitiated).async {
            let result: Result<HelperStatus, Error>
            do {
                let process = Process()
                process.executableURL = URL(fileURLWithPath: helper)
                process.arguments = arguments

                let output = Pipe()
                let errorPipe = Pipe()
                process.standardOutput = output
                process.standardError = errorPipe

                try process.run()
                process.waitUntilExit()

                let outputData = output.fileHandleForReading.readDataToEndOfFile()
                let errorData = errorPipe.fileHandleForReading.readDataToEndOfFile()
                if outputData.isEmpty {
                    let text = String(data: errorData, encoding: .utf8) ?? "helper 没有输出"
                    throw HelperRunError.message(text.trimmingCharacters(in: .whitespacesAndNewlines))
                }

                let decoded = try JSONDecoder().decode(HelperStatus.self, from: outputData)
                result = .success(decoded)
            } catch {
                result = .failure(error)
            }

            DispatchQueue.main.async {
                completion(result)
            }
        }
    }
}

enum HelperRunError: LocalizedError {
    case message(String)

    var errorDescription: String? {
        switch self {
        case .message(let value):
            return value.isEmpty ? "helper 执行失败" : value
        }
    }
}

struct ContentView: View {
    @StateObject private var model = FanControllerModel()
    @State private var filter: ParameterFilter = .all
    @AppStorage("restoreAutoOnExit") private var restoreAutoOnExit = true

    private var filteredParameters: [ParameterInfo] {
        model.status?.parameters.filter { filter.accepts($0) } ?? []
    }

    var body: some View {
        VStack(spacing: 14) {
            header
            controlPanel
            fanPanel
            parameterPanel
            diagnostics
        }
        .padding(18)
        .frame(minWidth: 840, minHeight: 680)
        .onAppear { model.start() }
        .onDisappear {
            model.stop()
            if restoreAutoOnExit {
                restoreAutoWithHelper()
            }
        }
    }

    private var header: some View {
        HStack(alignment: .center) {
            VStack(alignment: .leading, spacing: 4) {
                Text("Mac Fan Controller")
                    .font(.system(size: 24, weight: .semibold))
                Text(model.statusLine)
                    .font(.callout)
                    .foregroundStyle(.secondary)
            }
            Spacer()
            statusBadge
            Button {
                model.refresh()
            } label: {
                Label("刷新", systemImage: "arrow.clockwise")
            }
            .disabled(model.isRefreshing)
        }
    }

    private var statusBadge: some View {
        let ok = !model.hasProblem
        return HStack(spacing: 6) {
            Image(systemName: ok ? "checkmark.circle.fill" : "exclamationmark.triangle.fill")
            Text(ok ? "SMC 已连接" : "需要检查")
        }
        .font(.callout.weight(.medium))
        .foregroundStyle(ok ? Color.green : Color.orange)
        .padding(.horizontal, 10)
        .padding(.vertical, 6)
        .background(.quaternary, in: RoundedRectangle(cornerRadius: 8))
    }

    private var controlPanel: some View {
        VStack(alignment: .leading, spacing: 14) {
            HStack {
                VStack(alignment: .leading, spacing: 3) {
                    Text("控制")
                        .font(.headline)
                    Text("当前目标 \(model.modeText) · \(model.targetPreview)")
                        .foregroundStyle(.secondary)
                    Text(model.strategyLine)
                        .font(.caption)
                        .foregroundStyle(.secondary)
                }
                Spacer()
                Button {
                    model.setAuto()
                } label: {
                    Label("AUTO", systemImage: "fan")
                }
                .disabled(model.isApplying || !model.canControl)
                Button {
                    model.setMaximum()
                } label: {
                    Label("最高", systemImage: "gauge.with.dots.needle.100percent")
                }
                .disabled(model.isApplying || !model.canControl)
                Button {
                    model.applySlider()
                } label: {
                    Label("应用", systemImage: "checkmark")
                }
                .keyboardShortcut(.return, modifiers: [])
                .disabled(model.isApplying || !model.canApply || !model.canControl)
            }

            HStack(spacing: 12) {
                Text("AUTO")
                    .font(.caption.weight(.medium))
                    .frame(width: 44, alignment: .leading)
                Slider(value: $model.sliderValue, in: 0...100, step: 1) { editing in
                    model.setEditing(editing)
                }
                .disabled(!model.canControl)
                Text("MAX")
                    .font(.caption.weight(.medium))
                    .frame(width: 44, alignment: .trailing)
            }

            HStack {
                Text(model.message)
                    .foregroundStyle(.secondary)
                Spacer()
                Toggle("退出时恢复 AUTO", isOn: $restoreAutoOnExit)
                    .toggleStyle(.switch)
                Text(model.helperPath)
                    .font(.caption.monospaced())
                    .foregroundStyle(.tertiary)
                    .lineLimit(1)
                    .truncationMode(.middle)
            }
        }
        .panelStyle()
    }

    private var fanPanel: some View {
        VStack(alignment: .leading, spacing: 10) {
            HStack {
                Text("风扇读数")
                    .font(.headline)
                Spacer()
                Text("\(model.status?.fans.count ?? 0) 个")
                    .foregroundStyle(.secondary)
            }

            Grid(alignment: .leading, horizontalSpacing: 16, verticalSpacing: 8) {
                GridRow {
                    tableHeader("名称")
                    tableHeader("模式")
                    tableHeader("实际")
                    tableHeader("最小")
                    tableHeader("最大")
                    tableHeader("目标")
                }
                Divider().gridCellColumns(6)
                ForEach(model.status?.fans ?? []) { fan in
                    GridRow {
                        Text(fan.name)
                            .lineLimit(1)
                        Text(fan.manual ? "手动" : "AUTO")
                            .foregroundStyle(fan.manual ? .orange : .green)
                        Text(rpm(fan.actualRpm))
                        Text(rpm(fan.minRpm))
                        Text(rpm(fan.maxRpm))
                        Text(rpm(fan.targetRpm))
                    }
                    .font(.callout)
                }
                if model.status?.fans.isEmpty != false {
                    GridRow {
                        Text("没有读到风扇 SMC key")
                            .foregroundStyle(.secondary)
                            .gridCellColumns(6)
                    }
                }
            }
        }
        .panelStyle()
    }

    private var parameterPanel: some View {
        VStack(alignment: .leading, spacing: 10) {
            HStack {
                Text("SMC 参数")
                    .font(.headline)
                Picker("类别", selection: $filter) {
                    ForEach(ParameterFilter.allCases) { item in
                        Text(item.rawValue).tag(item)
                    }
                }
                .pickerStyle(.segmented)
                .frame(width: 280)
                Spacer()
                Text("\(filteredParameters.count) 项")
                    .foregroundStyle(.secondary)
            }

            ScrollView {
                LazyVGrid(columns: parameterColumns, alignment: .leading, spacing: 0) {
                    tableHeader("Key")
                    tableHeader("类型")
                    tableHeader("值")
                    tableHeader("Raw")

                    ForEach(filteredParameters) { parameter in
                        parameterCell(parameter.key, mono: true)
                        parameterCell(parameter.type.trimmingCharacters(in: .whitespaces), mono: true)
                        parameterCell(parameter.value)
                        parameterCell(parameter.raw, mono: true)
                    }
                }
                .padding(.vertical, 2)
            }
            .frame(minHeight: 180)
            .background(.background, in: RoundedRectangle(cornerRadius: 8))
            .overlay {
                RoundedRectangle(cornerRadius: 8)
                    .stroke(.quaternary)
            }
        }
        .panelStyle()
    }

    private var diagnostics: some View {
        VStack(alignment: .leading, spacing: 8) {
            HStack {
                Text("诊断")
                    .font(.headline)
                Spacer()
                if !model.visibleErrors.isEmpty {
                    Button {
                        model.clearDiagnostics()
                    } label: {
                        Label("清除", systemImage: "xmark.circle")
                    }
                    .buttonStyle(.borderless)
                }
                if let status = model.status {
                    Text("\(status.system.model) · \(status.smc.service) · euid \(status.effectiveUserId)")
                        .foregroundStyle(.secondary)
                }
            }
            let errors = model.visibleErrors
            if errors.isEmpty {
                Text("无错误")
                    .foregroundStyle(.secondary)
            } else {
                ScrollView {
                    VStack(alignment: .leading, spacing: 4) {
                        ForEach(errors, id: \.self) { error in
                            Text(error)
                                .font(.caption.monospaced())
                                .foregroundStyle(.orange)
                                .textSelection(.enabled)
                        }
                    }
                    .frame(maxWidth: .infinity, alignment: .leading)
                }
                .frame(maxHeight: 72)
            }
        }
        .panelStyle()
    }

    private var parameterColumns: [GridItem] {
        [
            GridItem(.fixed(64), alignment: .leading),
            GridItem(.fixed(70), alignment: .leading),
            GridItem(.flexible(minimum: 120, maximum: 240), alignment: .leading),
            GridItem(.flexible(minimum: 240), alignment: .leading)
        ]
    }

    private func tableHeader(_ text: String) -> some View {
        Text(text)
            .font(.caption.weight(.semibold))
            .foregroundStyle(.secondary)
            .textCase(.uppercase)
            .frame(maxWidth: .infinity, alignment: .leading)
    }

    private func parameterCell(_ text: String, mono: Bool = false) -> some View {
        Text(text.isEmpty ? "-" : text)
            .font(mono ? .caption.monospaced() : .caption)
            .lineLimit(1)
            .truncationMode(.middle)
            .padding(.vertical, 5)
            .padding(.trailing, 8)
            .textSelection(.enabled)
    }

    private func rpm(_ value: Double?) -> String {
        guard let value else { return "-" }
        return "\(Int(value.rounded())) RPM"
    }
}

private extension FanControllerModel {
    var statusLine: String {
        guard let status else { return "等待 helper 输出" }
        if status.smc.opened {
            if let profile = status.hardwareProfile {
                return "\(profile.modelIdentifier) · \(profile.platform) · macOS \(status.system.osVersion)"
            }
            return "\(status.system.model) · \(status.system.machine) · macOS \(status.system.osVersion)"
        }
        return status.smc.error ?? "SMC 未连接"
    }
}

private struct PanelStyle: ViewModifier {
    func body(content: Content) -> some View {
        content
            .padding(14)
            .background(.thinMaterial, in: RoundedRectangle(cornerRadius: 8))
            .overlay {
                RoundedRectangle(cornerRadius: 8)
                    .stroke(.quaternary)
            }
    }
}

private extension View {
    func panelStyle() -> some View {
        modifier(PanelStyle())
    }
}

final class AppDelegate: NSObject, NSApplicationDelegate {
    func applicationWillTerminate(_ notification: Notification) {
        let defaultValue = UserDefaults.standard.object(forKey: "restoreAutoOnExit")
        let shouldRestore = defaultValue == nil ? true : UserDefaults.standard.bool(forKey: "restoreAutoOnExit")
        guard shouldRestore else { return }
        restoreAutoWithHelper()
    }
}

@main
struct MacFanControllerApp: App {
    @NSApplicationDelegateAdaptor(AppDelegate.self) private var appDelegate

    var body: some Scene {
        WindowGroup {
            ContentView()
        }
        .commands {
            CommandGroup(replacing: .newItem) {}
        }
    }
}

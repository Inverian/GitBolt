import Cocoa
import FinderSync

class GitBoltFinderSync: FIFinderSync {
    override init() {
        super.init()
        // Watch home directory for git repos
        let homeDir = URL(fileURLWithPath: NSHomeDirectory())
        FIFinderSyncController.default().directoryURLs = [homeDir]
    }

    override func menuName() -> String { return "GitBolt" }

    override func menu(for menuKind: FIMenuKind) -> NSMenu {
        let menu = NSMenu(title: "GitBolt")
        menu.addItem(withTitle: "Open in GitBolt", action: #selector(openInGitBolt(_:)), keyEquivalent: "")
        menu.addItem(withTitle: "Git Status", action: #selector(showGitStatus(_:)), keyEquivalent: "")
        return menu
    }

    @objc func openInGitBolt(_ sender: AnyObject?) {
        guard let items = FIFinderSyncController.default().selectedItemURLs(), let first = items.first else { return }
        let path = first.path
        // Launch GitBolt with the path
        let task = Process()
        task.executableURL = URL(fileURLWithPath: "/usr/bin/open")
        task.arguments = ["-a", "GitBolt", "--args", path]
        try? task.run()
    }

    @objc func showGitStatus(_ sender: AnyObject?) {
        openInGitBolt(sender)
    }
}

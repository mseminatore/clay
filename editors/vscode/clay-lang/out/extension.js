"use strict";
Object.defineProperty(exports, "__esModule", { value: true });
exports.activate = activate;
exports.deactivate = deactivate;
const vscode = require("vscode");
const path = require("path");
class ClayDebugAdapterFactory {
    createDebugAdapterDescriptor(session, _executable) {
        const config = session.configuration;
        const clayPath = config.clayPath || 'clay';
        const program = config.program;
        // Resolve the program path relative to workspace
        let resolvedProgram = program;
        if (!path.isAbsolute(program) && vscode.workspace.workspaceFolders) {
            resolvedProgram = path.join(vscode.workspace.workspaceFolders[0].uri.fsPath, program);
        }
        const args = ['--dap', resolvedProgram];
        return new vscode.DebugAdapterExecutable(clayPath, args);
    }
}
function activate(context) {
    const factory = new ClayDebugAdapterFactory();
    context.subscriptions.push(vscode.debug.registerDebugAdapterDescriptorFactory('clay', factory));
}
function deactivate() { }
//# sourceMappingURL=extension.js.map
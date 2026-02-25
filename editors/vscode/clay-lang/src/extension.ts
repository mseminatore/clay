import * as vscode from 'vscode';
import * as path from 'path';

class ClayDebugAdapterFactory implements vscode.DebugAdapterDescriptorFactory {
    createDebugAdapterDescriptor(
        session: vscode.DebugSession,
        _executable: vscode.DebugAdapterExecutable | undefined
    ): vscode.ProviderResult<vscode.DebugAdapterDescriptor> {
        const config = session.configuration;
        const clayPath = config.clayPath || 'clay';
        const program = config.program;

        // Resolve the program path relative to workspace
        let resolvedProgram = program;
        if (!path.isAbsolute(program) && vscode.workspace.workspaceFolders) {
            resolvedProgram = path.join(
                vscode.workspace.workspaceFolders[0].uri.fsPath,
                program
            );
        }

        const args = ['--dap', resolvedProgram];
        return new vscode.DebugAdapterExecutable(clayPath, args);
    }
}

export function activate(context: vscode.ExtensionContext) {
    const factory = new ClayDebugAdapterFactory();
    context.subscriptions.push(
        vscode.debug.registerDebugAdapterDescriptorFactory('clay', factory)
    );
}

export function deactivate() {}

import * as vscode from 'vscode';
import * as path from 'path';
import * as cp from 'child_process';
import * as net from 'net';

class ClayDebugAdapterFactory implements vscode.DebugAdapterDescriptorFactory {
    createDebugAdapterDescriptor(
        session: vscode.DebugSession,
        _executable: vscode.DebugAdapterExecutable | undefined
    ): vscode.ProviderResult<vscode.DebugAdapterDescriptor> {
        const config = session.configuration;
        const clayPath = config.clayPath || 'clay';
        const program = config.program;

        let resolvedProgram = program;
        if (program && !path.isAbsolute(program) && vscode.workspace.workspaceFolders) {
            resolvedProgram = path.join(
                vscode.workspace.workspaceFolders[0].uri.fsPath,
                program
            );
        }

        const normalizedClayPath = path.normalize(clayPath);
        const normalizedProgram = path.normalize(resolvedProgram);
        console.log(`Clay DAP: launching "${normalizedClayPath}" --dap "${normalizedProgram}"`);

        return new vscode.DebugAdapterExecutable(normalizedClayPath, ['--dap', normalizedProgram], {
            cwd: vscode.workspace.workspaceFolders?.[0]?.uri.fsPath
        });
    }
}

class ClayDebugConfigProvider implements vscode.DebugConfigurationProvider {
    resolveDebugConfiguration(
        folder: vscode.WorkspaceFolder | undefined,
        config: vscode.DebugConfiguration,
        _token?: vscode.CancellationToken
    ): vscode.ProviderResult<vscode.DebugConfiguration> {
        if (!config.program) {
            const editor = vscode.window.activeTextEditor;
            if (editor && editor.document.languageId === 'clay') {
                config.program = editor.document.uri.fsPath;
            }
        }
        if (!config.clayPath) {
            config.clayPath = 'clay';
        }
        if (!config.type) {
            config.type = 'clay';
        }
        if (!config.request) {
            config.request = 'launch';
        }
        if (!config.name) {
            config.name = 'Debug Clay Program';
        }
        console.log('Clay DAP config:', JSON.stringify(config));
        return config;
    }

    resolveDebugConfigurationWithSubstitutedVariables(
        folder: vscode.WorkspaceFolder | undefined,
        config: vscode.DebugConfiguration,
        _token?: vscode.CancellationToken
    ): vscode.ProviderResult<vscode.DebugConfiguration> {
        console.log('Clay DAP resolved config:', JSON.stringify(config));
        return config;
    }
}

export function activate(context: vscode.ExtensionContext) {
    console.log('Clay Language extension activating...');

    const factory = new ClayDebugAdapterFactory();
    const configProvider = new ClayDebugConfigProvider();

    context.subscriptions.push(
        vscode.debug.registerDebugConfigurationProvider('clay', configProvider),
        vscode.debug.registerDebugAdapterDescriptorFactory('clay', factory)
    );

    console.log('Clay debug adapter registered for type "clay"');
}

export function deactivate() {}

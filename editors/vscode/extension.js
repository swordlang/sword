// Starts swordls and lets VS Code paint with the semantic tokens it sends, and
// runs programs and tests through shield. The TextMate grammar next to this file
// colours the buffer until the server answers, and whenever it is not running.
const vscode = require('vscode');
const { LanguageClient } = require('vscode-languageclient/node');
const fs = require('fs');
const os = require('os');
const path = require('path');

const INSTALL = 'https://github.com/swordlang/sword/blob/main/docs/install.md';

let client;

function runnable(file) {
  try {
    fs.accessSync(file, fs.constants.X_OK);
    return fs.statSync(file).isFile();
  } catch {
    return false;
  }
}

// A setting that names a tool, then the tree the extension sits in when it is
// run from the compiler's own repository, then PATH. Null when none of them has
// it: spawning a name that is not there fails with an error that explains
// nothing to somebody who has not installed Sword yet.
function locate(context, setting, name) {
  const configured = vscode.workspace.getConfiguration('sword').get(setting) || name;
  if (configured !== name) {
    if (path.isAbsolute(configured)) return runnable(configured) ? configured : null;
    if (configured.includes(path.sep)) {
      const folder = vscode.workspace.workspaceFolders?.[0]?.uri.fsPath ?? process.cwd();
      const resolved = path.resolve(folder, configured);
      return runnable(resolved) ? resolved : null;
    }
  }
  const sibling = path.resolve(context.extensionPath, '..', '..', configured);
  if (configured === name && runnable(sibling)) return sibling;
  for (const dir of (process.env.PATH || '').split(path.delimiter)) {
    if (dir && runnable(path.join(dir, configured))) return path.join(dir, configured);
  }
  return null;
}

const warned = new Set();

// Once per tool per session: a warning on every keystroke would be worse than
// none.
function missing(tool, consequence) {
  if (warned.has(tool)) return;
  warned.add(tool);
  vscode.window
    .showWarningMessage(`Sword: ${tool} was not found, so ${consequence}.`, 'Install Sword', 'Set the path')
    .then((choice) => {
      if (choice === 'Install Sword') vscode.env.openExternal(vscode.Uri.parse(INSTALL));
      if (choice === 'Set the path') vscode.commands.executeCommand('workbench.action.openSettings', 'sword.');
    });
}

function startServer(context) {
  const command = locate(context, 'serverPath', 'swordls');
  if (!command) {
    missing('swordls', 'there are no diagnostics or completion; colouring from the grammar still works');
    return;
  }
  client = new LanguageClient(
    'swordls',
    'Sword Language Server',
    { run: { command }, debug: { command } },
    {
      documentSelector: [{ scheme: 'file', language: 'sword' }],
      synchronize: { fileEvents: vscode.workspace.createFileSystemWatcher('**/*.sword') },
    }
  );
  client.start().catch((err) => {
    vscode.window.showErrorMessage(`Sword: could not start ${command}: ${err.message}`);
  });
}

// shield reads files from disk, so what is being run has to be what is on
// screen.
async function saveSword() {
  for (const document of vscode.workspace.textDocuments) {
    if (document.languageId === 'sword' && document.isDirty) await document.save();
  }
}

// One terminal per kind of run, replaced by the next one of the same kind: a
// terminal rather than a task because a task needs a workspace folder, and a
// single file opened on its own has none.
const terminals = new Map();

function inTerminal(name, shellPath, shellArgs, cwd) {
  terminals.get(name)?.dispose();
  const terminal = vscode.window.createTerminal({ name, shellPath, shellArgs, cwd });
  terminals.set(name, terminal);
  terminal.show(true);
  return terminal;
}

function sourceOf(uri) {
  const target = uri instanceof vscode.Uri ? uri : vscode.window.activeTextEditor?.document.uri;
  if (!target || target.scheme !== 'file') {
    vscode.window.showWarningMessage('Sword: there is no saved .sword file to run.');
    return null;
  }
  return target.fsPath;
}

async function runFile(context, uri) {
  const file = sourceOf(uri);
  const shield = file && locate(context, 'shieldPath', 'shield');
  if (!file) return;
  if (!shield) return missing('shield', 'there is nothing to build and run with');
  await saveSword();
  // Built outside the workspace, so a run leaves nothing behind in it.
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'sword-run-'));
  const binary = path.join(dir, path.basename(file, '.sword'));
  return inTerminal(
    'Sword: run',
    '/bin/sh',
    ['-c', '"$1" "$2" -o "$3" && exec "$3"', 'sword', shield, file, binary],
    path.dirname(file)
  );
}

async function runTests(context, uri, name) {
  const file = sourceOf(uri);
  const shield = file && locate(context, 'shieldPath', 'shield');
  if (!file) return;
  if (!shield) return missing('shield', 'there is nothing to run tests with');
  await saveSword();
  // A test file belongs to its package, and the package is the directory.
  const dir = path.dirname(file);
  const args = ['test', dir, ...(name ? ['-run', name] : [])];
  return inTerminal('Sword: test', shield, args, dir);
}

// A link above each test in a _test.sword file. The pattern is the compiler's
// rule for what counts as a test, short of the parameter count, which shield
// reports on if it is wrong.
const testLenses = {
  provideCodeLenses(document) {
    if (!document.fileName.endsWith('_test.sword')) return [];
    const lenses = [];
    for (let line = 0; line < document.lineCount; line++) {
      const found = /^func (Test[A-Za-z0-9_]+)\s*\(/.exec(document.lineAt(line).text);
      if (!found) continue;
      lenses.push(
        new vscode.CodeLens(new vscode.Range(line, 0, line, 0), {
          title: 'run test',
          command: 'sword.runTest',
          arguments: [document.uri, found[1]],
        })
      );
    }
    return lenses;
  },
};

function activate(context) {
  startServer(context);
  context.subscriptions.push(
    vscode.commands.registerCommand('sword.runFile', (uri) => runFile(context, uri)),
    vscode.commands.registerCommand('sword.runTests', (uri) => runTests(context, uri)),
    vscode.commands.registerCommand('sword.runTest', (uri, name) => runTests(context, uri, name)),
    vscode.languages.registerCodeLensProvider({ language: 'sword', scheme: 'file' }, testLenses),
    { dispose: () => terminals.forEach((terminal) => terminal.dispose()) }
  );
}

function deactivate() {
  return client ? client.stop() : undefined;
}

module.exports = { activate, deactivate };

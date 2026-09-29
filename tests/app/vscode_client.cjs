// Runs in the real VS Code extension host using Continue's existing test API.
const assert = require('node:assert/strict');
const fs = require('node:fs');
const vscode = require('vscode');
exports.run = async () => {
  const installed = vscode.extensions.getExtension('Continue.continue');
  assert(installed, 'Continue must be installed in the isolated extension directory');
  const api = await installed.activate();
  assert(api.extension?.core?.messenger, 'Continue test API unavailable');
  const {config} = await api.extension.configHandler.loadConfig();
  const model = config?.selectedModelByRole.chat;
  assert.equal(model?.model, 'custom');
  const stream = await api.extension.core.messenger.externalRequest('llm/streamChat', {
    messages: [{role: 'user', content: 'Say hello in one short sentence.'}],
    completionOptions: {maxTokens: 32, temperature: 0},
    messageOptions: {}
  });
  let text = '';
  for await (const chunk of stream) {
    assert(!chunk.toolCalls?.length, 'Unexpected tool invocation');
    if (typeof chunk.content === 'string') text += chunk.content;
  }
  assert(text.trim(), 'Real inference returned no text');
  fs.writeFileSync(process.env.GEIST_VSCODE_RESULT, JSON.stringify({
    vscode: vscode.version, continue: installed.packageJSON.version,
    provider: model.providerName, model: model.model,
    text, test: 'extension-host llm/streamChat', chat: 'passed', agent: 'unsupported'
  }, null, 2), {mode: 0o600});
};

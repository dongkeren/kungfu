#!/usr/bin/env node
// SPDX-License-Identifier: Apache-2.0

const crypto = require('node:crypto');
const { spawn } = require('node:child_process');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');

const mode = process.argv[2];
const coreDir = path.resolve(__dirname, '..', '..');
const binding = require(path.join(coreDir, 'dist', 'kungfu', 'kungfu_node.node'));
const temporaryRoot = process.platform === 'win32' ? os.tmpdir() : '/tmp';
const home = fs.mkdtempSync(path.join(temporaryRoot, 'kfwr.'));
const watcher = new binding.Watcher(
  path.join(home, 'runtime'),
  `runtime_${mode}`,
  true,
  2,
);

function printableStats() {
  return Object.fromEntries(
    Object.entries(watcher.runtimeStats()).map(([key, value]) => [
      key,
      typeof value === 'bigint' ? value.toString() : value,
    ]),
  );
}

function fail(message) {
  process.stderr.write(`${JSON.stringify({ mode, error: message })}\n`);
  process.exit(2);
}

function waitFor(predicate, timeoutMs, message) {
  return new Promise((resolve, reject) => {
    const deadline = Date.now() + timeoutMs;
    const poll = setInterval(() => {
      if (predicate()) {
        clearInterval(poll);
        resolve();
      } else if (Date.now() > deadline) {
        clearInterval(poll);
        reject(new Error(message));
      }
    }, 20);
  });
}

function startCoordinator() {
  const outputPath = path.join(home, 'coordinator.out');
  const logOffset = fs.existsSync(outputPath) ? fs.statSync(outputPath).size : 0;
  const output = fs.openSync(outputPath, 'a');
  const environment = { ...process.env };
  environment.PATH =
    environment.SHIFU_UV_ORIGINAL_PATH || environment.PATH;
  delete environment.SHIFU_UV_ADAPTER_MANIFEST;
  delete environment.UV_PROJECT_ENVIRONMENT;
  delete environment.UV_PROJECT;
  delete environment.UV_FROZEN;
  delete environment.VIRTUAL_ENV;
  const child = spawn(
    'uv',
    [
      'run',
      '--frozen',
      'python',
      '.devtools/kungfu_cli.py',
      '-H',
      home,
      'runtime',
      'run',
      '--home',
      home,
      '--runtime-dir',
      path.join(home, 'runtime'),
      '--low-latency',
    ],
    { cwd: coreDir, env: environment, stdio: ['ignore', output, output] },
  );
  fs.closeSync(output);
  child.coordinatorLogOffset = logOffset;
  return child;
}

function coordinatorReady(child) {
  const output = fs.readFileSync(path.join(home, 'coordinator.out'), 'utf8');
  return output.slice(child.coordinatorLogOffset).includes('live runtime setup done');
}

function waitForExit(child) {
  if (child.exitCode !== null || child.signalCode !== null) return Promise.resolve();
  return new Promise((resolve) => child.once('exit', resolve));
}

function waitForExitWithin(child, timeoutMs) {
  if (child.exitCode !== null || child.signalCode !== null) {
    return Promise.resolve(true);
  }
  return new Promise((resolve) => {
    const onExit = () => {
      clearTimeout(timer);
      resolve(true);
    };
    const timer = setTimeout(() => {
      child.off('exit', onExit);
      resolve(false);
    }, timeoutMs);
    child.once('exit', onExit);
  });
}

async function stopCoordinator(child) {
  if (child.exitCode !== null || child.signalCode !== null) return;
  child.kill('SIGTERM');
  if (await waitForExitWithin(child, 3_000)) return;
  child.kill('SIGKILL');
  if (!(await waitForExitWithin(child, 3_000))) {
    throw new Error('coordinator did not exit after SIGKILL');
  }
}

function runChildProbe(childMode) {
  return new Promise((resolve, reject) => {
    const child = spawn(process.execPath, [__filename, childMode], {
      cwd: path.resolve(coreDir, '..', '..'),
      stdio: ['ignore', 'pipe', 'pipe'],
    });
    let stdout = '';
    let stderr = '';
    child.stdout.on('data', (chunk) => {
      stdout += chunk;
    });
    child.stderr.on('data', (chunk) => {
      stderr += chunk;
    });
    child.once('exit', (code) => {
      if (code !== 0) {
        reject(new Error(`${childMode} child exited ${code}: ${stderr.trim()}`));
        return;
      }
      resolve(JSON.parse(stdout.trim().split('\n').at(-1)));
    });
  });
}

async function reconnectProbe() {
  let coordinator = startCoordinator();
  let result = null;
  try {
    await waitFor(
      () =>
        coordinatorReady(coordinator) ||
        coordinator.exitCode !== null ||
        coordinator.signalCode !== null,
      15_000,
      'coordinator startup did not reach a terminal state',
    );
    if (coordinator.exitCode !== null || coordinator.signalCode !== null) {
      throw new Error(
        `coordinator exited during startup: ${fs.readFileSync(path.join(home, 'coordinator.out'), 'utf8').trim()}`,
      );
    }
    watcher.start();
    await waitFor(() => watcher.isLive(), 8_000, 'watcher did not connect');

    coordinator.kill();
    await waitForExit(coordinator);
    await waitFor(
      () => !watcher.isLive(),
      8_000,
      'watcher did not observe coordinator exit',
    );

    coordinator = startCoordinator();
    await waitFor(
      () =>
        coordinatorReady(coordinator) ||
        coordinator.exitCode !== null ||
        coordinator.signalCode !== null,
      15_000,
      'replacement coordinator startup did not reach a terminal state',
    );
    if (coordinator.exitCode !== null || coordinator.signalCode !== null) {
      throw new Error('replacement coordinator exited during startup');
    }
    await waitFor(
      () => watcher.isLive(),
      10_000,
      'watcher did not reconnect to the restarted coordinator',
    );
    watcher.quit();
    await waitFor(
      () => !watcher.runtimeStats().running,
      5_000,
      'watcher did not stop after reconnect',
    );
    result = { mode, reconnected: true, stats: printableStats() };
  } finally {
    watcher.quit();
    await stopCoordinator(coordinator);
  }
  process.stdout.write(`${JSON.stringify(result)}\n`, () => process.exit(0));
}

if (mode === 'pool') {
  watcher.start();
  const startedAt = Date.now();
  let completed = false;
  crypto.pbkdf2('watcher', 'pool', 1_000, 32, 'sha256', () => {
    completed = true;
    process.stdout.write(
      `${JSON.stringify({
        mode,
        elapsedMs: Date.now() - startedAt,
        stats: printableStats(),
      })}\n`,
    );
    watcher.quit();
  });
  setTimeout(() => {
    if (!completed) fail('libuv worker pool was starved by the watcher');
  }, 1_500);
} else if (mode === 'lifecycle') {
  watcher.start();
  setTimeout(() => watcher.quit(), 25);
  const deadline = Date.now() + 2_000;
  const poll = setInterval(() => {
    const stats = printableStats();
    if (!stats.running) {
      clearInterval(poll);
      process.stdout.write(`${JSON.stringify({ mode, stats })}\n`);
    } else if (Date.now() > deadline) {
      clearInterval(poll);
      fail('watcher did not stop within the lifecycle deadline');
    }
  }, 10);
} else if (mode === 'lifecycle-race') {
  Promise.all(Array.from({ length: 4 }, () => runChildProbe('lifecycle')))
    .then((results) => {
      process.stdout.write(
        `${JSON.stringify({ mode, stats: results.map((item) => item.stats) })}\n`,
      );
    })
    .catch((error) => fail(error.message));
} else if (mode === 'addon-exit') {
  watcher.start();
  setTimeout(() => process.exit(0), 25);
} else if (mode === 'reconnect') {
  reconnectProbe().catch((error) => fail(error.message));
} else {
  fail(`unknown probe mode: ${mode}`);
}

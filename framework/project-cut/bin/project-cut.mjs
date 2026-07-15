#!/usr/bin/env node

import { readFileSync } from 'node:fs';
import { observeHistory, reconcileHistory } from '../src/history.mjs';
import { parseRootJson } from '../src/project-cut.mjs';
import {
  abandonSettlement,
  inspectSettlement,
  observeSettlementCommit,
  prepareSettlement,
  reconcileCommit,
  verifySettlement,
} from '../src/settlement.mjs';

function usage() {
  return `Usage:
  project-cut prepare --request FILE [--root DIR] [--xinfa-bin FILE] [--execute] [--stage] --json
  project-cut verify --state FILE [--root DIR] [--execute] --json
  project-cut commit-observe --state FILE --commit REF [--root DIR] [--execute] --json
  project-cut inspect --state FILE [--root DIR] --json
  project-cut reconcile --commit REF [--root DIR] --json
  project-cut abandon --state FILE [--root DIR] [--execute] --json
  project-cut history-observe --request FILE [--root DIR] --json
  project-cut history-reconcile --observations FILE [--root DIR] --json`;
}

function parseArguments(argv) {
  const action = argv.shift();
  if (!action || action === '--help' || action === '-h')
    return { action: 'help' };
  const values = {};
  const flags = new Set();
  while (argv.length > 0) {
    const name = argv.shift();
    if (['--execute', '--stage', '--json'].includes(name)) {
      flags.add(name);
      continue;
    }
    if (!name?.startsWith('--') || argv.length === 0)
      throw Object.assign(new Error(`invalid argument: ${name}`), {
        code: 'invalid-argument',
      });
    values[name] = argv.shift();
  }
  if (!flags.has('--json'))
    throw Object.assign(
      new Error('--json is required for the agent-first surface'),
      {
        code: 'json-required',
      },
    );
  return { action, values, flags };
}

function required(values, name) {
  if (!values[name])
    throw Object.assign(new Error(`${name} is required`), {
      code: 'missing-argument',
    });
  return values[name];
}

function responseError(action, error) {
  return {
    schema: responseSchema(action),
    ok: false,
    action,
    error: {
      code: error.code ?? 'project-cut-failed',
      message: String(error.message),
      details: error.details ?? {},
    },
  };
}

function responseSchema(action) {
  return action.startsWith('history-')
    ? 'project.cut.history-response/v1'
    : 'project.cut.settlement-response/v1';
}

let action = 'unknown';
try {
  const parsed = parseArguments(process.argv.slice(2));
  action = parsed.action;
  if (action === 'help') {
    process.stdout.write(`${usage()}\n`);
    process.exit(0);
  }
  const root = parsed.values['--root'] ?? '.';
  const execute = parsed.flags.has('--execute');
  let result;
  if (action === 'prepare') {
    const request = parseRootJson(
      readFileSync(required(parsed.values, '--request'), 'utf8'),
    );
    result = prepareSettlement(root, request, {
      execute,
      stage: parsed.flags.has('--stage'),
      xinfaBin: parsed.values['--xinfa-bin'],
    });
  } else if (action === 'verify') {
    result = verifySettlement(root, required(parsed.values, '--state'), {
      execute,
    });
  } else if (action === 'commit-observe') {
    result = observeSettlementCommit(
      root,
      required(parsed.values, '--state'),
      required(parsed.values, '--commit'),
      { execute },
    );
  } else if (action === 'inspect') {
    result = inspectSettlement(root, required(parsed.values, '--state'));
  } else if (action === 'reconcile') {
    result = reconcileCommit(root, required(parsed.values, '--commit'));
  } else if (action === 'abandon') {
    result = abandonSettlement(root, required(parsed.values, '--state'), {
      execute,
    });
  } else if (action === 'history-observe') {
    const request = parseRootJson(
      readFileSync(required(parsed.values, '--request'), 'utf8'),
    );
    result = observeHistory(root, request);
  } else if (action === 'history-reconcile') {
    const input = parseRootJson(
      readFileSync(required(parsed.values, '--observations'), 'utf8'),
    );
    const observations = Array.isArray(input) ? input : input.observations;
    if (!Array.isArray(observations))
      throw Object.assign(new Error('observations must be an array'), {
        code: 'invalid-observations',
      });
    result = reconcileHistory(root, observations, {
      archivedRoots: Array.isArray(input) ? [] : input.archivedRoots,
    });
  } else {
    throw Object.assign(new Error(`unknown action: ${action}`), {
      code: 'unknown-action',
    });
  }
  process.stdout.write(
    `${JSON.stringify({ schema: responseSchema(action), ...result })}\n`,
  );
  if (result.ok === false) process.exitCode = 1;
} catch (error) {
  process.stdout.write(`${JSON.stringify(responseError(action, error))}\n`);
  process.exitCode = 1;
}

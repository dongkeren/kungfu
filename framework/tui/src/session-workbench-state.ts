// SPDX-License-Identifier: Apache-2.0

import type { TerminalDimensions } from './terminal-canvas.js';
import { terminalCanvasRows } from './terminal-canvas.js';

export type PlaybackTiming = {
  eventIntervalMs: number;
  verdictIntervalMs: number;
};
export type IncrementalPlayback<TEvent> = {
  enqueue(event: TEvent): void;
  finish(): Promise<boolean>;
  cancel(): void;
};
export type WorkbenchFocus = 'session-1' | 'session-2' | 'correct' | 'failed';
export type WorkbenchReportDetail = 'correct' | 'failed';
export type WorkbenchActionButton<Action extends string = string> = {
  action: Action;
  label: string;
};
export type WorkbenchNextPrompt = { title: string; instruction: string };
export type WorkbenchGuideOverlay = {
  heading: string;
  title: string;
  lines: string[];
  footer: string;
};
export type WorkbenchLine = {
  session: 1 | 2;
  source: string;
  text: string;
  tone: 'normal' | 'running' | 'good' | 'bad' | 'dim';
};
export type WorkbenchSessionBuffers = Record<1 | 2, WorkbenchLine[]>;
export type WorkbenchScrollBack = Record<1 | 2, number>;
export type WorkbenchCheck = {
  id: string;
  passed: boolean;
  title: string;
  meaning: string;
};

export const WORKBENCH_SESSION_BUFFER_LIMIT = 1_000;

export function emptyWorkbenchSessionBuffers(): WorkbenchSessionBuffers {
  return { 1: [], 2: [] };
}

export function appendWorkbenchSessionLines({
  buffers,
  scrollBack,
  lines,
  limit = WORKBENCH_SESSION_BUFFER_LIMIT,
}: {
  buffers: WorkbenchSessionBuffers;
  scrollBack: WorkbenchScrollBack;
  lines: WorkbenchLine[];
  limit?: number;
}): {
  buffers: WorkbenchSessionBuffers;
  scrollBack: WorkbenchScrollBack;
} {
  const nextBuffers: WorkbenchSessionBuffers = {
    1: buffers[1],
    2: buffers[2],
  };
  const nextScrollBack: WorkbenchScrollBack = { ...scrollBack };
  for (const session of [1, 2] as const) {
    const appended = lines.filter((line) => line.session === session);
    if (appended.length === 0) continue;
    const combined = [...buffers[session], ...appended];
    nextBuffers[session] = combined.slice(
      Math.max(0, combined.length - Math.max(1, limit)),
    );
    if (scrollBack[session] > 0) {
      nextScrollBack[session] = Math.min(
        nextBuffers[session].length - 1,
        scrollBack[session] + appended.length,
      );
    }
  }
  return { buffers: nextBuffers, scrollBack: nextScrollBack };
}

export function scrollWorkbenchSession({
  current,
  lineCount,
  viewportRows,
  delta,
}: {
  current: number;
  lineCount: number;
  viewportRows: number;
  delta: number;
}): number {
  return Math.max(
    0,
    Math.min(
      Math.max(0, lineCount - Math.max(1, viewportRows)),
      current + delta,
    ),
  );
}

export function workbenchViewportRows({
  dimensions,
  showHelp,
  verdictDetail,
}: {
  dimensions: TerminalDimensions;
  showHelp: boolean;
  verdictDetail?: string;
}): number {
  const titleColumns = Math.max(1, Math.floor(dimensions.columns / 2) - 2);
  const textColumns = Math.max(1, titleColumns - 2);
  const wrappedRows = (text: string) =>
    Math.max(1, Math.ceil(text.length / textColumns));
  const chromeRows =
    4 +
    (showHelp ? 1 : 0) +
    (verdictDetail ? 7 : 6) +
    2 +
    1 +
    wrappedRows('PUBLIC ACTIVITY · SENSITIVE INTERNALS HIDDEN') +
    wrappedRows(
      'Mouse wheel scrolls the Session under the pointer · ↑↓ scroll focused Session',
    );
  return Math.max(4, terminalCanvasRows(dimensions.rows) - chromeRows);
}

export function workbenchSessionAtPoint({
  dimensions,
  showHelp,
  verdictDetail,
  column,
  row,
  topOffset = 0,
}: {
  dimensions: TerminalDimensions;
  showHelp: boolean;
  verdictDetail?: string;
  column: number;
  row: number;
  topOffset?: number;
}): 1 | 2 | undefined {
  const localRow = row - topOffset;
  const headerRows = 3 + (showHelp ? 1 : 0);
  const verdictRows = verdictDetail ? 7 : 6;
  const finalSessionRow = terminalCanvasRows(dimensions.rows) - verdictRows;
  if (
    column < 1 ||
    column > dimensions.columns ||
    localRow <= headerRows ||
    localRow > finalSessionRow
  ) {
    return undefined;
  }
  return column <= Math.floor(dimensions.columns / 2) ? 1 : 2;
}

export function workbenchActionAtPoint<Action extends string>({
  actions,
  column,
  row,
  topOffset = 0,
}: {
  actions: readonly WorkbenchActionButton<Action>[];
  column: number;
  row: number;
  topOffset?: number;
}): Action | undefined {
  if (row - topOffset !== 3 || column < 1) return undefined;
  let start = 1;
  for (const action of actions) {
    const end = start + action.label.length + 3;
    if (column >= start && column <= end) return action.action;
    start = end + 2;
  }
  return undefined;
}

export function horizontalPointerActionAtPoint<Action extends string>({
  actions,
  column,
  row,
  targetRow,
  startColumn = 1,
  gap = 1,
}: {
  actions: readonly WorkbenchActionButton<Action>[];
  column: number;
  row: number;
  targetRow: number;
  startColumn?: number;
  gap?: number;
}): Action | undefined {
  if (row !== targetRow || column < startColumn) return undefined;
  let start = startColumn;
  for (const action of actions) {
    const end = start + action.label.length - 1;
    if (column >= start && column <= end) return action.action;
    start = end + 1 + gap;
  }
  return undefined;
}

export function workbenchReportAtPoint({
  dimensions,
  column,
  row,
  topOffset = 0,
}: {
  dimensions: TerminalDimensions;
  column: number;
  row: number;
  topOffset?: number;
}): WorkbenchReportDetail | undefined {
  const localRow = row - topOffset;
  const canvasRows = terminalCanvasRows(dimensions.rows);
  if (
    column < 1 ||
    column > dimensions.columns ||
    localRow < canvasRows - 3 ||
    localRow > canvasRows - 1
  ) {
    return undefined;
  }
  return column <= Math.floor(dimensions.columns / 2) ? 'correct' : 'failed';
}

export function workbenchReportReturnAtPoint({
  dimensions,
  column,
  row,
  topOffset = 0,
}: {
  dimensions: TerminalDimensions;
  column: number;
  row: number;
  topOffset?: number;
}): boolean {
  const localRow = row - topOffset;
  const canvasRows = terminalCanvasRows(dimensions.rows);
  return (
    column >= 1 &&
    column <= dimensions.columns &&
    localRow >= canvasRows - 2 &&
    localRow <= canvasRows
  );
}

export function createIncrementalPlayback<TEvent>({
  timing,
  onEvent,
  onAssessing,
  isCurrent = () => true,
  wait = (milliseconds) =>
    new Promise<void>((resolve) => {
      setTimeout(resolve, milliseconds);
    }),
}: {
  timing: PlaybackTiming;
  onEvent: (event: TEvent) => void;
  onAssessing: () => void;
  isCurrent?: () => boolean;
  wait?: (milliseconds: number) => Promise<void>;
}): IncrementalPlayback<TEvent> {
  let active = true;
  let queue = Promise.resolve();
  return {
    enqueue(event) {
      queue = queue
        .then(() => wait(timing.eventIntervalMs))
        .then(() => {
          if (active && isCurrent()) onEvent(event);
        });
    },
    async finish() {
      await queue;
      if (!active || !isCurrent()) return false;
      onAssessing();
      await wait(timing.verdictIntervalMs);
      return active && isCurrent();
    },
    cancel() {
      active = false;
    },
  };
}

const WORKBENCH_FOCUS_ORDER: WorkbenchFocus[] = [
  'session-1',
  'session-2',
  'correct',
  'failed',
];

export function nextWorkbenchFocus(
  current: WorkbenchFocus,
  reportAvailable: boolean,
): WorkbenchFocus {
  const available = reportAvailable
    ? WORKBENCH_FOCUS_ORDER
    : WORKBENCH_FOCUS_ORDER.slice(0, 2);
  const currentIndex = Math.max(0, available.indexOf(current));
  return available[(currentIndex + 1) % available.length];
}

export function isWorkbenchReturnInput(input: string): boolean {
  return (
    input === '\r' ||
    input === '\n' ||
    input === '\u001b' ||
    input === '\u007f' ||
    input === '\b' ||
    input === 'b' ||
    input === 'B' ||
    input === '\u001b[D'
  );
}

export function sessionTitleBar({
  session,
  title,
  active,
  running,
  columns,
  activityFrame = 0,
}: {
  session: 1 | 2;
  title: string;
  active: boolean;
  running: boolean;
  columns: number;
  activityFrame?: number;
}): string {
  const prefix = `${active ? '>' : ' '} S${session} · `;
  const spinner = ['◐', '◓', '◑', '◒'][activityFrame % 4];
  const status = running ? `${spinner} RUNNING` : 'READY';
  const titleColumns = Math.max(0, columns - prefix.length - status.length - 1);
  const compactTitle =
    title.length <= titleColumns
      ? title
      : titleColumns > 1
        ? `${title.slice(0, titleColumns - 1)}…`
        : '';
  const left = `${prefix}${compactTitle}`;
  const gap = ' '.repeat(Math.max(1, columns - left.length - status.length));
  return `${left}${gap}${status}`.padEnd(columns).slice(0, columns);
}

export function boundedPromptRows(
  value: string,
  columns: number,
  maxRows = 2,
): string[] {
  const width = Math.max(1, columns);
  const rows: string[] = [];
  let remaining = value.trim();
  while (remaining && rows.length < maxRows) {
    if (remaining.length <= width) {
      rows.push(remaining);
      remaining = '';
      break;
    }
    const space = remaining.lastIndexOf(' ', width);
    const cut = space > 0 ? space : width;
    rows.push(remaining.slice(0, cut).trimEnd());
    remaining = remaining.slice(cut).trimStart();
  }
  if (remaining && rows.length > 0) {
    const last = rows.length - 1;
    rows[last] = `${rows[last].slice(0, Math.max(0, width - 1))}…`;
  }
  while (rows.length < maxRows) rows.push('');
  return rows;
}

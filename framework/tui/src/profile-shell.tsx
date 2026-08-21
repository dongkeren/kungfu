// SPDX-License-Identifier: Apache-2.0

import type {
  ProductSearchDocument,
  ProductSearchResult,
} from '@kungfu-tech/api/capability';
import { Box, Text } from 'ink';
import React from 'react';
import {
  CLOSED_CONTROL_PLANE,
  type ControlPlaneState,
  QUICK_COMMANDS,
  type QuickCommand,
  controlPlaneBarModel,
} from './control-plane-state.js';
import { resolveListWindow } from './list-window/index.js';
import { boundedIndex } from './navigation.js';
import {
  type SessionWorkbenchProps,
  SessionWorkbenchView,
} from './session-workbench-view.js';
import {
  KUNGFU_CIRCULAR_STARTUP_PATTERN,
  KUNGFU_EMPTY_WORK_NEBULA_PATTERN,
  type TerminalAnimationPattern,
  type TerminalDimensions,
  TitledBorderWindow,
  splitHorizontalPointerActionAtPoint,
  terminalAnimationPatternSize,
  terminalAnimationsEnabled,
  terminalCanvasRows,
  useTerminalAnimationFrame,
} from './terminal-canvas.js';
export * from './terminal-canvas.js';
import type { WorkLoopShellModel } from './work-loop-contribution.js';
export {
  CLOSED_CONTROL_PLANE,
  QUICK_COMMANDS,
  contextualProjectRestoreCanCommit,
  controlPlaneBarModel,
  createControlPlaneInputFence,
  directWorkspaceNavigationFromInput,
  buildTuiProductSearchDocuments,
  initialProductSurface,
  onboardingContinueSurface,
  projectWorkOwnsInput,
  quickCommandMatches,
  reduceControlPlaneInput,
  resolveProductStartupSurface,
  shouldStartContextualProjectRestore,
} from './control-plane-state.js';
export type {
  ControlPlaneInputFence,
  ControlPlaneMode,
  ControlPlaneState,
  ControlPlaneUpdate,
  ProductQuickCommandAction,
  ProductSurface,
  QuickCommand,
} from './control-plane-state.js';

export type ProfileShellCard = {
  id: string;
  title: string;
  status: string;
  summary: string;
};

export type ProfileShellModel = {
  profile: {
    id: string;
    title: string;
    version: string;
    suiteRoot: string;
    qualified: boolean;
    qualificationLabel?: string;
  };
  subject: { id: string; title: string; subtitle: string };
  navigation: Array<{ id: string; label: string; status: string }>;
  cards: ProfileShellCard[];
  evidence: Array<{ label: string; value: string }>;
  workLoop?: WorkLoopShellModel;
  workLoopError?: string;
  notice?: string;
  navigationTitle?: string;
  subjectNoun?: string;
  footer?: string;
  modeLabel?: string;
  retainNavigationInCompact?: boolean;
};

export type ProfileShellLayout = {
  mode: 'one-column' | 'two-column' | 'three-column';
  navigationWidth: number;
  evidenceWidth: number;
};

export function resolveProfileShellLayout(
  dimensions: TerminalDimensions,
): ProfileShellLayout {
  if (dimensions.columns >= 140 && dimensions.rows >= 32) {
    return { mode: 'three-column', navigationWidth: 24, evidenceWidth: 38 };
  }
  if (dimensions.columns >= 100 && dimensions.rows >= 28) {
    return { mode: 'two-column', navigationWidth: 28, evidenceWidth: 0 };
  }
  return { mode: 'one-column', navigationWidth: 0, evidenceWidth: 0 };
}

export function compactProfileNavigationWidth(
  dimensions: TerminalDimensions,
): number {
  return Math.min(20, Math.max(16, Math.floor(dimensions.columns * 0.24)));
}

export function resolveProfileShellNavigationWidth(
  model: ProfileShellModel,
  dimensions: TerminalDimensions,
  override?: number,
): number {
  if (override !== undefined) {
    return Math.max(0, Math.min(dimensions.columns - 1, override));
  }
  const layout = resolveProfileShellLayout(dimensions);
  if (layout.navigationWidth > 0) return layout.navigationWidth;
  return model.retainNavigationInCompact
    ? compactProfileNavigationWidth(dimensions)
    : 0;
}

function shortRoot(value: string): string {
  if (value.length <= 18) return value;
  return `${value.slice(0, 7)}…${value.slice(-10)}`;
}

function qualificationState(model: ProfileShellModel): string {
  if ((model.profile.qualificationLabel ?? 'KFD-3') === 'KFD-3') {
    return model.profile.qualified ? 'qualified' : 'not qualified';
  }
  return model.profile.qualified ? 'verified' : 'not verified';
}

function cardStatusColor(status: string): 'green' | 'yellow' | 'red' {
  const normalized = status.toLocaleLowerCase();
  if (
    normalized.includes('failed') ||
    normalized.includes('invalid') ||
    normalized.includes('attention')
  ) {
    return 'red';
  }
  if (
    normalized.includes('pending') ||
    normalized.includes('captured') ||
    normalized.includes('degraded') ||
    normalized.includes('review required')
  ) {
    return 'yellow';
  }
  return 'green';
}

function NavigationPanel({
  model,
  active,
}: { model: ProfileShellModel; active: boolean }) {
  return (
    <Box
      flexDirection="column"
      borderStyle="round"
      borderColor={active ? 'cyan' : undefined}
      paddingX={1}
      flexGrow={1}
    >
      <Text bold>{model.navigationTitle ?? 'Subjects'}</Text>
      {model.navigation.length === 0 ? (
        <Text dimColor>none admitted</Text>
      ) : null}
      {model.navigation.map((item) => (
        <Text
          key={item.id}
          color={item.id === model.subject.id ? 'cyan' : undefined}
        >
          {item.id === model.subject.id ? '› ' : '  '}
          {item.label} <Text dimColor>{item.status}</Text>
        </Text>
      ))}
    </Box>
  );
}

function CardPanel({
  model,
  selectedCard,
  active,
  maxRows,
  height,
  width,
}: {
  model: ProfileShellModel;
  selectedCard: number;
  active: boolean;
  maxRows: number;
  height: number;
  width: number;
}) {
  const cardRows = model.cards.some((card) => card.summary.trim()) ? 2 : 1;
  const visibleCount = Math.max(1, Math.floor((maxRows - 8) / cardRows));
  const window = resolveListWindow({
    selected: selectedCard,
    itemCount: model.cards.length,
    viewportRows: visibleCount,
  });
  const start = window.start;
  const visibleCards = model.cards.slice(window.start, window.end);
  return (
    <Box
      flexDirection="column"
      borderStyle="round"
      borderColor={active ? 'cyan' : undefined}
      paddingX={1}
      height={height}
      width={width}
      flexShrink={0}
      overflow="hidden"
    >
      <Text bold>{model.subject.title}</Text>
      <Text dimColor>{model.subject.subtitle}</Text>
      {model.cards.length === 0 ? (
        <Text color="yellow">
          No Profile answers are available at this cut.
        </Text>
      ) : null}
      {model.cards.length > visibleCards.length ? (
        <Text dimColor>
          showing {start + 1}–{start + visibleCards.length} of{' '}
          {model.cards.length}
        </Text>
      ) : null}
      {visibleCards.map((card, offset) => {
        const index = start + offset;
        return (
          <Box
            key={card.id}
            flexDirection="column"
            marginTop={offset === 0 ? 1 : 0}
          >
            <Text
              color={index === selectedCard ? 'cyan' : undefined}
              bold
              wrap="truncate-end"
            >
              {index === selectedCard ? '› ' : '  '}
              {index + 1}. {card.title}
              <Text color={cardStatusColor(card.status)}> [{card.status}]</Text>
            </Text>
            {card.summary.trim() ? (
              <Text wrap="truncate-end"> {card.summary}</Text>
            ) : null}
          </Box>
        );
      })}
    </Box>
  );
}

function EvidencePanel({
  model,
  active,
  height,
  width,
}: {
  model: ProfileShellModel;
  active: boolean;
  height: number;
  width: number;
}) {
  const qualificationLabel = model.profile.qualificationLabel ?? 'KFD-3';
  return (
    <Box
      flexDirection="column"
      borderStyle="round"
      borderColor={active ? 'cyan' : undefined}
      paddingX={1}
      height={height}
      width={width}
      flexShrink={0}
      overflow="hidden"
    >
      <Text bold>Exact-root evidence</Text>
      {model.evidence.map((row) => (
        <Box key={row.label} flexDirection="column">
          <Text dimColor>{row.label}</Text>
          <Text>{shortRoot(row.value || '—')}</Text>
        </Box>
      ))}
      <Text color={model.profile.qualified ? 'green' : 'yellow'}>
        {qualificationLabel} {qualificationState(model)}
      </Text>
      {model.notice ? <Text color="yellow">{model.notice}</Text> : null}
    </Box>
  );
}

function CompactContext({ model }: { model: ProfileShellModel }) {
  const activeIndex = model.navigation.findIndex(
    (item) => item.id === model.subject.id,
  );
  const subjectPosition = activeIndex < 0 ? 'none' : `${activeIndex + 1}`;
  const proof = model.evidence.find((row) => row.label === 'query proof');
  const qualificationLabel = model.profile.qualificationLabel ?? 'KFD-3';
  return (
    <>
      <Text wrap="truncate-end">
        {model.subjectNoun ?? 'Subject'} {subjectPosition}/
        {model.navigation.length || 0} · {model.subject.id || 'none'}
      </Text>
      <Text
        color={model.profile.qualified ? 'green' : 'yellow'}
        wrap="truncate-end"
      >
        {qualificationLabel} {qualificationState(model)} · proof{' '}
        {shortRoot(proof?.value ?? '—')}
        {model.notice ? ` · ${model.notice}` : ''}
      </Text>
    </>
  );
}

function WorkLoopContext({ model }: { model: WorkLoopShellModel }) {
  return (
    <Box flexDirection="column" paddingX={1}>
      <Text wrap="truncate-end">
        Cut <Text color="cyan">{model.cutStatus}</Text> · Work{' '}
        <Text color="cyan">{model.status}</Text> · confidence{' '}
        <Text color="yellow">{model.confidence}</Text> · root{' '}
        {shortRoot(model.cutRoot || '—')}
      </Text>
      <Text wrap="truncate-end" dimColor>
        current {model.workId || 'none'} · recovery {model.recoveryAction} (
        {model.recoveryCode})
      </Text>
      <Text wrap="truncate-end" dimColor>
        gaps {model.gaps.join(', ') || 'none'} · next{' '}
        {model.nextActions.join(', ') || 'none'}
      </Text>
    </Box>
  );
}

function WorkLoopFailure({ message }: { message: string }) {
  return (
    <Box paddingX={1}>
      <Text color="yellow" wrap="truncate-end">
        Work Loop unavailable · {message} · no mutation attempted
      </Text>
    </Box>
  );
}

function profileShellBodyMetrics(
  model: ProfileShellModel,
  dimensions: TerminalDimensions,
) {
  const workLoopRows = model.workLoop ? 3 : model.workLoopError ? 1 : 0;
  const bodyHeight = Math.max(6, dimensions.rows - 4 - workLoopRows);
  const desiredEvidenceHeight =
    4 + model.evidence.length * 2 + (model.notice ? 1 : 0);
  const twoColumnEvidenceHeight = Math.max(
    6,
    Math.min(desiredEvidenceHeight, Math.max(6, bodyHeight - 10)),
  );
  return {
    workLoopRows,
    bodyHeight,
    twoColumnEvidenceHeight,
    twoColumnCardHeight: bodyHeight - twoColumnEvidenceHeight,
  };
}

export function profileShellCardPanelContainsPoint({
  model,
  dimensions,
  column,
  row,
  topOffset = 0,
  navigationWidth,
}: {
  model: ProfileShellModel;
  dimensions: TerminalDimensions;
  column: number;
  row: number;
  topOffset?: number;
  navigationWidth?: number;
}): boolean {
  const layout = resolveProfileShellLayout(dimensions);
  const resolvedNavigationWidth = resolveProfileShellNavigationWidth(
    model,
    dimensions,
    navigationWidth,
  );
  const metrics = profileShellBodyMetrics(model, dimensions);
  const localRow = row - topOffset;
  const bodyStart = 2 + metrics.workLoopRows;
  const bodyEnd = bodyStart + metrics.bodyHeight - 1;
  if (
    column < 1 ||
    column > dimensions.columns ||
    localRow < bodyStart ||
    localRow > bodyEnd
  ) {
    return false;
  }
  if (layout.mode === 'one-column') {
    if (resolvedNavigationWidth > 0) return column > resolvedNavigationWidth;
    return localRow >= bodyStart + 2;
  }
  if (column <= resolvedNavigationWidth) return false;
  if (layout.mode === 'three-column') {
    return column <= dimensions.columns - layout.evidenceWidth;
  }
  return localRow < bodyStart + metrics.twoColumnCardHeight;
}

export function ProfileShell({
  model,
  dimensions,
  selectedCard = 0,
  activeRegion = 1,
  busy = false,
  navigationPanel,
  navigationWidth: navigationWidthOverride,
}: {
  model: ProfileShellModel;
  dimensions: TerminalDimensions;
  selectedCard?: number;
  activeRegion?: number;
  busy?: boolean;
  navigationPanel?: React.ReactNode;
  navigationWidth?: number;
}) {
  const layout = resolveProfileShellLayout(dimensions);
  const navigationWidth = resolveProfileShellNavigationWidth(
    model,
    dimensions,
    navigationWidthOverride,
  );
  const { bodyHeight, twoColumnEvidenceHeight, twoColumnCardHeight } =
    profileShellBodyMetrics(model, dimensions);
  const evidenceWidth =
    layout.mode === 'three-column' ? layout.evidenceWidth : 0;
  const cardPanelWidth = Math.max(
    1,
    dimensions.columns - navigationWidth - evidenceWidth,
  );
  const navigation = navigationPanel ?? (
    <NavigationPanel model={model} active={activeRegion === 0} />
  );

  return (
    <Box
      width={dimensions.columns}
      height={terminalCanvasRows(dimensions.rows)}
      flexDirection="column"
      overflow="hidden"
    >
      <Box justifyContent="space-between" paddingX={1}>
        <Text bold>{model.profile.title}</Text>
        <Text dimColor>
          {model.profile.version} · {layout.mode} ·{' '}
          {busy ? 'refreshing' : (model.modeLabel ?? 'read-only')}
        </Text>
      </Box>
      {model.workLoop ? <WorkLoopContext model={model.workLoop} /> : null}
      {model.workLoopError ? (
        <WorkLoopFailure message={model.workLoopError} />
      ) : null}
      {layout.mode === 'three-column' ? (
        <Box height={bodyHeight}>
          <Box width={navigationWidth} flexShrink={0} overflow="hidden">
            {navigation}
          </Box>
          <Box width={cardPanelWidth} flexShrink={0} overflow="hidden">
            <CardPanel
              model={model}
              selectedCard={selectedCard}
              active={activeRegion === 1}
              maxRows={bodyHeight}
              height={bodyHeight}
              width={cardPanelWidth}
            />
          </Box>
          <Box width={evidenceWidth} flexShrink={0} overflow="hidden">
            <EvidencePanel
              model={model}
              active={activeRegion === 2}
              height={bodyHeight}
              width={evidenceWidth}
            />
          </Box>
        </Box>
      ) : null}
      {layout.mode === 'two-column' ? (
        <Box height={bodyHeight}>
          <Box width={navigationWidth} flexShrink={0} overflow="hidden">
            {navigation}
          </Box>
          <Box
            width={cardPanelWidth}
            flexDirection="column"
            flexShrink={0}
            overflow="hidden"
          >
            <CardPanel
              model={model}
              selectedCard={selectedCard}
              active={activeRegion === 1}
              maxRows={twoColumnCardHeight}
              height={twoColumnCardHeight}
              width={cardPanelWidth}
            />
            <EvidencePanel
              model={model}
              active={activeRegion === 2}
              height={twoColumnEvidenceHeight}
              width={cardPanelWidth}
            />
          </Box>
        </Box>
      ) : null}
      {layout.mode === 'one-column' && navigationWidth === 0 ? (
        <Box height={bodyHeight} flexDirection="column">
          <CompactContext model={model} />
          <CardPanel
            model={model}
            selectedCard={selectedCard}
            active={activeRegion === 1}
            maxRows={bodyHeight - 2}
            height={bodyHeight - 2}
            width={dimensions.columns}
          />
        </Box>
      ) : null}
      {layout.mode === 'one-column' && navigationWidth > 0 ? (
        <Box height={bodyHeight}>
          <Box width={navigationWidth} flexShrink={0} overflow="hidden">
            {navigation}
          </Box>
          <Box width={cardPanelWidth} flexShrink={0} overflow="hidden">
            <CardPanel
              model={model}
              selectedCard={selectedCard}
              active={activeRegion === 1}
              maxRows={bodyHeight}
              height={bodyHeight}
              width={cardPanelWidth}
            />
          </Box>
        </Box>
      ) : null}
      <Box paddingX={1}>
        <Text dimColor>
          {model.footer ??
            '↑↓/jk answer · ←→/hl subject · tab region · [a] Agent Work Lab · r refresh · q quit'}
        </Text>
      </Box>
    </Box>
  );
}

function clipped(value: string, width: number): string {
  const normalized = value.replace(/\s+/g, ' ').trim();
  if (normalized.length <= width) return normalized.padEnd(width);
  return `${normalized.slice(0, Math.max(0, width - 1))}…`;
}

export function renderProfileShellSnapshot(
  model: ProfileShellModel,
  dimensions: TerminalDimensions,
): string {
  const layout = resolveProfileShellLayout(dimensions);
  const qualificationLabel = model.profile.qualificationLabel ?? 'KFD-3';
  const lines = [
    clipped(
      `${model.profile.title} · ${model.profile.version} · ${layout.mode} · ${model.modeLabel ?? 'read-only'}`,
      dimensions.columns,
    ),
    ...(model.workLoop
      ? [
          clipped(
            `Cut ${model.workLoop.cutStatus} · Work ${model.workLoop.status} · confidence ${model.workLoop.confidence} · root ${shortRoot(model.workLoop.cutRoot || '—')}`,
            dimensions.columns,
          ),
          clipped(
            `current ${model.workLoop.workId || 'none'} · recovery ${model.workLoop.recoveryAction} (${model.workLoop.recoveryCode})`,
            dimensions.columns,
          ),
          clipped(
            `gaps ${model.workLoop.gaps.join(', ') || 'none'} · next ${model.workLoop.nextActions.join(', ') || 'none'}`,
            dimensions.columns,
          ),
        ]
      : []),
    ...(model.workLoopError
      ? [
          clipped(
            `Work Loop unavailable · ${model.workLoopError} · no mutation attempted`,
            dimensions.columns,
          ),
        ]
      : []),
    clipped(
      `${model.subject.title} — ${model.subject.subtitle}`,
      dimensions.columns,
    ),
    clipped(
      `${(model.navigationTitle ?? 'subjects').toLocaleLowerCase()} ${model.navigation.map((item) => `${item.label}[${item.status}]`).join(' · ') || 'none'}`,
      dimensions.columns,
    ),
    ...model.cards.flatMap((card, index) => [
      clipped(
        `${index + 1}. ${card.title} [${card.status}]`,
        dimensions.columns,
      ),
      clipped(`   ${card.summary}`, dimensions.columns),
    ]),
    clipped(
      `evidence ${model.evidence.map((row) => `${row.label}=${shortRoot(row.value)}`).join(' · ')}`,
      dimensions.columns,
    ),
    clipped(
      `${qualificationLabel} ${qualificationState(model)}${model.notice ? ` · ${model.notice}` : ''}`,
      dimensions.columns,
    ),
    clipped(
      model.footer ??
        '↑↓/jk answer · ←→/hl subject · tab region · [a] Agent Work Lab · r refresh · q quit',
      dimensions.columns,
    ),
  ];
  while (lines.length < dimensions.rows)
    lines.push(' '.repeat(dimensions.columns));
  return lines.slice(0, dimensions.rows).join('\n');
}

export * from './session-workbench-state.js';
export type { SessionWorkbenchProps } from './session-workbench-view.js';

export function SessionWorkbench(props: SessionWorkbenchProps) {
  return <SessionWorkbenchView {...props} />;
}

function controlPlaneKindColor(kind: ProductSearchDocument['kind']): string {
  if (kind === 'work') return 'green';
  if (kind === 'command') return 'yellow';
  if (kind === 'view') return 'magenta';
  return 'cyan';
}

function ControlPlaneResultRows({
  rows,
  selected,
  height,
}: {
  rows: Array<ProductSearchDocument | ProductSearchResult>;
  selected: number;
  height: number;
}) {
  const visibleCount = Math.max(1, height);
  const start = Math.min(
    Math.max(0, selected - visibleCount + 1),
    Math.max(0, rows.length - visibleCount),
  );
  const visible = rows.slice(start, start + visibleCount);
  return (
    <Box flexDirection="column" flexGrow={1} minHeight={0}>
      {visible.length === 0 ? (
        <Text color="yellow">No matching results.</Text>
      ) : null}
      {visible.map((row, index) => {
        const absoluteIndex = start + index;
        return (
          <Box key={row.id} flexDirection="column">
            <Text
              bold={absoluteIndex === selected}
              color={
                absoluteIndex === selected
                  ? 'black'
                  : controlPlaneKindColor(row.kind)
              }
              backgroundColor={absoluteIndex === selected ? 'cyan' : undefined}
              wrap="truncate-end"
            >
              {absoluteIndex === selected ? '›' : ' '}{' '}
              {row.kind.toUpperCase().padEnd(7)} {row.title}
            </Text>
            <Text dimColor wrap="truncate-end">
              {'  '}
              {row.summary}
            </Text>
          </Box>
        );
      })}
    </Box>
  );
}

function QuickCommandRows({
  rows,
  selected,
  height,
}: {
  rows: QuickCommand[];
  selected: number;
  height: number;
}) {
  const visibleCount = Math.max(1, height);
  const start = Math.min(
    Math.max(0, selected - visibleCount + 1),
    Math.max(0, rows.length - visibleCount),
  );
  return (
    <Box flexDirection="column" flexGrow={1} minHeight={0}>
      {rows.length === 0 ? (
        <Text color="yellow">No matching quick action.</Text>
      ) : null}
      {rows.slice(start, start + visibleCount).map((row, index) => {
        const absoluteIndex = start + index;
        return (
          <Box key={row.id} flexDirection="column">
            <Text
              bold={absoluteIndex === selected}
              color={absoluteIndex === selected ? 'black' : 'yellow'}
              backgroundColor={absoluteIndex === selected ? 'cyan' : undefined}
            >
              {absoluteIndex === selected ? '›' : ' '} {row.command.padEnd(10)}{' '}
              {row.title}
            </Text>
            <Text dimColor wrap="truncate-end">
              {'  '}
              {row.summary}
            </Text>
          </Box>
        );
      })}
    </Box>
  );
}

export function ControlPlaneOverlay({
  dimensions,
  state,
  searchResults,
  quickCommands,
  catalogStatus,
}: {
  dimensions: TerminalDimensions;
  state: ControlPlaneState;
  searchResults: ProductSearchResult[];
  quickCommands: QuickCommand[];
  catalogStatus: string;
}) {
  if (state.mode === 'closed') return null;
  const height = terminalCanvasRows(dimensions.rows);
  const width = Math.max(24, dimensions.columns);
  const panelWidth = Math.max(20, width - 4);
  const panelHeight = Math.max(8, height - 2);
  const rowBudget = Math.max(1, Math.floor((panelHeight - 7) / 2));
  const title =
    state.mode === 'help'
      ? 'HELP'
      : state.mode === 'commands'
        ? 'QUICK ACTIONS'
        : state.mode === 'detail'
          ? 'RESULT DETAILS'
          : 'SEARCH KUNGFU';
  return (
    <Box
      position="absolute"
      width={width}
      height={height}
      flexDirection="column"
      overflow="hidden"
    >
      <Box width={width} height={height} flexDirection="column">
        {Array.from(
          { length: height },
          (_, index) => `backdrop-row-${index + 1}`,
        ).map((rowId) => (
          <Text key={rowId} backgroundColor="black">
            {' '.repeat(width)}
          </Text>
        ))}
      </Box>
      <Box
        position="absolute"
        marginLeft={2}
        marginTop={1}
        width={panelWidth}
        height={panelHeight}
        flexDirection="column"
        borderStyle="double"
        borderColor="cyan"
        paddingX={1}
        overflow="hidden"
      >
        <Text bold color="cyan">
          KUNGFU · {title}
        </Text>
        {state.mode === 'help' ? (
          <>
            <Text>? Help · / Quick actions · Ctrl+K Search · Esc return</Text>
            <Text dimColor>
              The focused input accepts text, but free-form Agent conversation
              is not available yet.
            </Text>
            <Text dimColor>
              Search covers system Help, the full governed Kungfu Command
              catalog, global Work, and available product views.
            </Text>
            <Text dimColor>
              Mouse requires terminal click reporting. In iTerm2, allow mouse
              clicks and drags for the active Profile.
            </Text>
            <Text color="yellow">Getting Started: /onboarding</Text>
            <Box marginTop={1} flexDirection="column">
              {quickCommands
                .slice(0, Math.max(1, rowBudget - 1))
                .map((command) => (
                  <Text key={command.id} color="yellow" wrap="truncate-end">
                    {command.command.padEnd(10)} {command.title}
                  </Text>
                ))}
            </Box>
          </>
        ) : null}
        {state.mode === 'commands' ? (
          <>
            <Text dimColor>
              Enter runs the selected bounded action · ↑↓ choose · Esc cancel
            </Text>
            <QuickCommandRows
              rows={quickCommands}
              selected={state.selected}
              height={rowBudget}
            />
          </>
        ) : null}
        {state.mode === 'search' ? (
          <>
            <Text dimColor>
              {catalogStatus} · Enter opens or explains · ↑↓ choose · Esc cancel
            </Text>
            <ControlPlaneResultRows
              rows={searchResults}
              selected={state.selected}
              height={rowBudget}
            />
          </>
        ) : null}
        {state.mode === 'detail' && state.detail ? (
          <Box flexDirection="column" marginTop={1}>
            <Text bold color={controlPlaneKindColor(state.detail.kind)}>
              {state.detail.kind.toUpperCase()} · {state.detail.title}
            </Text>
            <Text>{state.detail.summary}</Text>
            {state.detail.section ? (
              <Text dimColor>Section · {state.detail.section}</Text>
            ) : null}
            {state.detail.action.kind === 'describe-command' ? (
              <Text color="yellow">
                Inspect only · run `{state.detail.action.command} --help` in a
                shell. Search did not execute it.
              </Text>
            ) : null}
            <Text dimColor>Enter / Esc / Backspace returns.</Text>
          </Box>
        ) : null}
      </Box>
    </Box>
  );
}

export const CONTROL_PLANE_CURSOR_BLINK_MS = 530;

function BlinkingInputCursor({ active }: { active: boolean }) {
  const [visible, setVisible] = React.useState(true);
  React.useEffect(() => {
    setVisible(true);
    if (!active) return;
    const timer = setInterval(
      () => setVisible((current) => !current),
      CONTROL_PLANE_CURSOR_BLINK_MS,
    );
    return () => clearInterval(timer);
  }, [active]);
  if (!active) return null;
  return (
    <Text
      color={visible ? 'black' : undefined}
      backgroundColor={visible ? 'cyan' : undefined}
    >
      {' '}
    </Text>
  );
}

export function ControlPlaneBar({
  dimensions,
  state,
  resultCount,
  controlsLabel = 'VIEW CONTROLS',
  controlsHint = 'Workspace shortcuts active',
  workspaceInputActive = false,
}: {
  dimensions: TerminalDimensions;
  state: ControlPlaneState;
  resultCount: number;
  controlsLabel?: string;
  controlsHint?: string;
  workspaceInputActive?: boolean;
}) {
  const { acceptsText, glyph, hint, inputFocused, modeLabel, prompt, tone } =
    controlPlaneBarModel({
      state,
      resultCount,
      controlsLabel,
      controlsHint,
      workspaceInputActive,
    });
  return (
    <Box
      position="absolute"
      marginTop={Math.max(0, terminalCanvasRows(dimensions.rows) - 4)}
      width={dimensions.columns}
      height={4}
      flexDirection="column"
      overflow="hidden"
    >
      <TitledBorderWindow
        columns={dimensions.columns}
        title={modeLabel}
        borderColor={tone}
        rows={[
          <Text
            key="hint"
            color={state.notice ? 'yellow' : inputFocused ? 'white' : 'gray'}
            dimColor={!state.notice}
            wrap="truncate-end"
          >
            {hint}
          </Text>,
          <Text key="prompt" color={tone} wrap="truncate-end">
            <Text bold>{glyph}</Text>
            {prompt ? ` ${prompt}` : ' '}
            {acceptsText && !workspaceInputActive ? (
              <BlinkingInputCursor active={inputFocused} />
            ) : null}
          </Text>,
        ]}
      />
    </Box>
  );
}

export function PlaybackBar({
  dimensions,
  label,
  status,
  hint,
}: {
  dimensions: TerminalDimensions;
  label: string;
  status: string;
  hint: string;
}) {
  return (
    <Box
      position="absolute"
      marginTop={Math.max(0, terminalCanvasRows(dimensions.rows) - 3)}
      width={dimensions.columns}
      height={3}
      flexDirection="column"
      overflow="hidden"
    >
      <TitledBorderWindow
        columns={dimensions.columns}
        title={`${label}  ▶ ${status}`}
        rows={[
          <Text key="hint" color="white" dimColor wrap="truncate-end">
            {hint}
          </Text>,
        ]}
      />
    </Box>
  );
}

export function TerminalAnimation({
  pattern,
  dimensions,
  active = true,
  animate = terminalAnimationsEnabled(process.env),
}: {
  pattern: TerminalAnimationPattern;
  dimensions: TerminalDimensions;
  active?: boolean;
  animate?: boolean;
}) {
  const frame = useTerminalAnimationFrame({
    active,
    enabled: animate,
    pattern,
  });
  const patternSize = terminalAnimationPatternSize(dimensions, pattern);
  const cells = pattern.render(frame, patternSize);
  return (
    <Box flexDirection="column" width={patternSize.width}>
      {cells.map((line, row) => (
        <Text key={`${pattern.id}-${row}`}>
          {line.map((cell, column) => (
            <Text key={`${pattern.id}-${row}-${column}`} color={cell.color}>
              {cell.glyph}
            </Text>
          ))}
        </Text>
      ))}
    </Box>
  );
}

export function TerminalAmbientScene({
  dimensions,
  pattern = KUNGFU_EMPTY_WORK_NEBULA_PATTERN,
  animate,
}: {
  dimensions: TerminalDimensions;
  pattern?: TerminalAnimationPattern;
  animate?: boolean;
}) {
  return (
    <Box
      width={dimensions.columns}
      height={dimensions.rows}
      alignItems="center"
      justifyContent="center"
      overflow="hidden"
    >
      <TerminalAnimation
        pattern={pattern}
        dimensions={dimensions}
        animate={animate}
      />
    </Box>
  );
}

export function TerminalLoadingScene({
  dimensions,
  title,
  status,
  detail,
  pattern = KUNGFU_CIRCULAR_STARTUP_PATTERN,
  animate,
}: {
  dimensions: TerminalDimensions;
  title: string;
  status: string;
  detail?: string;
  pattern?: TerminalAnimationPattern;
  animate?: boolean;
}) {
  const compact = dimensions.columns < 58 || dimensions.rows < 15;
  const animationEnabled = animate ?? terminalAnimationsEnabled(process.env);
  const patternSize = terminalAnimationPatternSize(dimensions, pattern);
  return (
    <Box
      width={dimensions.columns}
      height={dimensions.rows}
      alignItems="center"
      justifyContent="center"
      overflow="hidden"
    >
      <Box flexDirection="row" alignItems="center" gap={compact ? 1 : 3}>
        <TerminalAnimation
          pattern={pattern}
          dimensions={dimensions}
          animate={animationEnabled}
        />
        <Box
          flexDirection="column"
          width={Math.max(
            8,
            Math.min(
              44,
              dimensions.columns - patternSize.width - (compact ? 1 : 3),
            ),
          )}
        >
          <Text bold color="cyan">
            {title}
          </Text>
          <Text>{status}</Text>
          {!compact && detail ? <Text dimColor>{detail}</Text> : null}
          <Text dimColor>
            {animationEnabled ? 'Loading · live' : 'Loading'}
          </Text>
        </Box>
      </Box>
    </Box>
  );
}
